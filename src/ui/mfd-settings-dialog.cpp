/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ui/mfd-settings-dialog.hpp"
#include "ui/mfd-controller-svg.hpp"
#include "ui/mfd-setup-sheet.hpp"
#include "obs/mfd-engine.h"
#include "core/ui-sentences.h"
#include "core/tbar-logic.h"
#include "core/mixer-math.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

#include <QAbstractItemView>
#include <QAbstractNativeEventFilter>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRectF>
#include <QScrollArea>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
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
	switch (addr.type) {
	case MFD_MIDI_CC:
		return QStringLiteral("Ch %1 CC %2%3")
			.arg(static_cast<int>(addr.channel) + 1)
			.arg(static_cast<int>(addr.number))
			.arg(addr.is_14bit ? QStringLiteral(" (14-bit)") : QString());
	case MFD_MIDI_PITCH_BEND:
		return QStringLiteral("Ch %1 Pitch Bend").arg(static_cast<int>(addr.channel) + 1);
	case MFD_MIDI_NOTE_ON:
		return QStringLiteral("Ch %1 Note %2")
			.arg(static_cast<int>(addr.channel) + 1)
			.arg(static_cast<int>(addr.number));
	default:
		return QStringLiteral("Unbound");
	}
}

static QStringList collect_live_obs_sources(bool require_video, bool require_audio)
{
	struct enum_ctx {
		QStringList names;
		bool req_video;
		bool req_audio;
	} ctx{QStringList{}, require_video, require_audio};

	if (!obs_initialized()) {
		return ctx.names;
	}

	obs_enum_sources(
		[](void *data, obs_source_t *source) -> bool {
			auto *c = static_cast<enum_ctx *>(data);
			if (!source) {
				return true;
			}
			const uint32_t flags = obs_source_get_output_flags(source);
			if (c->req_video && (flags & OBS_SOURCE_VIDEO) == 0U) {
				return true;
			}
			if (c->req_audio && (flags & OBS_SOURCE_AUDIO) == 0U) {
				return true;
			}
			const char *name = obs_source_get_name(source);
			if (name && *name && std::strncmp(name, "__mfd_", 6) != 0 &&
			    std::strncmp(name, "__midi_fadester_", 16) != 0) {
				c->names.append(QString::fromUtf8(name));
			}
			return true;
		},
		&ctx);

	ctx.names.sort(Qt::CaseInsensitive);
	return ctx.names;
}

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

static void build_strip_presentation_from_slot(int idx, const struct mfd_fader_slot &s,
					       struct mfd_ui_strip_presentation *out)
{
	struct mfd_ui_strip_input in;
	std::memset(&in, 0, sizeof(in));
	in.slot_index = idx;
	in.slot_label = s.label;
	in.is_horizontal = (idx == 0);
	in.enabled = s.enabled;
	in.addr_type = s.addr.type;
	in.target_type = s.target_type;
	in.scene_name = (s.scene_name[0] != '\0') ? s.scene_name : s.resolved_scene_name;
	in.source_a_name = s.source_a_name;
	in.source_b_name = s.source_b_name;
	in.available = s.cached_available;
	in.reason = s.cached_reason;
	in.raw_norm = s.last_raw_norm;
	in.applied_val = s.last_applied_val;
	in.alpha_a = s.last_alpha_a;
	in.alpha_b = s.last_alpha_b;
	in.audio_db = mfd_deflection_to_db(s.last_applied_val);
	in.auto_fade_active = s.auto_fade_active;
	in.tbar_state = s.tbar_state;
	in.takeover_state = s.takeover.state;
	in.takeover_hint = s.takeover.hint;
	in.has_last_fader = s.takeover.has_last_fader;
	in.polarity_reversed = s.takeover.polarity_reversed;
	in.target_obs = s.takeover.target_obs;
	mfd_ui_format_strip_presentation(&in, out);
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
		       int pickup_target_pct, const QString &arrow_glyph)
	{
		const int clamped_pct = std::clamp(bar_pct, 0, 100);
		const int clamped_pickup = std::clamp(pickup_target_pct, 0, 100);
		if (bar_pct_ == clamped_pct && dimmed_ == dimmed && severity_ == severity && at_end_ == at_end &&
		    show_pickup_ == show_pickup && pickup_pct_ == clamped_pickup && arrow_glyph_ == arrow_glyph) {
			return;
		}
		bar_pct_ = clamped_pct;
		dimmed_ = dimmed;
		severity_ = severity;
		at_end_ = at_end;
		show_pickup_ = show_pickup;
		pickup_pct_ = clamped_pickup;
		arrow_glyph_ = arrow_glyph;
		setProperty("barPercent", bar_pct_);
		setProperty("showingPickup", show_pickup_);
		setProperty("pickupPercent", pickup_pct_);
		setProperty("arrowGlyph", arrow_glyph_);
		update();
	}

	int bar_percent() const { return bar_pct_; }
	bool showing_pickup() const { return show_pickup_; }
	QString arrow_glyph() const { return arrow_glyph_; }

protected:
	void paintEvent(QPaintEvent *event) override
	{
		Q_UNUSED(event);
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);

		QColor accent(0x38, 0xbd, 0xf8);
		if (dimmed_) {
			accent = QColor(0x64, 0x74, 0x8b);
		} else if (severity_ == MFD_UI_SEVERITY_RED) {
			accent = QColor(0xef, 0x44, 0x44);
		} else if (severity_ == MFD_UI_SEVERITY_AMBER) {
			accent = QColor(0xf5, 0x9e, 0x0b);
		} else if (at_end_) {
			accent = QColor(0x10, 0xb9, 0x81);
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
};

