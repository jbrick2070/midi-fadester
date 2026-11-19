/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-desk-dock.hpp"

#include "core/apc40-palette.h"
#include "core/vj-pads.h"
#include "obs/mfd-engine.h"
#include "ui/mfd-text-qt.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr const char *kDockId = "midi-fadester-desk";
constexpr int kStrips = (int)MFD_VJ_STRIPS;
constexpr int kRows = 5; /* top to bottom: Restart, Show, Fade, Flash, Blend */
constexpr uint8_t kChPulse = 10U;
constexpr uint8_t kChBlink = 15U;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kLeds = 55U; /* 6 per strip, 5 scenes, Record, Session */

/* Row r (0 = top) is this cell of the strip (mfd_vj_pads_leds' order per strip) and this note base. */
const int kRowCell[kRows] = {MFD_DESK_RESTART, MFD_DESK_SHOW, MFD_DESK_FADE, MFD_DESK_FLASH, MFD_DESK_BLEND};
const uint8_t kRowNote[kRows] = {MFD_VJ_NOTE_ROW_RESTART, MFD_VJ_NOTE_ROW_SHOW, MFD_VJ_NOTE_ROW_FADE,
				 MFD_VJ_NOTE_ROW_FLASH, MFD_VJ_NOTE_ROW_BLEND};

/* Row r's name (0 = top). */
QString rowName(int r)
{
	switch (r) {
	case 0:
		return mfdText("Desk.RowRestart", "Restart (Shift: pin)");
	case 1:
		return mfdText("Desk.RowShow", "Show / hide (Shift: wake)");
	case 2:
		return mfdText("Desk.RowFade", "Fade (Shift: snap)");
	case 3:
		return mfdText("Desk.RowFlash", "Flash (Shift: blackout)");
	default:
		return mfdText("Desk.RowBlend", "Blend punch");
	}
}

/* Row r's short name for the grid (its full name is the tooltip). */
QString rowShortName(int r)
{
	switch (r) {
	case 0:
		return mfdText("Desk.RowRestartShort", "Restart");
	case 1:
		return mfdText("Desk.RowShowShort", "Show / hide");
	case 2:
		return mfdText("Desk.RowFadeShort", "Fade");
	case 3:
		return mfdText("Desk.RowFlashShort", "Flash");
	default:
		return mfdText("Desk.RowBlendShort", "Blend punch");
	}
}

QString rgbCss(uint32_t rgb)
{
	return QStringLiteral("#%1").arg(rgb & 0xFFFFFFU, 6, 16, QLatin1Char('0'));
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

public:
	void refresh();

private:
	QPushButton *makeButton(const QString &accessible, int min_w, int min_h);
	void pressPad(int row, int strip, bool on);
	void pressNote(uint8_t channel, uint8_t note, bool on, bool shift);
	void releaseHolds();
	void sayStatus(const QString &text);
	void paintButton(QPushButton *b, uint32_t rgb, bool enabled, bool bright_text = false);

	struct mfd_engine *eng_ = nullptr;
	QLabel *title_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *scenes_[MFD_VJ_SCENES] = {};
	QLabel *col_names_[kStrips] = {};
	QPushButton *pads_[kRows][kStrips] = {};
	QPushButton *clip_stops_[kStrips] = {};
	QPushButton *stop_all_ = nullptr;
	QPushButton *shift_ = nullptr;
	QPushButton *session_ = nullptr;
	QProgressBar *faders_[kStrips] = {};
	QProgressBar *xfader_ = nullptr;
	QLabel *bpm_ = nullptr;
	QLabel *lock_ = nullptr;
	QLabel *beats_[4] = {};
	QLabel *panic_ = nullptr;
	QPushButton *vs_panic_ = nullptr;
	QPushButton *vs_tap_ = nullptr;
	QPushButton *vs_lock_ = nullptr;
	QPushButton *vs_downbeat_ = nullptr;
	QElapsedTimer clock_;
	QElapsedTimer status_age_;
	struct mfd_desk_view view_ = {};
	bool holding_[kStrips] = {};
	uint32_t hold_epoch_[kStrips] = {};
	double bpm_value_ = 120.0;
	bool bpm_valid_ = false;
	bool status_is_reason_ = false;
	bool vs_loaded_ = false;
	long long last_beat_index_ = -1;
};

