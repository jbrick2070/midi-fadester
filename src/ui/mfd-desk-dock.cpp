/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-desk-dock.hpp"

#include "core/apc40-palette.h"
#include "core/takeover.h"
#include "core/vj-pads.h"
#include "obs/mfd-engine.h"
#include "ui/mfd-text-qt.hpp"
#include "vs/vs-module.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QColor>
#include <QDial>
#include <QDialog>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QWheelEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QScrollArea>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace {

constexpr const char *kDockId = "midi-fadester-desk";
void (*g_open_settings)(void) = nullptr; /* the settings dialog's opener, for the gear button */
constexpr int kStrips = (int)MFD_VJ_STRIPS;
constexpr int kRows = 5; /* top to bottom: Scene fade, Show, Fade, Flash, Blend */
constexpr uint8_t kChPulse = 10U;
constexpr uint8_t kChBlink = 15U;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kLeds = 55U; /* 6 per strip, 5 scenes, Record, Session */
/* No pad or button is lower than this: at 14 or 22 px they were smaller than everything else in OBS, and on a laptop
 * at 125 % too small to hit in a dark room (U1-11). */
constexpr int kButtonHeight = 28;

/* Row r (0 = top) is this cell of the strip (mfd_vj_pads_leds' order per strip) and this note base. */
const int kRowCell[kRows] = {MFD_DESK_RESTART, MFD_DESK_SHOW, MFD_DESK_FADE, MFD_DESK_FLASH, MFD_DESK_BLEND};
const uint8_t kRowNote[kRows] = {MFD_VJ_NOTE_ROW_RESTART, MFD_VJ_NOTE_ROW_SHOW, MFD_VJ_NOTE_ROW_FADE,
				 MFD_VJ_NOTE_ROW_FLASH, MFD_VJ_NOTE_ROW_BLEND};

/* Row r's name (0 = top). */
QString rowName(int r)
{
	switch (r) {
	case 0:
		return mfdText("Desk.RowSceneFade",
			       "Cut, then fade Preview on air over 32, 16, 8, 4, 2, 1 or \xC2\xBD beats");
	case 1:
		return mfdText("Desk.RowShow", "Show / hide (Shift: show at 100 %)");
	case 2:
		return mfdText("Desk.RowFade", "Fade out and back (Shift: cut out)");
	case 3:
		return mfdText("Desk.RowFlash", "Flash while held (Shift: dark while held)");
	default:
		return mfdText("Desk.RowBlend", "Blend (Shift: next blend)");
	}
}

/* Row r's short name for the grid (its full name is the tooltip); with Shift armed, what Shift makes it do. */
QString rowShortName(int r, bool shift = false)
{
	if (shift) {
		switch (r) {
		case 0:
			return mfdText("Desk.RowSceneFadeShort", "Fade, beats"); /* Shift changes nothing here */
		case 1:
			return mfdText("Desk.RowWakeShort", "Show at 100 %");
		case 2:
			return mfdText("Desk.RowSnapShort", "Cut out");
		case 3:
			return mfdText("Desk.RowDarkShort",
				       "Dark (hold)"); /* Blackout names the Program blackout only */
		default:
			return mfdText("Desk.RowPunchModeShort", "Next blend");
		}
	}
	switch (r) {
	case 0:
		return mfdText("Desk.RowSceneFadeShort", "Fade, beats");
	case 1:
		return mfdText("Desk.RowShowShort", "Show / hide");
	case 2:
		return mfdText("Desk.RowFadeShort", "Fade out");
	case 3:
		return mfdText("Desk.RowFlashShort", "Flash");
	default:
		return mfdText("Desk.RowBlendShort", "Blend");
	}
}

/* What is printed on a pad of the top row: a cut mark that needs no translation and fits a 30 px pad in every
 * language, then the fade's length in beats. The longest is not 32 beats below 96 BPM: OBS caps a transition at 20 s,
 * and the pad then says so. */
QString sceneFadePadText(int column, bool capped)
{
	static const char *const k_beats[] = {"32", "16", "8", "4", "2", "1", "\xC2\xBD"};
	if (column <= 0) {
		return QString::fromUtf8("\xE2\x9C\x82"); /* scissors */
	}
	if (column == 1 && capped) {
		return QStringLiteral("20s");
	}
	return QString::fromUtf8(k_beats[std::min(column, 7) - 1]);
}

/* The scene buttons' tooltip in Studio Mode: the key that sends a scene straight on air is Command on a Mac (Qt's
 * ControlModifier there). */
QString sceneTipStudio()
{
#ifdef __APPLE__
	return mfdText("Desk.SceneTip.StudioMac",
		       "Click: to Preview; click again: fade it on air. Cmd-click: straight on air.");
#else
	return mfdText("Desk.SceneTip.Studio",
		       "Click: to Preview; click again: fade it on air. Ctrl-click: straight on air.");
#endif
}

QString rgbCss(uint32_t rgb)
{
	return QStringLiteral("#%1").arg(rgb & 0xFFFFFFU, 6, 16, QLatin1Char('0'));
}

/* Dark text on a light colour, light text on a dark one (relative luminance, sRGB). */
bool lightBackground(uint32_t rgb)
{
	auto lin = [](uint32_t c) {
		double x = (double)(c & 0xFFU) / 255.0;
		return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
	};
	double y = 0.2126 * lin(rgb >> 16) + 0.7152 * lin(rgb >> 8) + 0.0722 * lin(rgb);
	return y > 0.22;
}

uint32_t mix(uint32_t a, uint32_t b, double t)
{
	auto ch = [&](int shift) {
		double x = (double)((a >> shift) & 0xFFU) * (1.0 - t) + (double)((b >> shift) & 0xFFU) * t;
		return (uint32_t)std::lround(x) & 0xFFU;
	};
	return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

/* An LED's colour as a screen shows it: a dim LED still glows on the desk, so a dark palette colour is lifted until its
 * brightest channel reads about as bright (palette 1, dim white, would be near black on the dark dock). Black stays. */
uint32_t onScreen(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xFFU, g = (rgb >> 8) & 0xFFU, b = rgb & 0xFFU;
	uint32_t top = std::max(r, std::max(g, b));
	if (top == 0U || top >= 110U) {
		return rgb;
	}
	auto up = [&](uint32_t c) {
		return std::min(255U, c * 110U / top);
	};
	return (up(r) << 16) | (up(g) << 8) | up(b);
}

/* What an LED shows at this moment: animated pads move between their dim and full colour over a two-beat cycle. */
uint32_t ledColor(const struct mfd_vj_led &led, double beat_pos, uint32_t lamp_on)
{
	if (!led.rgb) {
		return led.velocity ? lamp_on : 0x202020U;
	}
	uint32_t full = mfd_apc40_palette_rgb(led.velocity);
	if (led.channel != kChPulse && led.channel != kChBlink) {
		return onScreen(full);
	}
	uint32_t dim = mfd_apc40_palette_rgb(led.primary);
	double cycle = std::fmod(beat_pos / 2.0, 1.0); /* "1/2": a two-beat cycle */
	if (led.channel == kChBlink) {
		return onScreen(cycle < 0.5 ? full : dim);
	}
	return onScreen(mix(dim, full, 0.5 - 0.5 * std::cos(cycle * 2.0 * kPi)));
}

/* Qt re-polishes on every setStyleSheet and sends an event on every setToolTip / setText: only on change (30 Hz). */
void setCss(QWidget *w, const QString &css)
{
	if (w->styleSheet() != css) {
		w->setStyleSheet(css);
	}
}

void setTip(QWidget *w, const QString &tip)
{
	if (w->toolTip() != tip) {
		w->setToolTip(tip);
	}
}

void setLabel(QLabel *l, const QString &text)
{
	if (l->text() != text) {
		l->setText(text);
	}
}

void setButtonText(QPushButton *b, const QString &text)
{
	if (b->text() != text) {
		b->setText(text);
	}
}

/* False for a pressed that a held pointer re-sent by sliding back onto the button (MfdDeskDock::eventFilter). */
bool physical(QObject *b)
{
	return !b->property("mfdSlid").toBool();
}

/* An endless speed encoder: each notch is one 0.05x step, like the APC40's Tempo knob. Clicking never jumps (the press
 * only anchors); a large jump between two positions is ignored; the scroll wheel and keys act only once it has focus,
 * so scrolling the dock never changes the video's speed. */
class MfdSpeedDial final : public QDial {
public:
	explicit MfdSpeedDial(std::function<void(int)> on_steps, QWidget *parent = nullptr)
		: QDial(parent),
		  on_steps_(std::move(on_steps))
	{
		setWrapping(true);
		setRange(0, kNotches - 1);
		setNotchesVisible(true);
		setFocusPolicy(Qt::StrongFocus);
		connect(this, &QDial::valueChanged, this, [this](int value) { moved(value); });
	}

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		anchoring_ = true; /* QDial moves to the pointer on press: that is not a turn */
		QDial::mousePressEvent(e);
		anchoring_ = false;
		last_ = value();
	}
	void wheelEvent(QWheelEvent *e) override
	{
		if (!hasFocus()) {
			e->ignore(); /* the dock scrolls */
			return;
		}
		wheel_ += e->angleDelta().y();
		int steps = wheel_ / 120;
		wheel_ -= steps * 120;
		if (steps != 0) {
			on_steps_(std::clamp(steps, -5, 5));
		}
		e->accept();
	}
	void keyPressEvent(QKeyEvent *e) override
	{
		switch (e->key()) {
		case Qt::Key_Up:
		case Qt::Key_Right:
			on_steps_(1);
			break;
		case Qt::Key_Down:
		case Qt::Key_Left:
			on_steps_(-1);
			break;
		case Qt::Key_PageUp:
			on_steps_(5);
			break;
		case Qt::Key_PageDown:
			on_steps_(-5);
			break;
		case Qt::Key_Home:
		case Qt::Key_End:
			break; /* no jumps on an endless dial */
		default:
			e->ignore(); /* Escape and the rest go to the dock */
			return;
		}
		e->accept();
	}

private:
	static constexpr int kNotches = 24;
	void moved(int value)
	{
		int delta = value - last_;
		last_ = value;
		if (anchoring_) {
			return;
		}
		if (delta > kNotches / 2) {
			delta -= kNotches; /* across the wrap */
		} else if (delta < -kNotches / 2) {
			delta += kNotches;
		}
		if (delta != 0) { /* a fast spin counts, up to 6 notches (a quarter turn) in one move */
			on_steps_(std::clamp(delta, -6, 6));
		}
	}
	std::function<void(int)> on_steps_;
	bool anchoring_ = false;
	int last_ = 0;
	int wheel_ = 0;
};