class MfdSettingsDialog final : public QDialog {
public:
	explicit MfdSettingsDialog(struct mfd_engine *eng, QWidget *parent = nullptr)
		: QDialog(parent),
		  eng_(eng ? eng : mfd_engine_get()),
		  adv_catalog_(mfd_ui_catalog_create())
	{
		setObjectName(QStringLiteral("mfdSettingsDialog"));
		setWindowTitle(QStringLiteral("Midi Fadester %1 (commit %2, built %3)")
				       .arg(QString::fromUtf8(PLUGIN_VERSION), QString::fromUtf8(PLUGIN_GIT_COMMIT),
					    QString::fromUtf8(PLUGIN_BUILT_AT)));
		resize(1380, 660);
		setMinimumSize(1160, 560);
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
		auto *fr_lbl = new QLabel(
			QStringLiteral(
				"Start with the usual layout? Crossfader switches scenes, Master is Desktop Audio volume. "
				"Change anything by clicking a strip."),
			first_run_banner_);
		fr_lbl->setObjectName(QStringLiteral("mfdFirstRunText"));
		fr_lbl->setWordWrap(true);
		fr_lbl->setStyleSheet(
			QStringLiteral("color: #e0f2fe; font-weight: bold; background: transparent; border: none;"));
		auto *btn_fr_yes = new QPushButton(QStringLiteral("Yes"), first_run_banner_);
		btn_fr_yes->setObjectName(QStringLiteral("mfdFirstRunYesBtn"));
		btn_fr_yes->setCursor(Qt::PointingHandCursor);
		btn_fr_yes->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: #0284c7; color: #ffffff; font-weight: bold; padding: 6px 18px; border-radius: 5px; border: 1px solid #38bdf8; }"));
		auto *btn_fr_custom = new QPushButton(QStringLiteral("I'll set it up"), first_run_banner_);
		btn_fr_custom->setObjectName(QStringLiteral("mfdFirstRunCustomBtn"));
		btn_fr_custom->setCursor(Qt::PointingHandCursor);
		btn_fr_custom->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: #1e293b; color: #e2e8f0; font-weight: bold; padding: 6px 14px; border-radius: 5px; border: 1px solid #475569; }"));
		fr_lay->addWidget(fr_lbl, 1);
		fr_lay->addWidget(btn_fr_yes);
		fr_lay->addWidget(btn_fr_custom);
		first_run_banner_->hide();
		root->addWidget(first_run_banner_);

		connect(btn_fr_yes, &QPushButton::clicked, this, [this]() {
			mfd_engine_accept_first_run_usual_layout(eng_);
			refresh_from_engine();
		});
		connect(btn_fr_custom, &QPushButton::clicked, this, [this]() {
			mfd_engine_decline_first_run_usual_layout(eng_);
			refresh_from_engine();
		});

		/* --- Amber Studio Mode Banner --- */
		studio_note_widget_ = new QWidget(this);
		studio_note_widget_->setObjectName(QStringLiteral("mfdStudioNote"));
		studio_note_widget_->setStyleSheet(
			QStringLiteral("QWidget#mfdStudioNote { background-color: #452703; border: 1px solid #d97706; "
				       "border-radius: 6px; }"));
		auto *note_layout = new QHBoxLayout(studio_note_widget_);
		note_layout->setContentsMargins(12, 7, 12, 7);
		auto *note_label = new QLabel(
			QStringLiteral("Studio Mode is off. Turn on Studio Mode to switch scenes with the T-bar."),
			studio_note_widget_);
		note_label->setObjectName(QStringLiteral("mfdStudioNoteLabel"));
		note_label->setStyleSheet(
			QStringLiteral("color: #fde68a; font-weight: bold; background: transparent; border: none;"));
		note_label->setWordWrap(true);
		auto *btn_turn_on_studio = new QPushButton(QStringLiteral("Turn on Studio Mode"), studio_note_widget_);
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
		refresh_from_engine();
	}

	~MfdSettingsDialog() override
	{
		if (owns_engine_notify_ && eng_) {
			mfd_engine_set_ui_notify_fn(eng_, nullptr, nullptr);
		}
		if (setup_sheet_) {
			setup_sheet_->close();
		}
		if (controller_view_dlg_) {
			controller_view_dlg_->close();
		}
		mfd_ui_catalog_destroy(adv_catalog_);
		adv_catalog_ = nullptr;
	}

	void on_hardware_ports_changed() { populate_device_combos(); }

	void refresh_from_engine()
	{
		if (!eng_) {
			return;
		}
		mfd_engine_get_ui_snapshot(eng_, &snap_);

		if (snap_.midi_connected != last_midi_connected_) {
			last_midi_connected_ = snap_.midi_connected;
			const bool z1_open = zone1_device_combo_ && zone1_device_combo_->view() &&
					     zone1_device_combo_->view()->isVisible();
			const bool adv_open = device_combo_ && device_combo_->view() &&
					      device_combo_->view()->isVisible();
			if (!z1_open && !adv_open) {
				populate_device_combos();
			}
		}

		/* Top banners */
		first_run_banner_->setVisible(snap_.first_run_offer_pending);

		size_t active_tbar_slots = 0U;
		bool can_finish_any_tbar = false;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			const struct mfd_fader_slot &s = snap_.slots[i];
			if (s.enabled && s.target_type == MFD_TARGET_TBAR) {
				++active_tbar_slots;
				if (snap_.studio_mode_active && s.tbar_state == MFD_TBAR_THROWING) {
					can_finish_any_tbar = true;
				}
			}
		}
		studio_note_widget_->setVisible(
			mfd_should_show_studio_mode_note(snap_.studio_mode_active, active_tbar_slots));

		refresh_strips_tab();
		refresh_advanced_tab(can_finish_any_tbar);

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
		if (setup_sheet_ && setup_sheet_->isVisible()) {
			setup_sheet_->reject();
		}
		if (controller_view_dlg_ && controller_view_dlg_->isVisible()) {
			controller_view_dlg_->close();
		}
		QDialog::closeEvent(event);
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

		z1_name_label_ = new QLabel(QStringLiteral("Controller: None"), zone1);
		z1_name_label_->setObjectName(QStringLiteral("mfdControllerNameLabel"));
		z1_name_label_->setStyleSheet(QStringLiteral("color: #f8fafc; font-size: 13px; font-weight: bold;"));
		z1_lay->addWidget(z1_name_label_);

		z1_dot_label_ = new QLabel(QStringLiteral("\xE2\x97\x8F"), zone1);
		z1_dot_label_->setObjectName(QStringLiteral("mfdControllerDot"));
		z1_lay->addWidget(z1_dot_label_);

		z1_status_label_ = new QLabel(QStringLiteral("Connected"), zone1);
		z1_status_label_->setObjectName(QStringLiteral("mfdControllerStatusLabel"));
		z1_status_label_->setWordWrap(true);
		z1_lay->addWidget(z1_status_label_, 1);

		z1_btn_reconnect_ = new QPushButton(QStringLiteral("Reconnect"), zone1);
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
		zone1_device_combo_ = new QComboBox(z1_change_panel_);
		zone1_device_combo_->setObjectName(QStringLiteral("mfdZone1DeviceCombo"));
		zone1_device_combo_->setMinimumWidth(220);
		cp_lay->addWidget(zone1_device_combo_);
		z1_change_panel_->hide();
		z1_lay->addWidget(z1_change_panel_);