QPushButton *MfdDeskDock::makeButton(const QString &accessible, int min_w, int min_h)
{
	auto *b = new QPushButton(this);
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
	setMinimumSize(420, 380);
	setStyleSheet(QStringLiteral("QWidget#mfdDeskDock { background-color: #0b1120; }"
				     "QLabel { color: #e2e8f0; font-size: 11px; }"
				     "QPushButton { border: 1px solid #334155; border-radius: 3px; color: #0b1120;"
				     " font-size: 10px; padding: 1px; }"
				     "QPushButton:focus { border: 2px solid #f8fafc; }"
				     "QProgressBar { border: 1px solid #334155; background: #111827; }"
				     "QProgressBar::chunk { background: #94a3b8; }"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);

	title_ = new QLabel(this);
	title_->setObjectName(QStringLiteral("mfdDeskTitle"));
	title_->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: bold;"));
	root->addWidget(title_);

	/* Scene Launch 1..5 */
	auto *scene_row = new QHBoxLayout();
	scene_row->setSpacing(4);
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		QPushButton *b = makeButton(
			mfdFill(mfdText("Desk.SceneButton", "Scene Launch %1"), {QString::number(j + 1)}), 60, 28);
		scenes_[j] = b;
		connect(b, &QPushButton::pressed, this, [this, j]() {
			/* Ctrl-click (Cmd on a Mac): straight to Program, as Shift + Scene Launch on the desk */
			bool direct = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
			pressNote(0U, (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)j), true, direct);
		});
		connect(b, &QPushButton::released, this,
			[this, j]() { pressNote(0U, (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)j), false, false); });
		scene_row->addWidget(b);
	}
	root->addLayout(scene_row);

	/* the pad grid with the strips' source names over it and the row names beside it */
	auto *grid = new QGridLayout();
	grid->setHorizontalSpacing(3);
	grid->setVerticalSpacing(3);
	for (int i = 0; i < kStrips; ++i) {
		col_names_[i] = new QLabel(this);
		col_names_[i]->setAlignment(Qt::AlignCenter);
		col_names_[i]->setMinimumWidth(30);
		grid->addWidget(col_names_[i], 0, i + 1);
	}
	for (int r = 0; r < kRows; ++r) {
		auto *name = new QLabel(rowShortName(r), this);
		name->setToolTip(rowName(r));
		name->setMaximumWidth(110);
		grid->addWidget(name, r + 1, 0);
		for (int i = 0; i < kStrips; ++i) {
			QPushButton *b = makeButton(mfdFill(mfdText("Desk.PadName", "%1, column %2"),
							    {rowName(r), QString::number(i + 1)}),
						    30, 22);
			pads_[r][i] = b;
			b->setObjectName(QStringLiteral("mfdDeskPad_%1_%2").arg(r).arg(i));
			connect(b, &QPushButton::pressed, this, [this, r, i]() { pressPad(r, i, true); });
			connect(b, &QPushButton::released, this, [this, r, i]() { pressPad(r, i, false); });
			grid->addWidget(b, r + 1, i + 1);
		}
	}
	auto *clip_name = new QLabel(mfdText("Desk.RowClipStopShort", "Clip Stop"), this);
	clip_name->setToolTip(mfdText("Desk.RowClipStop", "Clip Stop (own blend)"));
	clip_name->setMaximumWidth(110);
	grid->addWidget(clip_name, kRows + 1, 0);
	for (int i = 0; i < kStrips; ++i) {
		QPushButton *b = makeButton(mfdFill(mfdText("Desk.ClipStopName", "Clip Stop, column %1"),
						    {QString::number(i + 1)}),
					    30, 14);
		clip_stops_[i] = b;
		connect(b, &QPushButton::pressed, this,
			[this, i]() { pressNote((uint8_t)i, MFD_VJ_NOTE_CLIP_STOP, true, false); });
		connect(b, &QPushButton::released, this,
			[this, i]() { pressNote((uint8_t)i, MFD_VJ_NOTE_CLIP_STOP, false, false); });
		grid->addWidget(b, kRows + 1, i + 1);
	}

	/* Stop All, Shift (latched for the next pad), Session: added after the grid below */
	auto *transport = new QHBoxLayout();
	stop_all_ = makeButton(mfdText("Desk.StopAll", "Stop All: every source back to its own blend"), 80, 24);
	stop_all_->setText(mfdText("Desk.StopAllShort", "Stop All"));
	connect(stop_all_, &QPushButton::pressed, this, [this]() { pressNote(0U, MFD_VJ_NOTE_STOP_ALL, true, false); });
	connect(stop_all_, &QPushButton::released, this,
		[this]() { pressNote(0U, MFD_VJ_NOTE_STOP_ALL, false, false); });
	shift_ = makeButton(mfdText("Desk.Shift", "Shift for the next pad"), 60, 24);
	shift_->setText(mfdText("Desk.ShiftShort", "Shift"));
	shift_->setCheckable(true);
	session_ = makeButton(mfdText("Desk.Session", "Session: take Preview to Program"), 70, 24);
	session_->setText(mfdText("Desk.SessionShort", "Take"));
	connect(session_, &QPushButton::pressed, this, [this]() { pressNote(0U, MFD_VJ_NOTE_SESSION, true, false); });
	connect(session_, &QPushButton::released, this, [this]() { pressNote(0U, MFD_VJ_NOTE_SESSION, false, false); });
	transport->addWidget(stop_all_);
	transport->addWidget(shift_);
	transport->addWidget(session_);

	/* fader levels (read-only), under their columns */
	auto *fader_name = new QLabel(mfdText("Desk.Faders", "Faders"), this);
	grid->addWidget(fader_name, kRows + 2, 0);
	for (int i = 0; i < kStrips; ++i) {
		auto *bar = new QProgressBar(this);
		bar->setOrientation(Qt::Vertical);
		bar->setRange(0, 1000);
		bar->setTextVisible(false);
		bar->setFixedHeight(46);
		bar->setMinimumWidth(10);
		faders_[i] = bar;
		grid->addWidget(bar, kRows + 2, i + 1, Qt::AlignHCenter);
	}
	root->addLayout(grid);
	root->addLayout(transport);
	xfader_ = new QProgressBar(this);
	xfader_->setRange(0, 1000);
	xfader_->setTextVisible(false);
	xfader_->setFixedHeight(8);
	xfader_->setAccessibleName(mfdText("Desk.Crossfader", "Crossfader (T-bar)"));
	xfader_->setToolTip(xfader_->accessibleName());
	root->addWidget(xfader_);

	/* Visual Sidechain */
	auto *vs_row = new QHBoxLayout();
	bpm_ = new QLabel(this);
	bpm_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
	bpm_->setMinimumWidth(92);
	vs_row->addWidget(bpm_);
	lock_ = new QLabel(this);
	vs_row->addWidget(lock_);
	for (int k = 0; k < 4; ++k) {
		beats_[k] = new QLabel(this);
		beats_[k]->setFixedSize(12, 12);
		vs_row->addWidget(beats_[k]);
	}
	vs_row->addStretch(1);
	root->addLayout(vs_row);
	panic_ = new QLabel(this);
	panic_->setWordWrap(true);
	root->addWidget(panic_);
	auto *vs_buttons = new QHBoxLayout();
	vs_panic_ = makeButton(mfdText("Desk.VsPanic", "Panic / Restore (Record)"), 70, 24);
	vs_panic_->setText(mfdText("Desk.VsPanicShort", "Panic"));
	vs_tap_ = makeButton(mfdText("Desk.VsTap", "Tap tempo (Metronome)"), 50, 24);
	vs_tap_->setText(mfdText("Desk.VsTapShort", "Tap"));
	vs_lock_ = makeButton(mfdText("Desk.VsLock", "Lock or unlock the tempo (Shift + Metronome)"), 50, 24);
	vs_lock_->setText(mfdText("Desk.VsLockShort", "Lock"));
	vs_downbeat_ = makeButton(mfdText("Desk.VsDownbeat", "Downbeat now (Shift + Tap Tempo)"), 70, 24);
	vs_downbeat_->setText(mfdText("Desk.VsDownbeatShort", "Downbeat"));
	const std::pair<QPushButton *, enum mfd_vj_command_kind> vs[] = {{vs_panic_, MFD_VJ_CMD_VS_PANIC},
									 {vs_tap_, MFD_VJ_CMD_VS_TAP},
									 {vs_lock_, MFD_VJ_CMD_VS_LOCK},
									 {vs_downbeat_, MFD_VJ_CMD_VS_RESYNC}};
	for (const auto &p : vs) {
		enum mfd_vj_command_kind kind = p.second;
		connect(p.first, &QPushButton::clicked, this, [this, kind]() {
			if (!mfd_engine_desk_vs(eng_, kind)) {
				sayStatus(mfdText("Desk.Busy", "Too many presses at once; try again."));
			}
		});
		vs_buttons->addWidget(p.first);
	}
	root->addLayout(vs_buttons);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color: #94a3b8;"));
	root->addWidget(status_);
	root->addStretch(1);

	clock_.start();
	refresh();
}