/* A column's fader on the dock: its level, and while the fader waits to pick up, the bar in amber with a marker line
 * where the physical fader starts to act (the marker the settings dialog's strip card draws). Without it a fader that
 * waits looks dead in the middle of a show (U1-14). */
class MfdFaderBar final : public QWidget {
public:
	explicit MfdFaderBar(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedHeight(46);
		setMinimumWidth(14);
		setMaximumWidth(22);
	}
	void setState(float level, bool pickup, float target)
	{
		const int l = (int)std::lround(std::clamp(level, 0.0f, 1.0f) * 1000.0f);
		const int t = (int)std::lround(std::clamp(target, 0.0f, 1.0f) * 1000.0f);
		if (l == level_ && pickup == pickup_ && (!pickup || t == target_)) {
			return;
		}
		level_ = l;
		pickup_ = pickup;
		target_ = t;
		setProperty("showingPickup", pickup_);
		setProperty("pickupPermille", pickup_ ? target_ : -1);
		update();
	}
	bool showingPickup() const { return pickup_; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const QRectF box(0.5, 0.5, (qreal)width() - 1.0, (qreal)height() - 1.0);
		p.setPen(QPen(QColor(0x33, 0x41, 0x55), 1.0));
		p.setBrush(QColor(0x11, 0x18, 0x27));
		p.drawRect(box);
		const qreal inner_h = box.height() - 2.0;
		const qreal fill_h = inner_h * (qreal)level_ / 1000.0;
		if (fill_h > 0.5) {
			p.setPen(Qt::NoPen);
			p.setBrush(pickup_ ? QColor(0xf5, 0x9e, 0x0b) : QColor(0x94, 0xa3, 0xb8));
			p.drawRect(QRectF(box.left() + 1.0, box.bottom() - 1.0 - fill_h, box.width() - 2.0, fill_h));
		}
		if (pickup_) {
			const qreal y = box.bottom() - 1.0 - inner_h * (qreal)target_ / 1000.0;
			p.setPen(QPen(QColor(0xfb, 0xbf, 0x24), 2.5));
			p.drawLine(QPointF(0.0, std::clamp(y, 1.5, (qreal)height() - 1.5)),
				   QPointF((qreal)width(), std::clamp(y, 1.5, (qreal)height() - 1.5)));
		}
	}

private:
	int level_ = -1;
	int target_ = 0;
	bool pickup_ = false;
};

class MfdDeskDock final : public QWidget {
public:
	explicit MfdDeskDock(struct mfd_engine *eng, QWidget *parent = nullptr);
	~MfdDeskDock() override
	{
		releaseHolds();
		mfd_engine_set_desk_visible(eng_, false); /* deleted while shown (no hide): the engine stops asking */
	}

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	bool event(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *obj, QEvent *e) override;

public:
	void refresh();

private:
	QPushButton *makeButton(const QString &accessible, int min_w, int min_h);
	void pressPad(int row, int strip, bool on);
	void pressNote(uint8_t channel, uint8_t note, bool on, bool shift);
	void releaseHolds();
	void sayStatus(const QString &text, bool over_reason = true);
	void paintButton(QPushButton *b, uint32_t rgb, bool enabled);
	void applyFontSizes();
	void showHelp();
	QPushButton *help_btn_ = nullptr;
	QPushButton *gear_btn_ = nullptr;
	QPointer<QDialog> help_;
	int base_px_ = 0;    /* the font size the larger texts were last worked out from */
	bool built_ = false; /* the constructor has made every widget (a font or style change can arrive before) */

	struct mfd_engine *eng_ = nullptr;
	QLabel *title_ = nullptr;
	QLabel *on_air_ = nullptr;  /* "ON AIR  <scene>", red */
	QLabel *preview_ = nullptr; /* "PREVIEW  <scene>", green; Studio Mode only */
	QLabel *row_names_[kRows] = {};
	QLabel *timing_ = nullptr;
	QPushButton *reconnect_ = nullptr;
	QPushButton *turn_on_ = nullptr; /* the pads, when they are switched off in the settings */
	QLabel *status_ = nullptr;
	QLabel *studio_note_ = nullptr;
	QPushButton *studio_btn_ = nullptr;
	QWidget *grid_box_ = nullptr; /* the pad grid: hidden while the pads cannot work for a lasting reason */
	QPushButton *scenes_[MFD_VJ_SCENES] = {};
	QLabel *col_names_[kStrips] = {};
	QPushButton *pads_[kRows][kStrips] = {};
	QPushButton *clip_stops_[kStrips] = {};
	QPushButton *stop_all_ = nullptr;
	QPushButton *shift_ = nullptr;
	QPushButton *session_ = nullptr; /* the dock's Fade */
	QPushButton *blackout_btn_ = nullptr;
	QLabel *blackout_ = nullptr;
	MfdFaderBar *faders_[kStrips] = {};
	QProgressBar *xfader_ = nullptr;
	QLabel *bpm_ = nullptr;
	QLabel *lock_ = nullptr;
	QLabel *beats_[4] = {};
	QLabel *panic_ = nullptr;
	QPushButton *vs_panic_ = nullptr;
	QPushButton *vs_tap_ = nullptr;
	QPushButton *vs_lock_ = nullptr;
	QPushButton *vs_downbeat_ = nullptr;
	MfdSpeedDial *speed_dial_ = nullptr;
	QPushButton *vj_play_ = nullptr;
	QPushButton *vj_presets_[6] = {};
	QWidget *vj_box_ = nullptr; /* the video speed rows: shown only when switched on (the Advanced tab) */
	void vjSteps(int steps);
	QElapsedTimer clock_;
	QElapsedTimer status_age_;
	struct mfd_desk_view view_ = {};
	bool holding_[kStrips] = {};
	uint32_t hold_epoch_[kStrips] = {};
	double bpm_value_ = 120.0;
	bool bpm_valid_ = false;
	bool status_is_reason_ = false;
	bool status_over_reason_ = true; /* the last message may stand over a reason (a speed asked for) */
	bool vs_loaded_ = false;
	long long last_beat_index_ = -1;
};

QPushButton *MfdDeskDock::makeButton(const QString &accessible, int min_w, int min_h)
{
	auto *b = new QPushButton(this);
	b->installEventFilter(this); /* notices a held pointer sliding off: see physical() */
	b->setAccessibleName(accessible);
	b->setToolTip(accessible);
	b->setMinimumSize(min_w, min_h);
	b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	b->setFocusPolicy(Qt::TabFocus);
	return b;
}

