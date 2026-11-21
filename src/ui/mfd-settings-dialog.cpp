/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbt.h>
#endif

#include "ui/mfd-settings-dialog.hpp"
#include "ui/mfd-desk-dock.hpp"
#include "ui/mfd-controller-svg.hpp"
#include "ui/mfd-setup-sheet.hpp"
#include "ui/mfd-text-qt.hpp"
#include "obs/mfd-engine.h"
#include "core/apc40-profile.h"
#include "core/filter-math.h"
#include "core/ui-sentences.h"
#include "core/tbar-logic.h"
#include "core/mixer-math.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

#include <QAbstractItemView>
#include <QAbstractNativeEventFilter>
#include <QCheckBox>
#include <QSpinBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRectF>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

static QString format_midi_addr(const struct mfd_midi_addr &addr)
{
	const QString ch = QString::number(static_cast<int>(addr.channel) + 1);
	const QString num = QString::number(static_cast<int>(addr.number));
	switch (addr.type) {
	case MFD_MIDI_CC:
		return addr.is_14bit ? mfdFill(mfdText("Dialog.MidiAddr.Cc14", "Ch %1 CC %2 (14-bit)"), {ch, num})
				     : mfdFill(mfdText("Dialog.MidiAddr.Cc", "Ch %1 CC %2"), {ch, num});
	case MFD_MIDI_PITCH_BEND:
		return mfdFill(mfdText("Dialog.MidiAddr.PitchBend", "Ch %1 Pitch Bend"), {ch});
	case MFD_MIDI_NOTE_ON:
		return mfdFill(mfdText("Dialog.MidiAddr.Note", "Ch %1 Note %2"), {ch, num});
	default:
		return mfdText("Dialog.MidiAddr.Unbound", "Not assigned");
	}
}

/* the Advanced table's columns (U1-17) */
enum {
	kAdvColOn = 0,
	kAdvColStrip,
	kAdvColMidi,
	kAdvColReverse,
	kAdvColJob,
	kAdvColOutput,
	kAdvColEdit,
	kAdvColumns,
};

static QWidget *make_centered_checkbox_cell(QWidget *parent, const QString &obj_name, bool checked,
					    const QString &tooltip, QCheckBox **out_checkbox)
{
	auto *container = new QWidget(parent);
	auto *layout = new QHBoxLayout(container);
	layout->setContentsMargins(4, 0, 4, 0);
	layout->setAlignment(Qt::AlignCenter);
	auto *chk = new QCheckBox(container);
	chk->setObjectName(obj_name);
	chk->setChecked(checked);
	if (!tooltip.isEmpty()) {
		chk->setToolTip(tooltip);
		container->setToolTip(tooltip);
	}
	layout->addWidget(chk);
	if (out_checkbox) {
		*out_checkbox = chk;
	}
	return container;
}

static QString output_bar_stylesheet(const QString &chunk_hex, const QString &border_hex)
{
	return QStringLiteral("QProgressBar { background-color: #111827; color: #f9fafb; border: 1px solid %2; "
			      "border-radius: 4px; text-align: center; font-weight: bold; min-height: 22px; } "
			      "QProgressBar::chunk { background-color: %1; border-radius: 3px; }")
		.arg(chunk_hex, border_hex);
}

static QString stream_deck_companion_url()
{
	return QStringLiteral("https://github.com/jbrick2070/streamdeck-fadester");
}

static void open_stream_deck_companion_url()
{
	QDesktopServices::openUrl(QUrl(stream_deck_companion_url()));
}

class MfdStripBarWidget final : public QWidget {
public:
	explicit MfdStripBarWidget(bool is_horizontal, QWidget *parent = nullptr)
		: QWidget(parent),
		  is_horizontal_(is_horizontal)
	{
		setFixedHeight(124);
	}

	void set_state(int bar_pct, bool dimmed, enum mfd_ui_severity severity, bool at_end, bool show_pickup,
		       int pickup_target_pct, const QString &arrow_glyph, bool is_fx_style = false)
	{
		const int clamped_pct = std::clamp(bar_pct, 0, 100);
		const int clamped_pickup = std::clamp(pickup_target_pct, 0, 100);
		if (bar_pct_ == clamped_pct && dimmed_ == dimmed && severity_ == severity && at_end_ == at_end &&
		    show_pickup_ == show_pickup && pickup_pct_ == clamped_pickup && arrow_glyph_ == arrow_glyph &&
		    is_fx_style_ == is_fx_style) {
			return;
		}
		bar_pct_ = clamped_pct;
		dimmed_ = dimmed;
		severity_ = severity;
		at_end_ = at_end;
		show_pickup_ = show_pickup;
		pickup_pct_ = clamped_pickup;
		arrow_glyph_ = arrow_glyph;
		is_fx_style_ = is_fx_style;
		setProperty("barPercent", bar_pct_);
		setProperty("showingPickup", show_pickup_);
		setProperty("pickupPercent", pickup_pct_);
		setProperty("arrowGlyph", arrow_glyph_);
		setProperty("fxStyle", is_fx_style_);
		update();
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		Q_UNUSED(event);
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);

		QColor accent = is_fx_style_ ? QColor(0x06, 0xb6, 0xd4) : QColor(0x38, 0xbd, 0xf8);
		if (dimmed_) {
			accent = QColor(0x64, 0x74, 0x8b);
		} else if (severity_ == MFD_UI_SEVERITY_RED) {
			accent = QColor(0xef, 0x44, 0x44);
		} else if (severity_ == MFD_UI_SEVERITY_AMBER) {
			accent = QColor(0xf5, 0x9e, 0x0b);
		} else if (at_end_) {
			accent = QColor(0x10, 0xb9, 0x81);
		} else if (is_fx_style_ && bar_pct_ >= 75) {
			accent = QColor(0xf5, 0x9e, 0x0b);
		}

		const QColor track_bg(0x1e, 0x29, 0x3b);
		const QColor cap_fill(0xf8, 0xfa, 0xfc);
		const QColor pickup_col(0xfb, 0xbf, 0x24);

		if (is_horizontal_) {
			const qreal margin_x = 12.0;
			const qreal track_w = std::max<qreal>(24.0, static_cast<qreal>(width()) - margin_x * 2.0);
			const qreal track_h = 12.0;
			const qreal track_y = static_cast<qreal>(height()) * 0.48 - track_h * 0.5;

			p.setPen(Qt::NoPen);
			p.setBrush(track_bg);
			p.drawRoundedRect(QRectF(margin_x, track_y, track_w, track_h), 5.0, 5.0);

			const qreal fill_w = (static_cast<qreal>(bar_pct_) / 100.0) * track_w;
			if (fill_w > 0.5) {
				p.setBrush(accent);
				p.drawRoundedRect(QRectF(margin_x, track_y, fill_w, track_h), 5.0, 5.0);
			}

			if (show_pickup_) {
				const qreal px = margin_x + (static_cast<qreal>(pickup_pct_) / 100.0) * track_w;
				p.setPen(QPen(pickup_col, 2.5));
				p.drawLine(QPointF(px, track_y - 9.0), QPointF(px, track_y + track_h + 9.0));
			}

			const qreal cap_w = 14.0;
			const qreal cap_h = 26.0;
			const qreal cap_x =
				std::clamp(margin_x + fill_w - cap_w * 0.5, margin_x - 3.0, margin_x + track_w - 11.0);
			p.setPen(QPen(accent, 1.8));
			p.setBrush(cap_fill);
			p.drawRoundedRect(QRectF(cap_x, track_y - 7.0, cap_w, cap_h), 3.0, 3.0);

			QFont f = font();
			f.setPixelSize(12);
			f.setBold(true);
			p.setFont(f);
			p.setPen(dimmed_ ? QColor(0x94, 0xa3, 0xb8) : QColor(0xe2, 0xe8, 0xf0));
			const QString pct_txt =
				show_pickup_ && !arrow_glyph_.isEmpty()
					? QStringLiteral("%1%  %2 %3%").arg(bar_pct_).arg(arrow_glyph_).arg(pickup_pct_)
					: QStringLiteral("%1%").arg(bar_pct_);
			p.drawText(QRectF(4.0, track_y + 22.0, static_cast<qreal>(width()) - 8.0, 22.0),
				   Qt::AlignCenter, pct_txt);
		} else {
			const qreal cx = static_cast<qreal>(width()) * 0.5;
			const qreal track_top = 10.0;
			const qreal track_h = 84.0;
			const qreal track_w = 10.0;

			p.setPen(Qt::NoPen);
			p.setBrush(track_bg);
			p.drawRoundedRect(QRectF(cx - track_w * 0.5, track_top, track_w, track_h), 5.0, 5.0);

			const qreal fill_h = (static_cast<qreal>(bar_pct_) / 100.0) * track_h;
			if (fill_h > 0.5) {
				p.setBrush(accent);
				p.drawRoundedRect(QRectF(cx - track_w * 0.5, track_top + track_h - fill_h, track_w,
							 fill_h),
						  5.0, 5.0);
			}

			if (show_pickup_) {
				const qreal py =
					track_top + track_h - (static_cast<qreal>(pickup_pct_) / 100.0) * track_h;
				p.setPen(QPen(pickup_col, 2.5));
				p.drawLine(QPointF(cx - 18.0, py), QPointF(cx + 18.0, py));
			}

			const qreal cap_w = 28.0;
			const qreal cap_h = 12.0;
			const qreal cap_y = std::clamp(track_top + track_h - fill_h - cap_h * 0.5, track_top - 2.0,
						       track_top + track_h - 10.0);
			p.setPen(QPen(accent, 1.8));
			p.setBrush(cap_fill);
			p.drawRoundedRect(QRectF(cx - cap_w * 0.5, cap_y, cap_w, cap_h), 3.0, 3.0);

			QFont f = font();
			f.setPixelSize(11);
			f.setBold(true);
			p.setFont(f);
			p.setPen(dimmed_ ? QColor(0x94, 0xa3, 0xb8) : QColor(0xe2, 0xe8, 0xf0));
			const QString pct_txt =
				show_pickup_ && !arrow_glyph_.isEmpty()
					? QStringLiteral("%1% %2%3%").arg(bar_pct_).arg(arrow_glyph_).arg(pickup_pct_)
					: QStringLiteral("%1%").arg(bar_pct_);
			p.drawText(QRectF(2.0, track_top + track_h + 6.0, static_cast<qreal>(width()) - 4.0, 20.0),
				   Qt::AlignCenter, pct_txt);
		}
	}

private:
	bool is_horizontal_ = false;
	int bar_pct_ = 0;
	bool dimmed_ = true;
	enum mfd_ui_severity severity_ = MFD_UI_SEVERITY_NONE;
	bool at_end_ = false;
	bool show_pickup_ = false;
	int pickup_pct_ = 0;
	QString arrow_glyph_;
	bool is_fx_style_ = false;
};

class MfdSettingsDialog final : public QDialog {
public:
	explicit MfdSettingsDialog(struct mfd_engine *eng, QWidget *parent = nullptr)
		: QDialog(parent),
		  eng_(eng ? eng : mfd_engine_get())
	{
		setObjectName(QStringLiteral("mfdSettingsDialog"));
		/* the version only: the commit and the build time are on the Advanced tab's last line (mfdAdvAbout) */
		setWindowTitle(mfdFill(mfdText("Dialog.WindowTitle", "Midi Fadester %1"),
				       {QString::fromUtf8(PLUGIN_VERSION)}));
		/* 1380 x 660 where it fits; on a 1080p laptop at 150 % (1280 x 720 points) or smaller the dialog is cut to
		 * the screen, and the strips scroll sideways (F-12). */
		QScreen *scr = parent ? parent->screen() : QGuiApplication::primaryScreen();
		const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1380, 660);
		const int dlg_w = std::min(1380, std::max(640, avail.width() - 40));
		const int dlg_h = std::min(660, std::max(400, avail.height() - 60));
		setMinimumSize(std::min(1160, dlg_w), std::min(560, dlg_h));
		resize(dlg_w, dlg_h);
		setStyleSheet(QStringLiteral(
			"QDialog#mfdSettingsDialog { background-color: #0b1120; color: #f8fafc; } "
			"QTabWidget::pane { border: 1px solid #1e293b; background-color: #0b1120; border-radius: 6px; } "
			"QTabBar::tab { background-color: #111827; color: #94a3b8; border: 1px solid #1e293b; "
			"padding: 7px 18px; margin-right: 4px; border-top-left-radius: 6px; border-top-right-radius: 6px; font-weight: bold; } "
			"QTabBar::tab:selected { background-color: #1e293b; color: #f8fafc; border-bottom-color: #38bdf8; } "
			"QComboBox { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; border-radius: 4px; padding: 4px 8px; } "
			"QComboBox QAbstractItemView { background-color: #1e293b; color: #f8fafc; selection-background-color: #2563eb; } "
			"QDoubleSpinBox { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; border-radius: 4px; padding: 3px 6px; } "
			"QTableWidget { background-color: #0f172a; alternate-background-color: #111c33; color: #f8fafc; gridline-color: #1e293b; border: 1px solid #1e293b; } "
			"QHeaderView::section { background-color: #1e293b; color: #e2e8f0; padding: 6px; border: 1px solid #334155; font-weight: bold; }"));

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(12, 10, 12, 10);
		root->setSpacing(8);

