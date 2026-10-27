/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-settings-dialog.hpp"
#include "obs/mfd-engine.h"
#include "obs/mfd-tbar.h"
#include "core/mixer-math.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
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

static QStringList collect_obs_sources(bool require_video, bool require_audio)
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
			if (name && *name) {
				c->names.append(QString::fromUtf8(name));
			}
			return true;
		},
		&ctx);

	ctx.names.sort(Qt::CaseInsensitive);
	return ctx.names;
}

class MfdSettingsDialog final : public QDialog {
public:
	explicit MfdSettingsDialog(QWidget *parent = nullptr) : QDialog(parent)
	{
		setWindowTitle(QStringLiteral("Midi Fadester — Video Mixer & Fader Control"));
		resize(1120, 620);

		auto *root = new QVBoxLayout(this);

		banner_label_ = new QLabel(this);
		banner_label_->setWordWrap(true);
		banner_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #5a1d1d; color: #ffd6d6; padding: 8px; "
				       "border: 1px solid #d9534f; border-radius: 4px; font-weight: bold; }"));
		banner_label_->hide();
		root->addWidget(banner_label_);

		auto *top_bar = new QHBoxLayout();
		top_bar->addWidget(new QLabel(QStringLiteral("MIDI In Device:"), this));

		device_combo_ = new QComboBox(this);
		device_combo_->setMinimumWidth(260);
		top_bar->addWidget(device_combo_);

		auto *btn_reconnect = new QPushButton(QStringLiteral("Reconnect"), this);
		top_bar->addWidget(btn_reconnect);

		auto *btn_defaults = new QPushButton(QStringLiteral("Load APC40 mkII Defaults"), this);
		top_bar->addWidget(btn_defaults);

		top_bar->addSpacing(16);
		top_bar->addWidget(new QLabel(QStringLiteral("Smoothing (\xCF\x84 ms):"), this));

		tau_spin_ = new QDoubleSpinBox(this);
		tau_spin_->setRange(0.0, 60.0);
		tau_spin_->setSingleStep(1.0);
		tau_spin_->setDecimals(1);
		tau_spin_->setSuffix(QStringLiteral(" ms"));
		top_bar->addWidget(tau_spin_);

		top_bar->addSpacing(12);
		flip_check_ = new QCheckBox(QStringLiteral("Bidirectional T-Bar Throw (0\xE2\x86\x92"
							   "1 then 1\xE2\x86\x92"
							   "0)"),
					    this);
		top_bar->addWidget(flip_check_);

		top_bar->addSpacing(16);
		last_midi_label_ = new QLabel(QStringLiteral("No MIDI received yet"), this);
		last_midi_label_->setStyleSheet(
			QStringLiteral("QLabel { background-color: #1f2937; color: #93c5fd; padding: 4px 8px; "
				       "border: 1px solid #374151; border-radius: 4px; font-weight: bold; }"));
		top_bar->addWidget(last_midi_label_);
		top_bar->addStretch(1);

		root->addLayout(top_bar);

		table_ = new QTableWidget(MFD_MAX_FADER_SLOTS, 9, this);
		table_->setHorizontalHeaderLabels({
			QStringLiteral("On"),
			QStringLiteral("Fader / Strip"),
			QStringLiteral("MIDI Binding"),
			QStringLiteral("Target Mode"),
			QStringLiteral("Source A (Bottom / Primary)"),
			QStringLiteral("Source B (Top Layer)"),
			QStringLiteral("Crossfade Curve"),
			QStringLiteral("AFV"),
			QStringLiteral("Live Output & Soft Takeover"),
		});
		table_->horizontalHeader()->setStretchLastSection(true);
		table_->verticalHeader()->setVisible(false);
		table_->setSelectionMode(QAbstractItemView::NoSelection);
		root->addWidget(table_, 1);

		auto *footer = new QHBoxLayout();
		auto *hint = new QLabel(
			QStringLiteral(
				"Tip: For Two-Source Layer Crossfade (A \xE2\x86\x94 B), place Source A below Source B in your OBS Scene. "
				"Dip-Free Dissolve holds Source A at 100% until midpoint so 0% black background bleeds through."),
			this);
		hint->setWordWrap(true);
		footer->addWidget(hint, 1);

		auto *btn_refresh_sources = new QPushButton(QStringLiteral("Refresh OBS Sources"), this);
		footer->addWidget(btn_refresh_sources);

		auto *btn_close = new QPushButton(QStringLiteral("Close"), this);
		footer->addWidget(btn_close);
		root->addLayout(footer);

		connect(btn_reconnect, &QPushButton::clicked, this, [this]() {
			struct mfd_engine *eng = mfd_engine_get();
			const QString chosen = device_combo_->currentData().toString();
			mfd_engine_reconnect_midi(eng, chosen.toUtf8().constData());
			mfd_engine_save_global_config(eng);
			populate_device_combo();
			update_live_status();
		});

		connect(btn_defaults, &QPushButton::clicked, this, [this]() {
			struct mfd_engine *eng = mfd_engine_get();
			mfd_engine_load_apc40_defaults(eng);
			mfd_engine_save_global_config(eng);
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

		connect(btn_refresh_sources, &QPushButton::clicked, this, [this]() { rebuild_table_rows(); });
		connect(btn_close, &QPushButton::clicked, this, &QDialog::accept);

		populate_device_combo();
		sync_header_controls();
		rebuild_table_rows();

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, [this]() { update_live_status(); });
		timer_->start(33);
		update_live_status();
	}