		const QString z1_btn_style = QStringLiteral(
			"QPushButton { background-color: #1e293b; color: #e2e8f0; border: 1px solid #475569; border-radius: 5px; padding: 5px 12px; font-weight: bold; } "
			"QPushButton:hover { border-color: #93c5fd; color: #ffffff; }");

		z1_btn_change_ctrl_ = new QPushButton(QStringLiteral("Change controller"), zone1);
		z1_btn_change_ctrl_->setObjectName(QStringLiteral("mfdBtnChangeController"));
		z1_btn_change_ctrl_->setCursor(Qt::PointingHandCursor);
		z1_btn_change_ctrl_->setStyleSheet(z1_btn_style);
		z1_lay->addWidget(z1_btn_change_ctrl_);

		z1_btn_show_ctrl_ = new QPushButton(QStringLiteral("Show controller"), zone1);
		z1_btn_show_ctrl_->setObjectName(QStringLiteral("mfdBtnShowController"));
		z1_btn_show_ctrl_->setCursor(Qt::PointingHandCursor);
		z1_btn_show_ctrl_->setStyleSheet(z1_btn_style);
		z1_lay->addWidget(z1_btn_show_ctrl_);

		lay->addWidget(zone1);

		connect(z1_btn_reconnect_, &QPushButton::clicked, this, [this]() {
			mfd_engine_reconnect_midi(eng_, snap_.configured_device_name);
			mfd_engine_save_global_config(eng_);
			mfd_engine_notify_state_changed(eng_);
			populate_device_combos();
			refresh_from_engine();
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
		connect(z1_btn_show_ctrl_, &QPushButton::clicked, this, [this]() { open_controller_svg_view(); });

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

			auto *btn_finish = new QPushButton(QStringLiteral("Finish now"), card);
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

			auto *btn_action = new QPushButton(QStringLiteral("Set up"), card);
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

		auto *btn_open_adv = new QPushButton(QStringLiteral("Advanced"), zone3);
		btn_open_adv->setObjectName(QStringLiteral("mfdBtnOpenAdvanced"));
		btn_open_adv->setCursor(Qt::PointingHandCursor);
		btn_open_adv->setStyleSheet(z1_btn_style);
		connect(btn_open_adv, &QPushButton::clicked, this, [this]() { main_tabs_->setCurrentIndex(1); });
		z3_lay->addWidget(btn_open_adv);

		lay->addWidget(zone3);
		main_tabs_->addTab(strips_tab, QStringLiteral("Strips"));
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
		auto *lbl_midi_in = new QLabel(QStringLiteral("MIDI In:"), adv_tab);
		lbl_midi_in->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		top_bar->addWidget(lbl_midi_in);

		device_combo_ = new QComboBox(adv_tab);
		device_combo_->setObjectName(QStringLiteral("mfdAdvDeviceCombo"));
		device_combo_->setMinimumWidth(220);
		top_bar->addWidget(device_combo_);

		conn_status_label_ = new QLabel(QStringLiteral("Checking..."), adv_tab);
		conn_status_label_->setObjectName(QStringLiteral("mfdAdvConnLabel"));
		top_bar->addWidget(conn_status_label_);

		auto *btn_reconnect = new QPushButton(QStringLiteral("Reconnect / Rescan"), adv_tab);
		btn_reconnect->setObjectName(QStringLiteral("mfdAdvReconnectBtn"));
		top_bar->addWidget(btn_reconnect);

		top_bar->addSpacing(8);
		auto *lbl_prof = new QLabel(QStringLiteral("Profile:"), adv_tab);
		lbl_prof->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		top_bar->addWidget(lbl_prof);

		profile_combo_ = new QComboBox(adv_tab);
		profile_combo_->setObjectName(QStringLiteral("mfdAdvProfileCombo"));
		profile_combo_->addItem(QStringLiteral("Generic MIDI (Custom)"), static_cast<int>(MFD_PROFILE_GENERIC));
		profile_combo_->addItem(QStringLiteral("Akai APC40 mkII"), static_cast<int>(MFD_PROFILE_APC40_MK2));
		top_bar->addWidget(profile_combo_);

		auto *btn_defaults = new QPushButton(QStringLiteral("Load APC40 mkII Defaults"), adv_tab);
		btn_defaults->setObjectName(QStringLiteral("mfdAdvDefaultsBtn"));
		top_bar->addWidget(btn_defaults);
		top_bar->addStretch(1);
		root->addLayout(top_bar);

		auto *telemetry_bar = new QHBoxLayout();
		studio_status_label_ = new QLabel(QStringLiteral("Studio Mode: Off"), adv_tab);
		studio_status_label_->setObjectName(QStringLiteral("mfdAdvStudioLabel"));
		telemetry_bar->addWidget(studio_status_label_);

		btn_finish_tbar_ = new QPushButton(QStringLiteral("Finish now"), adv_tab);
		btn_finish_tbar_->setObjectName(QStringLiteral("mfdAdvFinishBtn"));
		btn_finish_tbar_->setEnabled(false);
		telemetry_bar->addWidget(btn_finish_tbar_);

		telemetry_bar->addSpacing(16);
		auto *lbl_tau = new QLabel(QStringLiteral("Smoothing (ms):"), adv_tab);
		lbl_tau->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		telemetry_bar->addWidget(lbl_tau);

		tau_spin_ = new QDoubleSpinBox(adv_tab);
		tau_spin_->setObjectName(QStringLiteral("mfdAdvTauSpin"));
		tau_spin_->setRange(0.0, 250.0);
		tau_spin_->setSingleStep(5.0);
		tau_spin_->setDecimals(1);
		tau_spin_->setSuffix(QStringLiteral(" ms"));
		telemetry_bar->addWidget(tau_spin_);

		telemetry_bar->addSpacing(12);
		flip_check_ = new QCheckBox(QStringLiteral("T-bar works in both directions"), adv_tab);
		flip_check_->setObjectName(QStringLiteral("mfdAdvFlipCheck"));
		flip_check_->setStyleSheet(QStringLiteral("color: #e2e8f0; font-weight: bold;"));
		telemetry_bar->addWidget(flip_check_);
		telemetry_bar->addStretch(1);

		last_midi_label_ = new QLabel(QStringLiteral("No MIDI received yet"), adv_tab);
		last_midi_label_->setObjectName(QStringLiteral("mfdAdvLastMidiLabel"));
		last_midi_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #1f2937; color: #93c5fd; padding: 4px 10px; "
				       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		telemetry_bar->addWidget(last_midi_label_);
		root->addLayout(telemetry_bar);

		connect(btn_finish_tbar_, &QPushButton::clicked, this, [this]() {
			for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
				if (snap_.slots[i].enabled && snap_.slots[i].target_type == MFD_TARGET_TBAR &&
				    snap_.slots[i].tbar_state == MFD_TBAR_THROWING) {
					mfd_engine_finish_tbar_throw(eng_, i);
					break;
				}
			}
			refresh_from_engine();
		});

		table_ = new QTableWidget(MFD_MAX_FADER_SLOTS, 10, adv_tab);
		table_->setObjectName(QStringLiteral("mfdAdvancedTable"));
		table_->setHorizontalHeaderLabels({
			QStringLiteral("On"),
			QStringLiteral("Strip"),
			QStringLiteral("MIDI Control"),
			QStringLiteral("Reverse direction"),
			QStringLiteral("What it does"),
			QStringLiteral("Left"),
			QStringLiteral("Right"),
			QStringLiteral("Feel"),
			QStringLiteral("Also fade audio"),
			QStringLiteral("Output"),
		});
		table_->horizontalHeader()->setStretchLastSection(true);
		table_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
		table_->verticalHeader()->setVisible(false);
		table_->verticalHeader()->setDefaultSectionSize(36);
		table_->setAlternatingRowColors(true);
		table_->setSelectionMode(QAbstractItemView::NoSelection);
		table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		root->addWidget(table_, 1);

		auto *footer = new QHBoxLayout();
		auto *btn_back_strips = new QPushButton(QStringLiteral("Back to Strips"), adv_tab);
		btn_back_strips->setObjectName(QStringLiteral("mfdBtnBackToStrips"));
		footer->addWidget(btn_back_strips);
		connect(btn_back_strips, &QPushButton::clicked, this, [this]() { main_tabs_->setCurrentIndex(0); });

		auto *hint = new QLabel(
			QStringLiteral(
				"Switch scenes (T-bar) drives Preview \xE2\x86\x92 Program linearly 0\xE2\x80\x93"
				"100% and completes in the 3% target rail zone (returning to the 3% origin rail "
				"cancels). Mix two layers fades Left and Right inside one scene with automatic "
				"stack-order detection."),
			adv_tab);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px;"));
		footer->addWidget(hint, 1);

		auto *btn_import = new QPushButton(QStringLiteral("Import..."), adv_tab);
		btn_import->setObjectName(QStringLiteral("mfdBtnImport"));
		footer->addWidget(btn_import);

		auto *btn_export = new QPushButton(QStringLiteral("Export..."), adv_tab);
		btn_export->setObjectName(QStringLiteral("mfdBtnExport"));
		footer->addWidget(btn_export);

		auto *btn_refresh_sources = new QPushButton(QStringLiteral("Refresh Sources"), adv_tab);
		btn_refresh_sources->setObjectName(QStringLiteral("mfdBtnRefreshSources"));
		footer->addWidget(btn_refresh_sources);

		auto *btn_close = new QPushButton(QStringLiteral("Close"), adv_tab);
		btn_close->setObjectName(QStringLiteral("mfdBtnClose"));
		footer->addWidget(btn_close);
		root->addLayout(footer);

		connect(btn_import, &QPushButton::clicked, this, [this]() {
			const QString path = QFileDialog::getOpenFileName(
				this, QStringLiteral("Import Midi Fadester Configuration"), QString(),
				QStringLiteral("JSON Files (*.json);;All Files (*)"));
			if (path.isEmpty()) {
				return;
			}
			if (mfd_engine_import_config_file(eng_, path.toUtf8().constData())) {
				mfd_engine_save_global_config(eng_);
				mfd_engine_notify_state_changed(eng_);
				populate_device_combos();
				last_adv_topology_rev_ = ~0ULL;
				refresh_from_engine();
			}
		});

		connect(btn_export, &QPushButton::clicked, this, [this]() {
			const QString path =
				QFileDialog::getSaveFileName(this, QStringLiteral("Export Midi Fadester Configuration"),
							     QStringLiteral("midi-fadester-config.json"),
							     QStringLiteral("JSON Files (*.json);;All Files (*)"));
			if (path.isEmpty()) {
				return;
			}
			mfd_engine_export_config_file(eng_, path.toUtf8().constData());
		});

		connect(device_combo_, QOverload<int>::of(&QComboBox::activated), this,
			[this](int) { apply_controller_selection(device_combo_->currentData().toString()); });
		connect(btn_reconnect, &QPushButton::clicked, this,
			[this]() { apply_controller_selection(device_combo_->currentData().toString()); });

		connect(profile_combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
			const auto chosen_prof =
				static_cast<enum mfd_controller_profile>(profile_combo_->currentData().toInt());
			manual_profile_override_ = true;
			manual_profile_ = chosen_prof;
			if (chosen_prof == MFD_PROFILE_APC40_MK2) {
				ensure_apc40_profile_bindings();
			}
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		});