void MfdDeskDock::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
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
	}
	return QWidget::event(e);
}

void MfdDeskDock::sayStatus(const QString &text)
{
	status_->setText(text);
	status_is_reason_ = false;
	status_age_.start();
}

void MfdDeskDock::pressNote(uint8_t channel, uint8_t note, bool on, bool shift)
{
	enum mfd_desk_press_result r = mfd_engine_desk_press(eng_, channel, note, on, shift, nullptr);
	if (on && r == MFD_DESK_REJECTED) {
		sayStatus(mfdText("Desk.Ignored", "That button cannot act now (see its tooltip)."));
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
		sayStatus(mfdText("Desk.Ignored", "That button cannot act now (see its tooltip)."));
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

void MfdDeskDock::paintButton(QPushButton *b, uint32_t rgb, bool enabled, bool bright_text)
{
	QString css = QStringLiteral("QPushButton { background-color: %1; color: %2; }"
				     "QPushButton:disabled { background-color: #1f2937; color: #4b5563; }")
			      .arg(rgbCss(rgb), bright_text ? QStringLiteral("#f8fafc") : QStringLiteral("#0b1120"));
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

	setLabel(title_, mfdFill(mfdText("Desk.Title", "Fadester Desk \xE2\x80\x94 Midi Fadester %1 \xC2\xB7 %2"),
				 {QString::fromUtf8(PLUGIN_VERSION),
				  view_.desk_connected ? mfdText("Desk.DeskOn", "APC40 mkII connected")
						       : mfdText("Desk.DeskOff", "no APC40 mkII")}));

	bool pads_on = view_.finished_loading && view_.pads == MFD_DESK_PADS_ON && !view_.learn_or_setup;
	bool shift = shift_->isChecked();

	/* scenes */
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		const struct mfd_vj_led &led = view_.leds[48 + j];
		bool exists = j < view_.scene_count;
		QString name = exists ? QString::fromUtf8(view_.scenes[j]) : QString();
		setButtonText(scenes_[j], scenes_[j]->fontMetrics().elidedText(name, Qt::ElideRight,
									       std::max(40, scenes_[j]->width() - 8)));
		QString what =
			!view_.studio_mode
				? mfdText("Desk.SceneTip.Program", "Click: put this scene on air.")
				: mfdText("Desk.SceneTip.Studio",
					  "Click: to Preview; click again: take it to Program. Ctrl-click: straight on air.");
		bool can = pads_on && exists && !view_.tbar_busy && !view_.scene_owned[j];
		setTip(scenes_[j], !exists ? mfdText("Desk.Tip.NoScene", "No scene here.")
				   : view_.tbar_busy
					   ? mfdText("Desk.Tip.Throw", "Waits while the crossfader is thrown.")
				   : view_.scene_owned[j] ? mfdText("Desk.Tip.Owned", "A fader learned this button.")
							  : name + QStringLiteral("\n") + what);
		paintButton(scenes_[j], ledColor(led, beat_pos, 0x22C55EU), can, false);
	}

	/* the grid */
	for (int i = 0; i < kStrips; ++i) {
		const struct mfd_desk_strip &st = view_.strips[i];
		QString src = QString::fromUtf8(st.source);
		setLabel(col_names_[i],
			 col_names_[i]->fontMetrics().elidedText(src.isEmpty() ? QStringLiteral("\xE2\x80\x94") : src,
								 Qt::ElideRight, std::max(28, col_names_[i]->width())));
		setTip(col_names_[i],
		       src.isEmpty() ? mfdText("Desk.Tip.Empty", "No source on this column.")
				     : src + (st.follows ? mfdText("Desk.Follows", " (follows the scene on air)")
							 : mfdText("Desk.Pinned", " (pinned)")));
		for (int r = 0; r < kRows; ++r) {
			int cell = kRowCell[r];
			const struct mfd_vj_led &led = view_.leds[i * MFD_DESK_CELLS + cell];
			bool can = pads_on && !st.owned[cell];
			QString why;
			if (st.owned[cell]) {
				why = mfdText("Desk.Tip.Owned", "A fader learned this button.");
			} else if (cell == MFD_DESK_RESTART && shift) {
				can = can && st.can_pin_toggle;
			} else if (!st.has_layer) {
				can = false;
				why = mfdText("Desk.Tip.Empty", "No source on this column.");
			} else if ((cell == MFD_DESK_FLASH || cell == MFD_DESK_FADE) && !st.single_layer) {
				can = false;
				why = mfdText("Desk.Tip.SingleLayer", "Needs a Layer Opacity fader.");
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
			    pads_on && st.has_layer && !st.owned[MFD_DESK_CLIP_STOP]);
		faders_[i]->setValue((int)std::lround(std::clamp(st.level, 0.0f, 1.0f) * 1000.0f));
		setTip(faders_[i], src.isEmpty()
					   ? mfdFill(mfdText("Desk.FaderEmpty", "Fader %1"), {QString::number(i + 1)})
					   : mfdFill(mfdText("Desk.FaderTip", "Fader %1: %2, %3 %"),
						     {QString::number(i + 1), src,
						      QString::number((int)std::lround(st.level * 100.0f))}));
	}
	xfader_->setValue((int)std::lround(std::clamp(view_.crossfader, 0.0f, 1.0f) * 1000.0f));
	paintButton(stop_all_, 0x475569U, pads_on && !view_.stop_all_owned, true);
	paintButton(shift_, shift ? 0xF8FAFCU : 0x475569U, pads_on, !shift);
	session_->setVisible(view_.studio_mode);
	paintButton(session_, view_.take_ready ? 0x22C55EU : 0x475569U,
		    pads_on && view_.take_ready && !view_.tbar_busy && !view_.session_owned, !view_.take_ready);

	/* Visual Sidechain */
	if (!vs_loaded_) {
		setLabel(bpm_, QStringLiteral("\xE2\x80\x94 BPM"));
		setLabel(lock_, mfdText("Desk.VsMissing", "Visual Sidechain is not loaded."));
	} else {
		/* no estimate yet (no audio): no number, rather than the animation's 120 */
		setLabel(bpm_, bpm_valid_ ? QStringLiteral("%1 BPM").arg(bpm_value_, 0, 'f', 1)
					  : QStringLiteral("\xE2\x80\x94 BPM"));
		setLabel(lock_,
			 locked ? mfdText("Desk.Locked", "tempo locked") : mfdText("Desk.Unlocked", "following"));
	}
	int step = vs_loaded_ ? (int)(((beat_index % 4) + 4) % 4) : -1;
	bool fresh = beat_index != last_beat_index_ && beat_phase < 0.25;
	last_beat_index_ = beat_index;
	for (int k = 0; k < 4; ++k) {
		bool on = k == step;
		setCss(beats_[k], QStringLiteral("background-color: %1; border-radius: 6px;")
					  .arg(on ? (fresh || beat_phase < 0.25 ? QStringLiteral("#f8fafc")
										: QStringLiteral("#64748b"))
						  : QStringLiteral("#1f2937")));
	}
	if (view_.vs_panic_known && view_.vs_panic) {
		setLabel(panic_, mfdText("Desk.PanicOn", "PANIC \xE2\x80\x94 Visual Sidechain's effects are held safe. "
							 "Press Panic again to restore."));
		setCss(panic_, QStringLiteral("color: #f8fafc; background-color: #b91c1c; padding: 4px;"));
	} else if (view_.vs_last_failed) {
		setLabel(panic_, mfdText("Desk.VsNoAnswer",
					 "Visual Sidechain did not answer (it needs obs-websocket switched on)."));
		setCss(panic_, QStringLiteral("color: #fde68a; padding: 2px;"));
	} else {
		setLabel(panic_, QString());
		setCss(panic_, QString());
	}
	setButtonText(vs_panic_, view_.vs_panic_known && view_.vs_panic ? mfdText("Desk.VsRestoreShort", "Restore")
									: mfdText("Desk.VsPanicShort", "Panic"));
	paintButton(vs_panic_, view_.vs_panic ? 0xEF4444U : 0x475569U, vs_loaded_, true);
	paintButton(vs_tap_, 0x475569U, vs_loaded_, true);
	paintButton(vs_lock_, locked ? 0x0EA5E9U : 0x475569U, vs_loaded_, true);
	paintButton(vs_downbeat_, 0x475569U, vs_loaded_, true);

	/* the status line: why the pads wait, else the last message for a few seconds */
	QString why;
	if (!view_.finished_loading) {
		why = mfdText("Desk.Loading", "Loading\xE2\x80\xA6");
	} else if (view_.pads == MFD_DESK_PADS_NO_APC40) {
		why = mfdText("Desk.WhyNoDesk", "Connect the APC40 mkII to use the pads.");
	} else if (view_.pads == MFD_DESK_PADS_BUTTONS_OFF) {
		why = mfdText("Desk.WhyOff", "The APC40 buttons are switched off in Midi Fadester's settings.");
	} else if (view_.pads == MFD_DESK_PADS_OTHER_PROFILE) {
		why = mfdText("Desk.WhyProfile",
			      "Choose the APC40 mkII profile in Midi Fadester's settings to use the pads.");
	} else if (view_.learn_or_setup) {
		why = mfdText("Desk.WhyLearn", "The pads wait while MIDI Learn or Set up is running.");
	}
	if (!why.isEmpty()) {
		setLabel(status_, why);
		status_is_reason_ = true;
	} else if (status_is_reason_ || (status_age_.isValid() && status_age_.elapsed() > 4000)) {
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