MfdDeskDock::MfdDeskDock(struct mfd_engine *eng, QWidget *parent) : QWidget(parent), eng_(eng)
{
	setObjectName(QStringLiteral("mfdDeskDock"));
	setMinimumSize(300, 240);
	setStyleSheet(QStringLiteral("QWidget#mfdDeskDock, QWidget#mfdDeskBody { background-color: #0b1120; }"
				     "QScrollArea { border: none; background-color: #0b1120; }"
				     "QLabel { color: #e2e8f0; }"
				     "QPushButton { border: 1px solid #334155; border-radius: 3px; color: #0b1120;"
				     " padding: 1px; }"
				     "QPushButton:focus { border: 2px solid #f8fafc; }"
				     "QProgressBar { border: 1px solid #334155; background: #111827; }"
				     "QProgressBar::chunk { background: #94a3b8; }"));

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *body = new QWidget(scroll);
	body->setObjectName(QStringLiteral("mfdDeskBody"));
	body->setMinimumWidth(380); /* below that it scrolls sideways rather than squeezing the grid */
	scroll->setWidget(body);
	outer->addWidget(scroll);
	auto *root = new QVBoxLayout(body);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);

	title_ = new QLabel(this);
	title_->setObjectName(QStringLiteral("mfdDeskTitle"));
	title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	auto *title_row = new QHBoxLayout();
	title_row->setSpacing(4);
	title_row->addWidget(title_, 1);
	/* the only guide to the pads, where the pads are (it used to be three paragraphs on the Advanced tab) */
	help_btn_ = makeButton(mfdText("Desk.Help.Title", "What the pads do"), kButtonHeight, kButtonHeight);
	help_btn_->setObjectName(QStringLiteral("mfdDeskHelpBtn"));
	help_btn_->setText(QStringLiteral("?"));
	help_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	connect(help_btn_, &QPushButton::clicked, this, [this]() { showHelp(); });
	title_row->addWidget(help_btn_);
	/* the way from the show screen to the settings (it used to be the Tools menu only) */
	gear_btn_ = makeButton(mfdText("Desk.Settings", "Midi Fadester settings"), kButtonHeight, kButtonHeight);
	gear_btn_->setObjectName(QStringLiteral("mfdDeskGearBtn"));
	gear_btn_->setText(QString::fromUtf8("\xE2\x9A\x99"));
	gear_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	connect(gear_btn_, &QPushButton::clicked, this, []() {
		if (g_open_settings) {
			g_open_settings();
		}
	});
	title_row->addWidget(gear_btn_);
	root->addLayout(title_row);
	/* Panic first: it must be seen however short the dock is */
	panic_ = new QLabel(this);
	panic_->setObjectName(QStringLiteral("mfdDeskPanicBar"));
	panic_->setWordWrap(true);
	root->addWidget(panic_);
	blackout_ = new QLabel(this); /* its own line: Panic and Blackout can both be on */
	blackout_->setObjectName(QStringLiteral("mfdDeskBlackoutBar"));
	blackout_->setWordWrap(true);
	root->addWidget(blackout_);

	/* why something waits, at the top: in a short dock the last row is scrolled out of view (U1-06). The label always
	 * keeps one line, so a message that comes and goes never moves the buttons under the pointer. */
	auto *status_row = new QHBoxLayout();
	status_ = new QLabel(this);
	status_->setObjectName(QStringLiteral("mfdDeskStatus"));
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color: #94a3b8;"));
	status_->setMinimumHeight(status_->fontMetrics().lineSpacing());
	status_row->addWidget(status_, 1);
	turn_on_ = makeButton(mfdText("Desk.TurnOnTip", "Turn the APC40 pads on again (the same switch is on the "
							"Advanced tab of Midi Fadester's settings)"),
			      60, kButtonHeight);
	turn_on_->setObjectName(QStringLiteral("mfdDeskTurnOn"));
	turn_on_->setText(mfdText("Desk.TurnOn", "Turn on"));
	turn_on_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	connect(turn_on_, &QPushButton::clicked, this, [this]() {
		mfd_engine_set_vj_buttons(eng_, true, mfd_engine_vj_fade_ms(eng_));
		mfd_engine_save_global_config(eng_);
		mfd_engine_notify_state_changed(eng_); /* the settings dialog's switch follows */
		refresh();
	});
	status_row->addWidget(turn_on_);
	reconnect_ = makeButton(mfdText("Desk.ReconnectTip", "Look for the APC40 mkII again"), 80, kButtonHeight);
	reconnect_->setObjectName(QStringLiteral("mfdDeskReconnect"));
	reconnect_->setText(mfdText("Desk.Reconnect", "Reconnect"));
	reconnect_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	connect(reconnect_, &QPushButton::clicked, this, [this]() {
		mfd_engine_poll_midi_health_on_ui_thread(eng_);
		refresh();
	});
	status_row->addWidget(reconnect_);
	root->addLayout(status_row);

	/* Studio Mode off: said in words, with the button that turns it on (U1-07). Its own row: the status line above
	 * is shared with messages that stand for a few seconds. */
	auto *studio_row = new QHBoxLayout();
	studio_note_ = new QLabel(mfdText("Msg.StudioModeOff", "Studio Mode is off."), this);
	studio_note_->setObjectName(QStringLiteral("mfdDeskStudioNote"));
	studio_note_->setStyleSheet(QStringLiteral("color: #fde68a;"));
	studio_note_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	studio_row->addWidget(studio_note_, 1);
	studio_btn_ = makeButton(mfdText("Strip.TurnOnStudioMode", "Turn on Studio Mode"), 80, kButtonHeight);
	studio_btn_->setObjectName(QStringLiteral("mfdDeskStudioBtn"));
	studio_btn_->setText(mfdText("Strip.TurnOnStudioMode", "Turn on Studio Mode"));
	studio_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	studio_btn_->setStyleSheet(QStringLiteral("QPushButton { background-color: #fbbf24; color: #0b1120; "
						  "padding: 2px 8px; }"));
	connect(studio_btn_, &QPushButton::clicked, this, [this]() {
		mfd_engine_request_studio_mode(eng_, true);
		refresh();
	});
	studio_row->addWidget(studio_btn_);
	root->addLayout(studio_row);

	/* What the audience sees and what is next: the largest text on the dock, one under the other (two chips side by
	 * side do not fit the 364 px body), each with its word, so red against green is never the only sign (U1-03). */
	auto *air_box = new QVBoxLayout();
	air_box->setSpacing(3);
	on_air_ = new QLabel(this);
	on_air_->setObjectName(QStringLiteral("mfdDeskOnAir"));
	on_air_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	air_box->addWidget(on_air_);
	preview_ = new QLabel(this);
	preview_->setObjectName(QStringLiteral("mfdDeskPreview"));
	preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	air_box->addWidget(preview_);
	root->addLayout(air_box);

	/* Scene Launch 1..5 */
	auto *scene_row = new QHBoxLayout();
	scene_row->setSpacing(4);
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		QPushButton *b = makeButton(mfdFill(mfdText("Desk.SceneButton", "Scene %1"), {QString::number(j + 1)}),
					    60, kButtonHeight);
		scenes_[j] = b;
		b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); /* a long name never widens the dock */
		connect(b, &QPushButton::pressed, this, [this, j, b]() {
			if (!physical(b)) {
				return;
			}
			/* Ctrl-click (Cmd on a Mac): straight to Program, as Shift + Scene Launch on the desk */
			bool direct = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
			pressNote(0U, (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)j), true, direct);
		});
		connect(b, &QPushButton::released, this,
			[this, j]() { pressNote(0U, (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)j), false, false); });
		scene_row->addWidget(b);
	}
	root->addLayout(scene_row);

	/* the pad grid: the scene fades on top in their own green frame (they work on the whole scene, not on a column),
	 * then the strips' source names over the per-column rows, with the row names beside them */
	grid_box_ = new QWidget(this);
	grid_box_->setObjectName(QStringLiteral("mfdDeskGrid"));
	auto *grid = new QGridLayout(grid_box_);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setHorizontalSpacing(3);
	grid->setVerticalSpacing(3);
	auto *fade_frame = new QFrame(this); /* made before the pads, so it draws under them */
	fade_frame->setObjectName(QStringLiteral("mfdDeskFadeFrame"));
	fade_frame->setStyleSheet(QStringLiteral(
		"#mfdDeskFadeFrame { border: 1px solid #22c55e; border-radius: 4px; background: transparent; }"));
	fade_frame->setAttribute(Qt::WA_TransparentForMouseEvents);
	grid->addWidget(fade_frame, 0, 0, 1, kStrips + 2);
	grid->setRowMinimumHeight(0, 32);            /* room for the frame above and below the pads */
	grid->setColumnMinimumWidth(kStrips + 1, 4); /* and on the right */
	grid->setRowMinimumHeight(1, 4);             /* a gap between the scene fades and the columns */
	for (int i = 0; i < kStrips; ++i) {
		col_names_[i] = new QLabel(this);
		col_names_[i]->setAlignment(Qt::AlignCenter);
		col_names_[i]->setMinimumWidth(30);
		col_names_[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		grid->addWidget(col_names_[i], 2, i + 1);
	}
	for (int r = 0; r < kRows; ++r) {
		auto *name = new QLabel(rowShortName(r), this);
		name->setToolTip(rowName(r));
		name->setMaximumWidth(160); /* room for long languages and the wider macOS font */
		row_names_[r] = name;
		const int grid_row = r == 0 ? 0
					    : r + 2; /* the scene fades on top, the columns' rows under their names */
		if (r == 0) {
			name->setIndent(6); /* inside the frame */
		}
		grid->addWidget(name, grid_row, 0);
		for (int i = 0; i < kStrips; ++i) {
			QPushButton *b = makeButton(mfdFill(mfdText("Desk.PadName", "%1, column %2"),
							    {rowName(r), QString::number(i + 1)}),
						    30, kButtonHeight);
			pads_[r][i] = b;
			b->setObjectName(QStringLiteral("mfdDeskPad_%1_%2").arg(r).arg(i));
			if (r == 0) {
				b->setText(sceneFadePadText(i, false));
			}
			connect(b, &QPushButton::pressed, this, [this, r, i, b]() {
				if (physical(b)) {
					pressPad(r, i, true);
				}
			});
			connect(b, &QPushButton::released, this, [this, r, i]() { pressPad(r, i, false); });
			grid->addWidget(b, grid_row, i + 1, Qt::AlignVCenter);
		}
	}
	auto *clip_name = new QLabel(mfdText("Desk.RowClipStopShort", "Reset"), this);
	clip_name->setToolTip(mfdText("Desk.RowClipStopTip",
				      "Reset: this column back to its own blend (Shift: pin the column). On the "
				      "APC40: Clip Stop."));
	clip_name->setMaximumWidth(160); /* room for long languages and the wider macOS font */
	grid->addWidget(clip_name, kRows + 2, 0);
	for (int i = 0; i < kStrips; ++i) {
		QPushButton *b =
			makeButton(mfdFill(mfdText("Desk.ClipStopName", "Reset, column %1"), {QString::number(i + 1)}),
				   30, kButtonHeight);
		clip_stops_[i] = b;
		connect(b, &QPushButton::pressed, this, [this, i, b]() {
			if (!physical(b)) {
				return;
			}
			/* the latched Shift pins the column, as the lit Clip Stop row and its tooltip promise (F-01) */
			bool shift = shift_->isChecked();
			pressNote((uint8_t)i, MFD_VJ_NOTE_CLIP_STOP, true, shift);
			if (shift) {
				shift_->setChecked(false);
			}
		});
		connect(b, &QPushButton::released, this,
			[this, i]() { pressNote((uint8_t)i, MFD_VJ_NOTE_CLIP_STOP, false, false); });
		grid->addWidget(b, kRows + 2, i + 1);
	}

	/* Reset all and Shift (latched for the next pad) belong to the pads and go with them; Fade and Blackout stand in
	 * their own row below, Fade the wider of the two. Two rows, so no label has to be cut short in any language. */
	auto *pad_row = new QHBoxLayout();
	auto *transport = new QHBoxLayout();
	stop_all_ =
		makeButton(mfdText("Desk.StopAllTip", "Reset all: every effect off and every source back to its own "
						      "blend. With Shift: fade every source out. On the "
						      "APC40: Stop All Clips."),
			   80, kButtonHeight);
	stop_all_->setText(mfdText("Desk.StopAllShort", "Reset all"));
	connect(stop_all_, &QPushButton::pressed, this, [this]() {
		if (!physical(stop_all_)) {
			return;
		}
		bool shift = shift_->isChecked();
		pressNote(0U, MFD_VJ_NOTE_STOP_ALL, true, shift);
		if (shift) {
			shift_->setChecked(false);
		}
	});
	connect(stop_all_, &QPushButton::released, this,
		[this]() { pressNote(0U, MFD_VJ_NOTE_STOP_ALL, false, false); });
	shift_ = makeButton(mfdText("Desk.Shift", "Shift for the next pad"), 60, kButtonHeight);
	shift_->setText(mfdText("Desk.ShiftShort", "Shift"));
	shift_->setCheckable(true);
	session_ = makeButton(mfdText("Desk.Session", "Fade Preview on air"), 70, kButtonHeight);
	session_->setObjectName(QStringLiteral("mfdDeskFadeBtn"));
	session_->setText(mfdText("Desk.SessionShort", "Fade"));
	connect(session_, &QPushButton::clicked, this, [this]() {
		if (eng_) {
			mfd_engine_desk_fade(eng_); /* with or without the desk */
		}
	});
	blackout_btn_ = makeButton(mfdText("Desk.Blackout",
					   "Blackout: the audience sees black; again: back. On the APC40: Session."),
				   70, kButtonHeight);
	blackout_btn_->setObjectName(QStringLiteral("mfdDeskBlackoutBtn"));
	blackout_btn_->setText(mfdText("Desk.RowBlackoutShort", "Blackout"));
	connect(blackout_btn_, &QPushButton::clicked, this, [this]() {
		if (eng_) {
			mfd_engine_desk_blackout(eng_); /* always reachable, so a blackout can always be ended */
		}
	});
	pad_row->addWidget(stop_all_);
	pad_row->addWidget(shift_);
	transport->addWidget(session_, 2);
	transport->addSpacing(12); /* Blackout stands apart from Fade, the most used button */
	transport->addWidget(blackout_btn_, 1);

	/* fader levels (read-only), under their columns */
	auto *fader_name = new QLabel(mfdText("Desk.Faders", "Faders"), this);
	grid->addWidget(fader_name, kRows + 3, 0);
	for (int i = 0; i < kStrips; ++i) {
		auto *bar = new MfdFaderBar(this);
		bar->setObjectName(QStringLiteral("mfdDeskFader_%1").arg(i));
		faders_[i] = bar;
		grid->addWidget(bar, kRows + 3, i + 1, Qt::AlignHCenter);
	}
	root->addWidget(grid_box_);
	root->addLayout(pad_row);
	root->addLayout(transport);
	xfader_ = new QProgressBar(this);
	xfader_->setRange(0, 1000);
	xfader_->setTextVisible(false);
	xfader_->setFixedHeight(10);
	xfader_->setAccessibleName(mfdText("Desk.Crossfader", "Crossfader (T-bar)"));
	xfader_->setToolTip(xfader_->accessibleName());
	root->addWidget(xfader_);
	timing_ = new QLabel(this);
	timing_->setObjectName(QStringLiteral("mfdDeskTiming"));
	timing_->setStyleSheet(QStringLiteral("color: #94a3b8;"));
	root->addWidget(timing_);

	/* Visual Sidechain */
	auto *vs_row = new QHBoxLayout();
	bpm_ = new QLabel(this);
	bpm_->setObjectName(QStringLiteral("mfdDeskBpm"));
	bpm_->setMinimumWidth(72);
	vs_row->addWidget(bpm_);
	lock_ = new QLabel(this);
	lock_->setWordWrap(true); /* "Visual Sidechain is not loaded." is long in some languages */
	vs_row->addWidget(lock_, 1);
	for (int k = 0; k < 4; ++k) {
		beats_[k] = new QLabel(this);
		beats_[k]->setFixedSize(14, 14);
		vs_row->addWidget(beats_[k]);
	}
	root->addLayout(vs_row);
	auto *vs_buttons = new QHBoxLayout();
	vs_panic_ = makeButton(mfdText("Desk.VsPanic",
				       "Panic: all Visual Sidechain effects off; again: on again. On the APC40: "
				       "Record."),
			       70, kButtonHeight);
	vs_panic_->setObjectName(QStringLiteral("mfdDeskPanicBtn"));
	vs_panic_->setText(mfdText("Desk.VsPanicShort", "Panic"));
	vs_tap_ = makeButton(mfdText("Desk.VsTap", "Tap the tempo. On the APC40: Metronome."), 50, kButtonHeight);
	vs_tap_->setText(mfdText("Desk.VsTapShort", "Tap"));
	vs_lock_ = makeButton(mfdText("Desk.VsLock",
				      "Hold the BPM, or let it follow the music again. On the APC40: Shift + "
				      "Metronome."),
			      50, kButtonHeight);
	vs_lock_->setText(mfdText("Desk.VsLockShort", "Hold BPM"));
	vs_downbeat_ = makeButton(mfdText("Desk.VsDownbeat", "Now is beat 1. On the APC40: Shift + Tap Tempo."), 70,
				  kButtonHeight);
	vs_downbeat_->setText(mfdText("Desk.VsDownbeatShort", "Beat 1"));
	const std::pair<QPushButton *, enum mfd_vj_command_kind> vs[] = {{vs_panic_, MFD_VJ_CMD_VS_PANIC},
									 {vs_tap_, MFD_VJ_CMD_VS_TAP},
									 {vs_lock_, MFD_VJ_CMD_VS_LOCK},
									 {vs_downbeat_, MFD_VJ_CMD_VS_RESYNC}};
	for (const auto &p : vs) {
		enum mfd_vj_command_kind kind = p.second;
		QPushButton *vb = p.first;
		connect(vb, &QPushButton::pressed, this, [this, kind, vb]() {
			if (!physical(vb)) {
				return;
			}
			if (!mfd_engine_desk_vs(eng_, kind)) {
				sayStatus(mfdText("Desk.Busy", "Too many presses at once; try again."));
			}
		});
		vs_buttons->addWidget(p.first);
	}
	root->addLayout(vs_buttons);

	/* VJster: the video's speed through Fadester Companion. A dial for fine changes (the desk's Tempo knob), a row of
	 * preset speeds with 1x (the desk's Tap Tempo), and Play / Pause. */
	vj_box_ = new QWidget(this);
	vj_box_->setObjectName(QStringLiteral("mfdDeskVideoBox"));
	auto *vj_col = new QVBoxLayout(vj_box_);
	vj_col->setContentsMargins(0, 0, 0, 0);
	vj_col->setSpacing(6);
	auto *vj_row = new QHBoxLayout();
	auto *vj_name = new QLabel(mfdText("Desk.VjsterTitle", "Video speed"), this);
	vj_name->setToolTip(mfdText("Desk.VjsterNote",
				    "Video speed in VJster (Chrome or the OBS video page), through Fadester "
				    "Companion with VJster on (the preset speeds need 0.1.17 or later). Midi "
				    "Fadester does not hear back: watch the video."));
	vj_row->addWidget(vj_name);
	speed_dial_ = new MfdSpeedDial([this](int steps) { vjSteps(steps); }, this);
	speed_dial_->setObjectName(QStringLiteral("mfdDeskSpeedDial"));
	speed_dial_->setFixedSize(48, 48);
	speed_dial_->setAccessibleName(mfdText("Desk.VjsterDial",
					       "Speed dial: 0.05x per notch (click it first to use the "
					       "scroll wheel or the arrow keys)"));
	speed_dial_->setToolTip(speed_dial_->accessibleName());
	vj_row->addWidget(speed_dial_);
	vj_play_ = makeButton(mfdText("Desk.VjsterPlayTip", "Play or pause the video. On the APC40: Play."), 70,
			      kButtonHeight);
	vj_play_->setText(mfdText("Desk.VjsterPlay", "Play / Pause"));
	connect(vj_play_, &QPushButton::pressed, this, [this]() {
		if (physical(vj_play_)) {
			mfd_engine_desk_vjster(eng_, 0, false, true);
		}
	});
	vj_row->addWidget(vj_play_);
	vj_col->addLayout(vj_row);
	auto *preset_row = new QHBoxLayout();
	preset_row->setSpacing(3);
	static const double k_presets[6] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
	for (int k = 0; k < 6; ++k) {
		double rate = k_presets[k];
		QString label = QString::number(rate) + QStringLiteral("x");
		QPushButton *b = makeButton(
			rate == 1.0 ? mfdText("Desk.VjsterReset", "Back to 1x. On the APC40: Tap Tempo.")
				    : mfdFill(mfdText("Desk.VjsterPresetTip",
						      "Ask VJster for %1 (Fadester Companion 0.1.17 or later)"),
					      {label}),
			40, kButtonHeight);
		b->setText(label);
		b->setObjectName(QStringLiteral("mfdDeskSpeedPreset_%1").arg(k));
		vj_presets_[k] = b;
		connect(b, &QPushButton::pressed, this, [this, rate, label, b]() {
			if (!physical(b)) {
				return;
			}
			if (rate == 1.0) {
				mfd_engine_desk_vjster(eng_, 0, true, false); /* 1x: the reset every Companion knows */
			} else {
				mfd_engine_desk_vjster_rate(eng_, rate);
			}
			sayStatus(mfdFill(mfdText("Desk.VjsterAsked", "Last request from this dock: %1 (unconfirmed)"),
					  {label}));
		});
		preset_row->addWidget(b);
	}
	vj_col->addLayout(preset_row);
	root->addWidget(vj_box_);

	root->addStretch(1);

	clock_.start();
	built_ = true; /* every label exists from here on: event() may size them */
	applyFontSizes();
	refresh();
}