		/* --- First-Run Offer Banner --- */
		first_run_banner_ = new QWidget(this);
		first_run_banner_->setObjectName(QStringLiteral("mfdFirstRunBanner"));
		first_run_banner_->setStyleSheet(QStringLiteral(
			"QWidget#mfdFirstRunBanner { background-color: #102a43; border: 1px solid #38bdf8; border-radius: 6px; }"));
		auto *fr_lay = new QHBoxLayout(first_run_banner_);
		fr_lay->setContentsMargins(12, 8, 12, 8);
		fr_lay->setSpacing(10);
		/* a notice, not a question: the layout is already applied (mfd_engine_apply_first_run_layout); the text is
		 * set in refresh_from_engine, from what Master really got */
		auto *fr_lbl = new QLabel(first_run_banner_);
		first_run_text_ = fr_lbl;
		fr_lbl->setObjectName(QStringLiteral("mfdFirstRunText"));
		fr_lbl->setWordWrap(true);
		fr_lbl->setStyleSheet(
			QStringLiteral("color: #e0f2fe; font-weight: bold; background: transparent; border: none;"));
		auto *btn_fr_yes = new QPushButton(mfdText("Dialog.FirstRun.Yes", "OK"), first_run_banner_);
		btn_fr_yes->setObjectName(QStringLiteral("mfdFirstRunYesBtn"));
		btn_fr_yes->setCursor(Qt::PointingHandCursor);
		btn_fr_yes->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: #0284c7; color: #ffffff; font-weight: bold; padding: 6px 18px; border-radius: 5px; border: 1px solid #38bdf8; }"));
		auto *btn_fr_custom =
			new QPushButton(mfdText("Dialog.FirstRun.Custom", "Start empty"), first_run_banner_);
		btn_fr_custom->setObjectName(QStringLiteral("mfdFirstRunCustomBtn"));
		btn_fr_custom->setCursor(Qt::PointingHandCursor);
		/* small and quiet: it clears every strip, and asks first */
		btn_fr_custom->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: transparent; color: #93c5fd; font-size: 11px; padding: 4px 8px; border-radius: 5px; border: 1px solid #1e3a5f; } "
			"QPushButton:hover { color: #e0f2fe; border-color: #38bdf8; }"));
		fr_lay->addWidget(fr_lbl, 1);
		fr_lay->addWidget(btn_fr_yes);
		fr_lay->addWidget(btn_fr_custom);
		first_run_banner_->hide();
		root->addWidget(first_run_banner_);

		connect(btn_fr_yes, &QPushButton::clicked, this, [this]() {
			mfd_engine_acknowledge_first_run(eng_); /* no strip changes: only the notice goes */
			refresh_from_engine();
		});
		connect(btn_fr_custom, &QPushButton::clicked, this, [this]() {
			if (!confirm(mfdText("Dialog.FirstRun.Custom", "Start empty"),
				     mfdText("Dialog.FirstRun.ConfirmText",
					     "Clear all 18 strips? The crossfader, the faders, Master and the FX knobs "
					     "lose their jobs, and the faders no longer fill themselves from the scene on "
					     "air."),
				     mfdText("Dialog.FirstRun.Custom", "Start empty"))) {
				return;
			}
			mfd_engine_decline_first_run_usual_layout(eng_);
			refresh_from_engine();
		});

		/* --- No Ready Layout Banner: the open desk matches no layout, so its faders do nothing yet --- */
		no_layout_banner_ = new QWidget(this);
		no_layout_banner_->setObjectName(QStringLiteral("mfdNoLayoutBanner"));
		no_layout_banner_->setStyleSheet(QStringLiteral(
			"QWidget#mfdNoLayoutBanner { background-color: #102a43; border: 1px solid #38bdf8; border-radius: 6px; }"));
		auto *nl_lay = new QHBoxLayout(no_layout_banner_);
		nl_lay->setContentsMargins(12, 8, 12, 8);
		nl_lay->setSpacing(10);
		no_layout_text_ = new QLabel(no_layout_banner_);
		no_layout_text_->setObjectName(QStringLiteral("mfdNoLayoutText"));
		no_layout_text_->setWordWrap(true);
		no_layout_text_->setStyleSheet(
			QStringLiteral("color: #e0f2fe; font-weight: bold; background: transparent; border: none;"));
		auto *btn_nl_ok = new QPushButton(mfdText("Dialog.NoLayout.Ok", "OK"), no_layout_banner_);
		btn_nl_ok->setObjectName(QStringLiteral("mfdNoLayoutOkBtn"));
		btn_nl_ok->setCursor(Qt::PointingHandCursor);
		btn_nl_ok->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: #0284c7; color: #ffffff; font-weight: bold; padding: 6px 18px; border-radius: 5px; border: 1px solid #38bdf8; }"));
		nl_lay->addWidget(no_layout_text_, 1);
		nl_lay->addWidget(btn_nl_ok);
		no_layout_banner_->hide();
		root->addWidget(no_layout_banner_);

		connect(btn_nl_ok, &QPushButton::clicked, this, [this]() {
			/* hidden while this window is open; another desk without a layout brings it back */
			no_layout_dismissed_for_ = QString::fromUtf8(snap_.opened_device_name);
			no_layout_banner_->hide();
		});

		/* --- Amber Studio Mode Banner --- */
		studio_note_widget_ = new QWidget(this);
		studio_note_widget_->setObjectName(QStringLiteral("mfdStudioNote"));
		studio_note_widget_->setStyleSheet(
			QStringLiteral("QWidget#mfdStudioNote { background-color: #452703; border: 1px solid #d97706; "
				       "border-radius: 6px; }"));
		auto *note_layout = new QHBoxLayout(studio_note_widget_);
		note_layout->setContentsMargins(12, 7, 12, 7);
		auto *note_label =
			new QLabel(mfdText("Dialog.StudioNote.Text",
					   "Studio Mode is off. Turn on Studio Mode to switch scenes with the T-bar."),
				   studio_note_widget_);
		note_label->setObjectName(QStringLiteral("mfdStudioNoteLabel"));
		note_label->setStyleSheet(
			QStringLiteral("color: #fde68a; font-weight: bold; background: transparent; border: none;"));
		note_label->setWordWrap(true);
		auto *btn_turn_on_studio =
			new QPushButton(mfdText("Strip.TurnOnStudioMode", "Turn on Studio Mode"), studio_note_widget_);
		btn_turn_on_studio->setObjectName(QStringLiteral("mfdBannerTurnOnStudioBtn"));
		btn_turn_on_studio->setCursor(Qt::PointingHandCursor);
		btn_turn_on_studio->setStyleSheet(
			QStringLiteral("QPushButton { background-color: #d97706; color: #111827; font-weight: bold; "
				       "padding: 5px 14px; border-radius: 4px; }"));
		note_layout->addWidget(note_label, 1);
		note_layout->addWidget(btn_turn_on_studio);
		studio_note_widget_->hide();
		root->addWidget(studio_note_widget_);

		connect(btn_turn_on_studio, &QPushButton::clicked, this, [this]() {
			mfd_engine_request_studio_mode(eng_, true);
			refresh_from_engine();
		});

		/* --- Optional Stream Deck Companion Discovery --- */
		stream_deck_companion_card_ = new QFrame(this);
		stream_deck_companion_card_->setObjectName(QStringLiteral("mfdStreamDeckCompanionCard"));
		stream_deck_companion_card_->setStyleSheet(QStringLiteral(
			"QFrame#mfdStreamDeckCompanionCard { background-color: transparent; border: none; }"));
		/* one quiet line with a link and a cross: another product's notice must not be as loud as this one's own
		 * (U1-10) */
		auto *sd_lay = new QHBoxLayout(stream_deck_companion_card_);
		sd_lay->setContentsMargins(4, 0, 4, 0);
		sd_lay->setSpacing(8);
		auto *sd_copy = new QWidget(stream_deck_companion_card_);
		auto *sd_copy_lay = new QHBoxLayout(sd_copy);
		sd_copy_lay->setContentsMargins(0, 0, 0, 0);
		sd_copy_lay->setSpacing(8);
		auto *sd_title = new QLabel(mfdText("Dialog.StreamDeck.Title", "Use a Stream Deck?"), sd_copy);
		sd_title->setObjectName(QStringLiteral("mfdStreamDeckCompanionTitle"));
		sd_title->setStyleSheet(QStringLiteral(
			"color: #cbd5e1; font-weight: bold; background: transparent; border: none; font-size: 12px;"));
		auto *sd_text =
			new QLabel(mfdText("Dialog.StreamDeck.Text",
					   "Fadester for OBS adds touch controls, live meters, crossfader control, and "
					   "auto-fades to the same Midi Fadester strips."),
				   sd_copy);
		sd_text->setObjectName(QStringLiteral("mfdStreamDeckCompanionText"));
		sd_text->setWordWrap(true);
		sd_text->setStyleSheet(
			QStringLiteral("color: #94a3b8; background: transparent; border: none; font-size: 12px;"));
		sd_copy_lay->addWidget(sd_title);
		sd_copy_lay->addWidget(sd_text, 1);
		sd_lay->addWidget(sd_copy, 1);

		auto *btn_sd_learn = new QPushButton(mfdText("Dialog.StreamDeck.Learn", "Learn about Fadester for OBS"),
						     stream_deck_companion_card_);
		btn_sd_learn->setObjectName(QStringLiteral("mfdBtnStreamDeckLearn"));
		btn_sd_learn->setProperty("destinationUrl", stream_deck_companion_url());
		btn_sd_learn->setCursor(Qt::PointingHandCursor);
		btn_sd_learn->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: transparent; color: #93c5fd; border: none; text-decoration: underline; "
			"font-size: 12px; padding: 2px 4px; } QPushButton:hover { color: #e0f2fe; }"));
		sd_lay->addWidget(btn_sd_learn);

		auto *btn_sd_dismiss =
			new QPushButton(QString::fromUtf8("\xC3\x97"), stream_deck_companion_card_); /* a cross */
		btn_sd_dismiss->setObjectName(QStringLiteral("mfdBtnStreamDeckDismiss"));
		btn_sd_dismiss->setAccessibleName(mfdText("Dialog.StreamDeck.Hide", "Hide"));
		btn_sd_dismiss->setToolTip(mfdText("Dialog.StreamDeck.HideTip",
						   "Hide this card. The Fadester for OBS link stays available below."));
		btn_sd_dismiss->setCursor(Qt::PointingHandCursor);
		btn_sd_dismiss->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: transparent; color: #94a3b8; border: none; font-size: 14px; "
			"padding: 0px 6px; } QPushButton:hover { color: #ffffff; }"));
		sd_lay->addWidget(btn_sd_dismiss);
		stream_deck_companion_card_->hide();
		root->addWidget(stream_deck_companion_card_);

		connect(btn_sd_learn, &QPushButton::clicked, this, []() { open_stream_deck_companion_url(); });
		connect(btn_sd_dismiss, &QPushButton::clicked, this, [this]() {
			mfd_engine_set_stream_deck_companion_dismissed(eng_, true);
			refresh_from_engine();
		});

		/* --- Main Tab Widget (Strips | Advanced) --- */
		main_tabs_ = new QTabWidget(this);
		main_tabs_->setObjectName(QStringLiteral("mfdMainTabs"));
		root->addWidget(main_tabs_, 1);

		build_strips_tab();
		build_advanced_tab();

		connect(main_tabs_, &QTabWidget::currentChanged, this, [this](int idx) {
			if (idx == 1 && setup_sheet_ && setup_sheet_->isVisible()) {
				setup_sheet_->reject();
			}
			if (idx != 1 && eng_) {
				mfd_engine_cancel_learn(
					eng_); /* row Learn lives on the Advanced tab: leaving it ends Learn */
			}
			refresh_from_engine();
			if (idx == 1 && table_) {
				for (int c = 0; c < 9; ++c) {
					table_->resizeColumnToContents(c);
				}
				table_->doItemsLayout();
			}
		});

		/* For standalone headless test engines, connect ui_notify_fn through Qt's queued event loop */
		if (eng_ != mfd_engine_get()) {
			owns_engine_notify_ = true;
			mfd_engine_set_ui_notify_fn(eng_, &MfdSettingsDialog::on_custom_engine_notify, this);
		}

		populate_device_combos();
		mfdPlainLabels(this); /* names from OBS are shown as plain text (F-10); the help text stays rich */
		refresh_from_engine();
	}

	~MfdSettingsDialog() override
	{
		if (eng_) {
			mfd_engine_cancel_learn(eng_);
		}
		if (owns_engine_notify_ && eng_) {
			mfd_engine_set_ui_notify_fn(eng_, nullptr, nullptr);
		}
		if (setup_sheet_) {
			setup_sheet_->close();
		}
		if (controller_view_dlg_) {
			controller_view_dlg_->close();
		}
	}

	bool any_port_list_open() const
	{
		const bool z1_open = zone1_device_combo_ && zone1_device_combo_->view() &&
				     zone1_device_combo_->view()->isVisible();
		const bool adv_open = device_combo_ && device_combo_->view() && device_combo_->view()->isVisible();
		return z1_open || adv_open;
	}

	/* A MIDI device came or went. A port list that is dropped down is not refilled under the pointer (F-05): the
	 * refill waits for the next refresh after it closes. */
	void on_hardware_ports_changed()
	{
		if (any_port_list_open()) {
			ports_refill_pending_ = true;
			return;
		}
		ports_refill_pending_ = false;
		populate_device_combos();
	}

	void refresh_from_engine()
	{
		if (!eng_) {
			return;
		}
		mfd_engine_get_ui_snapshot(eng_, &snap_);

		if (snap_.midi_connected != last_midi_connected_ || ports_refill_pending_) {
			if (!any_port_list_open()) {
				last_midi_connected_ = snap_.midi_connected;
				ports_refill_pending_ = false;
				populate_device_combos();
			}
		}

		/* Top banners */
		/* One bar at a time (U1-10): the first-run notice, else the no-layout notice, else Studio Mode, else the
		 * Stream Deck line. Three at once were a question, a warning and an advert before any content. */
		first_run_banner_->setVisible(snap_.first_run_offer_pending);
		if (snap_.first_run_offer_pending) {
			const struct mfd_fader_slot &master = snap_.slots[9];
			const bool has_master = master.enabled && master.target_type == MFD_TARGET_AUDIO_VOLUME &&
						master.source_a_name[0] != '\0';
			const QString text =
				has_master
					? mfdFill(mfdText("Dialog.FirstRun.Text",
							  "Set up for you: the crossfader switches scenes, faders 1 to "
							  "8 follow the scene on air, and Master is the volume of %1. "
							  "Click a strip to change it."),
						  {QString::fromUtf8(master.source_a_name)})
					: mfdText("Dialog.FirstRun.TextNoMaster",
						  "Set up for you: the crossfader switches scenes, and faders 1 to 8 "
						  "follow the scene on air. Click a strip to change it.");
			if (first_run_text_->text() != text) {
				first_run_text_->setText(text);
			}
		}
		refresh_no_layout_banner();

		size_t active_tbar_slots = 0U;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			const struct mfd_fader_slot &s = snap_.slots[i];
			if (s.enabled && s.target_type == MFD_TARGET_TBAR) {
				++active_tbar_slots;
			}
		}
		const bool notice_showing = snap_.first_run_offer_pending || !no_layout_banner_->isHidden();
		const bool studio_note = !notice_showing &&
					 mfd_should_show_studio_mode_note(snap_.studio_mode_active, active_tbar_slots);
		studio_note_widget_->setVisible(studio_note);
		stream_deck_companion_card_->setVisible(!snap_.stream_deck_companion_dismissed && !notice_showing &&
							!studio_note);

		refresh_strips_tab();
		refresh_advanced_tab();

		if (setup_sheet_ && setup_sheet_->isVisible()) {
			setup_sheet_->refresh_from_snapshot(snap_);
		}
		if (controller_view_dlg_ && controller_view_dlg_->isVisible()) {
			controller_view_dlg_->refresh_from_snapshot(snap_);
		}
	}

protected:
	void closeEvent(QCloseEvent *event) override
	{
		if (eng_) {
			mfd_engine_cancel_learn(
				eng_); /* an armed row Learn must not bind a control after the window is gone */
		}
		if (setup_sheet_ && setup_sheet_->isVisible()) {
			setup_sheet_->reject();
		}
		if (controller_view_dlg_ && controller_view_dlg_->isVisible()) {
			controller_view_dlg_->close();
		}
		QDialog::closeEvent(event);
	}

	void hideEvent(QHideEvent *event) override
	{
		/* Esc and reject() hide the window without a closeEvent. */
		if (eng_) {
			mfd_engine_cancel_learn(eng_);
		}
		QDialog::hideEvent(event);
	}

