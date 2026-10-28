/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-settings-dialog.hpp"
#include "obs/mfd-engine.h"
#include "obs/mfd-tbar.h"
#include "core/tbar-logic.h"
#include "core/mixer-math.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstdio>

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

static QStringList collect_scene_sources(bool require_video, bool require_audio)
{
	struct enum_ctx {
		QStringList names;
		bool req_video;
		bool req_audio;
	} ctx{QStringList{}, require_video, require_audio};

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

static QWidget *make_centered_checkbox_cell(QWidget *parent, bool checked, const QString &tooltip,
					    QCheckBox **out_checkbox)
{
	auto *container = new QWidget(parent);
	auto *layout = new QHBoxLayout(container);
	layout->setContentsMargins(4, 0, 4, 0);
	layout->setAlignment(Qt::AlignCenter);
	auto *chk = new QCheckBox(container);
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

class MfdSettingsDialog final : public QDialog {
public:
	explicit MfdSettingsDialog(QWidget *parent = nullptr) : QDialog(parent)
	{
		setWindowTitle(QStringLiteral("Midi Fadester %1 (commit %2, built %3)")
				       .arg(QString::fromUtf8(PLUGIN_VERSION), QString::fromUtf8(PLUGIN_GIT_COMMIT),
					    QString::fromUtf8(PLUGIN_BUILT_AT)));
		resize(1360, 600);

		auto *root = new QVBoxLayout(this);
		root->setSpacing(8);

		banner_label_ = new QLabel(this);
		banner_label_->setWordWrap(true);
		banner_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #5a1d1d; color: #ffd6d6; padding: 8px 10px; "
				       "border: 1px solid #d9534f; border-radius: 4px; font-weight: bold; }"));
		banner_label_->hide();
		root->addWidget(banner_label_);

		studio_note_widget_ = new QWidget(this);
		studio_note_widget_->setStyleSheet(
			QStringLiteral("QWidget#mfdStudioNote { background-color: #4d3800; border: 1px solid #d97706; "
				       "border-radius: 4px; }"));
		studio_note_widget_->setObjectName(QStringLiteral("mfdStudioNote"));
		auto *note_layout = new QHBoxLayout(studio_note_widget_);
		note_layout->setContentsMargins(10, 6, 10, 6);
		auto *note_label = new QLabel(
			QStringLiteral(
				"Note: Studio Mode is currently OFF. Enable Studio Mode to use the Studio Mode T-Bar target."),
			studio_note_widget_);
		note_label->setStyleSheet(
			QStringLiteral("color: #fde68a; font-weight: bold; background: transparent; border: none;"));
		note_label->setWordWrap(true);
		auto *btn_turn_on_studio = new QPushButton(QStringLiteral("Turn on Studio Mode"), studio_note_widget_);
		btn_turn_on_studio->setStyleSheet(
			QStringLiteral("QPushButton { background-color: #d97706; color: #111827; font-weight: bold; "
				       "padding: 4px 12px; border-radius: 3px; }"));
		note_layout->addWidget(note_label, 1);
		note_layout->addWidget(btn_turn_on_studio);
		studio_note_widget_->hide();
		root->addWidget(studio_note_widget_);

		connect(btn_turn_on_studio, &QPushButton::clicked, this, [this]() {
			obs_frontend_set_preview_program_mode(true);
			update_live_status();
		});

		/* Top hardware & feel configuration bar */
		auto *top_bar = new QHBoxLayout();
		top_bar->addWidget(new QLabel(QStringLiteral("MIDI In:"), this));

		device_combo_ = new QComboBox(this);
		device_combo_->setMinimumWidth(240);
		device_combo_->setToolTip(
			QStringLiteral("Select a WinMM MIDI input controller or auto-detect an Akai APC40 mkII."));
		top_bar->addWidget(device_combo_);

		conn_status_label_ = new QLabel(QStringLiteral("Checking..."), this);
		top_bar->addWidget(conn_status_label_);

		auto *btn_reconnect = new QPushButton(QStringLiteral("Reconnect / Rescan"), this);
		btn_reconnect->setToolTip(
			QStringLiteral("Rescan WinMM MIDI ports and reconnect to the selected device."));
		top_bar->addWidget(btn_reconnect);

		top_bar->addSpacing(8);
		top_bar->addWidget(new QLabel(QStringLiteral("Profile:"), this));

		profile_combo_ = new QComboBox(this);
		profile_combo_->addItem(QStringLiteral("Generic MIDI (Custom)"), static_cast<int>(MFD_PROFILE_GENERIC));
		profile_combo_->addItem(QStringLiteral("Akai APC40 mkII"), static_cast<int>(MFD_PROFILE_APC40_MK2));
		profile_combo_->setToolTip(QStringLiteral(
			"Controller profile (auto-selects Akai APC40 mkII when an APC40 mkII port is active, or choose manually)."));
		top_bar->addWidget(profile_combo_);

		auto *btn_defaults = new QPushButton(QStringLiteral("Load APC40 mkII Defaults"), this);
		btn_defaults->setToolTip(QStringLiteral(
			"Reset all 10 slots to the Akai APC40 mkII default map (Crossfader + Faders 1–8 + Master)."));
		top_bar->addWidget(btn_defaults);

		top_bar->addSpacing(14);
		top_bar->addWidget(new QLabel(QStringLiteral("Smoothing (\xCF\x84):"), this));

		tau_spin_ = new QDoubleSpinBox(this);
		tau_spin_->setRange(0.0, 250.0);
		tau_spin_->setSingleStep(5.0);
		tau_spin_->setDecimals(1);
		tau_spin_->setSuffix(QStringLiteral(" ms"));
		tau_spin_->setToolTip(QStringLiteral(
			"Exponential slew time constant (0–250 ms). Default 30 ms; 0 ms = instant bypass."));
		top_bar->addWidget(tau_spin_);

		top_bar->addSpacing(10);
		flip_check_ = new QCheckBox(QStringLiteral("Bidirectional T-Bar (0\xE2\x86\x92"
							   "1 then 1\xE2\x86\x92"
							   "0)"),
					    this);
		flip_check_->setToolTip(QStringLiteral(
			"Alternate physical T-bar throw polarity after each completed transition so every throw starts from its resting rail."));
		top_bar->addWidget(flip_check_);
		top_bar->addStretch(1);

		root->addLayout(top_bar);

		/* Live telemetry & quick-action strip */
		auto *telemetry_bar = new QHBoxLayout();
		studio_status_label_ = new QLabel(QStringLiteral("Studio Mode: Off"), this);
		telemetry_bar->addWidget(studio_status_label_);

		btn_finish_tbar_ = new QPushButton(QStringLiteral("Finish T-Bar"), this);
		btn_finish_tbar_->setToolTip(
			QStringLiteral("Complete an in-progress manual T-bar transition one-shot and park at rail."));
		btn_finish_tbar_->setEnabled(false);
		telemetry_bar->addWidget(btn_finish_tbar_);

		telemetry_bar->addStretch(1);

		last_midi_label_ = new QLabel(QStringLiteral("No MIDI received yet"), this);
		last_midi_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #1f2937; color: #93c5fd; padding: 4px 10px; "
				       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		telemetry_bar->addWidget(last_midi_label_);

		root->addLayout(telemetry_bar);

		connect(btn_finish_tbar_, &QPushButton::clicked, this, [this]() {
			struct mfd_engine *eng = mfd_engine_get();
			for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
				struct mfd_fader_slot *slot = &eng->fader_slots[i];
				if (slot->enabled && slot->target_type == MFD_TARGET_TBAR &&
				    slot->tbar_state == MFD_TBAR_THROWING) {
					slot->auto_fade_active = false;
					slot->tbar_midi_active = false;
					mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
					mfd_engine_notify_fader_changed(eng, i, "api");
					mfd_engine_notify_state_changed(eng);
					break;
				}
			}
			update_live_status();
		});

		table_ = new QTableWidget(MFD_MAX_FADER_SLOTS, 10, this);
		table_->setHorizontalHeaderLabels({
			QStringLiteral("On"),
			QStringLiteral("Fader / Strip"),
			QStringLiteral("MIDI Binding"),
			QStringLiteral("Inv"),
			QStringLiteral("Target Mode"),
			QStringLiteral("Source A (Bottom / Primary)"),
			QStringLiteral("Source B (Top Layer)"),
			QStringLiteral("Crossfade Curve"),
			QStringLiteral("AFV"),
			QStringLiteral("Output"),
		});
		table_->horizontalHeader()->setStretchLastSection(true);
		table_->verticalHeader()->setVisible(false);
		table_->verticalHeader()->setDefaultSectionSize(36);
		table_->setAlternatingRowColors(true);
		table_->setSelectionMode(QAbstractItemView::NoSelection);
		table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		root->addWidget(table_, 1);

		auto *footer = new QHBoxLayout();
		auto *hint = new QLabel(
			QStringLiteral(
				"Tip \xE2\x80\x94 Studio Mode T-Bar vs. Two-Source Crossfade (A \xE2\x86\x94 B): "
				"Studio Mode T-Bar drives Preview \xE2\x86\x92 Program linearly from 0\xE2\x80\x93"
				"100% and completes only when the fader enters the calibrated 3% target rail zone "
				"(returning to the origin 3% rail cancels the throw cleanly), "
				"swapping the old scene into the left Preview box while the live Program output on the right stays smooth. "
				"For a DJ/video-mixer crossfader where Left is always Source A and Right is always Source B "
				"(0\xE2\x80\x93"
				"100% linear, no Preview swap, 0% black dip), stack Source A below Source B in one scene "
				"and pick Two-Source Crossfade (A \xE2\x86\x94 B)."),
			this);
		hint->setWordWrap(true);
		footer->addWidget(hint, 1);

		auto *btn_import = new QPushButton(QStringLiteral("Import..."), this);
		btn_import->setToolTip(QStringLiteral("Load fader bindings and settings from a JSON file."));
		footer->addWidget(btn_import);

		auto *btn_export = new QPushButton(QStringLiteral("Export..."), this);
		btn_export->setToolTip(QStringLiteral("Save current fader bindings and settings to a JSON file."));
		footer->addWidget(btn_export);

		auto *btn_refresh_sources = new QPushButton(QStringLiteral("Refresh Sources"), this);
		btn_refresh_sources->setToolTip(QStringLiteral("Reload current video and audio source lists."));
		footer->addWidget(btn_refresh_sources);

		auto *btn_close = new QPushButton(QStringLiteral("Close"), this);
		footer->addWidget(btn_close);
		root->addLayout(footer);

		connect(btn_import, &QPushButton::clicked, this, [this]() {
			const QString path = QFileDialog::getOpenFileName(
				this, QStringLiteral("Import Midi Fadester Configuration"), QString(),
				QStringLiteral("JSON Files (*.json);;All Files (*)"));
			if (path.isEmpty()) {
				return;
			}
			struct mfd_engine *eng = mfd_engine_get();
			if (mfd_engine_import_config_file(eng, path.toUtf8().constData())) {
				mfd_engine_save_global_config(eng);
				mfd_engine_notify_state_changed(eng);
				populate_device_combo();
				sync_header_controls();
				rebuild_table_rows();
				update_live_status();
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
			struct mfd_engine *eng = mfd_engine_get();
			mfd_engine_export_config_file(eng, path.toUtf8().constData());
		});

		auto apply_device_selection = [this]() {
			struct mfd_engine *eng = mfd_engine_get();
			const QString chosen = device_combo_->currentData().toString();
			mfd_engine_reconnect_midi(eng, chosen.toUtf8().constData());
			manual_profile_override_ = false;
			const enum mfd_controller_profile prof = mfd_profile_for_device(
				eng->configured_device_name, eng->midi_dev.is_open, eng->midi_dev.opened_name);
			if (prof == MFD_PROFILE_APC40_MK2) {
				ensure_apc40_profile_bindings(eng);
			}
			mfd_engine_save_global_config(eng);
			mfd_engine_notify_state_changed(eng);
			populate_device_combo();
			sync_profile_combo();
			rebuild_table_rows();
			update_live_status();
		};

		connect(device_combo_, QOverload<int>::of(&QComboBox::activated), this,
			[apply_device_selection](int) { apply_device_selection(); });

		connect(btn_reconnect, &QPushButton::clicked, this,
			[apply_device_selection]() { apply_device_selection(); });

		connect(profile_combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
			struct mfd_engine *eng = mfd_engine_get();
			const auto chosen_prof =
				static_cast<enum mfd_controller_profile>(profile_combo_->currentData().toInt());
			manual_profile_override_ = true;
			manual_profile_ = chosen_prof;
			if (chosen_prof == MFD_PROFILE_APC40_MK2) {
				ensure_apc40_profile_bindings(eng);
				mfd_engine_save_global_config(eng);
				mfd_engine_notify_state_changed(eng);
				rebuild_table_rows();
			}
			update_live_status();
		});

		connect(btn_defaults, &QPushButton::clicked, this, [this]() {
			struct mfd_engine *eng = mfd_engine_get();
			mfd_engine_load_apc40_defaults(eng);
			manual_profile_override_ = true;
			manual_profile_ = MFD_PROFILE_APC40_MK2;
			mfd_engine_save_global_config(eng);
			mfd_engine_notify_state_changed(eng);
			sync_profile_combo();
			rebuild_table_rows();
			update_live_status();
		});

		connect(tau_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [](double val) {
			struct mfd_engine *eng = mfd_engine_get();
			mfd_engine_set_smooth_tau_ms(eng, static_cast<float>(val));
			mfd_engine_save_global_config(eng);
		});

		connect(flip_check_, &QCheckBox::toggled, this, [](bool checked) {
			struct mfd_engine *eng = mfd_engine_get();
			mfd_engine_set_tbar_bidirectional_flip(eng, checked);
			mfd_engine_save_global_config(eng);
		});

		connect(btn_refresh_sources, &QPushButton::clicked, this, [this]() {
			rebuild_table_rows();
			update_live_status();
		});
		connect(btn_close, &QPushButton::clicked, this, &QDialog::accept);

		last_midi_connected_ = mfd_engine_get()->midi_dev.is_open;
		populate_device_combo();
		sync_header_controls();
		rebuild_table_rows();

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, [this]() { update_live_status(); });
		timer_->start(33);
		update_live_status();
	}