		connect(btn_defaults, &QPushButton::clicked, this, [this]() {
			mfd_engine_load_apc40_defaults(eng_);
			manual_profile_override_ = true;
			manual_profile_ = MFD_PROFILE_APC40_MK2;
			mfd_engine_save_global_config(eng_);
			mfd_engine_notify_state_changed(eng_);
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		});

		connect(tau_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_smooth_tau_ms(eng_, static_cast<float>(val));
				mfd_engine_save_global_config(eng_);
			}
		});

		connect(flip_check_, &QCheckBox::toggled, this, [this](bool checked) {
			if (!rebuilding_adv_table_) {
				mfd_engine_set_tbar_bidirectional_flip(eng_, checked);
				mfd_engine_save_global_config(eng_);
			}
		});

		connect(btn_refresh_sources, &QPushButton::clicked, this, [this]() {
			last_adv_topology_rev_ = ~0ULL;
			refresh_from_engine();
		});
		connect(btn_close, &QPushButton::clicked, this, &QDialog::accept);

		main_tabs_->addTab(adv_tab, QStringLiteral("Advanced"));
	}

	void ensure_apc40_profile_bindings()
	{
		if (!eng_) {
			return;
		}
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		bool has_custom_target = false;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			const struct mfd_fader_slot &s = snap_.slots[i];
			if (s.source_a_name[0] != '\0' || s.source_b_name[0] != '\0') {
				has_custom_target = true;
				break;
			}
			if (i > 0 && s.target_type != MFD_TARGET_UNASSIGNED) {
				has_custom_target = true;
				break;
			}
		}
		if (!has_custom_target) {
			mfd_engine_load_apc40_defaults(eng_);
			mfd_engine_save_global_config(eng_);
			mfd_engine_notify_state_changed(eng_);
		}
	}

	void apply_controller_selection(const QString &chosen_port)
	{
		mfd_engine_reconnect_midi(eng_, chosen_port.toUtf8().constData());
		manual_profile_override_ = false;
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		const enum mfd_controller_profile prof = mfd_profile_for_device(
			snap_.configured_device_name, snap_.midi_connected, snap_.opened_device_name);
		if (prof == MFD_PROFILE_APC40_MK2) {
			ensure_apc40_profile_bindings();
		}
		mfd_engine_save_global_config(eng_);
		mfd_engine_notify_state_changed(eng_);
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
			cb->addItem(QStringLiteral("(Auto-Detect APC40 mkII / First Port)"), QStringLiteral(""));
			int select_idx = 0;
			for (size_t i = 0U; i < count; ++i) {
				const QString name = QString::fromUtf8(devs[i].name);
				const QString label = devs[i].is_apc40_mk2 ? QStringLiteral("%1 [APC40 mkII]").arg(name)
									   : name;
				cb->addItem(label, name);
				if (name == cfg_name) {
					select_idx = static_cast<int>(i) + 1;
				}
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

	void refresh_strips_tab()
	{
		struct mfd_ui_controller_presentation ctrl;
		mfd_ui_format_controller_status(snap_.midi_connected, snap_.configured_device_name,
						snap_.opened_device_name, snap_.midi_error_kind, &ctrl);

		z1_name_label_->setText(QStringLiteral("Controller: %1").arg(QString::fromUtf8(ctrl.display_name)));
		z1_dot_label_->setStyleSheet(ctrl.dot_green ? QStringLiteral("color: #22c55e; font-size: 14px;")
							    : QStringLiteral("color: #ef4444; font-size: 14px;"));
		z1_status_label_->setText(QString::fromUtf8(ctrl.status_sentence));
		z1_status_label_->setStyleSheet(ctrl.dot_green ? QStringLiteral("color: #6ee7b7; font-weight: bold;")
							       : QStringLiteral("color: #fca5a5; font-weight: bold;"));
		z1_btn_reconnect_->setVisible(ctrl.show_reconnect);

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_ui_strip_presentation sp;
			build_strip_presentation_from_slot(i, snap_.slots[i], &sp);
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

			const bool at_end = (std::strcmp(sp.state_readout, "At end.") == 0);
			strip_bars_[i]->set_state(sp.bar_percent, sp.dimmed, sp.severity, at_end, sp.show_pickup_marker,
						  sp.pickup_target_percent, QString::fromUtf8(sp.arrow_glyph));

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
				strip_assign_btns_[i]->setToolTip(assign_q);
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
				QStringLiteral("Dropped MIDI queue events: %1").arg(snap_.midi_dropped_count));
		} else {
			z3_status_label_->setToolTip(QString());
		}
	}

	bool any_adv_combo_popup_open() const
	{
		for (int r = 0; r < MFD_MAX_FADER_SLOTS; ++r) {
			if (adv_mode_combos_[r] && adv_mode_combos_[r]->view() &&
			    adv_mode_combos_[r]->view()->isVisible()) {
				return true;
			}
			if (adv_left_combos_[r] && adv_left_combos_[r]->view() &&
			    adv_left_combos_[r]->view()->isVisible()) {
				return true;
			}
			if (adv_right_combos_[r] && adv_right_combos_[r]->view() &&
			    adv_right_combos_[r]->view()->isVisible()) {
				return true;
			}
			if (adv_feel_combos_[r] && adv_feel_combos_[r]->view() &&
			    adv_feel_combos_[r]->view()->isVisible()) {
				return true;
			}
		}
		return false;
	}

	static void populate_source_combo(QComboBox *combo, const QStringList &sources, const char *current_name)
	{
		combo->blockSignals(true);
		combo->clear();
		combo->addItem(QStringLiteral("(None)"), QStringLiteral(""));
		const QString cur = QString::fromUtf8(current_name ? current_name : "");
		int found_idx = 0;
		for (int i = 0; i < sources.size(); ++i) {
			combo->addItem(sources[i], sources[i]);
			if (sources[i] == cur) {
				found_idx = i + 1;
			}
		}
		if (!cur.isEmpty() && found_idx == 0) {
			combo->addItem(QStringLiteral("%1 (missing)").arg(cur), cur);
			found_idx = combo->count() - 1;
		}
		combo->setCurrentIndex(found_idx);
		combo->blockSignals(false);
	}

	void collect_advanced_source_lists(QStringList *out_video, QStringList *out_audio)
	{
		QStringList vids = collect_live_obs_sources(true, false);
		QStringList auds = collect_live_obs_sources(false, true);

		if (adv_catalog_) {
			mfd_engine_get_ui_catalog(eng_, adv_catalog_);
			for (size_t si = 0U; si < adv_catalog_->scene_count; ++si) {
				const struct mfd_ui_scene_entry &se = adv_catalog_->scenes[si];
				for (size_t li = 0U; li < se.layer_count; ++li) {
					const QString lnm = QString::fromUtf8(se.layers[li]);
					if (!vids.contains(lnm)) {
						vids.append(lnm);
					}
				}
			}
			for (size_t ai = 0U; ai < adv_catalog_->audio_count; ++ai) {
				const QString anm = QString::fromUtf8(adv_catalog_->audio_sources[ai].name);
				if (!auds.contains(anm)) {
					auds.append(anm);
				}
			}
		}
		vids.sort(Qt::CaseInsensitive);
		auds.sort(Qt::CaseInsensitive);
		*out_video = vids;
		*out_audio = auds;
	}

	struct mfd_slot_config_edit make_edit_from_snapshot_row(int row, bool preserve_scene) const
	{
		const struct mfd_fader_slot &s = snap_.slots[row];
		struct mfd_slot_config_edit edit;
		std::memset(&edit, 0, sizeof(edit));
		edit.enabled = s.enabled;
		edit.target_type = s.target_type;
		edit.preserve_scene = preserve_scene;
		std::snprintf(edit.scene_name, sizeof(edit.scene_name), "%s", s.scene_name);
		std::snprintf(edit.source_a_name, sizeof(edit.source_a_name), "%s", s.source_a_name);
		std::snprintf(edit.source_b_name, sizeof(edit.source_b_name), "%s", s.source_b_name);
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

		if (profile_combo_ && !(profile_combo_->view() && profile_combo_->view()->isVisible())) {
			const enum mfd_controller_profile desired =
				manual_profile_override_
					? manual_profile_
					: mfd_profile_for_device(snap_.configured_device_name, snap_.midi_connected,
								 snap_.opened_device_name);
			const int idx = profile_combo_->findData(static_cast<int>(desired));
			if (idx >= 0) {
				profile_combo_->blockSignals(true);
				profile_combo_->setCurrentIndex(idx);
				profile_combo_->blockSignals(false);
			}
		}

		QStringList video_sources;
		QStringList audio_sources;
		collect_advanced_source_lists(&video_sources, &audio_sources);

		for (int row = 0; row < MFD_MAX_FADER_SLOTS; ++row) {
			const struct mfd_fader_slot &slot = snap_.slots[row];

			/* Column 0: On */
			QCheckBox *chk_on = nullptr;
			QWidget *on_cell = make_centered_checkbox_cell(table_, QStringLiteral("mfdAdvOn_%1").arg(row),
								       slot.enabled,
								       QStringLiteral("Enable or disable this strip."),
								       &chk_on);
			connect(chk_on, &QCheckBox::toggled, this, [this, row](bool on) {
				if (rebuilding_adv_table_) {
					return;
				}
				struct mfd_slot_config_edit edit = make_edit_from_snapshot_row(row, true);
				edit.enabled = on;
				mfd_engine_update_slot_config(eng_, row, &edit);
				refresh_from_engine();
			});
			table_->setCellWidget(row, 0, on_cell);

			/* Column 1: Strip */
			char st_title[64] = {0};
			mfd_ui_strip_title(row, slot.label, st_title, sizeof(st_title));
			auto *lbl_name = new QLabel(QString::fromUtf8(st_title), table_);
			lbl_name->setMargin(6);
			lbl_name->setStyleSheet(QStringLiteral("font-weight: bold;"));
			lbl_name->setToolTip(QStringLiteral("Slot ID: %1").arg(QString::fromUtf8(slot.id)));
			table_->setCellWidget(row, 1, lbl_name);

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
			auto *btn_clear = new QPushButton(QStringLiteral("\xC3\x97"), bind_widget);
			btn_clear->setObjectName(QStringLiteral("mfdAdvClear_%1").arg(row));
			btn_clear->setFixedWidth(26);
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
			table_->setCellWidget(row, 2, bind_widget);

			/* Column 3: Reverse direction */
			QCheckBox *chk_inv = nullptr;
			QWidget *inv_cell = make_centered_checkbox_cell(
				table_, QStringLiteral("mfdAdvInv_%1").arg(row), slot.invert,
				QStringLiteral("Reverse physical fader direction (0% \xE2\x86\x94 100%)."), &chk_inv);
			connect(chk_inv, &QCheckBox::toggled, this, [this, row](bool inverted) {
				if (rebuilding_adv_table_) {
					return;
				}
				struct mfd_slot_config_edit edit = make_edit_from_snapshot_row(row, true);
				edit.invert = inverted;
				mfd_engine_update_slot_config(eng_, row, &edit);
				refresh_from_engine();
			});
			table_->setCellWidget(row, 3, inv_cell);

			/* Column 4: What it does */
			auto *target_combo = new QComboBox(table_);
			target_combo->setObjectName(QStringLiteral("mfdAdvMode_%1").arg(row));
			target_combo->addItem(QStringLiteral("Not used"), static_cast<int>(MFD_TARGET_UNASSIGNED));
			target_combo->addItem(QStringLiteral("Switch scenes (T-bar)"),
					      static_cast<int>(MFD_TARGET_TBAR));
			target_combo->addItem(QStringLiteral("Mix two layers"),
					      static_cast<int>(MFD_TARGET_TWO_SOURCE_MIX));
			target_combo->addItem(QStringLiteral("Show or hide a layer"),
					      static_cast<int>(MFD_TARGET_SINGLE_LAYER));
			target_combo->addItem(QStringLiteral("Volume"), static_cast<int>(MFD_TARGET_AUDIO_VOLUME));
			target_combo->setCurrentIndex(target_combo->findData(static_cast<int>(slot.target_type)));
			adv_mode_combos_[row] = target_combo;
			table_->setCellWidget(row, 4, target_combo);

			bool show_a = false;
			bool show_b = false;
			bool show_curve = false;
			bool show_afv = false;
			mfd_row_controls_for_target(slot.target_type, &show_a, &show_b, &show_curve, &show_afv);

			/* Column 5: Left */
			QComboBox *src_a_combo = nullptr;
			if (show_a) {
				src_a_combo = new QComboBox(table_);
				src_a_combo->setObjectName(QStringLiteral("mfdAdvLeft_%1").arg(row));
				src_a_combo->setMaximumWidth(175);
				const bool is_audio_only = (slot.target_type == MFD_TARGET_AUDIO_VOLUME);
				populate_source_combo(src_a_combo, is_audio_only ? audio_sources : video_sources,
						      slot.source_a_name);
				table_->setCellWidget(row, 5, src_a_combo);
			} else {
				table_->setCellWidget(row, 5, new QWidget(table_));
			}
			adv_left_combos_[row] = src_a_combo;

			/* Column 6: Right */
			QComboBox *src_b_combo = nullptr;
			if (show_b) {
				src_b_combo = new QComboBox(table_);
				src_b_combo->setObjectName(QStringLiteral("mfdAdvRight_%1").arg(row));
				src_b_combo->setMaximumWidth(175);
				populate_source_combo(src_b_combo, video_sources, slot.source_b_name);
				table_->setCellWidget(row, 6, src_b_combo);
			} else {
				table_->setCellWidget(row, 6, new QWidget(table_));
			}
			adv_right_combos_[row] = src_b_combo;

			/* Column 7: Feel */
			QComboBox *curve_combo = nullptr;
			if (show_curve) {
				curve_combo = new QComboBox(table_);
				curve_combo->setObjectName(QStringLiteral("mfdAdvFeel_%1").arg(row));
				curve_combo->setMaximumWidth(170);
				curve_combo->addItem(QStringLiteral("Dip-free"), static_cast<int>(MFD_CURVE_DIP_FREE));
				curve_combo->addItem(QStringLiteral("Full-center"),
						     static_cast<int>(MFD_CURVE_FULL_CENTER));
				curve_combo->addItem(QStringLiteral("Equal power"), static_cast<int>(MFD_CURVE_COSINE));
				curve_combo->addItem(QStringLiteral("Linear"), static_cast<int>(MFD_CURVE_LINEAR));
				curve_combo->addItem(QStringLiteral("Cut"), static_cast<int>(MFD_CURVE_CUT));
				curve_combo->setCurrentIndex(curve_combo->findData(static_cast<int>(slot.curve)));
				table_->setCellWidget(row, 7, curve_combo);
			} else {
				table_->setCellWidget(row, 7, new QWidget(table_));
			}
			adv_feel_combos_[row] = curve_combo;

			/* Column 8: Also fade audio */
			QCheckBox *chk_afv = nullptr;
			if (show_afv) {
				QWidget *afv_cell = make_centered_checkbox_cell(
					table_, QStringLiteral("mfdAdvAfv_%1").arg(row), slot.audio_follows_video,
					QStringLiteral("Also fade audio volume alongside layer opacity."), &chk_afv);
				table_->setCellWidget(row, 8, afv_cell);
			} else {
				table_->setCellWidget(row, 8, new QWidget(table_));
			}

			/* Column 9: Output */
			auto *out_bar = new QProgressBar(table_);
			out_bar->setObjectName(QStringLiteral("mfdAdvOutput_%1").arg(row));
			out_bar->setRange(0, 100);
			out_bar->setValue(0);
			out_bar->setTextVisible(true);
			out_bar->setAlignment(Qt::AlignCenter);
			out_bar->setMinimumWidth(165);
			output_bars_[row] = out_bar;
			table_->setCellWidget(row, 9, out_bar);

			connect(target_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this, target_combo, row](int) {
					if (rebuilding_adv_table_) {
						return;
					}
					struct mfd_slot_config_edit edit = make_edit_from_snapshot_row(row, true);
					edit.target_type =
						static_cast<enum mfd_target_type>(target_combo->currentData().toInt());
					mfd_engine_update_slot_config(eng_, row, &edit);
					last_adv_topology_rev_ = ~0ULL;
					refresh_from_engine();
				});

			if (src_a_combo) {
				connect(src_a_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, src_a_combo, row](int) {
						if (rebuilding_adv_table_) {
							return;
						}
						struct mfd_slot_config_edit edit =
							make_edit_from_snapshot_row(row, true);
						const QByteArray utf8 = src_a_combo->currentData().toString().toUtf8();
						std::snprintf(edit.source_a_name, sizeof(edit.source_a_name), "%s",
							      utf8.constData());
						mfd_engine_update_slot_config(eng_, row, &edit);
						refresh_from_engine();
					});
			}

			if (src_b_combo) {
				connect(src_b_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, src_b_combo, row](int) {
						if (rebuilding_adv_table_) {
							return;
						}
						struct mfd_slot_config_edit edit =
							make_edit_from_snapshot_row(row, true);
						const QByteArray utf8 = src_b_combo->currentData().toString().toUtf8();
						std::snprintf(edit.source_b_name, sizeof(edit.source_b_name), "%s",
							      utf8.constData());
						mfd_engine_update_slot_config(eng_, row, &edit);
						refresh_from_engine();
					});
			}

			if (curve_combo) {
				connect(curve_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, curve_combo, row](int) {
						if (rebuilding_adv_table_) {
							return;
						}
						struct mfd_slot_config_edit edit =
							make_edit_from_snapshot_row(row, true);
						edit.curve = static_cast<enum mfd_crossfade_curve>(
							curve_combo->currentData().toInt());
						mfd_engine_update_slot_config(eng_, row, &edit);
						refresh_from_engine();
					});
			}

			if (chk_afv) {
				connect(chk_afv, &QCheckBox::toggled, this, [this, row](bool checked) {
					if (rebuilding_adv_table_) {
						return;
					}
					struct mfd_slot_config_edit edit = make_edit_from_snapshot_row(row, true);
					edit.audio_follows_video = checked;
					mfd_engine_update_slot_config(eng_, row, &edit);
					refresh_from_engine();
				});
			}
		}

		for (int c = 0; c < 9; ++c) {
			table_->resizeColumnToContents(c);
		}
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

	void refresh_advanced_tab(bool can_finish_tbar)
	{
		if (snap_.config_topology_rev != last_adv_topology_rev_ && !any_adv_combo_popup_open()) {
			rebuild_table_rows();
		}

		struct mfd_ui_controller_presentation ctrl;
		mfd_ui_format_controller_status(snap_.midi_connected, snap_.configured_device_name,
						snap_.opened_device_name, snap_.midi_error_kind, &ctrl);

		if (ctrl.dot_green) {
			set_label_text_and_style(
				conn_status_label_,
				QStringLiteral("\xE2\x97\x8F Connected: %1").arg(QString::fromUtf8(ctrl.display_name)),
				QStringLiteral("QLabel { background-color: #064e3b; color: #6ee7b7; padding: 4px 10px; "
					       "border: 1px solid #10b981; border-radius: 4px; font-weight: bold; }"));
			banner_label_->hide();
		} else {
			set_label_text_and_style(
				conn_status_label_, QString::fromUtf8(ctrl.status_sentence),
				QStringLiteral("QLabel { background-color: #451a03; color: #fcd34d; padding: 4px 10px; "
					       "border: 1px solid #d97706; border-radius: 4px; font-weight: bold; }"));
			if (snap_.midi_last_error[0] != '\0') {
				banner_label_->setText(QString::fromUtf8(snap_.midi_last_error));
				banner_label_->show();
			} else {
				banner_label_->hide();
			}
		}

		if (snap_.studio_mode_active) {
			const QString prev_nm = (snap_.preview_scene_name[0] != '\0')
							? QString::fromUtf8(snap_.preview_scene_name)
							: QStringLiteral("\xE2\x80\x94");
			const QString prog_nm = (snap_.program_scene_name[0] != '\0')
							? QString::fromUtf8(snap_.program_scene_name)
							: QStringLiteral("\xE2\x80\x94");
			set_label_text_and_style(
				studio_status_label_,
				QStringLiteral("\xE2\x97\x8F Studio Mode on: Preview [%1] \xE2\x86\x92 Program [%2]")
					.arg(prev_nm, prog_nm),
				QStringLiteral("QLabel { background-color: #172554; color: #bfdbfe; padding: 4px 10px; "
					       "border: 1px solid #2563eb; border-radius: 4px; font-weight: bold; }"));
		} else {
			set_label_text_and_style(
				studio_status_label_, QStringLiteral("\xE2\x97\x8B Studio Mode off"),
				QStringLiteral("QLabel { background-color: #1f2937; color: #9ca3af; padding: 4px 10px; "
					       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		}

		btn_finish_tbar_->setEnabled(can_finish_tbar);

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

			QProgressBar *bar = output_bars_[row];
			if (!bar) {
				continue;
			}

			struct mfd_ui_strip_presentation sp;
			build_strip_presentation_from_slot(row, s, &sp);

			const int desired_pct = sp.bar_percent;
			QString desired_text;
			QString desired_style = style_disabled;
			QString desired_tip;

			if (!s.enabled) {
				desired_text = QStringLiteral("Off");
				desired_tip = QStringLiteral("Strip is turned off.");
			} else if (s.target_type == MFD_TARGET_UNASSIGNED) {
				desired_text = s.takeover.has_last_fader
						       ? QString::asprintf("%d%% (Not used)", desired_pct)
						       : QStringLiteral("Not used");
				desired_tip = QStringLiteral("Choose What it does to activate this strip.");
			} else if (!s.cached_available) {
				desired_text = QString::asprintf("%d%% \xC2\xB7 %s", desired_pct, sp.status_sentence);
				desired_style = style_unavail;
				desired_tip = QString::fromUtf8(sp.status_sentence);
			} else {
				if (sp.status_sentence[0] != '\0') {
					desired_text =
						QString::asprintf("%d%% \xC2\xB7 %s", desired_pct, sp.status_sentence);
					desired_style = style_waiting;
					desired_tip = QString::fromUtf8(sp.status_sentence);
				} else if (std::strcmp(sp.state_readout, "At end.") == 0) {
					desired_text = QString::asprintf("%d%% \xC2\xB7 At end.", desired_pct);
					desired_style = style_parked;
					desired_tip = QStringLiteral("At end (parked at rail).");
				} else {
					desired_text =
						(sp.state_readout[0] != '\0')
							? QString::asprintf("%d%% (%s)", desired_pct, sp.state_readout)
							: QString::asprintf("%d%%", desired_pct);
					desired_style = style_engaged;
					desired_tip = QString::fromUtf8(sp.assignment_sentence);
				}
			}

			if (bar->value() != desired_pct) {
				bar->setValue(desired_pct);
			}
			if (bar->format() != desired_text) {
				bar->setFormat(desired_text);
			}
			if (bar->styleSheet() != desired_style) {
				bar->setStyleSheet(desired_style);
			}
			if (bar->toolTip() != desired_tip) {
				bar->setToolTip(desired_tip);
			}
		}
	}

	struct mfd_engine *eng_ = nullptr;
	bool owns_engine_notify_ = false;
	std::atomic<bool> local_refresh_queued_{false};
	struct mfd_ui_snapshot snap_ {};
	struct mfd_ui_catalog *adv_catalog_ = nullptr;
	uint64_t last_adv_topology_rev_ = ~0ULL;
	bool rebuilding_adv_table_ = false;
	bool last_midi_connected_ = false;
	bool manual_profile_override_ = false;
	enum mfd_controller_profile manual_profile_ = MFD_PROFILE_GENERIC;

	QWidget *first_run_banner_ = nullptr;
	QWidget *studio_note_widget_ = nullptr;
	QTabWidget *main_tabs_ = nullptr;

	/* Strips tab widgets */
	QLabel *z1_name_label_ = nullptr;
	QLabel *z1_dot_label_ = nullptr;
	QLabel *z1_status_label_ = nullptr;
	QPushButton *z1_btn_reconnect_ = nullptr;
	QWidget *z1_change_panel_ = nullptr;
	QComboBox *zone1_device_combo_ = nullptr;
	QPushButton *z1_btn_change_ctrl_ = nullptr;
	QPushButton *z1_btn_show_ctrl_ = nullptr;

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
	QLabel *conn_status_label_ = nullptr;
	QLabel *studio_status_label_ = nullptr;
	QPushButton *btn_finish_tbar_ = nullptr;
	QLabel *last_midi_label_ = nullptr;
	QComboBox *device_combo_ = nullptr;
	QComboBox *profile_combo_ = nullptr;
	QDoubleSpinBox *tau_spin_ = nullptr;
	QCheckBox *flip_check_ = nullptr;
	QTableWidget *table_ = nullptr;
	QPointer<QLabel> addr_labels_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> learn_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> clear_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QComboBox> adv_mode_combos_[MFD_MAX_FADER_SLOTS];
	QPointer<QComboBox> adv_left_combos_[MFD_MAX_FADER_SLOTS];
	QPointer<QComboBox> adv_right_combos_[MFD_MAX_FADER_SLOTS];
	QPointer<QComboBox> adv_feel_combos_[MFD_MAX_FADER_SLOTS];
	QPointer<QProgressBar> output_bars_[MFD_MAX_FADER_SLOTS];
};

