/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "obs/mfd-engine.h"
#include <QDialog>
#include <QString>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QWidget;

class MfdSetupSheetDialog final : public QDialog {
public:
	explicit MfdSetupSheetDialog(struct mfd_engine *eng, QWidget *parent = nullptr);
	~MfdSetupSheetDialog() override;

	void open_for_slot(int slot_index);
	void refresh_from_snapshot(const struct mfd_ui_snapshot &snap);
	int current_slot_index() const { return slot_index_; }
	void reject() override;

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	struct mfd_setup_candidate build_candidate() const;
	const struct mfd_ui_scene_entry *find_catalog_scene(const QString &name) const;
	const struct mfd_filter_entry_info *find_selected_filter_entry() const;
	const struct mfd_filter_prop_info *find_selected_filter_prop() const;
	bool chosen_layers_have_audio() const;
	void select_mode_and_go_step2(enum mfd_target_type mode);
	void repopulate_step2_pickers();
	void repopulate_layer_combos_for_scene();
	void repopulate_filter_combos_for_host(bool preserve_range);
	void repopulate_prop_combo_for_filter(bool preserve_range);
	void configure_spinboxes_for_prop(bool preserve_range);
	void update_step2_notes_and_validation();
	void update_step3_controls();
	void update_footer_buttons();
	void on_done_clicked();

	struct mfd_engine *eng_ = nullptr;
	struct mfd_ui_catalog *catalog_ = nullptr;
	struct mfd_source_filter_catalog *filter_cat_ = nullptr;
	struct mfd_ui_snapshot snap_{};
	uint64_t last_topology_rev_ = 0ULL;
	int slot_index_ = 0;
	bool committed_or_unassigned_ = false;
	bool updating_widgets_ = false;

	enum mfd_target_type chosen_mode_ = MFD_TARGET_TBAR;
	QString chosen_scene_;
	QString chosen_left_;
	QString chosen_right_;
	QString chosen_audio_;
	QString chosen_filter_host_;
	QString chosen_filter_name_;
	QString chosen_filter_prop_;
	double chosen_filter_from_ = 0.0;
	double chosen_filter_to_ = 1.0;
	bool chosen_filter_has_custom_range_ = false;
	bool chosen_filter_bypass_ = false;
	enum mfd_crossfade_curve chosen_curve_ = MFD_CURVE_DIP_FREE;
	bool chosen_afv_ = false;
	bool chosen_tbar_bidir_ = true;

	QLabel *title_label_ = nullptr;
	QLabel *error_banner_ = nullptr;
	QStackedWidget *stack_ = nullptr;

	/* Step 1 widgets */
	QPushButton *card_tbar_ = nullptr;
	QPushButton *card_mix_ = nullptr;
	QPushButton *card_single_ = nullptr;
	QPushButton *card_volume_ = nullptr;
	QPushButton *card_filter_ = nullptr;
	QPushButton *btn_not_used_ = nullptr;

	/* Step 2 widgets */
	QWidget *step2_tbar_box_ = nullptr;
	QLabel *step2_tbar_info_ = nullptr;
	QPushButton *step2_turn_on_studio_btn_ = nullptr;

	QWidget *step2_scene_box_ = nullptr;
	QLabel *step2_empty_scene_note_ = nullptr;
	QComboBox *combo_scene_ = nullptr;
	QLabel *lbl_left_caption_ = nullptr;
	QComboBox *combo_left_ = nullptr;
	QWidget *right_row_widget_ = nullptr;
	QComboBox *combo_right_ = nullptr;
	QLabel *same_layer_refusal_ = nullptr;
	QLabel *live_scene_note_ = nullptr;
	QLabel *shared_layer_note_ = nullptr;

	QWidget *step2_audio_box_ = nullptr;
	QLabel *step2_empty_audio_note_ = nullptr;
	QComboBox *combo_audio_ = nullptr;

	QWidget *step2_filter_box_ = nullptr;
	QLabel *step2_empty_filter_note_ = nullptr;
	QComboBox *combo_filter_host_ = nullptr;
	QComboBox *combo_filter_name_ = nullptr;
	QComboBox *combo_filter_prop_ = nullptr;
	QDoubleSpinBox *spin_filter_from_ = nullptr;
	QPushButton *btn_use_current_from_ = nullptr;
	QDoubleSpinBox *spin_filter_to_ = nullptr;
	QPushButton *btn_use_current_to_ = nullptr;
	QLabel *lbl_filter_limits_note_ = nullptr;
	QCheckBox *chk_filter_bypass_ = nullptr;
	QLabel *filter_validation_refusal_ = nullptr;

	QLabel *catalog_truncated_note_ = nullptr;

	/* Step 3 widgets */
	QLabel *finished_sentence_ = nullptr;
	QProgressBar *candidate_meter_ = nullptr;
	QWidget *feel_box_ = nullptr;
	QComboBox *combo_feel_ = nullptr;
	QLabel *feel_desc_label_ = nullptr;
	QCheckBox *chk_also_fade_audio_ = nullptr;
	QLabel *volume_law_label_ = nullptr;
	QCheckBox *chk_reverse_dir_ = nullptr;
	QWidget *tbar_step3_box_ = nullptr;
	QCheckBox *chk_tbar_bidir_ = nullptr;
	QPushButton *btn_step3_finish_now_ = nullptr;
	QPushButton *btn_hw_toggle_ = nullptr;
	QWidget *hw_panel_ = nullptr;
	QLabel *candidate_addr_label_ = nullptr;
	QPushButton *btn_setup_learn_ = nullptr;
	QLabel *collision_notice_ = nullptr;

	/* Footer widgets */
	QPushButton *btn_cancel_ = nullptr;
	QPushButton *btn_back_ = nullptr;
	QPushButton *btn_next_ = nullptr;
	QPushButton *btn_done_ = nullptr;
};