/* What the pads do: the dock's own guide, opened by its "?" button. One window, raised when it is already open. */
void MfdDeskDock::showHelp()
{
	if (help_) {
		help_->show();
		help_->raise();
		help_->activateWindow();
		return;
	}
	auto *dlg = new QDialog(window());
	dlg->setObjectName(QStringLiteral("mfdDeskHelpDialog"));
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(mfdWindowTitle(mfdText("Desk.Help.Title", "What the pads do")));
	dlg->setStyleSheet(QStringLiteral("QDialog { background-color: #0b1120; } QLabel { color: #e2e8f0; }"));
	auto *col = new QVBoxLayout(dlg);
	col->setContentsMargins(14, 12, 14, 12);
	col->setSpacing(10);
	const std::pair<const char *, QString> parts[] = {
		{"mfdDeskHelpPads",
		 mfdText("Desk.Help.Pads",
			 "Columns 1\xE2\x80\x93"
			 "8 are the sources of the scene on air, bottom layer first: empty faders fill "
			 "themselves and follow the scene; a fader you set yourself stays. Blend (the bottom pad row on the APC40): "
			 "the source's other blend (Shift: next blend). Flash: 100 % while held (Shift: dark while held). Fade "
			 "out: out and back (Shift: cut out). Show / hide: the source's eye in OBS (Shift: show at 100 %). Fade, "
			 "beats (the top row): the yellow pad cuts Preview on air; the green pads fade it over 32, 16, 8, 4, 2, "
			 "1 or \xC2\xBD beats. Reset (Clip Stop on the APC40): that column back to its own blend (Shift: pin the "
			 "column to its source). Reset all (Stop All Clips): every effect off and every source back to its own "
			 "blend (Shift: fade every source out).")},
		{"mfdDeskHelpScenes",
		 mfdText("Desk.Help.Scenes",
			 "The five scene buttons (Scene Launch 1\xE2\x80\x93"
			 "5 on the APC40) are the first five scenes in the "
			 "Scenes list. Press once for Preview, again to fade it on air; Shift + button (Ctrl-click on the dock, "
			 "Cmd-click on a Mac) sends it straight on air. On air red, Preview green. Blackout (Session): the audience "
			 "sees black, and the picture comes back on the next press. Cue Level knob: transition length (Shift: source "
			 "fade time). Panic (Record): all Visual Sidechain effects off, then Restore. Tap (Metronome): its tap "
			 "tempo (Shift: Hold BPM). Shift + Tap Tempo: Beat 1.")},
		{"mfdDeskHelpSpeed",
		 mfdText("Desk.Help.Speed",
			 "Video speed (with Fadester Companion and VJster running): the Tempo knob makes the video 0.05x faster "
			 "or slower per click (Shift + Tempo: 0.25x), Nudge \xE2\x88\x92 / + 0.25x slower or faster, Tap Tempo "
			 "back to 1x, Play plays or pauses it. It goes through VJster because an OBS media source restarts when "
			 "its speed changes.")},
	};
	for (const auto &part : parts) {
		auto *text = new QLabel(part.second, dlg);
		text->setObjectName(QString::fromUtf8(part.first));
		text->setWordWrap(true);
		text->setTextInteractionFlags(Qt::TextSelectableByMouse);
		col->addWidget(text);
	}
	auto *buttons = new QHBoxLayout();
	buttons->addStretch(1);
	auto *close = new QPushButton(mfdText("Dialog.Close", "Close"), dlg);
	close->setObjectName(QStringLiteral("mfdDeskHelpClose"));
	connect(close, &QPushButton::clicked, dlg, &QDialog::close);
	buttons->addWidget(close);
	col->addLayout(buttons);
	dlg->resize(560, dlg->sizeHint().height());
	help_ = dlg;
	dlg->show();
}