private:
	static void ensure_apc40_profile_bindings(struct mfd_engine *eng)
	{
		if (!eng) {
			return;
		}
		bool has_custom_target = false;
		for (size_t i = 0U; i < MFD_MAX_FADER_SLOTS; ++i) {
			const struct mfd_fader_slot &s = eng->fader_slots[i];
			if (s.source_a_name[0] != '\0' || s.source_b_name[0] != '\0') {
				has_custom_target = true;
				break;
			}
			if (i > 0U && s.target_type != MFD_TARGET_UNASSIGNED) {
				has_custom_target = true;
				break;
			}
		}
		if (!has_custom_target) {
			mfd_engine_load_apc40_defaults(eng);
			return;
		}
		struct mfd_apc40_default_slot defs[MFD_APC40_DEFAULT_SLOT_COUNT];
		const size_t count = mfd_apc40_get_default_slots(defs, MFD_APC40_DEFAULT_SLOT_COUNT);
		for (size_t i = 0U; i < count && i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *s = &eng->fader_slots[i];
			if (s->addr.type == MFD_MIDI_NONE) {
				s->addr = defs[i].addr;
			}
			std::snprintf(s->label, sizeof(s->label), "%s", defs[i].label);
		}
	}

	void sync_profile_combo()
	{
		if (!profile_combo_ || (profile_combo_->view() && profile_combo_->view()->isVisible())) {
			return;
		}
		const struct mfd_engine *eng = mfd_engine_get();
		const enum mfd_controller_profile desired =
			manual_profile_override_
				? manual_profile_
				: mfd_profile_for_device(eng->configured_device_name, eng->midi_dev.is_open,
							 eng->midi_dev.opened_name);
		const int idx = profile_combo_->findData(static_cast<int>(desired));
		if (idx >= 0 && profile_combo_->currentIndex() != idx) {
			profile_combo_->blockSignals(true);
			profile_combo_->setCurrentIndex(idx);
			profile_combo_->blockSignals(false);
		}
	}

	void populate_device_combo()
	{
		struct mfd_engine *eng = mfd_engine_get();
		device_combo_->blockSignals(true);
		device_combo_->clear();

		struct mfd_midi_device_info devs[MFD_MAX_MIDI_DEVICES];
		size_t count = mfd_winmm_enumerate(devs, MFD_MAX_MIDI_DEVICES);
		last_device_count_ = count;

		device_combo_->addItem(QStringLiteral("(Auto-Detect APC40 mkII / First Port)"), QStringLiteral(""));
		int select_idx = 0;
		for (size_t i = 0U; i < count; ++i) {
			const QString name = QString::fromUtf8(devs[i].name);
			const QString label = devs[i].is_apc40_mk2 ? QStringLiteral("%1 [APC40 mkII]").arg(name) : name;
			device_combo_->addItem(label, name);
			if (name == QString::fromUtf8(eng->configured_device_name)) {
				select_idx = static_cast<int>(i) + 1;
			}
		}
		device_combo_->setCurrentIndex(select_idx);
		device_combo_->blockSignals(false);
	}

	void sync_header_controls()
	{
		struct mfd_engine *eng = mfd_engine_get();
		tau_spin_->blockSignals(true);
		tau_spin_->setValue(static_cast<double>(eng->smooth_tau_ms));
		tau_spin_->blockSignals(false);

		flip_check_->blockSignals(true);
		flip_check_->setChecked(eng->tbar_bidirectional_flip);
		flip_check_->blockSignals(false);

		sync_profile_combo();
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

	void rebuild_table_rows()
	{
		struct mfd_engine *eng = mfd_engine_get();
		const QStringList video_sources = collect_scene_sources(true, false);
		const QStringList audio_sources = collect_scene_sources(false, true);

		for (int row = 0; row < MFD_MAX_FADER_SLOTS; ++row) {
			struct mfd_fader_slot *slot = &eng->fader_slots[row];

			/* Column 0: On */
			QCheckBox *chk_on = nullptr;
			QWidget *on_cell = make_centered_checkbox_cell(
				table_, slot->enabled, QStringLiteral("Enable or disable this fader slot."), &chk_on);
			connect(chk_on, &QCheckBox::toggled, this, [this, row](bool on) {
				struct mfd_engine *e = mfd_engine_get();
				e->fader_slots[row].enabled = on;
				mfd_engine_reevaluate_slots(e, false);
				mfd_engine_save_global_config(e);
				mfd_engine_notify_state_changed(e);
				update_live_status();
			});
			table_->setCellWidget(row, 0, on_cell);

			/* Column 1: Fader / Strip */
			auto *lbl_name = new QLabel(QString::fromUtf8(slot->label), table_);
			lbl_name->setMargin(6);
			lbl_name->setStyleSheet(QStringLiteral("font-weight: bold;"));
			lbl_name->setToolTip(QStringLiteral("API Slot ID: %1").arg(QString::fromUtf8(slot->id)));
			table_->setCellWidget(row, 1, lbl_name);

			/* Column 2: MIDI Binding (Addr + Learn + Clear) */
			auto *bind_widget = new QWidget(table_);
			auto *bind_layout = new QHBoxLayout(bind_widget);
			bind_layout->setContentsMargins(6, 2, 6, 2);
			bind_layout->setSpacing(4);
			auto *lbl_addr = new QLabel(format_midi_addr(slot->addr), bind_widget);
			auto *btn_learn = new QPushButton(QString::fromUtf8(mfd_learn_button_text(false)), bind_widget);
			btn_learn->setMinimumWidth(112);
			btn_learn->setToolTip(QStringLiteral(
				"Click then move any MIDI fader/knob to bind its channel and CC (auto-detects 14-bit CC pairs)."));
			auto *btn_clear = new QPushButton(QStringLiteral("\xC3\x97"), bind_widget);
			btn_clear->setFixedWidth(26);
			btn_clear->setToolTip(QStringLiteral("Clear MIDI binding for this slot."));
			btn_clear->setEnabled(slot->addr.type != MFD_MIDI_NONE);

			bind_layout->addWidget(lbl_addr, 1);
			bind_layout->addWidget(btn_learn);
			bind_layout->addWidget(btn_clear);
			addr_labels_[row] = lbl_addr;
			learn_buttons_[row] = btn_learn;
			clear_buttons_[row] = btn_clear;

			connect(btn_learn, &QPushButton::clicked, this, [this, row]() {
				struct mfd_engine *e = mfd_engine_get();
				if (e->learning_slot_index == row) {
					mfd_engine_cancel_learn(e);
				} else {
					mfd_engine_start_learn(e, row);
				}
				update_live_status();
			});

			connect(btn_clear, &QPushButton::clicked, this, [this, row]() {
				struct mfd_engine *e = mfd_engine_get();
				if (e->learning_slot_index == row) {
					mfd_engine_cancel_learn(e);
				}
				e->fader_slots[row].addr.type = MFD_MIDI_NONE;
				e->fader_slots[row].addr.channel = 0U;
				e->fader_slots[row].addr.number = 0U;
				e->fader_slots[row].addr.is_14bit = false;
				mfd_engine_save_global_config(e);
				mfd_engine_notify_state_changed(e);
				update_live_status();
			});
			table_->setCellWidget(row, 2, bind_widget);

			/* Column 3: Invert */
			QCheckBox *chk_inv = nullptr;
			QWidget *inv_cell = make_centered_checkbox_cell(
				table_, slot->invert,
				QStringLiteral("Invert physical fader travel (0% \xE2\x86\x94 100%)."), &chk_inv);
			connect(chk_inv, &QCheckBox::toggled, this, [row](bool inverted) {
				struct mfd_engine *e = mfd_engine_get();
				e->fader_slots[row].invert = inverted;
				mfd_engine_save_global_config(e);
			});
			table_->setCellWidget(row, 3, inv_cell);

			/* Column 4: Target Mode */
			auto *target_combo = new QComboBox(table_);
			target_combo->addItem(QStringLiteral("Unassigned"), static_cast<int>(MFD_TARGET_UNASSIGNED));
			target_combo->addItem(QStringLiteral("Studio Mode T-Bar"), static_cast<int>(MFD_TARGET_TBAR));
			target_combo->addItem(QStringLiteral("Two-Source Crossfade (A \xE2\x86\x94 B)"),
					      static_cast<int>(MFD_TARGET_TWO_SOURCE_MIX));
			target_combo->addItem(QStringLiteral("Single Layer Opacity"),
					      static_cast<int>(MFD_TARGET_SINGLE_LAYER));
			target_combo->addItem(QStringLiteral("Audio Volume (Cubic dB)"),
					      static_cast<int>(MFD_TARGET_AUDIO_VOLUME));
			target_combo->setToolTip(QStringLiteral(
				"Studio Mode T-Bar: starts above the 6% origin band, drives Preview \xE2\x86\x92 Program linearly 0\xE2\x80\x93"
				"100%, completes at the 3% target rail zone, and cancels if returned to the 3% origin rail zone (zero timers).\n"
				"Two-Source Crossfade (A \xE2\x86\x94 B): DJ/video-mixer mode inside one scene \xE2\x80\x94 Left (0%) is always Source A, Right (100%) is always Source B, dip-free with no Preview swap.\n"
				"Single Layer Opacity: fades one video source or nested scene 0\xE2\x80\x93"
				"100%.\n"
				"Audio Volume (Cubic dB): smooth 60 log10 audio fader curve."));
			target_combo->setCurrentIndex(target_combo->findData(static_cast<int>(slot->target_type)));
			table_->setCellWidget(row, 4, target_combo);

			bool show_a = false;
			bool show_b = false;
			bool show_curve = false;
			bool show_afv = false;
			mfd_row_controls_for_target(slot->target_type, &show_a, &show_b, &show_curve, &show_afv);

			/* Column 5: Source A */
			QComboBox *src_a_combo = nullptr;
			if (show_a) {
				src_a_combo = new QComboBox(table_);
				src_a_combo->setMaximumWidth(175);
				const bool is_audio_only = (slot->target_type == MFD_TARGET_AUDIO_VOLUME);
				populate_source_combo(src_a_combo, is_audio_only ? audio_sources : video_sources,
						      slot->source_a_name);
				table_->setCellWidget(row, 5, src_a_combo);
			} else {
				table_->setCellWidget(row, 5, new QWidget(table_));
			}

			/* Column 6: Source B */
			QComboBox *src_b_combo = nullptr;
			if (show_b) {
				src_b_combo = new QComboBox(table_);
				src_b_combo->setMaximumWidth(175);
				populate_source_combo(src_b_combo, video_sources, slot->source_b_name);
				table_->setCellWidget(row, 6, src_b_combo);
			} else {
				table_->setCellWidget(row, 6, new QWidget(table_));
			}

			/* Column 7: Crossfade Curve */
			QComboBox *curve_combo = nullptr;
			if (show_curve) {
				curve_combo = new QComboBox(table_);
				curve_combo->setMaximumWidth(190);
				curve_combo->addItem(QStringLiteral("Dip-Free Dissolve (Video Mixer)"),
						     static_cast<int>(MFD_CURVE_DIP_FREE));
				curve_combo->addItem(QStringLiteral("Full-Center Additive Hold"),
						     static_cast<int>(MFD_CURVE_FULL_CENTER));
				curve_combo->addItem(QStringLiteral("Equal-Power Cosine"),
						     static_cast<int>(MFD_CURVE_COSINE));
				curve_combo->addItem(QStringLiteral("Linear Crossfade"),
						     static_cast<int>(MFD_CURVE_LINEAR));
				curve_combo->addItem(QStringLiteral("Cut (Instant at 50%)"),
						     static_cast<int>(MFD_CURVE_CUT));
				curve_combo->setCurrentIndex(curve_combo->findData(static_cast<int>(slot->curve)));
				table_->setCellWidget(row, 7, curve_combo);
			} else {
				table_->setCellWidget(row, 7, new QWidget(table_));
			}

			/* Column 8: AFV */
			QCheckBox *chk_afv = nullptr;
			if (show_afv) {
				QWidget *afv_cell = make_centered_checkbox_cell(
					table_, slot->audio_follows_video,
					QStringLiteral(
						"Audio Follows Video: scale source audio volume (cubic dB) alongside layer opacity."),
					&chk_afv);
				table_->setCellWidget(row, 8, afv_cell);
			} else {
				table_->setCellWidget(row, 8, new QWidget(table_));
			}

			/* Column 9: Output */
			auto *out_bar = new QProgressBar(table_);
			out_bar->setRange(0, 100);
			out_bar->setValue(0);
			out_bar->setTextVisible(true);
			out_bar->setAlignment(Qt::AlignCenter);
			out_bar->setMinimumWidth(165);
			output_bars_[row] = out_bar;
			table_->setCellWidget(row, 9, out_bar);

			connect(target_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this, target_combo, row](int) {
					struct mfd_engine *e = mfd_engine_get();
					struct mfd_fader_slot *s = &e->fader_slots[row];
					const enum mfd_target_type prev_target = s->target_type;
					const enum mfd_target_type new_target =
						static_cast<enum mfd_target_type>(target_combo->currentData().toInt());
					s->target_type = new_target;
					const bool is_tbar = (new_target == MFD_TARGET_TBAR);
					const bool is_two_rail = (new_target == MFD_TARGET_TWO_SOURCE_MIX ||
								  new_target == MFD_TARGET_SINGLE_LAYER ||
								  new_target == MFD_TARGET_AUDIO_VOLUME);
					if (prev_target == MFD_TARGET_UNASSIGNED &&
					    new_target != MFD_TARGET_UNASSIGNED) {
						const float init_val = (new_target == MFD_TARGET_SINGLE_LAYER ||
									new_target == MFD_TARGET_AUDIO_VOLUME)
									       ? 1.0f
									       : 0.0f;
						mfd_smoother_reset(&s->smoother, init_val);
						mfd_takeover_init(&s->takeover, init_val, MFD_DEFAULT_PICKUP_THRESHOLD,
								  is_tbar ? e->tbar_bidirectional_flip : false);
						s->last_raw_norm = init_val;
						s->last_applied_val = init_val;
					}
					s->takeover.bidirectional_flip = is_tbar ? e->tbar_bidirectional_flip : false;
					mfd_takeover_set_two_rail_mode(&s->takeover, is_two_rail);
					if (is_tbar && s->takeover.state != MFD_TAKEOVER_PARKED) {
						s->tbar_state = MFD_TBAR_IDLE;
						mfd_engine_unlatch_tbar(e);
					} else if (!is_tbar) {
						s->tbar_state = MFD_TBAR_IDLE;
					}
					mfd_engine_reevaluate_slots(e, false);
					mfd_engine_save_global_config(e);
					mfd_engine_notify_state_changed(e);
					rebuild_table_rows();
					update_live_status();
				});

			if (src_a_combo) {
				connect(src_a_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, src_a_combo, row](int) {
						struct mfd_engine *e = mfd_engine_get();
						const QByteArray utf8 = src_a_combo->currentData().toString().toUtf8();
						std::snprintf(e->fader_slots[row].source_a_name,
							      sizeof(e->fader_slots[row].source_a_name), "%s",
							      utf8.constData());
						mfd_engine_reevaluate_slots(e, false);
						mfd_engine_save_global_config(e);
						mfd_engine_notify_state_changed(e);
						update_live_status();
					});
			}

			if (src_b_combo) {
				connect(src_b_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[this, src_b_combo, row](int) {
						struct mfd_engine *e = mfd_engine_get();
						const QByteArray utf8 = src_b_combo->currentData().toString().toUtf8();
						std::snprintf(e->fader_slots[row].source_b_name,
							      sizeof(e->fader_slots[row].source_b_name), "%s",
							      utf8.constData());
						mfd_engine_reevaluate_slots(e, false);
						mfd_engine_save_global_config(e);
						mfd_engine_notify_state_changed(e);
						update_live_status();
					});
			}

			if (curve_combo) {
				connect(curve_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
					[curve_combo, row](int) {
						struct mfd_engine *e = mfd_engine_get();
						e->fader_slots[row].curve = static_cast<enum mfd_crossfade_curve>(
							curve_combo->currentData().toInt());
						mfd_engine_save_global_config(e);
						mfd_engine_notify_state_changed(e);
					});
			}

			if (chk_afv) {
				connect(chk_afv, &QCheckBox::toggled, this, [row](bool checked) {
					struct mfd_engine *e = mfd_engine_get();
					e->fader_slots[row].audio_follows_video = checked;
					mfd_engine_save_global_config(e);
					mfd_engine_notify_state_changed(e);
				});
			}
		}

		table_->resizeColumnsToContents();
		const int table_w = table_->horizontalHeader()->length() + 48;
		const int target_w = (table_w < 1240) ? 1240 : ((table_w > 1620) ? 1620 : table_w);
		resize(target_w, height());
	}

	void update_live_status()
	{
		struct mfd_engine *eng = mfd_engine_get();

		++rescan_tick_counter_;
		const bool combo_popup_open = device_combo_ && device_combo_->view() &&
					      device_combo_->view()->isVisible();
		if (eng->midi_dev.is_open != last_midi_connected_) {
			last_midi_connected_ = eng->midi_dev.is_open;
			if (!combo_popup_open) {
				populate_device_combo();
			}
		} else if (!combo_popup_open && rescan_tick_counter_ >= 60) {
			rescan_tick_counter_ = 0;
			struct mfd_midi_device_info devs[MFD_MAX_MIDI_DEVICES];
			const size_t cur_count = mfd_winmm_enumerate(devs, MFD_MAX_MIDI_DEVICES);
			if (cur_count != last_device_count_) {
				populate_device_combo();
			}
		}
		sync_profile_combo();

		/* Connection status pill */
		if (eng->midi_dev.is_open) {
			const QString dev_name = QString::fromUtf8(eng->midi_dev.opened_name);
			set_label_text_and_style(
				conn_status_label_, QStringLiteral("\xE2\x97\x8F Connected: %1").arg(dev_name),
				QStringLiteral("QLabel { background-color: #064e3b; color: #6ee7b7; padding: 4px 10px; "
					       "border: 1px solid #10b981; border-radius: 4px; font-weight: bold; }"));
		} else if (eng->configured_device_name[0] != '\0') {
			const QString waiting_for = QString::fromUtf8(eng->configured_device_name);
			set_label_text_and_style(
				conn_status_label_,
				QStringLiteral("\xE2\x97\x8B Disconnected \xE2\x80\x94 waiting for %1").arg(waiting_for),
				QStringLiteral("QLabel { background-color: #451a03; color: #fcd34d; padding: 4px 10px; "
					       "border: 1px solid #d97706; border-radius: 4px; font-weight: bold; }"));
		} else {
			set_label_text_and_style(
				conn_status_label_, QStringLiteral("\xE2\x97\x8B Disconnected"),
				QStringLiteral("QLabel { background-color: #1f2937; color: #9ca3af; padding: 4px 10px; "
					       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		}

		/* Error banner */
		QStringList errors;
		if (eng->midi_dev.last_error[0] != '\0') {
			errors.append(QString::fromUtf8(eng->midi_dev.last_error));
		}
		if (eng->tbar_error[0] != '\0') {
			errors.append(QString::fromUtf8(eng->tbar_error));
		}

		if (errors.isEmpty()) {
			banner_label_->hide();
		} else {
			const QString joined = errors.join(QStringLiteral("\n"));
			if (banner_label_->text() != joined) {
				banner_label_->setText(joined);
			}
			banner_label_->show();
		}

		/* Studio Mode pill, warning banner, and Finish T-Bar button */
		const bool studio_on = mfd_tbar_is_studio_mode_active();
		size_t active_tbar_slots = 0U;
		bool can_finish_tbar = false;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			const struct mfd_fader_slot &s = eng->fader_slots[i];
			if (s.enabled && s.target_type == MFD_TARGET_TBAR) {
				++active_tbar_slots;
				if (studio_on && s.tbar_state == MFD_TBAR_THROWING) {
					can_finish_tbar = true;
				}
			}
		}

		if (studio_note_widget_) {
			const bool show_note = mfd_should_show_studio_mode_note(studio_on, active_tbar_slots);
			studio_note_widget_->setVisible(show_note);
		}

		if (studio_on) {
			obs_source_t *prev_src = obs_frontend_get_current_preview_scene();
			obs_source_t *prog_src = obs_frontend_get_current_scene();
			const QString prev_nm = (prev_src && obs_source_get_name(prev_src))
							? QString::fromUtf8(obs_source_get_name(prev_src))
							: QStringLiteral("\xE2\x80\x94");
			const QString prog_nm = (prog_src && obs_source_get_name(prog_src))
							? QString::fromUtf8(obs_source_get_name(prog_src))
							: QStringLiteral("\xE2\x80\x94");
			if (prev_src) {
				obs_source_release(prev_src);
			}
			if (prog_src) {
				obs_source_release(prog_src);
			}
			if (prev_nm != prog_nm) {
				last_stable_program_scene_ = prog_nm;
			}
			const QString pill_text =
				(prev_nm == prog_nm && !last_stable_program_scene_.isEmpty() &&
				 last_stable_program_scene_ != prev_nm)
					? QStringLiteral("\xE2\x97\x8F Studio Mode: Transitioning [%1 \xE2\x86\x92 %2]")
						  .arg(last_stable_program_scene_, prev_nm)
					: QStringLiteral(
						  "\xE2\x97\x8F Studio Mode: Preview [%1] \xE2\x86\x92 Program [%2]")
						  .arg(prev_nm, prog_nm);
			studio_status_label_->setToolTip(QStringLiteral(
				"Right box (Program) is your live broadcast output. Left box (Preview) is the next scene on deck "
				"and automatically swaps when a T-bar throw reaches the 3% rail zone."));
			set_label_text_and_style(
				studio_status_label_, pill_text,
				QStringLiteral("QLabel { background-color: #172554; color: #bfdbfe; padding: 4px 10px; "
					       "border: 1px solid #2563eb; border-radius: 4px; font-weight: bold; }"));
		} else {
			set_label_text_and_style(
				studio_status_label_, QStringLiteral("\xE2\x97\x8B Studio Mode: Off"),
				QStringLiteral("QLabel { background-color: #1f2937; color: #9ca3af; padding: 4px 10px; "
					       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		}

		if (btn_finish_tbar_) {
			btn_finish_tbar_->setEnabled(can_finish_tbar);
		}

		if (last_midi_label_) {
			char midi_buf[128] = {0};
			mfd_format_last_midi_readout(eng->has_last_midi, &eng->last_midi_decoded, midi_buf,
						     sizeof(midi_buf));
			const QString midi_qstr = QString::fromUtf8(midi_buf);
			if (last_midi_label_->text() != midi_qstr) {
				last_midi_label_->setText(midi_qstr);
			}
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
			const struct mfd_fader_slot &s = eng->fader_slots[row];
			const bool is_learning = (eng->learning_slot_index == row);
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
			const QString row_bg = is_learning ? QStringLiteral("background-color: rgba(180, 83, 9, 0.28);")
							   : QString();
			for (int col = 0; col < table_->columnCount() - 1; ++col) {
				if (QWidget *cw = table_->cellWidget(row, col)) {
					if (cw->styleSheet() != row_bg) {
						cw->setStyleSheet(row_bg);
					}
				}
			}

			QProgressBar *bar = output_bars_[row];
			if (!bar) {
				continue;
			}

			int desired_pct = 0;
			QString desired_text;
			QString desired_style = style_disabled;
			QString desired_tip;

			if (!s.enabled) {
				desired_text = QStringLiteral("Disabled");
				desired_tip = QStringLiteral("Slot is disabled.");
			} else if (s.target_type == MFD_TARGET_UNASSIGNED) {
				desired_text = QStringLiteral("Unassigned");
				desired_tip = QStringLiteral("Choose a Target Mode to activate this fader.");
			} else {
				bool avail = false;
				char reason_buf[160] = {0};
				mfd_engine_get_slot_status(eng, &s, nullptr, 0U, &avail, reason_buf,
							   sizeof(reason_buf));
				if (!avail) {
					desired_pct = 0;
					desired_text = QString::fromUtf8(reason_buf);
					desired_style = style_unavail;
					desired_tip = desired_text;
				} else {
					char bar_text[128] = {0};
					const float db = mfd_deflection_to_db(s.last_applied_val);
					desired_pct = mfd_takeover_format_output_bar(&s.takeover, s.target_type,
										     s.last_applied_val, s.last_alpha_a,
										     s.last_alpha_b, db, bar_text,
										     sizeof(bar_text));
					desired_text = QString::fromUtf8(bar_text);
					if (s.takeover.state == MFD_TAKEOVER_WAITING) {
						desired_style = style_waiting;
						desired_tip = QStringLiteral(
							"Soft Takeover waiting: move physical fader across target to engage without jumping.");
					} else if (s.takeover.state == MFD_TAKEOVER_PARKED) {
						desired_style = style_parked;
						desired_text.append(QStringLiteral(" \xC2\xB7 Parked"));
						desired_tip = QStringLiteral(
							"Parked at rail (8% hysteresis absorbs fader jitter; move > 8% from peak to unpark).");
					} else {
						desired_style = style_engaged;
						desired_tip = QStringLiteral("Engaged and tracking fader.");
					}
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

	QLabel *banner_label_ = nullptr;
	QWidget *studio_note_widget_ = nullptr;
	QLabel *conn_status_label_ = nullptr;
	QLabel *studio_status_label_ = nullptr;
	QPushButton *btn_finish_tbar_ = nullptr;
	QLabel *last_midi_label_ = nullptr;
	QComboBox *device_combo_ = nullptr;
	QComboBox *profile_combo_ = nullptr;
	QDoubleSpinBox *tau_spin_ = nullptr;
	QCheckBox *flip_check_ = nullptr;
	QTableWidget *table_ = nullptr;
	QTimer *timer_ = nullptr;
	bool last_midi_connected_ = false;
	size_t last_device_count_ = 0U;
	int rescan_tick_counter_ = 0;
	bool manual_profile_override_ = false;
	enum mfd_controller_profile manual_profile_ = MFD_PROFILE_GENERIC;
	QString last_stable_program_scene_;
	QPointer<QLabel> addr_labels_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> learn_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> clear_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QProgressBar> output_bars_[MFD_MAX_FADER_SLOTS];
};

static QPointer<MfdSettingsDialog> g_dialog;
static QTimer *g_midi_health_timer = nullptr;

static void on_tools_menu_clicked(void *unused)
{
	UNUSED_PARAMETER(unused);
	QWidget *main_win = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!g_dialog) {
		g_dialog = new MfdSettingsDialog(main_win);
		g_dialog->setAttribute(Qt::WA_DeleteOnClose, true);
	}
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}

} // namespace

void mfd_ui_init(void)
{
	obs_frontend_add_tools_menu_item(obs_module_text("ToolsMenuTitle"), on_tools_menu_clicked, nullptr);

	/*
	 * Poll WinMM device health and automatic 2-second hot-plug rescan exclusively on the Qt UI thread
	 * (R-MIDI-2 + R-THR-1). Skipped during MFD_SELF_TEST so scripted port switching is never raced.
	 */
	if (!qEnvironmentVariableIsSet("MFD_SELF_TEST")) {
		g_midi_health_timer = new QTimer();
		QObject::connect(g_midi_health_timer, &QTimer::timeout,
				 []() { mfd_engine_poll_midi_health_on_ui_thread(mfd_engine_get()); });
		g_midi_health_timer->start(2000);
	}
}

void mfd_ui_shutdown(void)
{
	if (g_midi_health_timer) {
		g_midi_health_timer->stop();
		delete g_midi_health_timer;
		g_midi_health_timer = nullptr;
	}
	if (g_dialog) {
		g_dialog->close();
		g_dialog.clear();
	}
}