private:
	void populate_device_combo()
	{
		struct mfd_engine *eng = mfd_engine_get();
		device_combo_->blockSignals(true);
		device_combo_->clear();

		struct mfd_midi_device_info devs[MFD_MAX_MIDI_DEVICES];
		size_t count = mfd_winmm_enumerate(devs, MFD_MAX_MIDI_DEVICES);

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

	void rebuild_table_rows()
	{
		struct mfd_engine *eng = mfd_engine_get();
		const QStringList video_sources = collect_obs_sources(true, false);
		const QStringList audio_sources = collect_obs_sources(false, true);

		for (int row = 0; row < MFD_MAX_FADER_SLOTS; ++row) {
			struct mfd_fader_slot *slot = &eng->fader_slots[row];

			auto *chk_on = new QCheckBox(table_);
			chk_on->setChecked(slot->enabled);
			connect(chk_on, &QCheckBox::toggled, this, [row](bool on) {
				struct mfd_engine *e = mfd_engine_get();
				e->fader_slots[row].enabled = on;
				mfd_engine_save_global_config(e);
			});
			table_->setCellWidget(row, 0, chk_on);

			auto *lbl_name = new QLabel(QString::fromUtf8(slot->label), table_);
			lbl_name->setMargin(4);
			table_->setCellWidget(row, 1, lbl_name);

			auto *bind_widget = new QWidget(table_);
			auto *bind_layout = new QHBoxLayout(bind_widget);
			bind_layout->setContentsMargins(4, 2, 4, 2);
			auto *lbl_addr = new QLabel(format_midi_addr(slot->addr), bind_widget);
			auto *btn_learn = new QPushButton(QString::fromUtf8(mfd_learn_button_text(false)), bind_widget);
			btn_learn->setMinimumWidth(118);
			bind_layout->addWidget(lbl_addr, 1);
			bind_layout->addWidget(btn_learn);
			addr_labels_[row] = lbl_addr;
			learn_buttons_[row] = btn_learn;

			connect(btn_learn, &QPushButton::clicked, this, [this, row]() {
				struct mfd_engine *e = mfd_engine_get();
				if (e->learning_slot_index == row) {
					mfd_engine_cancel_learn(e);
				} else {
					mfd_engine_start_learn(e, row);
				}
				update_live_status();
			});
			table_->setCellWidget(row, 2, bind_widget);

			auto *target_combo = new QComboBox(table_);
			target_combo->addItem(QStringLiteral("Studio Mode T-Bar"), static_cast<int>(MFD_TARGET_TBAR));
			target_combo->addItem(QStringLiteral("Two-Source Crossfade (A \xE2\x86\x94 B)"),
					      static_cast<int>(MFD_TARGET_TWO_SOURCE_MIX));
			target_combo->addItem(QStringLiteral("Single Layer Opacity"),
					      static_cast<int>(MFD_TARGET_SINGLE_LAYER));
			target_combo->addItem(QStringLiteral("Audio Volume (Cubic dB)"),
					      static_cast<int>(MFD_TARGET_AUDIO_VOLUME));
			target_combo->setCurrentIndex(static_cast<int>(slot->target_type));
			table_->setCellWidget(row, 3, target_combo);

			auto *src_a_combo = new QComboBox(table_);
			const bool is_audio_only = (slot->target_type == MFD_TARGET_AUDIO_VOLUME);
			populate_source_combo(src_a_combo, is_audio_only ? audio_sources : video_sources,
					      slot->source_a_name);
			src_a_combo->setEnabled(slot->target_type != MFD_TARGET_TBAR);
			table_->setCellWidget(row, 4, src_a_combo);

			auto *src_b_combo = new QComboBox(table_);
			populate_source_combo(src_b_combo, video_sources, slot->source_b_name);
			src_b_combo->setEnabled(slot->target_type == MFD_TARGET_TWO_SOURCE_MIX);
			table_->setCellWidget(row, 5, src_b_combo);

			auto *curve_combo = new QComboBox(table_);
			curve_combo->addItem(QStringLiteral("Dip-Free Dissolve (Video Mixer)"),
					     static_cast<int>(MFD_CURVE_DIP_FREE));
			curve_combo->addItem(QStringLiteral("Full-Center Additive Hold"),
					     static_cast<int>(MFD_CURVE_FULL_CENTER));
			curve_combo->addItem(QStringLiteral("Equal-Power Cosine"), static_cast<int>(MFD_CURVE_COSINE));
			curve_combo->addItem(QStringLiteral("Linear Crossfade"), static_cast<int>(MFD_CURVE_LINEAR));
			curve_combo->setCurrentIndex(curve_combo->findData(static_cast<int>(slot->curve)));
			curve_combo->setEnabled(slot->target_type == MFD_TARGET_TWO_SOURCE_MIX);
			table_->setCellWidget(row, 6, curve_combo);

			auto *chk_afv = new QCheckBox(table_);
			chk_afv->setChecked(slot->audio_follows_video);
			chk_afv->setEnabled(slot->target_type == MFD_TARGET_TWO_SOURCE_MIX ||
					    slot->target_type == MFD_TARGET_SINGLE_LAYER);
			table_->setCellWidget(row, 7, chk_afv);

			auto *lbl_status = new QLabel(QStringLiteral("--"), table_);
			lbl_status->setMargin(4);
			status_labels_[row] = lbl_status;
			table_->setCellWidget(row, 8, lbl_status);

			connect(target_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this, row](int idx) {
					struct mfd_engine *e = mfd_engine_get();
					e->fader_slots[row].target_type = static_cast<enum mfd_target_type>(idx);
					mfd_engine_save_global_config(e);
					rebuild_table_rows();
				});

			connect(src_a_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[src_a_combo, row](int) {
					struct mfd_engine *e = mfd_engine_get();
					const QByteArray utf8 = src_a_combo->currentData().toString().toUtf8();
					std::snprintf(e->fader_slots[row].source_a_name,
						      sizeof(e->fader_slots[row].source_a_name), "%s",
						      utf8.constData());
					mfd_engine_save_global_config(e);
				});

			connect(src_b_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[src_b_combo, row](int) {
					struct mfd_engine *e = mfd_engine_get();
					const QByteArray utf8 = src_b_combo->currentData().toString().toUtf8();
					std::snprintf(e->fader_slots[row].source_b_name,
						      sizeof(e->fader_slots[row].source_b_name), "%s",
						      utf8.constData());
					mfd_engine_save_global_config(e);
				});

			connect(curve_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[curve_combo, row](int) {
					struct mfd_engine *e = mfd_engine_get();
					e->fader_slots[row].curve = static_cast<enum mfd_crossfade_curve>(
						curve_combo->currentData().toInt());
					mfd_engine_save_global_config(e);
				});

			connect(chk_afv, &QCheckBox::toggled, this, [row](bool checked) {
				struct mfd_engine *e = mfd_engine_get();
				e->fader_slots[row].audio_follows_video = checked;
				mfd_engine_save_global_config(e);
			});
		}

		table_->resizeColumnsToContents();
	}

	void update_live_status()
	{
		struct mfd_engine *eng = mfd_engine_get();

		QStringList errors;
		if (eng->midi_dev.last_error[0] != '\0') {
			errors.append(QString::fromUtf8(eng->midi_dev.last_error));
		}
		if (eng->tbar_error[0] != '\0') {
			errors.append(QString::fromUtf8(eng->tbar_error));
		}
		if (!mfd_tbar_is_studio_mode_active()) {
			bool any_tbar_slot = false;
			for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
				if (eng->fader_slots[i].enabled && eng->fader_slots[i].target_type == MFD_TARGET_TBAR) {
					any_tbar_slot = true;
					break;
				}
			}
			if (any_tbar_slot) {
				errors.append(QStringLiteral(
					"Note: Studio Mode is currently OFF. Enable Studio Mode in OBS to use the Studio Mode T-Bar target, or switch Target Mode to 'Two-Source Crossfade (A \xE2\x86\x94 B)'."));
			}
		}

		if (errors.isEmpty()) {
			banner_label_->hide();
		} else {
			banner_label_->setText(errors.join(QStringLiteral("\n")));
			banner_label_->show();
		}

		if (last_midi_label_) {
			char midi_buf[128] = {0};
			mfd_format_last_midi_readout(eng->has_last_midi, &eng->last_midi_decoded, midi_buf,
						     sizeof(midi_buf));
			last_midi_label_->setText(QString::fromUtf8(midi_buf));
		}

		for (int row = 0; row < MFD_MAX_FADER_SLOTS; ++row) {
			const struct mfd_fader_slot &s = eng->fader_slots[row];
			const bool is_learning = (eng->learning_slot_index == row);
			if (addr_labels_[row]) {
				addr_labels_[row]->setText(format_midi_addr(s.addr));
			}
			if (learn_buttons_[row]) {
				learn_buttons_[row]->setText(QString::fromUtf8(mfd_learn_button_text(is_learning)));
				learn_buttons_[row]->setStyleSheet(
					is_learning ? QStringLiteral("QPushButton { background-color: #b45309; "
								     "color: #ffffff; font-weight: bold; }")
						    : QString());
			}
			for (int col = 0; col < table_->columnCount(); ++col) {
				if (QWidget *cw = table_->cellWidget(row, col)) {
					cw->setStyleSheet(
						is_learning
							? QStringLiteral("background-color: rgba(180, 83, 9, 0.28);")
							: QString());
				}
			}
			if (!status_labels_[row]) {
				continue;
			}
			if (!s.enabled) {
				status_labels_[row]->setText(QStringLiteral("Disabled"));
				continue;
			}

			QString takeover_str;
			if (s.takeover.state == MFD_TAKEOVER_ENGAGED) {
				takeover_str = QStringLiteral("\xE2\x97\x8F Engaged");
			} else if (s.takeover.hint == MFD_HINT_MOVE_UP) {
				takeover_str = QStringLiteral("\xE2\x86\x91 Move Fader Up");
			} else if (s.takeover.hint == MFD_HINT_MOVE_DOWN) {
				takeover_str = QStringLiteral("\xE2\x86\x93 Move Fader Down");
			} else {
				takeover_str = QStringLiteral("Waiting");
			}

			if (s.target_type == MFD_TARGET_TWO_SOURCE_MIX) {
				status_labels_[row]->setText(QStringLiteral("Mix %1% (A:%2% B:%3%) | %4")
								     .arg(std::lround(s.last_applied_val * 100.0f))
								     .arg(std::lround(s.last_alpha_a * 100.0f))
								     .arg(std::lround(s.last_alpha_b * 100.0f))
								     .arg(takeover_str));
			} else if (s.target_type == MFD_TARGET_AUDIO_VOLUME) {
				const float db = mfd_deflection_to_db(s.last_applied_val);
				const QString db_str = std::isfinite(db) ? QStringLiteral("%1 dB").arg(db, 0, 'f', 1)
									 : QStringLiteral("-inf dB");
				status_labels_[row]->setText(QStringLiteral("%1 | %2").arg(db_str, takeover_str));
			} else {
				status_labels_[row]->setText(QStringLiteral("%1% | %2")
								     .arg(std::lround(s.last_applied_val * 100.0f))
								     .arg(takeover_str));
			}
		}
	}

	QLabel *banner_label_ = nullptr;
	QLabel *last_midi_label_ = nullptr;
	QComboBox *device_combo_ = nullptr;
	QDoubleSpinBox *tau_spin_ = nullptr;
	QCheckBox *flip_check_ = nullptr;
	QTableWidget *table_ = nullptr;
	QTimer *timer_ = nullptr;
	QPointer<QLabel> addr_labels_[MFD_MAX_FADER_SLOTS];
	QPointer<QPushButton> learn_buttons_[MFD_MAX_FADER_SLOTS];
	QPointer<QLabel> status_labels_[MFD_MAX_FADER_SLOTS];
};

static QPointer<MfdSettingsDialog> g_dialog;

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
}

void mfd_ui_shutdown(void)
{
	if (g_dialog) {
		g_dialog->close();
		g_dialog.clear();
	}
}