/* The dock's text is OBS's own, its font-size setting included: nothing here fixes a size. Only what must stand out is
 * set, worked out from that size: the title a little larger, ON AIR and PREVIEW the largest text on the dock. */
void MfdDeskDock::applyFontSizes()
{
	const int base = fontInfo().pixelSize();
	if (base <= 0 || base == base_px_) {
		return;
	}
	base_px_ = base;
	const QString strong = QStringLiteral("font-size: %1px; font-weight: bold;").arg(base + 1);
	title_->setStyleSheet(strong);
	bpm_->setStyleSheet(strong); /* smaller than what is on air */
	const QString chip = QStringLiteral("font-size: %1px; font-weight: bold; color: #ffffff; background-color: %2; "
					    "border-radius: 3px; padding: 3px 6px;");
	const QString big = QString::number((int)std::lround((double)base * 1.4));
	on_air_->setStyleSheet(chip.arg(big, QStringLiteral("#b91c1c")));
	preview_->setStyleSheet(chip.arg(big, QStringLiteral("#15803d")));
	status_->setMinimumHeight(status_->fontMetrics().lineSpacing());
}

void MfdDeskDock::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	applyFontSizes();
	mfd_engine_set_desk_visible(eng_, true); /* the engine's frame tick asks for repaints now */
	refresh();
}

void MfdDeskDock::hideEvent(QHideEvent *e)
{
	releaseHolds();
	mfd_engine_set_desk_visible(eng_, false);
	QWidget::hideEvent(e);
}

bool MfdDeskDock::event(QEvent *e)
{
	/* Alt-Tab or a click elsewhere while a pad is held down: let go of the dock's holds. */
	if (e->type() == QEvent::WindowDeactivate) {
		releaseHolds();
	} else if ((e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) && built_) {
		applyFontSizes(); /* OBS's theme or font size changed */
	}
	return QWidget::event(e);
}