private:
	static void on_custom_engine_notify(void *user_data)
	{
		auto *dlg = static_cast<MfdSettingsDialog *>(user_data);
		if (!dlg) {
			return;
		}
		if (!dlg->local_refresh_queued_.exchange(true)) {
			const bool ok = QMetaObject::invokeMethod(
				dlg,
				[dlg]() {
					dlg->local_refresh_queued_.store(false);
					if (dlg->eng_) {
						mfd_engine_flush_pending_ui_work(dlg->eng_);
					}
					dlg->refresh_from_engine();
				},
				Qt::QueuedConnection);
			if (!ok) {
				dlg->local_refresh_queued_.store(false);
			}
		}
	}

	void build_strips_tab()
	{
		auto *strips_tab = new QWidget(main_tabs_);
		strips_tab->setObjectName(QStringLiteral("mfdStripsTab"));
		auto *lay = new QVBoxLayout(strips_tab);
		lay->setContentsMargins(10, 10, 10, 10);
		lay->setSpacing(10);

		/* --- Zone 1: Controller Bar --- */
		auto *zone1 = new QFrame(strips_tab);
		zone1->setObjectName(QStringLiteral("mfdControllerBar"));
		zone1->setStyleSheet(QStringLiteral(
			"QFrame#mfdControllerBar { background-color: #111827; border: 1px solid #1e293b; border-radius: 7px; }"));
		auto *z1_lay = new QHBoxLayout(zone1);
		z1_lay->setContentsMargins(12, 8, 12, 8);
		z1_lay->setSpacing(10);

		z1_name_label_ = new QLabel(mfdText("Dialog.Controller.NoneName", "Controller: None"), zone1);
		z1_name_label_->setObjectName(QStringLiteral("mfdControllerNameLabel"));
		z1_name_label_->setStyleSheet(QStringLiteral("color: #f8fafc; font-size: 13px; font-weight: bold;"));
		z1_lay->addWidget(z1_name_label_);

		z1_dot_label_ = new QLabel(QString::fromUtf8("\xE2\x97\x8F"), zone1);
		z1_dot_label_->setObjectName(QStringLiteral("mfdControllerDot"));
		z1_lay->addWidget(z1_dot_label_);

		z1_status_label_ = new QLabel(mfdText("Controller.Connected", "Connected"), zone1);
		z1_status_label_->setObjectName(QStringLiteral("mfdControllerStatusLabel"));
		z1_status_label_->setWordWrap(true);
		z1_lay->addWidget(z1_status_label_, 1);

		z1_beta_badge_ = new QLabel(zone1);
		z1_beta_badge_->setObjectName(QStringLiteral("mfdBetaProfileBadge"));
		z1_beta_badge_->setStyleSheet(QStringLiteral(
			"QLabel { background-color: #451a03; color: #fde68a; border: 1px solid #f59e0b; "
			"border-radius: 10px; padding: 3px 10px; font-size: 11px; font-weight: bold; }"));
		z1_beta_badge_->hide();
		z1_lay->addWidget(z1_beta_badge_);

		z1_btn_reconnect_ = new QPushButton(mfdText("Dialog.Reconnect", "Reconnect"), zone1);
		z1_btn_reconnect_->setObjectName(QStringLiteral("mfdBtnReconnect"));
		z1_btn_reconnect_->setCursor(Qt::PointingHandCursor);
		z1_btn_reconnect_->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: #b91c1c; color: #ffffff; font-weight: bold; border: 1px solid #ef4444; border-radius: 5px; padding: 5px 14px; }"));
		z1_lay->addWidget(z1_btn_reconnect_);

		z1_change_panel_ = new QWidget(zone1);
		z1_change_panel_->setObjectName(QStringLiteral("mfdChangeControllerPanel"));
		auto *cp_lay = new QHBoxLayout(z1_change_panel_);
		cp_lay->setContentsMargins(0, 0, 0, 0);
		cp_lay->setSpacing(6);
		auto *lbl_z1_port = new QLabel(mfdText("Dialog.Port", "Port:"), z1_change_panel_);
		lbl_z1_port->setStyleSheet(QStringLiteral("color: #cbd5e1; font-size: 12px; font-weight: bold;"));
		cp_lay->addWidget(lbl_z1_port);
		zone1_device_combo_ = new QComboBox(z1_change_panel_);
		zone1_device_combo_->setObjectName(QStringLiteral("mfdZone1DeviceCombo"));
		zone1_device_combo_->setMinimumWidth(190);
		cp_lay->addWidget(zone1_device_combo_);
		auto *lbl_z1_layout = new QLabel(mfdText("Dialog.Layout", "Layout:"), z1_change_panel_);
		lbl_z1_layout->setStyleSheet(QStringLiteral("color: #cbd5e1; font-size: 12px; font-weight: bold;"));
		cp_lay->addWidget(lbl_z1_layout);
		zone1_profile_combo_ = new QComboBox(z1_change_panel_);
		zone1_profile_combo_->setObjectName(QStringLiteral("mfdZone1ProfileCombo"));
		zone1_profile_combo_->setMinimumWidth(210);
		populate_profile_combo(zone1_profile_combo_);
		cp_lay->addWidget(zone1_profile_combo_);
		z1_change_panel_->hide();
		z1_lay->addWidget(z1_change_panel_);

		const QString z1_btn_style = QStringLiteral(
			"QPushButton { background-color: #1e293b; color: #e2e8f0; border: 1px solid #475569; border-radius: 5px; padding: 5px 12px; font-weight: bold; } "
			"QPushButton:hover { border-color: #93c5fd; color: #ffffff; }");

		z1_btn_change_ctrl_ = new QPushButton(mfdText("Dialog.ChangeController", "Change controller"), zone1);
		z1_btn_change_ctrl_->setObjectName(QStringLiteral("mfdBtnChangeController"));
		z1_btn_change_ctrl_->setCursor(Qt::PointingHandCursor);
		z1_btn_change_ctrl_->setStyleSheet(z1_btn_style);
		z1_lay->addWidget(z1_btn_change_ctrl_);

		z1_btn_show_ctrl_ = new QPushButton(mfdText("Dialog.ShowController", "Show controller"), zone1);
		z1_btn_show_ctrl_->setObjectName(QStringLiteral("mfdBtnShowController"));
		z1_btn_show_ctrl_->setCursor(Qt::PointingHandCursor);
		z1_btn_show_ctrl_->setStyleSheet(z1_btn_style);
		z1_lay->addWidget(z1_btn_show_ctrl_);

		/* the show screen, one click from the first tab (it was a button on the Advanced tab, U1-01) */
		auto *z1_btn_show_desk = new QPushButton(mfdText("Dialog.ShowDesk", "Show Fadester Desk"), zone1);
		z1_btn_show_desk->setObjectName(QStringLiteral("mfdBtnShowDesk"));
		z1_btn_show_desk->setToolTip(
			mfdText("Dialog.ShowDesk.Tip", "The APC40 on screen, for the show. Also in OBS's Docks menu."));
		z1_btn_show_desk->setCursor(Qt::PointingHandCursor);
		z1_btn_show_desk->setStyleSheet(z1_btn_style);
		z1_lay->addWidget(z1_btn_show_desk);
		connect(z1_btn_show_desk, &QPushButton::clicked, this, []() { mfd_desk_dock_show(); });

		lay->addWidget(zone1);

		connect(z1_btn_reconnect_, &QPushButton::clicked, this, [this]() {
			/* Same as the Advanced tab's Reconnect: rescan the configured port with its layout rules. */
			mfd_engine_get_ui_snapshot(eng_, &snap_);
			apply_controller_selection(QString::fromUtf8(snap_.configured_device_name));
		});
		connect(z1_btn_change_ctrl_, &QPushButton::clicked, this, [this]() {
			populate_device_combos();
			z1_change_panel_->setVisible(!z1_change_panel_->isVisible());
		});
		connect(zone1_device_combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
			const QString chosen = zone1_device_combo_->currentData().toString();
			apply_controller_selection(chosen);
			z1_change_panel_->hide();
		});
		connect(zone1_profile_combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
			const auto chosen_prof =
				static_cast<enum mfd_controller_profile>(zone1_profile_combo_->currentData().toInt());
			z1_change_panel_->hide();
			if (chosen_prof == snap_.active_profile) {
				return; /* picking the layout already in use must not touch the strips */
			}
			mfd_engine_select_profile(eng_, chosen_prof, false);
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		});
		connect(z1_btn_show_ctrl_, &QPushButton::clicked, this, [this]() { open_controller_svg_view(); });

		/* --- Bank Switcher & Clean FX Bar --- */
		auto *bank_bar = new QFrame(strips_tab);
		bank_bar->setObjectName(QStringLiteral("mfdBankBar"));
		bank_bar->setStyleSheet(QStringLiteral(
			"QFrame#mfdBankBar { background-color: #0f172a; border: 1px solid #1e293b; border-radius: 7px; }"));
		auto *bb_lay = new QHBoxLayout(bank_bar);
		bb_lay->setContentsMargins(10, 6, 10, 6);
		bb_lay->setSpacing(8);

		const QString pill_style = QStringLiteral(
			"QPushButton { background-color: #1e293b; color: #cbd5e1; border: 1px solid #475569; "
			"border-radius: 5px; padding: 5px 14px; font-weight: bold; } "
			"QPushButton:hover { border-color: #93c5fd; color: #ffffff; } "
			"QPushButton:checked { background-color: #0284c7; color: #ffffff; border: 1px solid #38bdf8; }");

		btn_bank_faders_ = new QPushButton(mfdText("Dialog.Bank.Faders", "Faders"), bank_bar);
		btn_bank_faders_->setObjectName(QStringLiteral("mfdBtnBankFaders"));
		btn_bank_faders_->setCheckable(true);
		btn_bank_faders_->setChecked(true);
		btn_bank_faders_->setCursor(Qt::PointingHandCursor);
		btn_bank_faders_->setStyleSheet(pill_style);
		bb_lay->addWidget(btn_bank_faders_);

		btn_bank_fx_ = new QPushButton(mfdText("Dialog.Bank.Fx", "FX && Knobs (8)"), bank_bar);
		btn_bank_fx_->setObjectName(QStringLiteral("mfdBtnBankFx"));
		btn_bank_fx_->setCheckable(true);
		btn_bank_fx_->setChecked(false);
		btn_bank_fx_->setCursor(Qt::PointingHandCursor);
		btn_bank_fx_->setStyleSheet(pill_style);
		bb_lay->addWidget(btn_bank_fx_);

		bank_hint_label_ = new QLabel(bank_bar);
		bank_hint_label_->setObjectName(QStringLiteral("mfdBankHintLabel"));
		bank_hint_label_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px; padding-left: 6px;"));
		bb_lay->addWidget(bank_hint_label_, 1);

		btn_clean_all_fx_ = new QPushButton(mfdText("Dialog.CleanAllFx", "Clean / All FX Off"), bank_bar);
		btn_clean_all_fx_->setObjectName(QStringLiteral("mfdBtnCleanAllFx"));
		btn_clean_all_fx_->setCursor(Qt::PointingHandCursor);
		btn_clean_all_fx_->setToolTip(
			mfdText("Dialog.CleanAllFx.Tip",
				"Instantly return enabled filter controls in either bank to 0% (Clean). "
				"Audio, layer and T-bar controls stay unchanged."));
		bb_lay->addWidget(btn_clean_all_fx_);

		lay->addWidget(bank_bar);

		connect(btn_bank_faders_, &QPushButton::clicked, this, [this]() {
			showing_fx_bank_ = false;
			refresh_from_engine();
		});
		connect(btn_bank_fx_, &QPushButton::clicked, this, [this]() {
			showing_fx_bank_ = true;
			refresh_from_engine();
		});
		connect(btn_clean_all_fx_, &QPushButton::clicked, this, [this]() {
			mfd_engine_reset_fx(eng_, 0, nullptr, nullptr, nullptr, nullptr, 0U);
			refresh_from_engine();
		});

		/* --- Zone 2: Hardware Strips --- */
		auto *scroll = new QScrollArea(strips_tab);
		scroll->setObjectName(QStringLiteral("mfdStripsScrollArea"));
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

		auto *strips_container = new QWidget(scroll);
		strips_container->setObjectName(QStringLiteral("mfdStripsContainer"));
		strips_container->setStyleSheet(QStringLiteral("background: transparent;"));
		auto *strips_lay = new QHBoxLayout(strips_container);
		strips_lay->setContentsMargins(0, 0, 0, 0);
		strips_lay->setSpacing(6);

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			auto *card = new QFrame(strips_container);
			card->setObjectName(QStringLiteral("mfdStrip_%1").arg(i));
			card->setMinimumWidth(i == 0 ? 150 : 118);
			auto *c_lay = new QVBoxLayout(card);
			c_lay->setContentsMargins(7, 8, 7, 8);
			c_lay->setSpacing(6);

			auto *lbl_title = new QLabel(card);
			lbl_title->setObjectName(QStringLiteral("mfdStripTitle_%1").arg(i));
			lbl_title->setAlignment(Qt::AlignCenter);
			lbl_title->setWordWrap(true);
			c_lay->addWidget(lbl_title);

			auto *bar_w = new MfdStripBarWidget(i == 0, card);
			bar_w->setObjectName(QStringLiteral("mfdStripBar_%1").arg(i));
			c_lay->addWidget(bar_w);

			auto *lbl_state = new QLabel(card);
			lbl_state->setObjectName(QStringLiteral("mfdStripState_%1").arg(i));
			lbl_state->setAlignment(Qt::AlignCenter);
			lbl_state->setWordWrap(true);
			lbl_state->setMinimumHeight(32);
			c_lay->addWidget(lbl_state);

			auto *btn_finish = new QPushButton(mfdText("Dialog.FinishNow", "Finish now"), card);
			btn_finish->setObjectName(QStringLiteral("mfdStripFinishBtn_%1").arg(i));
			btn_finish->setCursor(Qt::PointingHandCursor);
			btn_finish->setStyleSheet(QStringLiteral(
				"QPushButton { background-color: #0284c7; color: #ffffff; font-size: 11px; font-weight: bold; "
				"border-radius: 4px; padding: 3px 6px; border: 1px solid #38bdf8; }"));
			btn_finish->hide();
			c_lay->addWidget(btn_finish);
			connect(btn_finish, &QPushButton::clicked, this, [this, i]() {
				mfd_engine_finish_tbar_throw(eng_, i);
				refresh_from_engine();
			});

			auto *btn_assign = new QPushButton(card);
			btn_assign->setObjectName(QStringLiteral("mfdStripAssign_%1").arg(i));
			btn_assign->setCursor(Qt::PointingHandCursor);
			btn_assign->setMinimumHeight(64);
			auto *assign_lay = new QVBoxLayout(btn_assign);
			assign_lay->setContentsMargins(6, 5, 6, 5);
			auto *lbl_assign = new QLabel(btn_assign);
			lbl_assign->setObjectName(QStringLiteral("mfdStripAssignLabel_%1").arg(i));
			lbl_assign->setAttribute(Qt::WA_TransparentForMouseEvents, true);
			lbl_assign->setWordWrap(true);
			lbl_assign->setAlignment(Qt::AlignCenter);
			assign_lay->addWidget(lbl_assign);
			c_lay->addWidget(btn_assign, 1);
			connect(btn_assign, &QPushButton::clicked, this, [this, i]() { open_setup_for_slot(i); });

			auto *lbl_status = new QLabel(card);
			lbl_status->setObjectName(QStringLiteral("mfdStripStatus_%1").arg(i));
			lbl_status->setWordWrap(true);
			lbl_status->setAlignment(Qt::AlignCenter);
			lbl_status->hide();
			c_lay->addWidget(lbl_status);

			auto *btn_action = new QPushButton(mfdText("Strip.SetUp", "Set up"), card);
			btn_action->setObjectName(QStringLiteral("mfdStripActionBtn_%1").arg(i));
			btn_action->setCursor(Qt::PointingHandCursor);
			c_lay->addWidget(btn_action);
			connect(btn_action, &QPushButton::clicked, this, [this, i]() {
				if (strip_bottom_actions_[i] == MFD_UI_ACTION_TURN_ON_STUDIO_MODE) {
					mfd_engine_request_studio_mode(eng_, true);
					refresh_from_engine();
				} else {
					open_setup_for_slot(i);
				}
			});

			strip_cards_[i] = card;
			strip_titles_[i] = lbl_title;
			strip_bars_[i] = bar_w;
			strip_states_[i] = lbl_state;
			strip_finish_btns_[i] = btn_finish;
			strip_assign_btns_[i] = btn_assign;
			strip_assign_labels_[i] = lbl_assign;
			strip_status_labels_[i] = lbl_status;
			strip_action_btns_[i] = btn_action;
			strips_lay->addWidget(card, i == 0 ? 5 : 4);
		}

		scroll->setWidget(strips_container);
		lay->addWidget(scroll, 1);

		/* --- Zone 3: Status Line --- */
		auto *zone3 = new QFrame(strips_tab);
		zone3->setObjectName(QStringLiteral("mfdZone3Bar"));
		zone3->setStyleSheet(QStringLiteral(
			"QFrame#mfdZone3Bar { background-color: #111827; border: 1px solid #1e293b; border-radius: 7px; }"));
		auto *z3_lay = new QHBoxLayout(zone3);
		z3_lay->setContentsMargins(12, 7, 12, 7);
		z3_lay->setSpacing(10);

		z3_status_label_ = new QLabel(zone3);
		z3_status_label_->setObjectName(QStringLiteral("mfdZone3StatusLabel"));
		z3_status_label_->setStyleSheet(QStringLiteral("color: #cbd5e1; font-size: 13px; font-weight: bold;"));
		z3_lay->addWidget(z3_status_label_, 1);

		auto *btn_open_adv = new QPushButton(mfdText("Dialog.Advanced", "Advanced"), zone3);
		btn_open_adv->setObjectName(QStringLiteral("mfdBtnOpenAdvanced"));
		btn_open_adv->setCursor(Qt::PointingHandCursor);
		btn_open_adv->setStyleSheet(z1_btn_style);
		connect(btn_open_adv, &QPushButton::clicked, this, [this]() { main_tabs_->setCurrentIndex(1); });
		z3_lay->addWidget(btn_open_adv);

		auto *btn_stream_deck =
			new QPushButton(mfdText("Dialog.StreamDeck.Button", "Fadester for OBS..."), zone3);
		btn_stream_deck->setObjectName(QStringLiteral("mfdBtnStreamDeckCompanion"));
		btn_stream_deck->setProperty("destinationUrl", stream_deck_companion_url());
		btn_stream_deck->setToolTip(mfdText("Dialog.StreamDeck.ButtonTip",
						    "Learn about the optional Fadester for OBS Stream Deck plugin."));
		btn_stream_deck->setCursor(Qt::PointingHandCursor);
		btn_stream_deck->setStyleSheet(z1_btn_style);
		connect(btn_stream_deck, &QPushButton::clicked, this, []() { open_stream_deck_companion_url(); });
		z3_lay->addWidget(btn_stream_deck);

		lay->addWidget(zone3);
		main_tabs_->addTab(strips_tab, mfdText("Dialog.Tab.Strips", "Strips"));
	}

	void build_advanced_tab()
	{
		auto *adv_tab = new QWidget(main_tabs_);
		adv_tab->setObjectName(QStringLiteral("mfdAdvancedTab"));
		auto *root = new QVBoxLayout(adv_tab);
		root->setContentsMargins(10, 10, 10, 10);
		root->setSpacing(8);

		banner_label_ = new QLabel(adv_tab);
		banner_label_->setObjectName(QStringLiteral("mfdAdvErrorBanner"));
		banner_label_->setWordWrap(true);
		banner_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #5a1d1d; color: #ffd6d6; padding: 8px 10px; "
				       "border: 1px solid #d9534f; border-radius: 4px; font-weight: bold; }"));
		banner_label_->hide();
		root->addWidget(banner_label_);

		auto *top_bar = new QHBoxLayout();
		auto *lbl_midi_in = new QLabel(mfdText("Dialog.MidiIn", "MIDI In:"), adv_tab);
		lbl_midi_in->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		top_bar->addWidget(lbl_midi_in);

		device_combo_ = new QComboBox(adv_tab);
		device_combo_->setObjectName(QStringLiteral("mfdAdvDeviceCombo"));
		device_combo_->setMinimumWidth(220);
		top_bar->addWidget(device_combo_);

		conn_status_label_ = new QLabel(mfdText("Dialog.Checking", "Checking..."), adv_tab);
		conn_status_label_->setObjectName(QStringLiteral("mfdAdvConnLabel"));
		top_bar->addWidget(conn_status_label_);

		auto *btn_reconnect = new QPushButton(mfdText("Dialog.Reconnect", "Reconnect"), adv_tab);
		btn_reconnect->setObjectName(QStringLiteral("mfdAdvReconnectBtn"));
		top_bar->addWidget(btn_reconnect);

		top_bar->addSpacing(8);
		auto *lbl_prof = new QLabel(mfdText("Dialog.Layout", "Layout:"), adv_tab);
		lbl_prof->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		top_bar->addWidget(lbl_prof);

		profile_combo_ = new QComboBox(adv_tab);
		profile_combo_->setObjectName(QStringLiteral("mfdAdvProfileCombo"));
		populate_profile_combo(profile_combo_);
		top_bar->addWidget(profile_combo_);

		btn_defaults_ = new QPushButton(
			mfdText("Dialog.LoadApc40Defaults", "Reset all strips to the APC40 mkII layout"), adv_tab);
		btn_defaults_->setObjectName(QStringLiteral("mfdAdvDefaultsBtn"));
		top_bar->addWidget(btn_defaults_);
		top_bar->addStretch(1);
		root->addLayout(top_bar);

		adv_beta_note_ = new QLabel(adv_tab);
		adv_beta_note_->setObjectName(QStringLiteral("mfdAdvBetaNote"));
		adv_beta_note_->setWordWrap(true);
		adv_beta_note_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #3f2b05; color: #fde68a; border: 1px solid #d97706; "
				       "border-radius: 5px; padding: 6px 10px; font-size: 12px; font-weight: bold; }"));
		adv_beta_note_->hide();
		root->addWidget(adv_beta_note_);

		auto *telemetry_bar = new QHBoxLayout();
		studio_status_label_ = new QLabel(mfdText("Dialog.StudioModeOffLabel", "Studio Mode: Off"), adv_tab);
		studio_status_label_->setObjectName(QStringLiteral("mfdAdvStudioLabel"));
		telemetry_bar->addWidget(studio_status_label_);

		telemetry_bar->addSpacing(16);
		auto *lbl_tau = new QLabel(mfdText("Dialog.Smoothing", "Smoothing:"), adv_tab);
		lbl_tau->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		telemetry_bar->addWidget(lbl_tau);

		tau_spin_ = new QDoubleSpinBox(adv_tab);
		tau_spin_->setObjectName(QStringLiteral("mfdAdvTauSpin"));
		tau_spin_->setRange(0.0, 250.0);
		tau_spin_->setSingleStep(5.0);
		tau_spin_->setDecimals(1);
		tau_spin_->setSuffix(mfdText("Dialog.Suffix.Ms", " ms"));
		telemetry_bar->addWidget(tau_spin_);

		telemetry_bar->addSpacing(12);
		flip_check_ =
			new QCheckBox(mfdText("Dialog.TbarBothDirections", "T-bar works in both directions"), adv_tab);
		flip_check_->setObjectName(QStringLiteral("mfdAdvFlipCheck"));
		flip_check_->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		telemetry_bar->addWidget(flip_check_);
		telemetry_bar->addStretch(1);

		last_midi_label_ = new QLabel(mfdText("LastMidi.None", "No MIDI received yet"), adv_tab);
		last_midi_label_->setObjectName(QStringLiteral("mfdAdvLastMidiLabel"));
		last_midi_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #1f2937; color: #93c5fd; padding: 4px 10px; "
				       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		telemetry_bar->addWidget(last_midi_label_);
		root->addLayout(telemetry_bar);

		auto *applies_note = new QLabel(mfdText("Dialog.Advanced.AppliesAtOnce",
							"Changes on this page apply at once and are saved."),
						adv_tab);
		applies_note->setObjectName(QStringLiteral("mfdAdvAppliesAtOnce"));
		applies_note->setWordWrap(true);
		applies_note->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px;"));
		root->addWidget(applies_note);

		/* APC40 mkII VJ pads (core/vj-pads.h): on/off, the fade time and what each pad row does. */
		auto *vj_bar = new QHBoxLayout();
		vj_check_ = new QCheckBox(mfdText("Dialog.VjPads", "Use the APC40 pads"), adv_tab);
		vj_check_->setObjectName(QStringLiteral("mfdAdvVjCheck"));
		vj_check_->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		vj_bar->addWidget(vj_check_);
		vj_bar->addSpacing(12);
		auto *lbl_vj_fade = new QLabel(mfdText("Dialog.VjFadeTime", "Source fade time:"), adv_tab);
		lbl_vj_fade->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		vj_bar->addWidget(lbl_vj_fade);
		vj_fade_spin_ = new QSpinBox(adv_tab);
		vj_fade_spin_->setObjectName(QStringLiteral("mfdAdvVjFadeSpin"));
		vj_fade_spin_->setRange(50, 20000);
		vj_fade_spin_->setSingleStep(100);
		vj_fade_spin_->setSuffix(mfdText("Dialog.Suffix.Ms", " ms"));
		vj_bar->addWidget(vj_fade_spin_);
		vj_bar->addStretch(1);
		root->addLayout(vj_bar);
		/* the dock's video speed rows act only with Fadester Companion and VJster running: off unless asked for */
		desk_video_check_ = new QCheckBox(
			mfdText("Dialog.DeskVideo",
				"Show video speed controls on the dock (needs Fadester Companion and VJster)"),
			adv_tab);
		desk_video_check_->setObjectName(QStringLiteral("mfdAdvDeskVideoCheck"));
		desk_video_check_->setStyleSheet(QStringLiteral("color: #e2e8f0;"));
		root->addWidget(desk_video_check_);
		/* what each pad row does is one click away on the dock itself (its "?" button, U1-09): three paragraphs here left the
		 * strip table three rows */
		auto *vj_fill_bar = new QHBoxLayout();
		auto *btn_vj_fill =
			new QPushButton(mfdText("Dialog.VjFill", "Fill empty faders from the scene on air"), adv_tab);
		btn_vj_fill->setObjectName(QStringLiteral("mfdAdvVjFill"));
		vj_fill_bar->addWidget(btn_vj_fill);
		vj_fill_result_ = new QLabel(adv_tab);
		vj_fill_result_->setObjectName(QStringLiteral("mfdAdvVjFillResult"));
		vj_fill_result_->setWordWrap(true);
		vj_fill_result_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px;"));
		vj_fill_bar->addWidget(vj_fill_result_, 1);
		root->addLayout(vj_fill_bar);
		connect(btn_vj_fill, &QPushButton::clicked, this, [this]() { fill_faders_from_scene(); });
		/* A map of the 18 strips with one way in (U1-17): what a strip does is its own sentence, and Edit... opens Set up on
		 * it. On and Reverse stay switches here: each is one yes-or-no that picks no job, source or range, and this page
		 * says above the table that its changes apply at once. */
		table_ = new QTableWidget(MFD_MAX_FADER_SLOTS, kAdvColumns, adv_tab);
		table_->setObjectName(QStringLiteral("mfdAdvancedTable"));
		table_->setHorizontalHeaderLabels({
			mfdText("Dialog.Column.On", "On"),
			mfdText("Dialog.Column.Strip", "Strip"),
			mfdText("Dialog.Column.MidiControl", "MIDI Control"),
			mfdText("Dialog.Column.Reverse", "Reverse"),
			mfdText("Dialog.Column.WhatItDoes", "What it does"),
			mfdText("Dialog.Column.Output", "Output"),
			QString(),
		});
		table_->horizontalHeader()->setStretchLastSection(false);
		table_->horizontalHeader()->setSectionResizeMode(kAdvColJob, QHeaderView::Stretch);
		table_->verticalHeader()->setVisible(false);
		table_->verticalHeader()->setDefaultSectionSize(36);
		table_->setAlternatingRowColors(true);
		table_->setSelectionMode(QAbstractItemView::NoSelection);
		table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		root->addWidget(table_, 1);

		auto *footer = new QHBoxLayout();
		auto *btn_back_strips = new QPushButton(mfdText("Dialog.BackToStrips", "Back to Strips"), adv_tab);
		btn_back_strips->setObjectName(QStringLiteral("mfdBtnBackToStrips"));
		footer->addWidget(btn_back_strips);
		connect(btn_back_strips, &QPushButton::clicked, this, [this]() { main_tabs_->setCurrentIndex(0); });
		footer->addStretch(1);

		auto *btn_import = new QPushButton(mfdText("Dialog.Import", "Import..."), adv_tab);
		btn_import->setObjectName(QStringLiteral("mfdBtnImport"));
		footer->addWidget(btn_import);

		auto *btn_export = new QPushButton(mfdText("Dialog.Export", "Export..."), adv_tab);
		btn_export->setObjectName(QStringLiteral("mfdBtnExport"));
		footer->addWidget(btn_export);

		auto *btn_close = new QPushButton(mfdText("Dialog.Close", "Close"), adv_tab);
		btn_close->setObjectName(QStringLiteral("mfdBtnClose"));
		footer->addWidget(btn_close);
		root->addLayout(footer);

		/* for support: which build this is (the title bar shows the version only) */
		auto *about =
			new QLabel(mfdFill(mfdText("Dialog.About", "Midi Fadester %1 (commit %2, built %3)"),
					   {QString::fromUtf8(PLUGIN_VERSION), QString::fromUtf8(PLUGIN_GIT_COMMIT),
					    QString::fromUtf8(PLUGIN_BUILT_AT)}),
				   adv_tab);
		about->setObjectName(QStringLiteral("mfdAdvAbout"));
		about->setTextInteractionFlags(Qt::TextSelectableByMouse);
		about->setStyleSheet(QStringLiteral("QLabel#mfdAdvAbout { color: #64748b; font-size: 11px; }"));
		root->addWidget(about);

		connect(btn_import, &QPushButton::clicked, this, [this]() {
			const QString path = QFileDialog::getOpenFileName(
				this,
				mfdWindowTitle(mfdText("Dialog.Import.Title", "Import Midi Fadester Configuration")),
				QString(), mfdText("Dialog.FileFilter.Json", "JSON Files (*.json);;All Files (*)"));
			if (path.isEmpty()) {
				return;
			}
			import_config(path);
		});

		connect(btn_export, &QPushButton::clicked, this, [this]() {
			const QString path = QFileDialog::getSaveFileName(
				this,
				mfdWindowTitle(mfdText("Dialog.Export.Title", "Export Midi Fadester Configuration")),
				QStringLiteral("midi-fadester-config.json"),
				mfdText("Dialog.FileFilter.Json", "JSON Files (*.json);;All Files (*)"));
			if (path.isEmpty()) {
				return;
			}
			if (!mfd_engine_export_config_file(eng_, path.toUtf8().constData())) {
				QMessageBox::warning(
					this,
					mfdWindowTitle(
						mfdText("Dialog.Export.Title", "Export Midi Fadester Configuration")),
					mfdFill(mfdText("Dialog.Export.Failed",
							"Could not write %1. Choose another folder and try again."),
						{QDir::toNativeSeparators(path)}));
			}
		});

		connect(device_combo_, QOverload<int>::of(&QComboBox::activated), this,
			[this](int) { apply_controller_selection(device_combo_->currentData().toString()); });
		connect(btn_reconnect, &QPushButton::clicked, this, [this]() {
			/* Rescan the configured port, never the list's entry: a named port keeps its layout and learned
			 * bindings; Auto-Detect follows a newly plugged known desk. */
			mfd_engine_get_ui_snapshot(eng_, &snap_);
			apply_controller_selection(QString::fromUtf8(snap_.configured_device_name));
		});

		connect(profile_combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
			const auto chosen_prof =
				static_cast<enum mfd_controller_profile>(profile_combo_->currentData().toInt());
			if (chosen_prof == snap_.active_profile) {
				return; /* picking the layout already in use must not touch the strips */
			}
			mfd_engine_select_profile(eng_, chosen_prof, false);
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		});

		connect(btn_defaults_, &QPushButton::clicked, this, [this]() {
			const auto chosen_prof = profile_combo_ ? static_cast<enum mfd_controller_profile>(
									  profile_combo_->currentData().toInt())
								: snap_.active_profile;
			/* one click here removes every job on every strip, in the middle of a show if the dialog is open:
			 * it asks first (U1-18) */
			const struct mfd_controller_profile_def *chosen_def = mfd_profile_get_def(chosen_prof);
			const bool generic = !chosen_def || chosen_def->id == MFD_PROFILE_GENERIC;
			if (!confirm(btn_defaults_->text(),
				     generic ? mfdText("Dialog.Clear.Confirm",
						       "Clear all 18 strips? The jobs you gave them are removed.")
					     : mfdFill(mfdText("Dialog.Reset.Confirm",
							       "Reset all 18 strips to the %1 layout? The jobs you gave "
							       "them are removed."),
						       {QString::fromUtf8(mfd_profile_ui_short_name(chosen_def))}),
				     generic ? mfdText("Dialog.Clear.Yes", "Clear")
					     : mfdText("Dialog.Reset.Yes", "Reset"))) {
				return;
			}
			mfd_engine_select_profile(eng_, chosen_prof, true);
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		});

		/* F-06: each step applies at once, but the settings file is written once, when the edit is done (focus
		 * leaves the box or Enter). Holding the arrow no longer writes the file many times a second. */
		connect(tau_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_smooth_tau_ms(eng_, static_cast<float>(val));
			}
		});
		connect(tau_spin_, &QDoubleSpinBox::editingFinished, this, [this]() {
			if (!rebuilding_adv_table_) {
				mfd_engine_save_global_config(eng_);
			}
		});

		connect(flip_check_, &QCheckBox::toggled, this, [this](bool checked) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_tbar_bidirectional_flip(eng_, checked);
				mfd_engine_save_global_config(eng_);
			}
		});

		connect(desk_video_check_, &QCheckBox::toggled, this, [this](bool checked) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_desk_video_controls(eng_, checked);
				mfd_engine_save_global_config(eng_);
				mfd_desk_dock_refresh();
			}
		});
		connect(vj_check_, &QCheckBox::toggled, this, [this](bool checked) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_vj_buttons(eng_, checked, vj_fade_spin_->value());
				mfd_engine_save_global_config(eng_);
			}
		});
		/* the engine asks for the save itself (once, at the next flush): no second write here (F-06) */
		connect(vj_fade_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int ms) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_vj_buttons(eng_, vj_check_->isChecked(), ms);
			}
		});

		connect(btn_close, &QPushButton::clicked, this, &QDialog::accept);

		main_tabs_->addTab(adv_tab, mfdText("Dialog.Advanced", "Advanced"));
	}

	static void populate_profile_combo(QComboBox *cb)
	{
		if (!cb) {
			return;
		}
		cb->blockSignals(true);
		cb->clear();
		const size_t n = mfd_profile_count();
		for (size_t i = 0U; i < n; ++i) {
			const struct mfd_controller_profile_def *def = mfd_profile_get_by_index(i);
			if (!def) {
				continue;
			}
			const QString display = QString::fromUtf8(mfd_profile_ui_display_name(def));
			const QString name =
				(def->tier == MFD_PROFILE_TIER_BETA_UNTESTED)
					? mfdFill(mfdText("Dialog.Profile.BetaName", "%1 (Beta \xE2\x80\x94 untested)"),
						  {display})
					: display;
			cb->addItem(name, static_cast<int>(def->id));
		}
		cb->blockSignals(false);
	}

	void apply_controller_selection(const QString &chosen_port)
	{
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		const QByteArray prev_port(snap_.configured_device_name);
		const enum mfd_controller_profile current = snap_.active_profile;
		const QByteArray chosen = chosen_port.toUtf8();
		mfd_engine_reconnect_midi(eng_, chosen.constData());
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		enum mfd_controller_profile next = current;
		if (mfd_profile_for_port_change(prev_port.constData(), chosen.constData(), snap_.midi_connected,
						snap_.opened_device_name, current, &next)) {
			mfd_engine_select_profile(eng_, next, false);
		} else {
			mfd_engine_save_global_config(eng_);
			mfd_engine_notify_state_changed(eng_);
		}
		populate_device_combos();
		last_adv_topology_rev_ = ~0ULL;
		refresh_from_engine();
	}

	void populate_device_combos()
	{
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		struct mfd_midi_device_info devs[MFD_MAX_MIDI_DEVICES];
		const size_t count = mfd_winmm_enumerate(devs, MFD_MAX_MIDI_DEVICES);
		const QString cfg_name = QString::fromUtf8(snap_.configured_device_name);

		auto fill_one = [&](QComboBox *cb) {
			if (!cb) {
				return;
			}
			cb->blockSignals(true);
			cb->clear();
			cb->addItem(mfdText("Dialog.Device.AutoDetect", "Automatic"), QStringLiteral(""));
			int select_idx = 0;
			for (size_t i = 0U; i < count; ++i) {
				const QString name = QString::fromUtf8(devs[i].name);
				QString label = name;
				if (devs[i].detected_profile != MFD_PROFILE_GENERIC) {
					const struct mfd_controller_profile_def *pdef =
						mfd_profile_get_def(devs[i].detected_profile);
					if (pdef) {
						const QString short_name =
							QString::fromUtf8(mfd_profile_ui_short_name(pdef));
						label = (pdef->tier == MFD_PROFILE_TIER_BETA_UNTESTED)
								? mfdFill(mfdText("Dialog.Device.BetaLabel",
										  "%1 [%2 \xE2\x80\x94 Beta]"),
									  {name, short_name})
								: mfdFill(mfdText("Dialog.Device.Label", "%1 [%2]"),
									  {name, short_name});
					}
				}
				cb->addItem(label, name);
				if (name == cfg_name) {
					select_idx = static_cast<int>(i) + 1;
				}
			}
			if (!cfg_name.isEmpty() && select_idx == 0) {
				/* the saved port is unplugged: the engine waits for it, so the list says so instead of
				 * showing Auto-Detect (F-11) */
				cb->addItem(mfdFill(mfdText("Dialog.Source.Missing", "%1 (missing)"), {cfg_name}),
					    cfg_name);
				select_idx = cb->count() - 1;
			}
			cb->setCurrentIndex(select_idx);
			cb->blockSignals(false);
		};

		fill_one(zone1_device_combo_);
		fill_one(device_combo_);
	}

	void open_setup_for_slot(int slot_index)
	{
		if (!setup_sheet_) {
			setup_sheet_ = new MfdSetupSheetDialog(eng_, this);
		}
		if (testAttribute(Qt::WA_DontShowOnScreen)) {
			setup_sheet_->setAttribute(Qt::WA_DontShowOnScreen, true);
		}
		setup_sheet_->open_for_slot(slot_index);
		refresh_from_engine();
	}

	void open_controller_svg_view()
	{
		if (!controller_view_dlg_) {
			controller_view_dlg_ = new MfdControllerViewDialog(this);
		}
		if (testAttribute(Qt::WA_DontShowOnScreen)) {
			controller_view_dlg_->setAttribute(Qt::WA_DontShowOnScreen, true);
		}
		controller_view_dlg_->refresh_from_snapshot(snap_);
		controller_view_dlg_->show();
		controller_view_dlg_->raise();
	}

	/* Import replaces every strip's job and control: it asks first (U1-18). Public to the test through the button's
	 * path only; the file dialog is the one part a test cannot drive. */
	void import_config(const QString &path)
	{
		if (!confirm(mfdText("Dialog.Import.Title", "Import Midi Fadester Configuration"),
			     mfdFill(mfdText("Dialog.Import.Confirm",
					     "Replace the whole set-up with %1? Every strip's job and control is replaced. "
					     "Export first to keep the current one."),
				     {QDir::toNativeSeparators(path)}),
			     mfdText("Dialog.Import.Yes", "Import"))) {
			return;
		}
		if (mfd_engine_import_config_file_and_reopen(eng_, path.toUtf8().constData())) {
			mfd_engine_save_global_config(eng_);
			mfd_engine_notify_state_changed(eng_);
			populate_device_combos();
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		} else {
			QMessageBox::warning(
				this,
				mfdWindowTitle(mfdText("Dialog.Import.Title", "Import Midi Fadester Configuration")),
				mfdFill(mfdText("Dialog.Import.Failed",
						"Could not read %1. It is not a Midi Fadester settings "
						"file, or it is damaged. Nothing was changed."),
					{QDir::toNativeSeparators(path)}));
		}
	}

	bool confirm(const QString &window, const QString &question, const QString &do_it)
	{
		return mfdConfirm(this, window, question, do_it);
	}

	/* Shown while the open desk's name matches no ready layout and a layout for another desk is in use (the
	 * Generic layout is the one for such a desk), until OK. */
	void refresh_no_layout_banner()
	{
		const QString opened = QString::fromUtf8(snap_.opened_device_name);
		const bool show = snap_.midi_connected && !opened.isEmpty() && !snap_.first_run_offer_pending &&
				  snap_.active_profile != MFD_PROFILE_GENERIC &&
				  mfd_profile_match_device_name(snap_.opened_device_name) == MFD_PROFILE_GENERIC &&
				  opened != no_layout_dismissed_for_;
		if (show) {
			const QString text =
				mfdFill(mfdText("Dialog.NoLayout.Text",
						"No ready layout for %1. Click Set up under a strip and move the "
						"control you want."),
					{opened});
			if (no_layout_text_->text() != text) {
				no_layout_text_->setText(text);
			}
		}
		no_layout_banner_->setVisible(show);
	}

	void refresh_strips_tab()
	{
		struct mfd_ui_controller_presentation ctrl;
		mfd_ui_format_controller_status(snap_.midi_connected, snap_.configured_device_name,
						snap_.opened_device_name, snap_.midi_error_kind, &ctrl);

		z1_name_label_->setText(mfdFill(mfdText("Dialog.Controller.Name", "Controller: %1"),
						{QString::fromUtf8(ctrl.display_name)}));
		const QString dot_css = ctrl.dot_green ? QStringLiteral("color: #22c55e; font-size: 14px;")
						       : QStringLiteral("color: #ef4444; font-size: 14px;");
		if (z1_dot_label_->styleSheet() != dot_css) { /* refreshed up to 30 Hz while the desk dock is shown */
			z1_dot_label_->setStyleSheet(dot_css);
		}
		z1_status_label_->setText(QString::fromUtf8(ctrl.status_sentence));
		const QString status_css = ctrl.dot_green ? QStringLiteral("color: #6ee7b7; font-weight: bold;")
							  : QStringLiteral("color: #fca5a5; font-weight: bold;");
		if (z1_status_label_->styleSheet() != status_css) {
			z1_status_label_->setStyleSheet(status_css);
		}
		z1_btn_reconnect_->setVisible(ctrl.show_reconnect);

		const struct mfd_controller_profile_def *pdef = mfd_profile_get_def(snap_.active_profile);
		if (zone1_profile_combo_ &&
		    !(zone1_profile_combo_->view() && zone1_profile_combo_->view()->isVisible())) {
			const int pidx = zone1_profile_combo_->findData(static_cast<int>(snap_.active_profile));
			if (pidx >= 0) {
				zone1_profile_combo_->blockSignals(true);
				zone1_profile_combo_->setCurrentIndex(pidx);
				zone1_profile_combo_->blockSignals(false);
			}
		}
		if (z1_beta_badge_) {
			if (pdef && pdef->tier == MFD_PROFILE_TIER_BETA_UNTESTED) {
				z1_beta_badge_->setText(
					mfdFill(mfdText("Dialog.Controller.BetaBadge", "BETA: %1 (untested spec)"),
						{QString::fromUtf8(mfd_profile_ui_short_name(pdef))}));
				z1_beta_badge_->setToolTip(QString::fromUtf8(mfd_profile_ui_spec_note(pdef)));
				z1_beta_badge_->show();
			} else {
				z1_beta_badge_->hide();
			}
		}

		if (btn_bank_faders_ && btn_bank_fx_) {
			btn_bank_faders_->setChecked(!showing_fx_bank_);
			btn_bank_fx_->setChecked(showing_fx_bank_);
		}
		if (bank_hint_label_) {
			const QString hint_txt =
				showing_fx_bank_
					? mfdText("Dialog.Bank.FxHint",
						  "FX strips for filters, shader controls, knobs and Stream Deck + "
						  "dials.")
					: mfdText("Dialog.Bank.FadersHint",
						  "Channel faders, master, and A/B crossfader.");
			if (bank_hint_label_->text() != hint_txt) {
				bank_hint_label_->setText(hint_txt);
			}
		}
		if (btn_clean_all_fx_) {
			int eligible_fx = 0;
			int active_fx = 0;
			for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
				const struct mfd_fader_slot &fs = snap_.slots[i];
				if (mfd_slot_is_reset_fx_eligible(&fs)) {
					++eligible_fx;
					if (fs.last_applied_val > MFD_FILTER_BYPASS_ZERO_THRESHOLD ||
					    fs.auto_fade_active) {
						++active_fx;
					}
				}
			}
			const QString clean_style =
				(active_fx > 0)
					? QStringLiteral(
						  "QPushButton { background-color: #b45309; color: #ffffff; border: 1px solid #f59e0b; "
						  "border-radius: 5px; padding: 5px 14px; font-weight: bold; } "
						  "QPushButton:hover { background-color: #d97706; }")
					: QStringLiteral(
						  "QPushButton { background-color: #164e63; color: #cffafe; border: 1px solid #0891b2; "
						  "border-radius: 5px; padding: 5px 14px; font-weight: bold; } "
						  "QPushButton:hover { border-color: #22d3ee; color: #ffffff; }");
			if (btn_clean_all_fx_->styleSheet() != clean_style) {
				btn_clean_all_fx_->setStyleSheet(clean_style);
			}
			btn_clean_all_fx_->setEnabled(eligible_fx > 0); /* nothing to clean: it says so by being grey */
		}

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			const struct mfd_fader_slot &s = snap_.slots[i];
			bool show_strip = false;
			if (showing_fx_bank_) {
				show_strip = mfd_slot_is_fx_bank(i);
			} else {
				show_strip = !mfd_slot_is_fx_bank(i) &&
					     (mfd_profile_has_physical_slot(snap_.active_profile, i) ||
					      (s.enabled && (s.addr.type != MFD_MIDI_NONE ||
							     s.target_type != MFD_TARGET_UNASSIGNED)));
			}
			strip_cards_[i]->setVisible(show_strip);

			struct mfd_ui_strip_presentation sp;
			mfd_ui_strip_presentation_from_slot(i, s, &sp);
			strip_bottom_actions_[i] = sp.bottom_action;

			QString border_hex = QStringLiteral("#1e293b");
			QString card_bg = sp.dimmed ? QStringLiteral("#0d1424") : QStringLiteral("#111c33");
			if (!sp.dimmed) {
				if (sp.severity == MFD_UI_SEVERITY_RED) {
					border_hex = QStringLiteral("#ef4444");
				} else if (sp.severity == MFD_UI_SEVERITY_AMBER) {
					border_hex = QStringLiteral("#d97706");
				} else {
					border_hex = QStringLiteral("#334155");
				}
			}
			const QString card_style =
				QStringLiteral("QFrame#mfdStrip_%1 { background-color: %2; border: 1px solid %3; "
					       "border-radius: 8px; }")
					.arg(i)
					.arg(card_bg, border_hex);
			if (strip_cards_[i]->styleSheet() != card_style) {
				strip_cards_[i]->setStyleSheet(card_style);
			}

			const QString title_q = QString::fromUtf8(sp.title);
			if (strip_titles_[i]->text() != title_q) {
				strip_titles_[i]->setText(title_q);
			}
			const QString title_style =
				sp.dimmed ? QStringLiteral("color: #94a3b8; font-size: 13px; font-weight: bold;")
					  : QStringLiteral("color: #f8fafc; font-size: 13px; font-weight: bold;");
			if (strip_titles_[i]->styleSheet() != title_style) {
				strip_titles_[i]->setStyleSheet(title_style);
			}

			const bool at_end = sp.at_end;
			const bool is_fx_style = mfd_slot_is_fx_bank(i) || (s.target_type == MFD_TARGET_FILTER_PARAM);
			strip_bars_[i]->set_state(sp.bar_percent, sp.dimmed, sp.severity, at_end, sp.show_pickup_marker,
						  sp.pickup_target_percent, QString::fromUtf8(sp.arrow_glyph),
						  is_fx_style);

			const QString state_q = QString::fromUtf8(sp.state_readout);
			if (strip_states_[i]->text() != state_q) {
				strip_states_[i]->setText(state_q);
			}
			const QString state_style =
				at_end ? QStringLiteral("color: #34d399; font-size: 12px; font-weight: bold;")
				       : QStringLiteral("color: #7dd3fc; font-size: 12px; font-weight: bold;");
			if (strip_states_[i]->styleSheet() != state_style) {
				strip_states_[i]->setStyleSheet(state_style);
			}

			strip_finish_btns_[i]->setVisible(sp.show_finish_now);

			const QString assign_q = QString::fromUtf8(sp.assignment_sentence);
			if (strip_assign_labels_[i]->text() != assign_q) {
				strip_assign_labels_[i]->setText(assign_q);
				strip_assign_btns_[i]->setToolTip(mfdPlainTip(assign_q));
			}
			const QString assign_lbl_style =
				sp.dimmed
					? QStringLiteral("color: #64748b; font-size: 12px; background: transparent;")
					: QStringLiteral(
						  "color: #e2e8f0; font-size: 12px; font-weight: bold; background: transparent;");
			if (strip_assign_labels_[i]->styleSheet() != assign_lbl_style) {
				strip_assign_labels_[i]->setStyleSheet(assign_lbl_style);
			}
			const QString assign_btn_style = QStringLiteral(
				"QPushButton { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 6px; } "
				"QPushButton:hover { border-color: #38bdf8; background-color: #132038; }");
			if (strip_assign_btns_[i]->styleSheet() != assign_btn_style) {
				strip_assign_btns_[i]->setStyleSheet(assign_btn_style);
			}

			const QString status_q = QString::fromUtf8(sp.status_sentence);
			if (status_q.isEmpty()) {
				strip_status_labels_[i]->clear();
				strip_status_labels_[i]->hide();
			} else {
				if (strip_status_labels_[i]->text() != status_q) {
					strip_status_labels_[i]->setText(status_q);
				}
				const QString st_style =
					(sp.severity == MFD_UI_SEVERITY_RED)
						? QStringLiteral(
							  "QLabel { background-color: #451a1a; color: #fecaca; border: 1px solid #ef4444; "
							  "border-radius: 5px; padding: 4px 5px; font-size: 11px; font-weight: bold; }")
						: QStringLiteral(
							  "QLabel { background-color: #3f2b05; color: #fde68a; border: 1px solid #d97706; "
							  "border-radius: 5px; padding: 4px 5px; font-size: 11px; font-weight: bold; }");
				if (strip_status_labels_[i]->styleSheet() != st_style) {
					strip_status_labels_[i]->setStyleSheet(st_style);
				}
				strip_status_labels_[i]->show();
			}

			const QString act_q = QString::fromUtf8(sp.bottom_button_text);
			if (strip_action_btns_[i]->text() != act_q) {
				strip_action_btns_[i]->setText(act_q);
			}
			const QString act_style =
				(sp.bottom_action == MFD_UI_ACTION_TURN_ON_STUDIO_MODE)
					? QStringLiteral(
						  "QPushButton { background-color: #d97706; color: #111827; font-size: 11px; font-weight: bold; "
						  "border-radius: 5px; padding: 5px 6px; }")
					: QStringLiteral(
						  "QPushButton { background-color: #1e293b; color: #e2e8f0; font-size: 12px; font-weight: bold; "
						  "border: 1px solid #475569; border-radius: 5px; padding: 5px 8px; } "
						  "QPushButton:hover { border-color: #93c5fd; color: #ffffff; }");
			if (strip_action_btns_[i]->styleSheet() != act_style) {
				strip_action_btns_[i]->setStyleSheet(act_style);
			}
		}

		/* Zone 3 status line */
		const char *moved_lbl =
			(snap_.last_moved_slot_index >= 0 && snap_.last_moved_slot_index < MFD_MAX_FADER_SLOTS)
				? snap_.slots[snap_.last_moved_slot_index].label
				: "";
		char last_move_buf[128] = {0};
		mfd_ui_format_last_move(snap_.last_moved_slot_index, moved_lbl, snap_.has_last_midi,
					&snap_.last_midi_decoded, last_move_buf, sizeof(last_move_buf));

		char zone3_buf[320] = {0};
		mfd_ui_format_zone3_status(snap_.studio_mode_active, snap_.preview_scene_name, snap_.program_scene_name,
					   last_move_buf, zone3_buf, sizeof(zone3_buf));
		z3_status_label_->setText(QString::fromUtf8(zone3_buf));
		if (snap_.midi_dropped_count > 0U) {
			z3_status_label_->setToolTip(
				mfdFill(mfdText("Dialog.DroppedMidiEvents", "Dropped MIDI queue events: %1"),
					{QString::number(snap_.midi_dropped_count)}));
		} else {
			z3_status_label_->setToolTip(QString());
		}
	}

	void apply_advanced_row_edit(int row, const struct mfd_slot_config_edit &edit)
	{
		struct mfd_msg err_msg;
		if (!mfd_engine_update_slot_config_msg(eng_, row, &edit, &err_msg)) {
			char err_buf[256] = {0};
			mfd_ui_format_msg(&err_msg, err_buf, sizeof(err_buf));
			adv_edit_error_ = (err_buf[0] != '\0') ? QString::fromUtf8(err_buf)
							       : mfdText("Dialog.UpdateFailed",
									 "Could not update strip configuration.");
			last_adv_topology_rev_ = ~0ULL;
		} else {
			adv_edit_error_.clear();
		}
		refresh_from_engine();
	}

	struct mfd_slot_config_edit make_edit_from_snapshot_row(int row, bool preserve_scene) const
	{
		const struct mfd_fader_slot &s = snap_.slots[row];
		struct mfd_slot_config_edit edit;
		std::memset(&edit, 0, sizeof(edit));
		edit.enabled = s.enabled;
		edit.target_type = s.target_type;
		edit.preserve_scene = preserve_scene;
		edit.keep_layer = true; /* On and Reverse change neither the job nor the layer */
		std::snprintf(edit.scene_name, sizeof(edit.scene_name), "%s", s.scene_name);
		std::snprintf(edit.source_a_name, sizeof(edit.source_a_name), "%s", s.source_a_name);
		std::snprintf(edit.source_b_name, sizeof(edit.source_b_name), "%s", s.source_b_name);
		std::snprintf(edit.filter_name, sizeof(edit.filter_name), "%s", s.filter_name);
		std::snprintf(edit.filter_prop, sizeof(edit.filter_prop), "%s", s.filter_prop);
		edit.has_filter_range = (s.target_type == MFD_TARGET_FILTER_PARAM);
		edit.filter_from_val = s.filter_from_val;
		edit.filter_to_val = s.filter_to_val;
		edit.filter_bypass_at_zero = s.filter_bypass_at_zero;
		edit.curve = s.curve;
		edit.audio_follows_video = s.audio_follows_video;
		edit.invert = s.invert;
		return edit;
	}

	void rebuild_table_rows()
	{
		rebuilding_adv_table_ = true;

		for (int r = 0; r < MFD_MAX_FADER_SLOTS; ++r) {
			for (int c = 0; c < table_->columnCount(); ++c) {
				if (QWidget *old_w = table_->cellWidget(r, c)) {
					old_w->hide();
					old_w->setObjectName(QString());
					const auto old_children = old_w->findChildren<QObject *>();
					for (QObject *ch : old_children) {
						ch->setObjectName(QString());
					}
					table_->removeCellWidget(r, c);
				}
			}
		}

		tau_spin_->blockSignals(true);
		tau_spin_->setValue(static_cast<double>(snap_.smooth_tau_ms));
		tau_spin_->blockSignals(false);

		flip_check_->blockSignals(true);
		flip_check_->setChecked(snap_.tbar_bidirectional_flip);
		flip_check_->blockSignals(false);

		if (vj_check_ && vj_fade_spin_) {
			vj_check_->blockSignals(true);
			vj_check_->setChecked(mfd_engine_vj_buttons_enabled(eng_));
			vj_check_->blockSignals(false);
			if (desk_video_check_) {
				desk_video_check_->blockSignals(true);
				desk_video_check_->setChecked(mfd_engine_desk_video_controls(eng_));
				desk_video_check_->blockSignals(false);
			}
			if (!vj_fade_spin_->hasFocus()) {
				vj_fade_spin_->blockSignals(true);
				vj_fade_spin_->setValue(mfd_engine_vj_fade_ms(eng_));
				vj_fade_spin_->blockSignals(false);
			}
		}

		if (profile_combo_ && !(profile_combo_->view() && profile_combo_->view()->isVisible())) {
			const int idx = profile_combo_->findData(static_cast<int>(snap_.active_profile));
			if (idx >= 0) {
				profile_combo_->blockSignals(true);
				profile_combo_->setCurrentIndex(idx);
				profile_combo_->blockSignals(false);
			}
		}

		for (int row = 0; row < MFD_MAX_FADER_SLOTS; ++row) {
			const struct mfd_fader_slot &slot = snap_.slots[row];

			/* Column 0: On */
			QCheckBox *chk_on = nullptr;
			QWidget *on_cell = make_centered_checkbox_cell(
				table_, QStringLiteral("mfdAdvOn_%1").arg(row), slot.enabled,
				mfdText("Dialog.Tip.EnableStrip", "Enable or disable this strip."), &chk_on);
			connect(chk_on, &QCheckBox::toggled, this, [this, row](bool on) {
				if (rebuilding_adv_table_) {
					return;
				}
				struct mfd_slot_config_edit edit = make_edit_from_snapshot_row(row, true);
				edit.enabled = on;
				apply_advanced_row_edit(row, edit);
			});
			table_->setCellWidget(row, kAdvColOn, on_cell);

			/* Column 1: Strip */
			char st_title[64] = {0};
			mfd_ui_strip_title(row, slot.label, st_title, sizeof(st_title));
			auto *lbl_name = new QLabel(QString::fromUtf8(st_title), table_);
			lbl_name->setMargin(6);
			lbl_name->setStyleSheet(QStringLiteral("font-weight: bold;"));
			lbl_name->setToolTip(
				mfdFill(mfdText("Dialog.Tip.SlotId", "Slot ID: %1"), {QString::fromUtf8(slot.id)}));
			table_->setCellWidget(row, kAdvColStrip, lbl_name);

			/* Column 2: MIDI Control */
			auto *bind_widget = new QWidget(table_);
			auto *bind_layout = new QHBoxLayout(bind_widget);
			bind_layout->setContentsMargins(6, 2, 6, 2);
			bind_layout->setSpacing(4);
			auto *lbl_addr = new QLabel(format_midi_addr(slot.addr), bind_widget);
			lbl_addr->setObjectName(QStringLiteral("mfdAdvAddr_%1").arg(row));
			auto *btn_learn = new QPushButton(QString::fromUtf8(mfd_learn_button_text(false)), bind_widget);
			btn_learn->setObjectName(QStringLiteral("mfdAdvLearn_%1").arg(row));
			btn_learn->setMinimumWidth(106);
			auto *btn_clear = new QPushButton(QString::fromUtf8("\xC3\x97"), bind_widget);
			btn_clear->setObjectName(QStringLiteral("mfdAdvClear_%1").arg(row));
			btn_clear->setFixedWidth(26);
			btn_clear->setToolTip(
				mfdText("Dialog.Tip.ClearMidi", "Remove the MIDI control from this strip."));
			btn_clear->setEnabled(slot.addr.type != MFD_MIDI_NONE);

			bind_layout->addWidget(lbl_addr, 1);
			bind_layout->addWidget(btn_learn);
			bind_layout->addWidget(btn_clear);
			addr_labels_[row] = lbl_addr;
			learn_buttons_[row] = btn_learn;
			clear_buttons_[row] = btn_clear;

			connect(btn_learn, &QPushButton::clicked, this, [this, row]() {
				if (snap_.learning_slot_index == row) {
					mfd_engine_cancel_learn(eng_);
				} else {
					mfd_engine_start_learn(eng_, row);
				}
				refresh_from_engine();
			});
			connect(btn_clear, &QPushButton::clicked, this, [this, row]() {
				mfd_engine_clear_slot_binding(eng_, row);
				refresh_from_engine();
			});
			table_->setCellWidget(row, kAdvColMidi, bind_widget);

			/* Column 3: Reverse direction */
			QCheckBox *chk_inv = nullptr;
			QWidget *inv_cell = make_centered_checkbox_cell(
				table_, QStringLiteral("mfdAdvInv_%1").arg(row), slot.invert,
				mfdText("Dialog.Tip.Reverse",
					"Reverse physical fader direction (0% \xE2\x86\x94 100%)."),
				&chk_inv);
			connect(chk_inv, &QCheckBox::toggled, this, [this, row](bool inverted) {
				if (rebuilding_adv_table_) {
					return;
				}
				struct mfd_slot_config_edit edit = make_edit_from_snapshot_row(row, true);
				edit.invert = inverted;
				apply_advanced_row_edit(row, edit);
			});
			table_->setCellWidget(row, kAdvColReverse, inv_cell);

			/* Column 4: What it does, in the strip's own sentence (the one on its card) */
			auto *lbl_job = new QLabel(table_);
			lbl_job->setObjectName(QStringLiteral("mfdAdvJob_%1").arg(row));
			lbl_job->setMargin(6);
			lbl_job->setTextFormat(Qt::PlainText);
			lbl_job->setMinimumWidth(220);
			adv_job_labels_[row] = lbl_job;
			table_->setCellWidget(row, kAdvColJob, lbl_job);

			/* Column 5: Output */
			auto *out_bar = new QProgressBar(table_);
			out_bar->setObjectName(QStringLiteral("mfdAdvOutput_%1").arg(row));
			out_bar->setRange(0, 100);
			out_bar->setValue(0);
			out_bar->setTextVisible(true);
			out_bar->setAlignment(Qt::AlignCenter);
			out_bar->setMinimumWidth(300);
			output_bars_[row] = out_bar;
			table_->setCellWidget(row, kAdvColOutput, out_bar);

			/* Column 6: the one way in to a strip's job, source, range and feel */
			auto *edit_cell = new QWidget(table_);
			auto *edit_lay = new QHBoxLayout(edit_cell);
			edit_lay->setContentsMargins(6, 2, 6, 2);
			auto *btn_edit = new QPushButton(mfdText("Dialog.Edit", "Edit..."), edit_cell);
			btn_edit->setObjectName(QStringLiteral("mfdAdvEdit_%1").arg(row));
			btn_edit->setToolTip(mfdText("Dialog.Tip.Edit", "Open Set up for this strip."));
			edit_lay->addWidget(btn_edit);
			connect(btn_edit, &QPushButton::clicked, this, [this, row]() {
				adv_edit_error_.clear();
				open_setup_for_slot(row);
			});
			table_->setCellWidget(row, kAdvColEdit, edit_cell);
		}

		for (int c = 0; c < kAdvColumns; ++c) {
			if (c == kAdvColJob) {
				continue; /* the sentence has the room that is left */
			}
			table_->resizeColumnToContents(c);
		}
		mfdPlainLabels(table_); /* the rows' labels are new */
		last_adv_topology_rev_ = snap_.config_topology_rev;
		rebuilding_adv_table_ = false;
	}

	static void set_label_text_and_style(QLabel *lbl, const QString &text, const QString &style)
	{
		if (!lbl) {
			return;
		}
		if (lbl->text() != text) {
			lbl->setText(text);
		}
		if (lbl->styleSheet() != style) {
			lbl->setStyleSheet(style);
		}
	}

	void refresh_advanced_tab()
	{
		/* The table (18 rows of widgets, two source enumerations and a catalog read) is rebuilt only while the
		 * Advanced tab is the one shown (F-06). Hidden, it stays stale: last_adv_topology_rev_ is left alone, so
		 * the refresh that a tab switch runs rebuilds it. A rebuild that was asked for outright (~0) is done. */
		const bool adv_shown = !main_tabs_ || main_tabs_->currentIndex() == 1;
		if (snap_.config_topology_rev != last_adv_topology_rev_ &&
		    (adv_shown || last_adv_topology_rev_ == ~0ULL)) {
			rebuild_table_rows();
		}

		const struct mfd_controller_profile_def *pdef = mfd_profile_get_def(snap_.active_profile);
		if (btn_defaults_ && pdef) {
			const QString btn_txt =
				(pdef->id == MFD_PROFILE_GENERIC)
					? mfdText("Dialog.ResetGenericSlots", "Clear all strips")
					: mfdFill(mfdText("Dialog.LoadDefaults", "Reset all strips to the %1 layout"),
						  {QString::fromUtf8(mfd_profile_ui_short_name(pdef))});
			if (btn_defaults_->text() != btn_txt) {
				btn_defaults_->setText(btn_txt);
			}
		}
		if (adv_beta_note_) {
			if (pdef && pdef->tier == MFD_PROFILE_TIER_BETA_UNTESTED) {
				const QString note_txt = QString::fromUtf8(mfd_profile_ui_spec_note(pdef));
				if (adv_beta_note_->text() != note_txt) {
					adv_beta_note_->setText(note_txt);
				}
				adv_beta_note_->show();
			} else {
				adv_beta_note_->hide();
			}
		}

		struct mfd_ui_controller_presentation ctrl;
		mfd_ui_format_controller_status(snap_.midi_connected, snap_.configured_device_name,
						snap_.opened_device_name, snap_.midi_error_kind, &ctrl);

		if (ctrl.dot_green) {
			set_label_text_and_style(
				conn_status_label_,
				mfdFill(mfdText("Dialog.ConnectedTo", "\xE2\x97\x8F Connected: %1"),
					{QString::fromUtf8(ctrl.display_name)}),
				QStringLiteral("QLabel { background-color: #064e3b; color: #6ee7b7; padding: 4px 10px; "
					       "border: 1px solid #10b981; border-radius: 4px; font-weight: bold; }"));
			if (!adv_edit_error_.isEmpty()) {
				banner_label_->setText(adv_edit_error_);
				banner_label_->show();
			} else {
				banner_label_->hide();
			}
		} else {
			set_label_text_and_style(
				conn_status_label_, QString::fromUtf8(ctrl.status_sentence),
				QStringLiteral("QLabel { background-color: #451a03; color: #fcd34d; padding: 4px 10px; "
					       "border: 1px solid #d97706; border-radius: 4px; font-weight: bold; }"));
			if (!adv_edit_error_.isEmpty()) {
				banner_label_->setText(adv_edit_error_);
				banner_label_->show();
			} else if (snap_.midi_last_error[0] != '\0') {
				/* worded from the error's kind, not the English last_error the log and API keep */
				char device_error[320] = {0};
				mfd_ui_format_device_error(snap_.midi_error_kind, snap_.midi_error_operation,
							   snap_.midi_error_code, snap_.midi_error_device_name,
							   device_error, sizeof(device_error));
				const QString device_text = device_error[0] != '\0'
								    ? QString::fromUtf8(device_error)
								    : QString::fromUtf8(snap_.midi_last_error);
				/* the status beside the port list already says it */
				if (device_text == QString::fromUtf8(ctrl.status_sentence)) {
					banner_label_->hide();
				} else {
					banner_label_->setText(device_text);
					banner_label_->show();
				}
			} else {
				banner_label_->hide();
			}
		}

		if (snap_.studio_mode_active) {
			const QString prev_nm = (snap_.preview_scene_name[0] != '\0')
							? QString::fromUtf8(snap_.preview_scene_name)
							: QString::fromUtf8("\xE2\x80\x94");
			const QString prog_nm = (snap_.program_scene_name[0] != '\0')
							? QString::fromUtf8(snap_.program_scene_name)
							: QString::fromUtf8("\xE2\x80\x94");
			set_label_text_and_style(
				studio_status_label_,
				mfdFill(mfdText("Dialog.StudioModeOn",
						"\xE2\x97\x8F Studio Mode on: Preview [%1] \xE2\x86\x92 Program [%2]"),
					{prev_nm, prog_nm}),
				QStringLiteral("QLabel { background-color: #172554; color: #bfdbfe; padding: 4px 10px; "
					       "border: 1px solid #2563eb; border-radius: 4px; font-weight: bold; }"));
		} else {
			set_label_text_and_style(
				studio_status_label_, mfdText("Dialog.StudioModeOff", "\xE2\x97\x8B Studio Mode off"),
				QStringLiteral("QLabel { background-color: #1f2937; color: #9ca3af; padding: 4px 10px; "
					       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		}

		char midi_buf[128] = {0};
		mfd_format_last_midi_readout(snap_.has_last_midi, &snap_.last_midi_decoded, midi_buf, sizeof(midi_buf));
		const QString midi_qstr = QString::fromUtf8(midi_buf);
		if (last_midi_label_->text() != midi_qstr) {
			last_midi_label_->setText(midi_qstr);
		}

		const QString style_disabled =
			output_bar_stylesheet(QStringLiteral("#374151"), QStringLiteral("#4b5563"));
		const QString style_unavail =
			output_bar_stylesheet(QStringLiteral("#78350f"), QStringLiteral("#b45309"));
		const QString style_waiting =
			output_bar_stylesheet(QStringLiteral("#d97706"), QStringLiteral("#f59e0b"));
		const QString style_parked =
			output_bar_stylesheet(QStringLiteral("#059669"), QStringLiteral("#10b981"));
		const QString style_engaged =
			output_bar_stylesheet(QStringLiteral("#2563eb"), QStringLiteral("#3b82f6"));

		for (int row = 0; row < MFD_MAX_FADER_SLOTS; ++row) {
			const struct mfd_fader_slot &s = snap_.slots[row];
			const bool is_learning = (snap_.learning_slot_index == row);
			if (addr_labels_[row]) {
				const QString addr_str = format_midi_addr(s.addr);
				if (addr_labels_[row]->text() != addr_str) {
					addr_labels_[row]->setText(addr_str);
				}
			}
			if (clear_buttons_[row]) {
				clear_buttons_[row]->setEnabled(s.addr.type != MFD_MIDI_NONE);
			}
			if (learn_buttons_[row]) {
				const QString learn_txt = QString::fromUtf8(mfd_learn_button_text(is_learning));
				if (learn_buttons_[row]->text() != learn_txt) {
					learn_buttons_[row]->setText(learn_txt);
				}
				const QString learn_style =
					is_learning ? QStringLiteral("QPushButton { background-color: #b45309; "
								     "color: #ffffff; font-weight: bold; }")
						    : QString();
				if (learn_buttons_[row]->styleSheet() != learn_style) {
					learn_buttons_[row]->setStyleSheet(learn_style);
				}
			}

			struct mfd_ui_strip_presentation sp;
			mfd_ui_strip_presentation_from_slot(row, s, &sp);
			if (adv_job_labels_[row]) {
				const QString job = QString::fromUtf8(sp.assignment_sentence);
				if (adv_job_labels_[row]->text() != job) {
					adv_job_labels_[row]->setText(job);
					adv_job_labels_[row]->setToolTip(mfdPlainTip(job));
				}
				const QString job_style = (s.enabled && s.target_type != MFD_TARGET_UNASSIGNED)
								  ? QStringLiteral("color: #e2e8f0;")
								  : QStringLiteral("color: #94a3b8;");
				if (adv_job_labels_[row]->styleSheet() != job_style) {
					adv_job_labels_[row]->setStyleSheet(job_style);
				}
			}

			QProgressBar *bar = output_bars_[row];
			if (!bar) {
				continue;
			}

			const int desired_pct = sp.bar_percent;
			const QString pct_q = QString::number(desired_pct);
			QString desired_text;
			QString desired_style = style_disabled;
			QString desired_tip;

			if (!s.enabled) {
				desired_text = mfdText("Dialog.Output.Off", "Off");
				desired_tip = mfdText("Dialog.Output.OffTip", "Strip is turned off.");
			} else if (s.target_type == MFD_TARGET_UNASSIGNED) {
				desired_text =
					s.takeover.has_last_fader
						? mfdFill(mfdText("Dialog.Output.PercentNotUsed", "%1% (Not used)"),
							  {pct_q})
						: mfdText("Target.NotUsed", "Not used");
				desired_tip =
					mfdText("Dialog.Output.NotUsedTip", "Click Edit... to give this strip a job.");
			} else if (!s.cached_available) {
				desired_text = mfdFill(mfdText("Dialog.Output.PercentStatus", "%1% \xC2\xB7 %2"),
						       {pct_q, QString::fromUtf8(sp.status_sentence)});
				desired_style = style_unavail;
				desired_tip = QString::fromUtf8(sp.status_sentence);
			} else {
				if (sp.status_sentence[0] != '\0') {
					desired_text =
						mfdFill(mfdText("Dialog.Output.PercentStatus", "%1% \xC2\xB7 %2"),
							{pct_q, QString::fromUtf8(sp.status_sentence)});
					desired_style = style_waiting;
					desired_tip = QString::fromUtf8(sp.status_sentence);
				} else if (sp.at_end) {
					desired_text = mfdFill(
						mfdText("Dialog.Output.PercentAtEnd", "%1% \xC2\xB7 At end."), {pct_q});
					desired_style = style_parked;
					desired_tip = mfdText("Dialog.Output.AtEndTip", "At end (parked at rail).");
				} else {
					desired_text =
						(sp.state_readout[0] != '\0')
							? mfdFill(mfdText("Dialog.Output.PercentReadout", "%1% (%2)"),
								  {pct_q, QString::fromUtf8(sp.state_readout)})
							: mfdFill(mfdText("Readout.Percent", "%1%"), {pct_q});
					desired_style = style_engaged;
					desired_tip = QString::fromUtf8(sp.assignment_sentence);
				}
			}

			if (bar->value() != desired_pct) {
				bar->setValue(desired_pct);
			}
			desired_text = mfdProgressFormat(desired_text);
			if (bar->format() != desired_text) {
				bar->setFormat(desired_text);
			}
			if (bar->styleSheet() != desired_style) {
				bar->setStyleSheet(desired_style);
			}
			if (bar->toolTip() != mfdPlainTip(desired_tip)) {
				bar->setToolTip(mfdPlainTip(desired_tip));
			}
		}
	}

	struct mfd_engine *eng_ = nullptr;
	bool owns_engine_notify_ = false;
	std::atomic<bool> local_refresh_queued_{false};
	struct mfd_ui_snapshot snap_ {};
	uint64_t last_adv_topology_rev_ = ~0ULL;
	bool rebuilding_adv_table_ = false;
	bool last_midi_connected_ = false;
	bool ports_refill_pending_ = false; /* ports changed while a port list was dropped down (F-05) */
	bool showing_fx_bank_ = false;
	QString adv_edit_error_;

	QWidget *first_run_banner_ = nullptr;
	QLabel *first_run_text_ = nullptr;
	QWidget *no_layout_banner_ = nullptr;
	QLabel *no_layout_text_ = nullptr;
	QString no_layout_dismissed_for_;
	QWidget *studio_note_widget_ = nullptr;
	QWidget *stream_deck_companion_card_ = nullptr;
	QTabWidget *main_tabs_ = nullptr;

	/* Strips tab widgets */
	QLabel *z1_name_label_ = nullptr;
	QLabel *z1_dot_label_ = nullptr;
	QLabel *z1_status_label_ = nullptr;
	QLabel *z1_beta_badge_ = nullptr;
	QPushButton *z1_btn_reconnect_ = nullptr;
	QWidget *z1_change_panel_ = nullptr;
	QComboBox *zone1_device_combo_ = nullptr;
	QComboBox *zone1_profile_combo_ = nullptr;
	QPushButton *z1_btn_change_ctrl_ = nullptr;
	QPushButton *z1_btn_show_ctrl_ = nullptr;
	QPushButton *btn_bank_faders_ = nullptr;
	QPushButton *btn_bank_fx_ = nullptr;
	QLabel *bank_hint_label_ = nullptr;
	QPushButton *btn_clean_all_fx_ = nullptr;

	QFrame *strip_cards_[MFD_MAX_FADER_SLOTS]{};
	QLabel *strip_titles_[MFD_MAX_FADER_SLOTS]{};
	MfdStripBarWidget *strip_bars_[MFD_MAX_FADER_SLOTS]{};
	QLabel *strip_states_[MFD_MAX_FADER_SLOTS]{};
	QPushButton *strip_finish_btns_[MFD_MAX_FADER_SLOTS]{};
	QPushButton *strip_assign_btns_[MFD_MAX_FADER_SLOTS]{};
	QLabel *strip_assign_labels_[MFD_MAX_FADER_SLOTS]{};
	QLabel *strip_status_labels_[MFD_MAX_FADER_SLOTS]{};
	QPushButton *strip_action_btns_[MFD_MAX_FADER_SLOTS]{};
	enum mfd_ui_strip_action strip_bottom_actions_[MFD_MAX_FADER_SLOTS] {};

	QLabel *z3_status_label_ = nullptr;

	QPointer<MfdSetupSheetDialog> setup_sheet_;
	QPointer<MfdControllerViewDialog> controller_view_dlg_;

	/* Advanced tab widgets */
	QLabel *banner_label_ = nullptr;
	QLabel *adv_beta_note_ = nullptr;
	QLabel *conn_status_label_ = nullptr;
	QLabel *studio_status_label_ = nullptr;
	QLabel *last_midi_label_ = nullptr;
	QComboBox *device_combo_ = nullptr;
	QComboBox *profile_combo_ = nullptr;
	QPushButton *btn_defaults_ = nullptr;
	QDoubleSpinBox *tau_spin_ = nullptr;
	QCheckBox *flip_check_ = nullptr;
	QCheckBox *vj_check_ = nullptr;
	QCheckBox *desk_video_check_ = nullptr;
	QSpinBox *vj_fade_spin_ = nullptr;
	QLabel *vj_fill_result_ = nullptr;

	/* "Fill empty faders from the scene on air": the engine fills them, the line beside the button says how it went. */
	void fill_faders_from_scene()
	{
		enum mfd_fill_status st = MFD_FILL_NO_SCENE;
		char scene[MFD_SOURCE_NAME_MAX] = {0};
		const int n = mfd_engine_fill_strips_from_scene(eng_, nullptr, true, false, &st, scene, sizeof(scene));
		const QString sc = QString::fromUtf8(scene);
		QString msg;
		switch (st) {
		case MFD_FILL_OK:
			msg = n == 1 ? mfdFill(mfdText("Dialog.VjFill.One",
						       "Filled 1 fader from %1. It follows the scene on air."),
					       {sc})
				     : mfdFill(mfdText("Dialog.VjFill.Many",
						       "Filled %1 faders from %2. They follow the scene on air."),
					       {QString::number(n), sc});
			break;
		case MFD_FILL_NO_EMPTY_STRIPS:
			msg = mfdText("Dialog.VjFill.NoEmpty",
				      "Every fader has a job of its own. Unassign one to fill it.");
			break;
		case MFD_FILL_NO_VIDEO_LAYERS:
			msg = mfdFill(mfdText("Dialog.VjFill.NoVideo", "%1 has no video source left for the faders."),
				      {sc});
			break;
		case MFD_FILL_NO_SCENE:
		default:
			msg = mfdText("Dialog.VjFill.NoScene", "No scene is on air.");
			break;
		}
		if (vj_fill_result_) {
			vj_fill_result_->setText(msg);
		}
		mfd_engine_flush_pending_ui_work(eng_);
		last_adv_topology_rev_ = ~0ULL;
		refresh_from_engine();
	}
	QTableWidget *table_ = nullptr;
	QPointer<QLabel> addr_labels_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> learn_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> clear_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QLabel> adv_job_labels_[MFD_MAX_FADER_SLOTS];
	QPointer<QProgressBar> output_bars_[MFD_MAX_FADER_SLOTS];
};

class MfdUiDispatcher final : public QObject {
public:
	explicit MfdUiDispatcher(QObject *parent = nullptr) : QObject(parent) {}
	void on_drain();
};

static std::mutex g_ui_bridge_mu;
static std::atomic<bool> g_ui_refresh_queued{false};
static std::atomic<bool> g_ports_changed_queued{false};
/* The engine change counters as the settings window last saw them (F-08). */
static uint64_t g_dialog_seen_topology_rev = ~0ULL;
static uint64_t g_dialog_seen_telemetry_rev = ~0ULL;
static MfdUiDispatcher *g_ui_dispatcher = nullptr;
static QPointer<MfdSettingsDialog> g_dialog;

/* The Fadester Desk dock shows itself once, the first time an APC40 mkII is open: after OBS has finished loading (its
 * dock layout is restored by then), on the UI thread. An installation that already has settings never gets it. */
static void show_desk_the_first_time()
{
	/* the usual layout too, without a question (U1-02): the settings dialog then says what was set up */
	mfd_engine_apply_first_run_layout(mfd_engine_get());
	if (mfd_engine_desk_first_show_due(mfd_engine_get()) && mfd_desk_dock_show()) {
		mfd_engine_desk_first_show_done(mfd_engine_get()); /* only once it really showed */
	}
}

void MfdUiDispatcher::on_drain()
{
	g_ui_refresh_queued.store(false);
	mfd_engine_flush_pending_ui_work(mfd_engine_get());
	show_desk_the_first_time();
	/* The engine wakes this about 30 times a second while the dock is visible, for the dock's pulsing pads. The
	 * settings window is refreshed only when something it shows has changed (F-08), not on every dock frame. */
	uint64_t topology_rev = 0U;
	uint64_t telemetry_rev = 0U;
	mfd_engine_get_ui_revs(mfd_engine_get(), &topology_rev, &telemetry_rev, nullptr);
	const bool dialog_changed = topology_rev != g_dialog_seen_topology_rev ||
				    telemetry_rev != g_dialog_seen_telemetry_rev;
	g_dialog_seen_topology_rev = topology_rev;
	g_dialog_seen_telemetry_rev = telemetry_rev;
	if (g_dialog && dialog_changed) {
		g_dialog->refresh_from_engine();
	}
	mfd_desk_dock_refresh();
}

static void ui_notify_bridge_cb(void *unused)
{
	UNUSED_PARAMETER(unused);
	std::lock_guard<std::mutex> lk(g_ui_bridge_mu);
	if (!g_ui_dispatcher) {
		return;
	}
	if (!g_ui_refresh_queued.exchange(true)) {
		MfdUiDispatcher *disp = g_ui_dispatcher;
		const bool ok = QMetaObject::invokeMethod(
			disp,
			[disp]() {
				if (g_ui_dispatcher == disp) {
					disp->on_drain();
				}
			},
			Qt::QueuedConnection);
		if (!ok) {
			g_ui_refresh_queued.store(false);
		}
	}
}

/* A MIDI device was plugged in or out: re-check the open port and refresh the dialog's port list. */
static void on_midi_ports_changed()
{
	g_ports_changed_queued.store(false);
	mfd_engine_poll_midi_health_on_ui_thread(mfd_engine_get());
	if (g_dialog) {
		g_dialog->on_hardware_ports_changed();
		g_dialog->refresh_from_engine();
	}
	show_desk_the_first_time(); /* an APC40 plugged in after OBS started */
	mfd_desk_dock_refresh();
}

/*
 * One queued re-check for a burst of device messages (F-05). Windows sends WM_DEVICECHANGE to every top-level window
 * and several times per plug or unplug, and CoreMIDI sends one notification per object: the first one queues a call on
 * the UI thread and the rest are dropped until it has run. No timer (the rule in tests/test-shader-syntax.c).
 */
static void queue_midi_ports_changed()
{
	if (!g_ui_dispatcher || g_ports_changed_queued.exchange(true)) {
		return;
	}
	if (!QMetaObject::invokeMethod(g_ui_dispatcher, on_midi_ports_changed, Qt::QueuedConnection)) {
		g_ports_changed_queued.store(false);
	}
}

#ifdef _WIN32
class MfdDeviceChangeFilter final : public QAbstractNativeEventFilter {
public:
	bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
	{
		Q_UNUSED(result);
		if (!message) {
			return false;
		}
		if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
			const auto *msg = static_cast<const MSG *>(message);
			/* Only the events that can add or remove a MIDI port: a device tree change, an arrival, a
			 * completed removal. Queries, pending removals and custom events are not looked at. */
			if (msg->message == WM_DEVICECHANGE &&
			    (msg->wParam == DBT_DEVNODES_CHANGED || msg->wParam == DBT_DEVICEARRIVAL ||
			     msg->wParam == DBT_DEVICEREMOVECOMPLETE)) {
				queue_midi_ports_changed();
			}
		}
		return false;
	}
};