class MfdUiDispatcher final : public QObject {
public:
	explicit MfdUiDispatcher(QObject *parent = nullptr) : QObject(parent) {}
	void on_drain();
};

static std::mutex g_ui_bridge_mu;
static std::atomic<bool> g_ui_refresh_queued{false};
static MfdUiDispatcher *g_ui_dispatcher = nullptr;
static QPointer<MfdSettingsDialog> g_dialog;

void MfdUiDispatcher::on_drain()
{
	g_ui_refresh_queued.store(false);
	mfd_engine_flush_pending_ui_work(mfd_engine_get());
	if (g_dialog) {
		g_dialog->refresh_from_engine();
	}
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
			if (msg->message == WM_DEVICECHANGE) {
				mfd_engine_poll_midi_health_on_ui_thread(mfd_engine_get());
				if (g_dialog) {
					g_dialog->on_hardware_ports_changed();
					g_dialog->refresh_from_engine();
				}
			}
		}
		return false;
	}
};

static MfdDeviceChangeFilter *g_dev_change_filter = nullptr;

static void on_tools_menu_clicked(void *unused)
{
	UNUSED_PARAMETER(unused);
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

} // namespace

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

	if (!qEnvironmentVariableIsSet("MFD_SELF_TEST")) {
		if (QCoreApplication::instance() && !g_dev_change_filter) {
			g_dev_change_filter = new MfdDeviceChangeFilter();
			QCoreApplication::instance()->installNativeEventFilter(g_dev_change_filter);
		}
		mfd_engine_poll_midi_health_on_ui_thread(mfd_engine_get());
	}
}

void mfd_ui_shutdown(void)
{
	mfd_engine_set_ui_notify_fn(mfd_engine_get(), nullptr, nullptr);

	if (g_dev_change_filter) {
		if (QCoreApplication::instance()) {
			QCoreApplication::instance()->removeNativeEventFilter(g_dev_change_filter);
		}
		delete g_dev_change_filter;
		g_dev_change_filter = nullptr;
	}

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