void MfdDeskDock::vjSteps(int steps)
{
	mfd_engine_desk_vjster(eng_, steps, false, false);
	sayStatus(mfdFill(mfdText("Desk.VjsterAskedStep", "Last request from this dock: %1 (unconfirmed)"),
			  {QStringLiteral("%1%2x")
				   .arg(steps > 0 ? QStringLiteral("+") : QString::fromUtf8("\xE2\x88\x92"))
				   .arg(std::abs(steps) * 0.05, 0, 'f', 2)}));
}

/* QPushButton emits pressed again when a held pointer slides off it and back on: that must not press a desk button a
 * second time (Lock would toggle twice, a scene would be taken). The filter marks a held pointer that left the button;
 * a new press or the release clears it. Keyboard, click() and accessibility presses are untouched. */
bool MfdDeskDock::eventFilter(QObject *obj, QEvent *e)
{
	switch (e->type()) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonDblClick:
	case QEvent::MouseButtonRelease:
	case QEvent::FocusOut: /* Qt ends a held press these ways too, without a release reaching the button */
	case QEvent::Hide:
	case QEvent::EnabledChange:
	case QEvent::WindowDeactivate:
		obj->setProperty("mfdSlid", false);
		break;
	case QEvent::MouseMove: {
		auto *me = static_cast<QMouseEvent *>(e);
		auto *w = qobject_cast<QWidget *>(obj);
		if (w && (me->buttons() & Qt::LeftButton) && !w->rect().contains(me->position().toPoint())) {
			obj->setProperty("mfdSlid", true);
		}
		break;
	}
	default:
		break;
	}
	return QWidget::eventFilter(obj, e);
}

void MfdDeskDock::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape) {
		releaseHolds(); /* and the dock's Shift */
		refresh();
		e->accept();
		return;
	}
	QWidget::keyPressEvent(e);
}

void MfdDeskDock::sayStatus(const QString &text, bool over_reason)
{
	status_->setText(text);
	status_is_reason_ = false;
	status_over_reason_ = over_reason;
	status_age_.start();
}

void MfdDeskDock::pressNote(uint8_t channel, uint8_t note, bool on, bool shift)
{
	enum mfd_desk_press_result r = mfd_engine_desk_press(eng_, channel, note, on, shift, nullptr);
	if (on && r == MFD_DESK_REJECTED) {
		sayStatus(mfdText("Desk.Ignored", "Not available right now."), false);
	}
	refresh();
}

void MfdDeskDock::pressPad(int row, int strip, bool on)
{
	uint8_t note = (uint8_t)(kRowNote[row] + (unsigned)strip);
	bool shift = shift_->isChecked();
	bool hold_row = kRowCell[row] == MFD_DESK_FLASH;
	if (hold_row && !on) {
		if (holding_[strip]) {
			holding_[strip] = false;
			mfd_engine_desk_press(eng_, 0U, note, false, shift, &hold_epoch_[strip]);
		}
		if (shift) {
			shift_->setChecked(false); /* a blackout kept Shift until it was let go */
		}
		refresh();
		return;
	}
	if (!on) {
		mfd_engine_desk_press(eng_, 0U, note, false, false, nullptr);
		refresh();
		return;
	}
	uint32_t epoch = 0;
	enum mfd_desk_press_result r = mfd_engine_desk_press(eng_, 0U, note, true, shift, &epoch);
	if (r == MFD_DESK_HOLD_STARTED) {
		holding_[strip] = true;
		hold_epoch_[strip] = epoch;
	} else if (r == MFD_DESK_REJECTED) {
		sayStatus(mfdText("Desk.Ignored", "Not available right now."), false);
	}
	if (shift && !hold_row) {
		shift_->setChecked(false); /* Shift is for the next pad only; a blackout keeps it until let go */
	}
	refresh();
}

void MfdDeskDock::releaseHolds()
{
	for (int i = 0; i < kStrips; ++i) {
		if (holding_[i]) {
			holding_[i] = false;
			mfd_engine_desk_press(eng_, 0U, (uint8_t)(MFD_VJ_NOTE_ROW_FLASH + (unsigned)i), false, false,
					      &hold_epoch_[i]);
		}
	}
	if (shift_) {
		shift_->setChecked(false);
	}
}

void MfdDeskDock::paintButton(QPushButton *b, uint32_t rgb, bool enabled)
{
	QString css =
		QStringLiteral("QPushButton { background-color: %1; color: %2; font-weight: bold; }"
			       "QPushButton:disabled { background-color: #1f2937; color: #6b7280; }")
			.arg(rgbCss(rgb), lightBackground(rgb) ? QStringLiteral("#0b1120") : QStringLiteral("#f8fafc"));
	if (b->property("mfdCss").toString() != css) {
		b->setStyleSheet(css);
		b->setProperty("mfdCss", css);
	}
	if (b->isEnabled() != enabled) {
		b->setEnabled(enabled);
	}
}