static MfdDeviceChangeFilter *g_dev_change_filter = nullptr;
#endif

static void open_settings_from_dock()
{
	mfd_ui_show_settings();
}

static void on_tools_menu_clicked(void *unused)
{
	UNUSED_PARAMETER(unused);
	mfd_ui_show_settings();
}

} // namespace

void mfd_ui_show_settings(void)
{
	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!g_dialog) {
		g_dialog = new MfdSettingsDialog(mfd_engine_get(), main_win);
		g_dialog->setAttribute(Qt::WA_DeleteOnClose, true);
	} else {
		g_dialog->refresh_from_engine();
	}
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}

QDialog *mfd_ui_create_settings_dialog(struct mfd_engine *eng, QWidget *parent)
{
	return new MfdSettingsDialog(eng, parent);
}

void mfd_ui_init(void)
{
	{
		std::lock_guard<std::mutex> lk(g_ui_bridge_mu);
		if (!g_ui_dispatcher) {
			g_ui_dispatcher = new MfdUiDispatcher();
		}
		g_ui_refresh_queued.store(false);
	}
	mfd_engine_set_ui_notify_fn(mfd_engine_get(), ui_notify_bridge_cb, nullptr);

	obs_frontend_add_tools_menu_item(obs_module_text("ToolsMenuTitle"), on_tools_menu_clicked, nullptr);
	mfd_desk_dock_init(); /* before OBS restores the dock layout */
	mfd_desk_dock_set_settings_opener(open_settings_from_dock);

#ifdef MFD_ENABLE_SELF_TEST
	const bool self_test = qgetenv("MFD_SELF_TEST") == "1"; /* a build with the harness, asked to run it (H-02) */
#else
	const bool self_test = false;
#endif
	if (!self_test) {
#ifdef _WIN32
		if (QCoreApplication::instance() && !g_dev_change_filter) {
			g_dev_change_filter = new MfdDeviceChangeFilter();
			QCoreApplication::instance()->installNativeEventFilter(g_dev_change_filter);
		}
#else
		mfd_coremidi_set_setup_callback(queue_midi_ports_changed);
#endif
		mfd_engine_poll_midi_health_on_ui_thread(mfd_engine_get());
	}
}

void mfd_ui_shutdown(void)
{
	mfd_desk_dock_set_settings_opener(nullptr);
	mfd_desk_dock_shutdown();
	mfd_engine_set_ui_notify_fn(mfd_engine_get(), nullptr, nullptr);

#ifdef _WIN32
	if (g_dev_change_filter) {
		if (QCoreApplication::instance()) {
			QCoreApplication::instance()->removeNativeEventFilter(g_dev_change_filter);
		}
		delete g_dev_change_filter;
		g_dev_change_filter = nullptr;
	}
#else
	mfd_coremidi_set_setup_callback(nullptr);
#endif

	if (g_dialog) {
		g_dialog->close();
		delete g_dialog.data();
		g_dialog.clear();
	}

	{
		std::lock_guard<std::mutex> lk(g_ui_bridge_mu);
		delete g_ui_dispatcher;
		g_ui_dispatcher = nullptr;
		g_ui_refresh_queued.store(false);
	}
}