void MfdDeskDock::refresh()
{
	mfd_engine_get_desk_view(eng_, shift_ && shift_->isChecked(), &view_);
	if (view_.led_count < kLeds) {
		return; /* not the APC40 layout this dock draws */
	}

	/* Visual Sidechain's signals: its in-process proc, outside the engine lock */
	vs_loaded_ = false;
	bpm_valid_ = false;
	double beat_phase = 0.0;
	long long beat_index = 0;
	bool locked = false;
	if (proc_handler_t *ph = obs_initialized() ? obs_get_proc_handler() : nullptr) {
		calldata_t cd;
		calldata_init(&cd);
		if (proc_handler_call(ph, "visual_sidechain_get_signals", &cd) && calldata_bool(&cd, "ok")) {
			vs_loaded_ = true;
			double bpm = calldata_float(&cd, "bpm");
			if (std::isfinite(bpm) && bpm >= 20.0 && bpm <= 400.0) {
				bpm_value_ = bpm;
				bpm_valid_ = true;
			}
			beat_phase = calldata_float(&cd, "beat_phase");
			beat_index = calldata_int(&cd, "beat_index");
			locked = calldata_bool(&cd, "bpm_locked");
		}
		calldata_free(&cd);
	}
	/* the animation clock: Visual Sidechain's beat when it runs, else 120 BPM from the dock's own clock */
	double beat_pos = vs_loaded_ && beat_index > 0 ? (double)beat_index + beat_phase
						       : (double)clock_.elapsed() / 1000.0 * (bpm_value_ / 60.0);

	setLabel(title_, mfdFill(mfdText("Desk.Title", "%2 \xC2\xB7 Midi Fadester %1"),
				 {QString::fromUtf8(PLUGIN_VERSION),
				  view_.desk_connected ? mfdText("Desk.DeskOn", "APC40 mkII connected")
						       : mfdText("Desk.DeskOff", "no APC40 mkII")}));

	bool pads_on = view_.finished_loading && view_.pads == MFD_DESK_PADS_ON && !view_.learn_or_setup;
	/* The pads cannot work until the user changes something (no APC40, pads switched off, another layout): the grid
	 * goes, and what still works stays. Never while loading or while Learn or Set up runs: those pass by themselves,
	 * and the dock must not flash or change size for them. */
	bool pads_gone = view_.finished_loading &&
			 (view_.pads == MFD_DESK_PADS_NO_APC40 || view_.pads == MFD_DESK_PADS_BUTTONS_OFF ||
			  view_.pads == MFD_DESK_PADS_OTHER_PROFILE);
	if (grid_box_->isHidden() != pads_gone) {
		grid_box_->setVisible(!pads_gone);
		stop_all_->setVisible(!pads_gone);
		shift_->setVisible(!pads_gone);
	}
	/* why a scene fade cannot start now: the tooltip of the grey pads and of the grey Fade button */
	QString no_fade = !view_.studio_mode  ? mfdText("Msg.StudioModeOff", "Studio Mode is off.")
			  : !view_.take_ready ? mfdText("Desk.Tip.SameScene",
							"Preview and on air are the same scene. Pick another scene.")
					      : QString();
	bool shift = shift_->isChecked() || view_.desk_shift; /* what a click on a pad does now */
	for (int r = 0; r < kRows; ++r) {
		setLabel(row_names_[r], rowShortName(r, shift));
	}
	setButtonText(stop_all_, shift ? mfdText("Desk.StopAllFadeShort", "Fade all out")
				       : mfdText("Desk.StopAllShort", "Reset all"));
	QString program = QString::fromUtf8(view_.program);
	QString preview = QString::fromUtf8(view_.preview);
	const QString none = QString::fromUtf8("\xE2\x80\x94");
	/* each line elides on its own; the tooltip holds the whole name */
	auto chip = [](QLabel *l, const QString &full) {
		setTip(l, full);
		setLabel(l, l->fontMetrics().elidedText(full, Qt::ElideRight, std::max(60, l->width() - 14)));
	};
	chip(on_air_, mfdFill(mfdText("Desk.OnAirChip", "ON AIR  %1"), {program.isEmpty() ? none : program}));
	bool show_preview = view_.studio_mode && !preview.isEmpty();
	if (preview_->isHidden() == show_preview) {
		preview_->setVisible(show_preview);
	}
	if (show_preview) {
		chip(preview_, mfdFill(mfdText("Desk.PreviewChip", "PREVIEW  %1"), {preview}));
	}
	timing_->setVisible(obs_initialized());
	if (obs_initialized()) {
		/* one unit for both: seconds */
		setLabel(timing_, mfdFill(mfdText("Desk.Timing", "Transition %1 s \xC2\xB7 Source fade %2 s"),
					  {QString::number(obs_frontend_get_transition_duration() / 1000.0, 'f', 1),
					   QString::number(view_.fade_ms / 1000.0, 'f', 1)}));
	}

	/* scenes */
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		const struct mfd_vj_led &led = view_.leds[48 + j];
		bool exists = j < view_.scene_count;
		QString name = exists ? QString::fromUtf8(view_.scenes[j]) : QString();
		/* A button reads '&' as a mnemonic: "Q&A" would show as "QA" and, on Windows, Alt+A would press the
		 * button. Doubled, it is shown as written (F-02). */
		/* a mark that is not colour: a filled dot on the scene on air, a ring on the one in Preview */
		QString mark;
		if (exists && name == program) {
			mark = QString::fromUtf8("\xE2\x97\x8F ");
		} else if (exists && view_.studio_mode && name == preview) {
			mark = QString::fromUtf8("\xE2\x97\x8B ");
		}
		QString shown = scenes_[j]->fontMetrics().elidedText(mark + name, Qt::ElideRight,
								     std::max(40, scenes_[j]->width() - 8));
		shown.replace(QLatin1Char('&'), QStringLiteral("&&"));
		setButtonText(scenes_[j], shown);
		QString what = !view_.studio_mode ? mfdText("Desk.SceneTip.Program", "Click: put this scene on air.")
						  : sceneTipStudio();
		bool can = pads_on && exists && !view_.tbar_busy && !view_.scene_owned[j];
		setTip(scenes_[j],
		       !exists           ? mfdText("Desk.Tip.NoScene", "No scene here.")
		       : view_.tbar_busy ? mfdText("Desk.Tip.Throw", "Waits while the crossfader is thrown.")
		       : view_.scene_owned[j]
			       ? mfdText("Desk.Tip.Owned",
					 "This button is assigned to a strip. Clear it in Tools, Midi Fadester: Set up "
					 "or Advanced.")
			       : name + QStringLiteral("\n") + what);
		paintButton(scenes_[j], ledColor(led, beat_pos, 0x22C55EU), can);
	}

	/* the grid */
	for (int i = 0; i < kStrips; ++i) {
		const struct mfd_desk_strip &st = view_.strips[i];
		QString src = QString::fromUtf8(st.source);
		QString label = QString::fromUtf8(st.label); /* a fader on audio or a filter: named, greyed below */
		QString header = !src.isEmpty() ? src : !label.isEmpty() ? label : QString::fromUtf8("\xE2\x80\x94");
		setLabel(col_names_[i], col_names_[i]->fontMetrics().elidedText(header, Qt::ElideRight,
										std::max(28, col_names_[i]->width())));
		setCss(col_names_[i], src.isEmpty() ? QStringLiteral("color: #6b7280;") : QString()); /* not a layer */
		setTip(col_names_[i],
		       !src.isEmpty() ? src + (st.follows ? mfdText("Desk.Follows", " (follows the scene on air)")
							  : mfdText("Desk.Pinned", " (pinned)"))
		       : !label.isEmpty()
			       ? mfdFill(mfdText("Desk.Tip.NotLayer", "Not a layer: its fader drives %1."), {label})
			       : mfdText("Desk.Tip.Empty", "No source on this column."));
		for (int r = 0; r < kRows; ++r) {
			int cell = kRowCell[r];
			const struct mfd_vj_led &led = view_.leds[i * MFD_DESK_CELLS + cell];
			bool can = pads_on && !st.owned[cell];
			QString why;
			if (st.owned[cell]) {
				why = mfdText(
					"Desk.Tip.Owned",
					"This button is assigned to a strip. Clear it in Tools, Midi Fadester: Set up "
					"or Advanced.");
			} else if (cell == MFD_DESK_RESTART) {
				/* the top row: scene fades, whatever the column holds */
				can = can && view_.take_ready && !view_.tbar_busy;
				why = i == 0 ? mfdText("Desk.Tip.SceneCut", "Cut Preview on air at once")
				      : i == 1
					      ? mfdText("Desk.Tip.SceneFadeCapped",
							"Fade Preview on air over 32 beats. Below 96 BPM, OBS caps this "
							"fade at 20 s.")
					      : mfdFill(mfdText("Desk.Tip.SceneFade",
								"Fade Preview on air over %1 beats"),
							{QString::number(32.0 / (double)(1 << (i - 1)), 'g', 3)});
				/* the engine times these fades by Visual Sidechain's tempo (120 BPM without one) */
				setButtonText(pads_[r][i], sceneFadePadText(i, bpm_valid_ && bpm_value_ < 96.0));
				if (!no_fade.isEmpty()) {
					why = no_fade; /* a grey pad says why it is grey, not what it would do */
				}
			} else if (!st.has_layer) {
				can = false;
				why = mfdText("Desk.Tip.Empty", "No source on this column.");
			} else if ((cell == MFD_DESK_FLASH || cell == MFD_DESK_FADE) && !st.single_layer) {
				can = false;
				why = st.source_missing
					      ? mfdText("Desk.Tip.SourceMissing",
							"Its source is not in the scene right now.")
					      : mfdText("Desk.Tip.SingleLayer",
							"Give this column's fader the job Show or hide a layer to use "
							"this pad.");
			}
			if (!why.isEmpty()) {
				setTip(pads_[r][i], why);
			} else {
				setTip(pads_[r][i], pads_[r][i]->accessibleName());
			}
			paintButton(pads_[r][i], ledColor(led, beat_pos, 0x22C55EU), can);
		}
		const struct mfd_vj_led &stop = view_.leds[i * MFD_DESK_CELLS + MFD_DESK_CLIP_STOP];
		paintButton(clip_stops_[i], ledColor(stop, beat_pos, 0x22C55EU),
			    pads_on && !st.owned[MFD_DESK_CLIP_STOP] && (shift ? st.can_pin_toggle : st.has_layer));
		faders_[i]->setState(st.level, st.pickup, st.pickup_target);
		QString fader_tip = src.isEmpty()
					    ? mfdFill(mfdText("Desk.FaderEmpty", "Fader %1"), {QString::number(i + 1)})
					    : mfdFill(mfdText("Desk.FaderTip", "Fader %1: %2, %3 %"),
						      {QString::number(i + 1), src,
						       QString::number((int)std::lround(st.level * 100.0f))});
		if (st.pickup) {
			/* the sentence of the settings dialog's strip card */
			const QString pct = QString::number(mfd_calibrated_percent(st.pickup_target));
			fader_tip +=
				QStringLiteral("\n") +
				(st.pickup_up
					 ? mfdFill(mfdText("Pickup.FaderUp", "Move the fader up to %1% to start."),
						   {pct})
					 : mfdFill(mfdText("Pickup.FaderDown", "Move the fader down to %1% to start."),
						   {pct}));
		}
		setTip(faders_[i], fader_tip);
	}
	xfader_->setValue((int)std::lround(std::clamp(view_.crossfader, 0.0f, 1.0f) * 1000.0f));
	paintButton(stop_all_, 0x475569U, pads_on && !view_.stop_all_owned);
	paintButton(shift_, shift ? 0xF8FAFCU : 0x475569U, pads_on);
	/* Fade keeps its place when it cannot act: buttons that move are pressed by mistake (U1-07) */
	paintButton(session_, view_.take_ready ? 0x22C55EU : 0x475569U, view_.take_ready && !view_.tbar_busy);
	setTip(session_, no_fade.isEmpty() ? session_->accessibleName() : no_fade);
	bool studio_off = view_.finished_loading && !view_.studio_mode;
	if (studio_note_->isHidden() == studio_off) {
		studio_note_->setVisible(studio_off);
		studio_btn_->setVisible(studio_off);
	}
	paintButton(blackout_btn_, view_.blackout ? 0xEF4444U : 0x475569U, view_.finished_loading);
	/* the button says what a press does now, as Panic turns into Restore */
	setButtonText(blackout_btn_, view_.blackout ? mfdText("Desk.BlackoutEndShort", "End blackout")
						    : mfdText("Desk.RowBlackoutShort", "Blackout"));
	if (view_.blackout) {
		/* as solid as the Panic bar: the audience sees nothing, which is the larger of the two emergencies */
		setLabel(blackout_, mfdText("Desk.BlackoutOn", "BLACKOUT: the audience sees black. Press End blackout "
							       "(Session on the APC40)."));
		setCss(blackout_, QStringLiteral("color: #f8fafc; background-color: #b91c1c; font-weight: bold; "
						 "padding: 4px;"));
	} else {
		setLabel(blackout_, QString());
		setCss(blackout_, QString());
	}
	blackout_->setVisible(!blackout_->text().isEmpty());

	/* Visual Sidechain */
	if (!vs_loaded_) {
		setLabel(bpm_, QString::fromUtf8("\xE2\x80\x94 BPM"));
		setLabel(lock_, mfdText("Desk.VsMissing", "Visual Sidechain is not loaded."));
	} else {
		/* no estimate yet (no audio): no number, rather than the animation's 120 */
		setLabel(bpm_, bpm_valid_ ? QStringLiteral("%1 BPM").arg(bpm_value_, 0, 'f', 1)
					  : QString::fromUtf8("\xE2\x80\x94 BPM"));
		setLabel(lock_, locked ? mfdText("Desk.Locked", "held") : mfdText("Desk.Unlocked", "auto"));
	}
	int step = vs_loaded_ ? (int)(((beat_index % 4) + 4) % 4) : -1;
	bool fresh = beat_index != last_beat_index_ && beat_phase < 0.25;
	last_beat_index_ = beat_index;
	for (int k = 0; k < 4; ++k) {
		bool on = k == step;
		bool flash = fresh || beat_phase < 0.25;
		QString lit =
			k == 0 ? (flash ? QStringLiteral("#f59e0b") : QStringLiteral("#92400e")) /* beat 1: amber */
			       : (flash ? QStringLiteral("#f8fafc") : QStringLiteral("#64748b"));
		setCss(beats_[k],
		       QStringLiteral("background-color: %1; border-radius: 7px;")
			       .arg(on ? lit : (k == 0 ? QStringLiteral("#3b2a12") : QStringLiteral("#1f2937"))));
	}
	if (const char *old_vs = vs_module_conflict_path()) {
		/* the old standalone plugin is loaded: Visual Sidechain runs from that copy until it is removed */
		setLabel(panic_, mfdFill(mfdText("Desk.VsConflict",
						 "Remove the old Visual Sidechain plugin (%1) and restart OBS: "
						 "Visual Sidechain is part of Midi Fadester now."),
					 {QString::fromUtf8(old_vs)}));
		setCss(panic_, QStringLiteral("color: #0b1120; background-color: #fbbf24; padding: 4px;"));
	} else if (view_.vs_panic_known && view_.vs_panic) {
		setLabel(panic_,
			 mfdText("Desk.PanicOn", "PANIC: all Visual Sidechain effects are off. Press Restore."));
		setCss(panic_, QStringLiteral("color: #f8fafc; background-color: #b91c1c; font-weight: bold; "
					      "padding: 4px;"));
	} else if (view_.vs_last_failed) {
		setLabel(panic_,
			 mfdText("Desk.VsNoAnswer", "Visual Sidechain did not answer. Turn on the server in Tools, "
						    "WebSocket Server Settings."));
		setCss(panic_, QStringLiteral("color: #fde68a; padding: 2px;"));
	} else {
		setLabel(panic_, QString());
		setCss(panic_, QString());
	}
	panic_->setVisible(!panic_->text().isEmpty()); /* no gap when there is nothing to warn about */
	setButtonText(vs_panic_, view_.vs_panic_known && view_.vs_panic ? mfdText("Desk.VsRestoreShort", "Restore")
									: mfdText("Desk.VsPanicShort", "Panic"));
	paintButton(vs_panic_, view_.vs_panic_known && view_.vs_panic ? 0xEF4444U : 0x475569U, vs_loaded_);
	paintButton(vs_tap_, 0x475569U, vs_loaded_);
	paintButton(help_btn_, 0x334155U, true);
	paintButton(gear_btn_, 0x334155U, g_open_settings != nullptr);
	paintButton(vs_lock_, locked ? 0x0EA5E9U : 0x475569U, vs_loaded_);
	paintButton(vs_downbeat_, 0x475569U, vs_loaded_);
	if (vj_box_->isHidden() == view_.video_controls) {
		vj_box_->setVisible(view_.video_controls);
	}
	paintButton(vj_play_, 0x334155U, true); /* always: VJster does not need the desk */
	for (int k = 0; k < 6; ++k) {
		paintButton(vj_presets_[k], k == 2 ? 0x64748BU : 0x334155U, true); /* 1x stands out */
	}

	/* the status line: why the pads wait, else the last message for a few seconds */
	QString why;
	if (!view_.finished_loading) {
		why = mfdText("Desk.Loading", "Loading\xE2\x80\xA6");
	} else if (view_.pads == MFD_DESK_PADS_NO_APC40) {
		why = mfdText("Desk.WhyNoDesk", "Connect the APC40 mkII to use the pads.");
	} else if (view_.pads == MFD_DESK_PADS_BUTTONS_OFF) {
		why = mfdText("Desk.WhyOff", "The APC40 pads are turned off.");
	} else if (view_.pads == MFD_DESK_PADS_OTHER_PROFILE) {
		why = mfdText("Desk.WhyProfile",
			      "The pads need the APC40 mkII layout. Open Tools, Midi Fadester, Change controller.");
	} else if (view_.learn_or_setup) {
		why = mfdText("Desk.WhyLearn", "The pads wait while MIDI Learn or Set up is running.");
	} else if (view_.tbar_busy) {
		why = mfdText("Desk.TbarBusy", "Crossfader throw under way: the scene buttons wait.");
	} else if (view_.studio_mode && !view_.take_ready) {
		why = no_fade; /* Studio Mode off has its own row, with its button */
	}
	bool lost = view_.finished_loading && view_.pads == MFD_DESK_PADS_NO_APC40;
	bool off = view_.finished_loading && view_.pads == MFD_DESK_PADS_BUTTONS_OFF;
	reconnect_->setVisible(lost);
	turn_on_->setVisible(off);
	paintButton(reconnect_, 0xFBBF24U, true);
	paintButton(turn_on_, 0xFBBF24U, true);
	setCss(status_,
	       pads_gone ? QStringLiteral("color: #fde68a; font-weight: bold;") : QStringLiteral("color: #94a3b8;"));
	bool fresh_message = !status_is_reason_ && status_over_reason_ && status_age_.isValid() &&
			     status_age_.elapsed() <=
				     4000;      /* "cannot act now" gives way to the reason that explains it */
	if (!why.isEmpty() && !fresh_message) { /* a message just said (a speed asked for) stays its 4 s */
		setLabel(status_, why);
		status_is_reason_ = true;
	} else if (why.isEmpty() && (status_is_reason_ || (status_age_.isValid() && status_age_.elapsed() > 4000))) {
		setLabel(status_, QString()); /* the reason is gone (loaded, desk connected), or the message is old */
		status_is_reason_ = false;
		status_age_.invalidate();
	}
}

/* --- registration --- */

QPointer<MfdDeskDock> g_dock;
bool g_registered = false;
bool g_callback = false;

void remove_dock()
{
	if (g_registered) {
		g_registered = false;
		obs_frontend_remove_dock(kDockId); /* OBS deletes the widget it adopted */
	}
	g_dock.clear();
}

void on_frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (g_dock) {
			g_dock->hide(); /* lets go of held pads; the engine stops asking for repaints */
		}
		remove_dock();
		if (g_callback) { /* OBS takes its callbacks away right after EXIT, before modules unload */
			g_callback = false;
			obs_frontend_remove_event_callback(on_frontend_event, nullptr);
		}
	}
}

} // namespace

void mfd_desk_dock_init(void)
{
	if (g_registered || !QApplication::instance()) {
		return;
	}
	auto *dock = new MfdDeskDock(mfd_engine_get());
	if (obs_frontend_add_dock_by_id(kDockId, obs_module_text("Desk.DockTitle"), dock)) {
		g_dock = dock;
		g_registered = true;
		obs_frontend_add_event_callback(on_frontend_event, nullptr);
		g_callback = true;
	} else {
		delete dock; /* OBS did not adopt it (no frontend, or the id is taken) */
	}
}

void mfd_desk_dock_refresh(void)
{
	if (g_dock && g_dock->isVisible()) {
		g_dock->refresh();
	}
}

bool mfd_desk_dock_show(void)
{
	if (!g_dock) {
		return false;
	}
	for (QWidget *w = g_dock->parentWidget(); w; w = w->parentWidget()) {
		if (auto *dw = qobject_cast<QDockWidget *>(w)) { /* OBS wraps the widget in its own dock */
			dw->show();
			dw->raise();
			return true;
		}
	}
	return false;
}

void mfd_desk_dock_set_settings_opener(void (*open_settings)(void))
{
	g_open_settings = open_settings;
}

void mfd_desk_dock_shutdown(void)
{
	remove_dock();
	if (g_callback) {
		g_callback = false;
		obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	}
}

QWidget *mfd_desk_dock_create(struct mfd_engine *eng, QWidget *parent)
{
	return new MfdDeskDock(eng, parent);
}
