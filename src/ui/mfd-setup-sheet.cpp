/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-setup-sheet.hpp"
#include "ui/mfd-controller-svg.hpp"
#include "ui/mfd-svg-canvas.hpp"
#include "core/apc40-profile.h"
#include "core/filter-math.h"
#include "core/ui-sentences.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

static QString format_plain_midi_addr(const struct mfd_midi_addr &addr)
{
	switch (addr.type) {
	case MFD_MIDI_CC:
		return QStringLiteral("Channel %1, CC %2%3")
			.arg(static_cast<int>(addr.channel) + 1)
			.arg(static_cast<int>(addr.number))
			.arg(addr.is_14bit ? QStringLiteral(" (14-bit)") : QString());
	case MFD_MIDI_PITCH_BEND:
		return QStringLiteral("Channel %1, Pitch Bend").arg(static_cast<int>(addr.channel) + 1);
	case MFD_MIDI_NOTE_ON:
		return QStringLiteral("Channel %1, Note %2")
			.arg(static_cast<int>(addr.channel) + 1)
			.arg(static_cast<int>(addr.number));
	default:
		return QStringLiteral("No MIDI control bound");
	}
}

static QPushButton *create_mode_card_button(QWidget *parent, const QString &obj_name, enum mfd_target_type mode,
					    const QString &headline, const QString &subline)
{
	auto *btn = new QPushButton(parent);
	btn->setObjectName(obj_name);
	btn->setCheckable(true);
	btn->setMinimumSize(250, 114);
	btn->setCursor(Qt::PointingHandCursor);
	btn->setStyleSheet(QStringLiteral(
		"QPushButton { background-color: #111827; border: 2px solid #334155; border-radius: 10px; "
		"text-align: left; padding: 8px; } "
		"QPushButton:hover { border-color: #60a5fa; background-color: #172033; } "
		"QPushButton:checked { border-color: #38bdf8; background-color: #16243d; }"));

	auto *lay = new QVBoxLayout(btn);
	lay->setContentsMargins(10, 6, 10, 6);
	lay->setSpacing(4);

	auto *svg = new MfdSvgCanvas(btn);
	svg->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	svg->setFixedHeight(46);
	svg->set_svg_utf8(mfd_make_mode_card_svg(mode));
	lay->addWidget(svg);

	auto *lbl_head = new QLabel(headline, btn);
	lbl_head->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	lbl_head->setStyleSheet(
		QStringLiteral("color: #f8fafc; font-size: 13px; font-weight: bold; background: transparent;"));
	lay->addWidget(lbl_head);

	auto *lbl_sub = new QLabel(subline, btn);
	lbl_sub->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	lbl_sub->setWordWrap(true);
	lbl_sub->setStyleSheet(QStringLiteral("color: #cbd5e1; font-size: 11px; background: transparent;"));
	lay->addWidget(lbl_sub, 1);

	return btn;
}

} // namespace

MfdSetupSheetDialog::MfdSetupSheetDialog(struct mfd_engine *eng, QWidget *parent)
	: QDialog(parent),
	  eng_(eng ? eng : mfd_engine_get()),
	  catalog_(mfd_ui_catalog_create()),
	  filter_cat_(mfd_source_filter_catalog_create())
{
	setObjectName(QStringLiteral("mfdSetupSheet"));
	setWindowTitle(QStringLiteral("Midi Fadester \xE2\x80\x94 Set up strip"));
	resize(640, 560);
	setStyleSheet(QStringLiteral(
		"QDialog { background-color: #0b1120; color: #f8fafc; } "
		"QLabel { color: #f8fafc; } "
		"QComboBox { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; "
		"border-radius: 5px; padding: 6px 10px; min-height: 24px; } "
		"QComboBox QAbstractItemView { background-color: #1e293b; color: #f8fafc; selection-background-color: #2563eb; } "
		"QDoubleSpinBox { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; "
		"border-radius: 5px; padding: 5px 8px; min-height: 24px; } "
		"QCheckBox { color: #e2e8f0; spacing: 8px; font-size: 13px; }"));

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(18, 16, 18, 16);
	root->setSpacing(12);

	title_label_ = new QLabel(QStringLiteral("Set up strip"), this);
	title_label_->setObjectName(QStringLiteral("mfdSetupTitle"));
	title_label_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: bold; color: #f8fafc;"));
	root->addWidget(title_label_);

	error_banner_ = new QLabel(this);
	error_banner_->setObjectName(QStringLiteral("mfdSetupErrorBanner"));
	error_banner_->setWordWrap(true);
	error_banner_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #451a1a; color: #fecaca; border: 1px solid #ef4444; "
			       "border-radius: 6px; padding: 8px 12px; font-weight: bold; }"));
	error_banner_->hide();
	root->addWidget(error_banner_);

	stack_ = new QStackedWidget(this);
	stack_->setObjectName(QStringLiteral("mfdSetupStack"));
	root->addWidget(stack_, 1);

	/* --- Page 0: Step 1 ("What should this fader do?") --- */
	auto *page1 = new QWidget(stack_);
	auto *p1_lay = new QVBoxLayout(page1);
	p1_lay->setContentsMargins(0, 0, 0, 0);
	p1_lay->setSpacing(10);

	auto *p1_prompt = new QLabel(QStringLiteral("What should this fader do?"), page1);
	p1_prompt->setStyleSheet(QStringLiteral("color: #93c5fd; font-size: 14px; font-weight: bold;"));
	p1_lay->addWidget(p1_prompt);

	auto *grid = new QGridLayout();
	grid->setSpacing(10);
	card_tbar_ = create_mode_card_button(page1, QStringLiteral("mfdCardSwitchScenes"), MFD_TARGET_TBAR,
					     QStringLiteral("Switch scenes"),
					     QStringLiteral("Fade Program to Preview, like OBS's T-bar."));
	card_mix_ = create_mode_card_button(
		page1, QStringLiteral("mfdCardMixTwoLayers"), MFD_TARGET_TWO_SOURCE_MIX,
		QStringLiteral("Mix two layers"),
		QStringLiteral("In one scene, fade between two layers. Left is one, right is the other."));
	card_single_ = create_mode_card_button(page1, QStringLiteral("mfdCardShowHideLayer"), MFD_TARGET_SINGLE_LAYER,
					       QStringLiteral("Show or hide a layer"),
					       QStringLiteral("Fade one layer in (right) or out (left)."));
	card_volume_ = create_mode_card_button(page1, QStringLiteral("mfdCardVolume"), MFD_TARGET_AUDIO_VOLUME,
					       QStringLiteral("Volume"),
					       QStringLiteral("Control one audio source, like OBS's mixer slider."));
	card_filter_ = create_mode_card_button(
		page1, QStringLiteral("mfdCardFilterSetting"), MFD_TARGET_FILTER_PARAM,
		QStringLiteral("Filter or effect setting"),
		QStringLiteral("Sweep any numeric filter, shader, or reactivity slider on a layer or scene."));
	grid->addWidget(card_tbar_, 0, 0);
	grid->addWidget(card_mix_, 0, 1);
	grid->addWidget(card_single_, 1, 0);
	grid->addWidget(card_volume_, 1, 1);
	grid->addWidget(card_filter_, 2, 0, 1, 2);
	p1_lay->addLayout(grid, 1);

	auto *not_used_row = new QHBoxLayout();
	btn_not_used_ = new QPushButton(QStringLiteral("Not used (turn off this strip's assignment)"), page1);
	btn_not_used_->setObjectName(QStringLiteral("mfdBtnNotUsed"));
	btn_not_used_->setCursor(Qt::PointingHandCursor);
	btn_not_used_->setStyleSheet(
		QStringLiteral("QPushButton { background-color: #1e293b; color: #cbd5e1; border: 1px solid #475569; "
			       "border-radius: 6px; padding: 7px 14px; } "
			       "QPushButton:hover { border-color: #94a3b8; color: #f8fafc; }"));
	not_used_row->addWidget(btn_not_used_);
	not_used_row->addStretch(1);
	p1_lay->addLayout(not_used_row);

	connect(card_tbar_, &QPushButton::clicked, this, [this]() { select_mode_and_go_step2(MFD_TARGET_TBAR); });
	connect(card_mix_, &QPushButton::clicked, this,
		[this]() { select_mode_and_go_step2(MFD_TARGET_TWO_SOURCE_MIX); });
	connect(card_single_, &QPushButton::clicked, this,
		[this]() { select_mode_and_go_step2(MFD_TARGET_SINGLE_LAYER); });
	connect(card_volume_, &QPushButton::clicked, this,
		[this]() { select_mode_and_go_step2(MFD_TARGET_AUDIO_VOLUME); });
	connect(card_filter_, &QPushButton::clicked, this,
		[this]() { select_mode_and_go_step2(MFD_TARGET_FILTER_PARAM); });
	connect(btn_not_used_, &QPushButton::clicked, this, [this]() {
		committed_or_unassigned_ = true;
		mfd_engine_unassign_slot_preserve_addr(eng_, slot_index_);
		accept();
	});

	stack_->addWidget(page1);

	/* --- Page 1: Step 2 ("Which?") --- */
	auto *page2 = new QWidget(stack_);
	auto *p2_lay = new QVBoxLayout(page2);
	p2_lay->setContentsMargins(0, 0, 0, 0);
	p2_lay->setSpacing(12);

	auto *p2_prompt = new QLabel(QStringLiteral("Which?"), page2);
	p2_prompt->setStyleSheet(QStringLiteral("color: #93c5fd; font-size: 14px; font-weight: bold;"));
	p2_lay->addWidget(p2_prompt);

	/* Step 2: Switch scenes box */
	step2_tbar_box_ = new QWidget(page2);
	auto *tbar_box_lay = new QVBoxLayout(step2_tbar_box_);
	tbar_box_lay->setContentsMargins(12, 12, 12, 12);
	tbar_box_lay->setSpacing(10);
	step2_tbar_info_ = new QLabel(step2_tbar_box_);
	step2_tbar_info_->setObjectName(QStringLiteral("mfdStep2TbarInfo"));
	step2_tbar_info_->setWordWrap(true);
	step2_tbar_info_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: bold;"));
	tbar_box_lay->addWidget(step2_tbar_info_);

	step2_turn_on_studio_btn_ = new QPushButton(QStringLiteral("Turn on Studio Mode"), step2_tbar_box_);
	step2_turn_on_studio_btn_->setObjectName(QStringLiteral("mfdStep2TurnOnStudioBtn"));
	step2_turn_on_studio_btn_->setCursor(Qt::PointingHandCursor);
	step2_turn_on_studio_btn_->setFixedWidth(200);
	step2_turn_on_studio_btn_->setStyleSheet(
		QStringLiteral("QPushButton { background-color: #d97706; color: #111827; font-weight: bold; "
			       "border-radius: 5px; padding: 7px 14px; }"));
	connect(step2_turn_on_studio_btn_, &QPushButton::clicked, this, [this]() {
		mfd_engine_request_studio_mode(eng_, true);
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		update_step2_notes_and_validation();
	});
	tbar_box_lay->addWidget(step2_turn_on_studio_btn_);
	p2_lay->addWidget(step2_tbar_box_);

	/* Step 2: Mix two layers / Show or hide a layer box */
	step2_scene_box_ = new QWidget(page2);
	auto *sc_lay = new QVBoxLayout(step2_scene_box_);
	sc_lay->setContentsMargins(0, 0, 0, 0);
	sc_lay->setSpacing(10);

	step2_empty_scene_note_ = new QLabel(step2_scene_box_);
	step2_empty_scene_note_->setObjectName(QStringLiteral("mfdStep2EmptySceneNote"));
	step2_empty_scene_note_->setWordWrap(true);
	step2_empty_scene_note_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #3f2b05; color: #fde68a; border: 1px solid #d97706; "
			       "border-radius: 6px; padding: 8px 12px; }"));
	sc_lay->addWidget(step2_empty_scene_note_);

	auto *row_sc = new QHBoxLayout();
	auto *lbl_sc = new QLabel(QStringLiteral("Scene:"), step2_scene_box_);
	lbl_sc->setFixedWidth(70);
	lbl_sc->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_scene_ = new QComboBox(step2_scene_box_);
	combo_scene_->setObjectName(QStringLiteral("mfdComboScene"));
	row_sc->addWidget(lbl_sc);
	row_sc->addWidget(combo_scene_, 1);
	sc_lay->addLayout(row_sc);

	auto *row_left = new QHBoxLayout();
	lbl_left_caption_ = new QLabel(QStringLiteral("Left:"), step2_scene_box_);
	lbl_left_caption_->setFixedWidth(70);
	lbl_left_caption_->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_left_ = new QComboBox(step2_scene_box_);
	combo_left_->setObjectName(QStringLiteral("mfdComboLeft"));
	row_left->addWidget(lbl_left_caption_);
	row_left->addWidget(combo_left_, 1);
	sc_lay->addLayout(row_left);

	right_row_widget_ = new QWidget(step2_scene_box_);
	auto *row_right = new QHBoxLayout(right_row_widget_);
	row_right->setContentsMargins(0, 0, 0, 0);
	auto *lbl_right = new QLabel(QStringLiteral("Right:"), right_row_widget_);
	lbl_right->setFixedWidth(70);
	lbl_right->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_right_ = new QComboBox(right_row_widget_);
	combo_right_->setObjectName(QStringLiteral("mfdComboRight"));
	row_right->addWidget(lbl_right);
	row_right->addWidget(combo_right_, 1);
	sc_lay->addWidget(right_row_widget_);

	same_layer_refusal_ = new QLabel(QStringLiteral("Left and Right cannot be the same layer."), step2_scene_box_);
	same_layer_refusal_->setObjectName(QStringLiteral("mfdSameLayerRefusal"));
	same_layer_refusal_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #451a1a; color: #fecaca; border: 1px solid #ef4444; "
			       "border-radius: 5px; padding: 6px 10px; font-weight: bold; }"));
	same_layer_refusal_->hide();
	sc_lay->addWidget(same_layer_refusal_);

	live_scene_note_ = new QLabel(step2_scene_box_);
	live_scene_note_->setObjectName(QStringLiteral("mfdLiveSceneNote"));
	live_scene_note_->setWordWrap(true);
	live_scene_note_->setStyleSheet(QStringLiteral("color: #93c5fd; font-size: 13px;"));
	sc_lay->addWidget(live_scene_note_);

	shared_layer_note_ = new QLabel(step2_scene_box_);
	shared_layer_note_->setObjectName(QStringLiteral("mfdSharedLayerNote"));
	shared_layer_note_->setWordWrap(true);
	shared_layer_note_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #3f2b05; color: #fde68a; border: 1px solid #d97706; "
			       "border-radius: 5px; padding: 7px 10px; font-size: 12px; }"));
	shared_layer_note_->hide();
	sc_lay->addWidget(shared_layer_note_);

	p2_lay->addWidget(step2_scene_box_);

	/* Step 2: Volume box */
	step2_audio_box_ = new QWidget(page2);
	auto *aud_lay = new QVBoxLayout(step2_audio_box_);
	aud_lay->setContentsMargins(0, 0, 0, 0);
	aud_lay->setSpacing(10);

	step2_empty_audio_note_ =
		new QLabel(QStringLiteral("No sources with audio were found in OBS."), step2_audio_box_);
	step2_empty_audio_note_->setObjectName(QStringLiteral("mfdStep2EmptyAudioNote"));
	step2_empty_audio_note_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #3f2b05; color: #fde68a; border: 1px solid #d97706; "
			       "border-radius: 6px; padding: 8px 12px; }"));
	aud_lay->addWidget(step2_empty_audio_note_);

	auto *row_aud = new QHBoxLayout();
	auto *lbl_aud = new QLabel(QStringLiteral("Audio source:"), step2_audio_box_);
	lbl_aud->setFixedWidth(96);
	lbl_aud->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_audio_ = new QComboBox(step2_audio_box_);
	combo_audio_->setObjectName(QStringLiteral("mfdComboAudioSource"));
	row_aud->addWidget(lbl_aud);
	row_aud->addWidget(combo_audio_, 1);
	aud_lay->addLayout(row_aud);
	p2_lay->addWidget(step2_audio_box_);

	/* Step 2: Filter or effect setting box */
	step2_filter_box_ = new QWidget(page2);
	auto *flt_lay = new QVBoxLayout(step2_filter_box_);
	flt_lay->setContentsMargins(0, 0, 0, 0);
	flt_lay->setSpacing(9);

	step2_empty_filter_note_ = new QLabel(step2_filter_box_);
	step2_empty_filter_note_->setObjectName(QStringLiteral("mfdStep2EmptyFilterNote"));
	step2_empty_filter_note_->setWordWrap(true);
	step2_empty_filter_note_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #3f2b05; color: #fde68a; border: 1px solid #d97706; "
			       "border-radius: 6px; padding: 8px 12px; }"));
	step2_empty_filter_note_->hide();
	flt_lay->addWidget(step2_empty_filter_note_);

	auto *row_fhost = new QHBoxLayout();
	auto *lbl_fhost = new QLabel(QStringLiteral("Layer or Scene:"), step2_filter_box_);
	lbl_fhost->setFixedWidth(112);
	lbl_fhost->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_filter_host_ = new QComboBox(step2_filter_box_);
	combo_filter_host_->setObjectName(QStringLiteral("mfdComboFilterHost"));
	row_fhost->addWidget(lbl_fhost);
	row_fhost->addWidget(combo_filter_host_, 1);
	flt_lay->addLayout(row_fhost);

	auto *row_fname = new QHBoxLayout();
	auto *lbl_fname = new QLabel(QStringLiteral("Filter:"), step2_filter_box_);
	lbl_fname->setFixedWidth(112);
	lbl_fname->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_filter_name_ = new QComboBox(step2_filter_box_);
	combo_filter_name_->setObjectName(QStringLiteral("mfdComboFilterName"));
	row_fname->addWidget(lbl_fname);
	row_fname->addWidget(combo_filter_name_, 1);
	flt_lay->addLayout(row_fname);

	auto *row_fprop = new QHBoxLayout();
	auto *lbl_fprop = new QLabel(QStringLiteral("Setting:"), step2_filter_box_);
	lbl_fprop->setFixedWidth(112);
	lbl_fprop->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_filter_prop_ = new QComboBox(step2_filter_box_);
	combo_filter_prop_->setObjectName(QStringLiteral("mfdComboFilterProp"));
	row_fprop->addWidget(lbl_fprop);
	row_fprop->addWidget(combo_filter_prop_, 1);
	flt_lay->addLayout(row_fprop);

	const QString use_cur_style =
		QStringLiteral("QPushButton { background-color: #1e293b; color: #e2e8f0; border: 1px solid #475569; "
			       "border-radius: 5px; padding: 5px 10px; font-size: 12px; font-weight: bold; } "
			       "QPushButton:hover { border-color: #93c5fd; color: #ffffff; } "
			       "QPushButton:disabled { color: #64748b; border-color: #334155; }");

	auto *row_ffrom = new QHBoxLayout();
	auto *lbl_ffrom = new QLabel(QStringLiteral("At 0% (Clean):"), step2_filter_box_);
	lbl_ffrom->setFixedWidth(112);
	lbl_ffrom->setStyleSheet(QStringLiteral("font-weight: bold;"));
	spin_filter_from_ = new QDoubleSpinBox(step2_filter_box_);
	spin_filter_from_->setObjectName(QStringLiteral("mfdSpinFilterFrom"));
	btn_use_current_from_ = new QPushButton(QStringLiteral("Use current"), step2_filter_box_);
	btn_use_current_from_->setObjectName(QStringLiteral("mfdBtnUseCurrentFrom"));
	btn_use_current_from_->setCursor(Qt::PointingHandCursor);
	btn_use_current_from_->setStyleSheet(use_cur_style);
	row_ffrom->addWidget(lbl_ffrom);
	row_ffrom->addWidget(spin_filter_from_, 1);
	row_ffrom->addWidget(btn_use_current_from_);
	flt_lay->addLayout(row_ffrom);

	auto *row_fto = new QHBoxLayout();
	auto *lbl_fto = new QLabel(QStringLiteral("At 100% (Max):"), step2_filter_box_);
	lbl_fto->setFixedWidth(112);
	lbl_fto->setStyleSheet(QStringLiteral("font-weight: bold;"));
	spin_filter_to_ = new QDoubleSpinBox(step2_filter_box_);
	spin_filter_to_->setObjectName(QStringLiteral("mfdSpinFilterTo"));
	btn_use_current_to_ = new QPushButton(QStringLiteral("Use current"), step2_filter_box_);
	btn_use_current_to_->setObjectName(QStringLiteral("mfdBtnUseCurrentTo"));
	btn_use_current_to_->setCursor(Qt::PointingHandCursor);
	btn_use_current_to_->setStyleSheet(use_cur_style);
	row_fto->addWidget(lbl_fto);
	row_fto->addWidget(spin_filter_to_, 1);
	row_fto->addWidget(btn_use_current_to_);
	flt_lay->addLayout(row_fto);

	lbl_filter_limits_note_ = new QLabel(step2_filter_box_);
	lbl_filter_limits_note_->setObjectName(QStringLiteral("mfdFilterLimitsNote"));
	lbl_filter_limits_note_->setWordWrap(true);
	lbl_filter_limits_note_->setStyleSheet(QStringLiteral("color: #93c5fd; font-size: 12px;"));
	flt_lay->addWidget(lbl_filter_limits_note_);

	chk_filter_bypass_ = new QCheckBox(QStringLiteral("Turn filter off at 0% (saves GPU)"), step2_filter_box_);
	chk_filter_bypass_->setObjectName(QStringLiteral("mfdChkFilterBypass"));
	flt_lay->addWidget(chk_filter_bypass_);

	filter_validation_refusal_ = new QLabel(step2_filter_box_);
	filter_validation_refusal_->setObjectName(QStringLiteral("mfdFilterValidationRefusal"));
	filter_validation_refusal_->setWordWrap(true);
	filter_validation_refusal_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #451a1a; color: #fecaca; border: 1px solid #ef4444; "
			       "border-radius: 5px; padding: 6px 10px; font-weight: bold; }"));
	filter_validation_refusal_->hide();
	flt_lay->addWidget(filter_validation_refusal_);

	p2_lay->addWidget(step2_filter_box_);

	catalog_truncated_note_ = new QLabel(
		QStringLiteral("Showing the first 48 scenes/layers or 64 audio sources in this collection."), page2);
	catalog_truncated_note_->setObjectName(QStringLiteral("mfdCatalogTruncatedNote"));
	catalog_truncated_note_->setWordWrap(true);
	catalog_truncated_note_->setStyleSheet(QStringLiteral("color: #fcd34d; font-size: 12px;"));
	catalog_truncated_note_->hide();
	p2_lay->addWidget(catalog_truncated_note_);
	p2_lay->addStretch(1);

	connect(combo_scene_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_scene_ = combo_scene_->currentData().toString();
		repopulate_layer_combos_for_scene();
		update_step2_notes_and_validation();
	});
	connect(combo_left_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_left_ = combo_left_->currentData().toString();
		update_step2_notes_and_validation();
	});
	connect(combo_right_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_right_ = combo_right_->currentData().toString();
		update_step2_notes_and_validation();
	});
	connect(combo_audio_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_audio_ = combo_audio_->currentData().toString();
		update_step2_notes_and_validation();
	});
	connect(combo_filter_host_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_filter_host_ = combo_filter_host_->currentData().toString();
		chosen_filter_has_custom_range_ = false;
		repopulate_filter_combos_for_host(false);
		update_step2_notes_and_validation();
	});
	connect(combo_filter_name_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_filter_name_ = combo_filter_name_->currentData().toString();
		chosen_filter_has_custom_range_ = false;
		repopulate_prop_combo_for_filter(false);
		update_step2_notes_and_validation();
	});
	connect(combo_filter_prop_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_filter_prop_ = combo_filter_prop_->currentData().toString();
		chosen_filter_has_custom_range_ = false;
		configure_spinboxes_for_prop(false);
		update_step2_notes_and_validation();
	});
	connect(spin_filter_from_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
		if (updating_widgets_) {
			return;
		}
		chosen_filter_from_ = val;
		chosen_filter_has_custom_range_ = true;
		update_step2_notes_and_validation();
	});
	connect(spin_filter_to_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double val) {
		if (updating_widgets_) {
			return;
		}
		chosen_filter_to_ = val;
		chosen_filter_has_custom_range_ = true;
		update_step2_notes_and_validation();
	});
	connect(btn_use_current_from_, &QPushButton::clicked, this, [this]() {
		if (filter_cat_ && !chosen_filter_host_.isEmpty()) {
			mfd_engine_get_source_filter_catalog(eng_, chosen_filter_host_.toUtf8().constData(),
							     filter_cat_);
		}
		const struct mfd_filter_prop_info *pi = find_selected_filter_prop();
		if (!pi) {
			return;
		}
		spin_filter_from_->setValue(pi->current_val);
	});
	connect(btn_use_current_to_, &QPushButton::clicked, this, [this]() {
		if (filter_cat_ && !chosen_filter_host_.isEmpty()) {
			mfd_engine_get_source_filter_catalog(eng_, chosen_filter_host_.toUtf8().constData(),
							     filter_cat_);
		}
		const struct mfd_filter_prop_info *pi = find_selected_filter_prop();
		if (!pi) {
			return;
		}
		spin_filter_to_->setValue(pi->current_val);
	});
	connect(chk_filter_bypass_, &QCheckBox::toggled, this, [this](bool checked) {
		if (!updating_widgets_) {
			chosen_filter_bypass_ = checked;
			update_step2_notes_and_validation();
		}
	});

	stack_->addWidget(page2);

	/* --- Page 2: Step 3 ("Try it.") --- */
	auto *page3 = new QWidget(stack_);
	auto *p3_lay = new QVBoxLayout(page3);
	p3_lay->setContentsMargins(0, 0, 0, 0);
	p3_lay->setSpacing(10);

	auto *p3_prompt = new QLabel(QStringLiteral("Try it."), page3);
	p3_prompt->setStyleSheet(QStringLiteral("color: #93c5fd; font-size: 14px; font-weight: bold;"));
	p3_lay->addWidget(p3_prompt);

	finished_sentence_ = new QLabel(page3);
	finished_sentence_->setObjectName(QStringLiteral("mfdFinishedSentence"));
	finished_sentence_->setWordWrap(true);
	finished_sentence_->setStyleSheet(
		QStringLiteral("QLabel { background-color: #111827; color: #f8fafc; border: 1px solid #38bdf8; "
			       "border-radius: 7px; padding: 10px 14px; font-size: 14px; font-weight: bold; }"));
	p3_lay->addWidget(finished_sentence_);

	candidate_meter_ = new QProgressBar(page3);
	candidate_meter_->setObjectName(QStringLiteral("mfdCandidateMeter"));
	candidate_meter_->setRange(0, 100);
	candidate_meter_->setValue(0);
	candidate_meter_->setTextVisible(true);
	candidate_meter_->setAlignment(Qt::AlignCenter);
	candidate_meter_->setStyleSheet(
		QStringLiteral("QProgressBar { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; "
			       "border-radius: 6px; min-height: 26px; font-weight: bold; text-align: center; } "
			       "QProgressBar::chunk { background-color: #2563eb; border-radius: 5px; }"));
	p3_lay->addWidget(candidate_meter_);

	feel_box_ = new QWidget(page3);
	auto *feel_lay = new QVBoxLayout(feel_box_);
	feel_lay->setContentsMargins(0, 0, 0, 0);
	feel_lay->setSpacing(4);
	auto *feel_row = new QHBoxLayout();
	auto *lbl_feel = new QLabel(QStringLiteral("Feel:"), feel_box_);
	lbl_feel->setFixedWidth(56);
	lbl_feel->setStyleSheet(QStringLiteral("font-weight: bold;"));
	combo_feel_ = new QComboBox(feel_box_);
	combo_feel_->setObjectName(QStringLiteral("mfdComboFeel"));
	combo_feel_->addItem(QStringLiteral("Dip-free"), static_cast<int>(MFD_CURVE_DIP_FREE));
	combo_feel_->addItem(QStringLiteral("Equal power"), static_cast<int>(MFD_CURVE_COSINE));
	combo_feel_->addItem(QStringLiteral("Linear"), static_cast<int>(MFD_CURVE_LINEAR));
	combo_feel_->addItem(QStringLiteral("Cut"), static_cast<int>(MFD_CURVE_CUT));
	feel_row->addWidget(lbl_feel);
	feel_row->addWidget(combo_feel_, 1);
	feel_lay->addLayout(feel_row);

	feel_desc_label_ = new QLabel(feel_box_);
	feel_desc_label_->setObjectName(QStringLiteral("mfdFeelDescLabel"));
	feel_desc_label_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px; padding-left: 60px;"));
	feel_lay->addWidget(feel_desc_label_);
	p3_lay->addWidget(feel_box_);

	connect(combo_feel_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
		if (updating_widgets_) {
			return;
		}
		chosen_curve_ = static_cast<enum mfd_crossfade_curve>(combo_feel_->currentData().toInt());
		feel_desc_label_->setText(QString::fromUtf8(mfd_ui_curve_plain_description(chosen_curve_)));
	});

	chk_also_fade_audio_ = new QCheckBox(QStringLiteral("Also fade audio"), page3);
	chk_also_fade_audio_->setObjectName(QStringLiteral("mfdChkAlsoFadeAudio"));
	connect(chk_also_fade_audio_, &QCheckBox::toggled, this, [this](bool checked) {
		if (!updating_widgets_) {
			chosen_afv_ = checked;
		}
	});
	p3_lay->addWidget(chk_also_fade_audio_);

	volume_law_label_ = new QLabel(
		QStringLiteral(
			"Uses OBS's own cubic dB fader curve (0.0 dB at top, smooth fade to silence at bottom)."),
		page3);
	volume_law_label_->setObjectName(QStringLiteral("mfdVolumeLawLabel"));
	volume_law_label_->setWordWrap(true);
	volume_law_label_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px;"));
	p3_lay->addWidget(volume_law_label_);

	chk_reverse_dir_ = new QCheckBox(QStringLiteral("Reverse direction"), page3);
	chk_reverse_dir_->setObjectName(QStringLiteral("mfdChkReverseDirection"));
	connect(chk_reverse_dir_, &QCheckBox::toggled, this, [this](bool checked) {
		if (!updating_widgets_) {
			mfd_engine_set_setup_invert(eng_, checked);
			mfd_engine_get_ui_snapshot(eng_, &snap_);
			update_step3_controls();
		}
	});
	p3_lay->addWidget(chk_reverse_dir_);

	tbar_step3_box_ = new QWidget(page3);
	auto *tb3_lay = new QHBoxLayout(tbar_step3_box_);
	tb3_lay->setContentsMargins(0, 0, 0, 0);
	chk_tbar_bidir_ = new QCheckBox(QStringLiteral("T-bar works in both directions"), tbar_step3_box_);
	chk_tbar_bidir_->setObjectName(QStringLiteral("mfdChkTbarBidir"));
	connect(chk_tbar_bidir_, &QCheckBox::toggled, this, [this](bool checked) {
		if (!updating_widgets_) {
			chosen_tbar_bidir_ = checked;
		}
	});
	btn_step3_finish_now_ = new QPushButton(QStringLiteral("Finish now"), tbar_step3_box_);
	btn_step3_finish_now_->setObjectName(QStringLiteral("mfdStep3FinishNowBtn"));
	btn_step3_finish_now_->setStyleSheet(
		QStringLiteral("QPushButton { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; "
			       "border-radius: 5px; padding: 5px 12px; } "
			       "QPushButton:disabled { color: #64748b; border-color: #334155; }"));
	connect(btn_step3_finish_now_, &QPushButton::clicked, this, [this]() {
		mfd_engine_finish_tbar_throw(eng_, slot_index_);
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		update_step3_controls();
	});
	tb3_lay->addWidget(chk_tbar_bidir_, 1);
	tb3_lay->addWidget(btn_step3_finish_now_);
	p3_lay->addWidget(tbar_step3_box_);

	btn_hw_toggle_ = new QPushButton(QStringLiteral("Which hardware control? \xE2\x96\xBE"), page3);
	btn_hw_toggle_->setObjectName(QStringLiteral("mfdBtnHardwareToggle"));
	btn_hw_toggle_->setCursor(Qt::PointingHandCursor);
	btn_hw_toggle_->setFlat(true);
	btn_hw_toggle_->setStyleSheet(
		QStringLiteral("QPushButton { color: #93c5fd; text-align: left; font-weight: bold; border: none; "
			       "padding: 4px 0px; }"));
	p3_lay->addWidget(btn_hw_toggle_);

	hw_panel_ = new QWidget(page3);
	hw_panel_->setObjectName(QStringLiteral("mfdHardwarePanel"));
	auto *hw_lay = new QVBoxLayout(hw_panel_);
	hw_lay->setContentsMargins(10, 8, 10, 8);
	hw_lay->setSpacing(6);
	hw_panel_->setStyleSheet(QStringLiteral(
		"QWidget#mfdHardwarePanel { background-color: #111827; border: 1px solid #334155; border-radius: 6px; }"));

	auto *hw_top = new QHBoxLayout();
	candidate_addr_label_ = new QLabel(hw_panel_);
	candidate_addr_label_->setObjectName(QStringLiteral("mfdCandidateAddrLabel"));
	btn_setup_learn_ = new QPushButton(QStringLiteral("Learn"), hw_panel_);
	btn_setup_learn_->setObjectName(QStringLiteral("mfdSetupLearnBtn"));
	btn_setup_learn_->setMinimumWidth(140);
	connect(btn_setup_learn_, &QPushButton::clicked, this, [this]() {
		if (snap_.setup.learning) {
			mfd_engine_cancel_setup_learn(eng_);
		} else {
			mfd_engine_start_setup_learn(eng_);
		}
		mfd_engine_get_ui_snapshot(eng_, &snap_);
		update_step3_controls();
	});
	hw_top->addWidget(candidate_addr_label_, 1);
	hw_top->addWidget(btn_setup_learn_);
	hw_lay->addLayout(hw_top);

	collision_notice_ = new QLabel(hw_panel_);
	collision_notice_->setObjectName(QStringLiteral("mfdCollisionNotice"));
	collision_notice_->setWordWrap(true);
	collision_notice_->setStyleSheet(QStringLiteral("color: #fcd34d; font-size: 12px;"));
	collision_notice_->hide();
	hw_lay->addWidget(collision_notice_);
	hw_panel_->hide();
	p3_lay->addWidget(hw_panel_);

	connect(btn_hw_toggle_, &QPushButton::clicked, this,
		[this]() { hw_panel_->setVisible(!hw_panel_->isVisible()); });

	p3_lay->addStretch(1);
	stack_->addWidget(page3);

	/* --- Footer --- */
	auto *sep = new QFrame(this);
	sep->setFrameShape(QFrame::HLine);
	sep->setStyleSheet(QStringLiteral("color: #1e293b;"));
	root->addWidget(sep);

	auto *footer = new QHBoxLayout();
	btn_cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	btn_cancel_->setObjectName(QStringLiteral("mfdSetupCancelBtn"));
	btn_back_ = new QPushButton(QStringLiteral("Back"), this);
	btn_back_->setObjectName(QStringLiteral("mfdSetupBackBtn"));
	btn_next_ = new QPushButton(QStringLiteral("Next"), this);
	btn_next_->setObjectName(QStringLiteral("mfdSetupNextBtn"));
	btn_done_ = new QPushButton(QStringLiteral("Done"), this);
	btn_done_->setObjectName(QStringLiteral("mfdSetupDoneBtn"));

	const QString sec_btn_style =
		QStringLiteral("QPushButton { background-color: #1e293b; color: #e2e8f0; border: 1px solid #475569; "
			       "border-radius: 6px; padding: 7px 16px; font-weight: bold; } "
			       "QPushButton:disabled { color: #64748b; border-color: #334155; }");
	const QString pri_btn_style = QStringLiteral(
		"QPushButton { background-color: #2563eb; color: #ffffff; border: 1px solid #3b82f6; "
		"border-radius: 6px; padding: 7px 20px; font-weight: bold; } "
		"QPushButton:disabled { background-color: #1e293b; color: #64748b; border-color: #334155; }");

	btn_cancel_->setStyleSheet(sec_btn_style);
	btn_back_->setStyleSheet(sec_btn_style);
	btn_next_->setStyleSheet(pri_btn_style);
	btn_done_->setStyleSheet(pri_btn_style);

	footer->addWidget(btn_cancel_);
	footer->addStretch(1);
	footer->addWidget(btn_back_);
	footer->addWidget(btn_next_);
	footer->addWidget(btn_done_);
	root->addLayout(footer);

	connect(btn_cancel_, &QPushButton::clicked, this, &MfdSetupSheetDialog::reject);
	connect(btn_back_, &QPushButton::clicked, this, [this]() {
		error_banner_->hide();
		if (stack_->currentIndex() > 0) {
			stack_->setCurrentIndex(stack_->currentIndex() - 1);
			update_footer_buttons();
		}
	});
	connect(btn_next_, &QPushButton::clicked, this, [this]() {
		error_banner_->hide();
		if (stack_->currentIndex() == 0) {
			select_mode_and_go_step2(chosen_mode_);
		} else if (stack_->currentIndex() == 1) {
			struct mfd_setup_candidate cand = build_candidate();
			char rsn[160] = {0};
			if (!mfd_engine_validate_setup_candidate(eng_, &cand, rsn, sizeof(rsn))) {
				error_banner_->setText(QString::fromUtf8(rsn));
				error_banner_->show();
				update_step2_notes_and_validation();
				return;
			}
			stack_->setCurrentIndex(2);
			update_step3_controls();
			update_footer_buttons();
		}
	});
	connect(btn_done_, &QPushButton::clicked, this, &MfdSetupSheetDialog::on_done_clicked);
}

MfdSetupSheetDialog::~MfdSetupSheetDialog()
{
	if (!committed_or_unassigned_ && eng_) {
		mfd_engine_cancel_setup_session(eng_);
	}
	mfd_source_filter_catalog_destroy(filter_cat_);
	filter_cat_ = nullptr;
	mfd_ui_catalog_destroy(catalog_);
	catalog_ = nullptr;
}

void MfdSetupSheetDialog::reject()
{
	if (!committed_or_unassigned_ && eng_) {
		committed_or_unassigned_ = true;
		mfd_engine_cancel_setup_session(eng_);
	}
	QDialog::reject();
}

void MfdSetupSheetDialog::closeEvent(QCloseEvent *event)
{
	if (!committed_or_unassigned_ && eng_) {
		committed_or_unassigned_ = true;
		mfd_engine_cancel_setup_session(eng_);
	}
	QDialog::closeEvent(event);
}

void MfdSetupSheetDialog::open_for_slot(int slot_index)
{
	if (!eng_ || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	slot_index_ = slot_index;
	committed_or_unassigned_ = false;
	error_banner_->hide();

	mfd_engine_begin_setup_session(eng_, slot_index_);
	mfd_engine_get_ui_snapshot(eng_, &snap_);
	if (catalog_) {
		mfd_engine_get_ui_catalog(eng_, catalog_);
		last_topology_rev_ = catalog_->config_topology_rev;
	}

	const struct mfd_fader_slot &s = snap_.slots[slot_index_];
	if (s.target_type == MFD_TARGET_UNASSIGNED) {
		chosen_mode_ = mfd_slot_is_fx_bank(slot_index_) ? MFD_TARGET_FILTER_PARAM : MFD_TARGET_TWO_SOURCE_MIX;
	} else {
		chosen_mode_ = s.target_type;
	}
	chosen_scene_ = QString::fromUtf8((s.scene_name[0] != '\0') ? s.scene_name : s.resolved_scene_name);
	if (s.target_type == MFD_TARGET_AUDIO_VOLUME) {
		chosen_audio_ = QString::fromUtf8(s.source_a_name);
		chosen_left_.clear();
		chosen_right_.clear();
	} else if (s.target_type == MFD_TARGET_FILTER_PARAM) {
		chosen_left_.clear();
		chosen_right_.clear();
		chosen_audio_.clear();
	} else {
		chosen_left_ = QString::fromUtf8(s.source_a_name);
		chosen_right_ = QString::fromUtf8(s.source_b_name);
		chosen_audio_.clear();
	}
	if (s.target_type == MFD_TARGET_FILTER_PARAM) {
		chosen_filter_host_ = QString::fromUtf8(s.source_a_name);
		chosen_filter_name_ = QString::fromUtf8(s.filter_name);
		chosen_filter_prop_ = QString::fromUtf8(s.filter_prop);
		chosen_filter_from_ = s.filter_from_val;
		chosen_filter_to_ = s.filter_to_val;
		chosen_filter_has_custom_range_ =
			(s.source_a_name[0] != '\0' && s.filter_name[0] != '\0' && s.filter_prop[0] != '\0');
		chosen_filter_bypass_ = s.filter_bypass_at_zero;
	} else {
		chosen_filter_host_ = (s.target_type != MFD_TARGET_AUDIO_VOLUME && s.source_a_name[0] != '\0')
					      ? QString::fromUtf8(s.source_a_name)
					      : QString();
		chosen_filter_name_.clear();
		chosen_filter_prop_.clear();
		chosen_filter_from_ = 0.0;
		chosen_filter_to_ = 1.0;
		chosen_filter_has_custom_range_ = false;
		chosen_filter_bypass_ = false;
	}
	chosen_curve_ = (s.curve == MFD_CURVE_FULL_CENTER) ? MFD_CURVE_DIP_FREE : s.curve;
	chosen_afv_ = s.audio_follows_video;
	chosen_tbar_bidir_ = snap_.tbar_bidirectional_flip;

	card_tbar_->setChecked(chosen_mode_ == MFD_TARGET_TBAR);
	card_mix_->setChecked(chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX);
	card_single_->setChecked(chosen_mode_ == MFD_TARGET_SINGLE_LAYER);
	card_volume_->setChecked(chosen_mode_ == MFD_TARGET_AUDIO_VOLUME);
	card_filter_->setChecked(chosen_mode_ == MFD_TARGET_FILTER_PARAM);

	hw_panel_->setVisible(snap_.setup.candidate_addr.type == MFD_MIDI_NONE);
	stack_->setCurrentIndex(0);
	repopulate_step2_pickers();
	update_step3_controls();
	update_footer_buttons();

	show();
	raise();
}

void MfdSetupSheetDialog::refresh_from_snapshot(const struct mfd_ui_snapshot &snap)
{
	snap_ = snap;
	if (!isVisible()) {
		return;
	}
	if (!snap_.setup.active || snap_.setup.slot_index != slot_index_) {
		committed_or_unassigned_ = true;
		close();
		return;
	}
	if (catalog_ && snap_.config_topology_rev != last_topology_rev_) {
		mfd_engine_get_ui_catalog(eng_, catalog_);
		last_topology_rev_ = catalog_->config_topology_rev;
		repopulate_step2_pickers();
	} else {
		update_step2_notes_and_validation();
	}
	update_step3_controls();
	update_footer_buttons();
}

struct mfd_setup_candidate MfdSetupSheetDialog::build_candidate() const
{
	struct mfd_setup_candidate cand;
	std::memset(&cand, 0, sizeof(cand));
	cand.slot_index = slot_index_;
	cand.enabled = true;
	cand.target_type = chosen_mode_;
	cand.curve = chosen_curve_;
	cand.audio_follows_video =
		(chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX || chosen_mode_ == MFD_TARGET_SINGLE_LAYER) &&
		chosen_layers_have_audio() && chosen_afv_;
	cand.apply_tbar_bidir = (chosen_mode_ == MFD_TARGET_TBAR);
	cand.tbar_bidirectional_flip = chosen_tbar_bidir_;

	if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX) {
		const QByteArray sc = chosen_scene_.toUtf8();
		const QByteArray sa = chosen_left_.toUtf8();
		const QByteArray sb = chosen_right_.toUtf8();
		std::snprintf(cand.scene_name, sizeof(cand.scene_name), "%s", sc.constData());
		std::snprintf(cand.source_a_name, sizeof(cand.source_a_name), "%s", sa.constData());
		std::snprintf(cand.source_b_name, sizeof(cand.source_b_name), "%s", sb.constData());
	} else if (chosen_mode_ == MFD_TARGET_SINGLE_LAYER) {
		const QByteArray sc = chosen_scene_.toUtf8();
		const QByteArray sa = chosen_left_.toUtf8();
		std::snprintf(cand.scene_name, sizeof(cand.scene_name), "%s", sc.constData());
		std::snprintf(cand.source_a_name, sizeof(cand.source_a_name), "%s", sa.constData());
	} else if (chosen_mode_ == MFD_TARGET_AUDIO_VOLUME) {
		const QByteArray sa = chosen_audio_.toUtf8();
		std::snprintf(cand.source_a_name, sizeof(cand.source_a_name), "%s", sa.constData());
	} else if (chosen_mode_ == MFD_TARGET_FILTER_PARAM) {
		const QByteArray sa = chosen_filter_host_.toUtf8();
		const QByteArray fn = chosen_filter_name_.toUtf8();
		const QByteArray fp = chosen_filter_prop_.toUtf8();
		std::snprintf(cand.source_a_name, sizeof(cand.source_a_name), "%s", sa.constData());
		std::snprintf(cand.filter_name, sizeof(cand.filter_name), "%s", fn.constData());
		std::snprintf(cand.filter_prop, sizeof(cand.filter_prop), "%s", fp.constData());
		cand.filter_from_val = chosen_filter_from_;
		cand.filter_to_val = chosen_filter_to_;
		cand.filter_bypass_at_zero = chosen_filter_bypass_;
	}
	return cand;
}

const struct mfd_ui_scene_entry *MfdSetupSheetDialog::find_catalog_scene(const QString &name) const
{
	if (!catalog_ || name.isEmpty()) {
		return nullptr;
	}
	const QByteArray utf8 = name.toUtf8();
	for (size_t i = 0U; i < catalog_->scene_count; ++i) {
		if (std::strcmp(catalog_->scenes[i].name, utf8.constData()) == 0) {
			return &catalog_->scenes[i];
		}
	}
	return nullptr;
}

const struct mfd_filter_entry_info *MfdSetupSheetDialog::find_selected_filter_entry() const
{
	if (!filter_cat_ || chosen_filter_name_.isEmpty()) {
		return nullptr;
	}
	const QByteArray fn_utf8 = chosen_filter_name_.toUtf8();
	for (size_t i = 0U; i < filter_cat_->filter_count; ++i) {
		if (std::strcmp(filter_cat_->filters[i].name, fn_utf8.constData()) == 0) {
			return &filter_cat_->filters[i];
		}
	}
	return nullptr;
}

const struct mfd_filter_prop_info *MfdSetupSheetDialog::find_selected_filter_prop() const
{
	const struct mfd_filter_entry_info *fe = find_selected_filter_entry();
	if (!fe || chosen_filter_prop_.isEmpty()) {
		return nullptr;
	}
	const QByteArray fp_utf8 = chosen_filter_prop_.toUtf8();
	for (size_t i = 0U; i < fe->prop_count; ++i) {
		if (std::strcmp(fe->props[i].name, fp_utf8.constData()) == 0) {
			return &fe->props[i];
		}
	}
	return nullptr;
}

bool MfdSetupSheetDialog::chosen_layers_have_audio() const
{
	const struct mfd_ui_scene_entry *se = find_catalog_scene(chosen_scene_);
	if (!se) {
		return false;
	}
	const QByteArray la = chosen_left_.toUtf8();
	const QByteArray lb = chosen_right_.toUtf8();
	for (size_t i = 0U; i < se->layer_count; ++i) {
		if (!se->layer_has_audio[i]) {
			continue;
		}
		if (!la.isEmpty() && std::strcmp(se->layers[i], la.constData()) == 0) {
			return true;
		}
		if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX && !lb.isEmpty() &&
		    std::strcmp(se->layers[i], lb.constData()) == 0) {
			return true;
		}
	}
	return false;
}

void MfdSetupSheetDialog::select_mode_and_go_step2(enum mfd_target_type mode)
{
	chosen_mode_ = mode;
	card_tbar_->setChecked(mode == MFD_TARGET_TBAR);
	card_mix_->setChecked(mode == MFD_TARGET_TWO_SOURCE_MIX);
	card_single_->setChecked(mode == MFD_TARGET_SINGLE_LAYER);
	card_volume_->setChecked(mode == MFD_TARGET_AUDIO_VOLUME);
	card_filter_->setChecked(mode == MFD_TARGET_FILTER_PARAM);
	error_banner_->hide();
	repopulate_step2_pickers();
	stack_->setCurrentIndex(1);
	update_footer_buttons();
}

void MfdSetupSheetDialog::repopulate_step2_pickers()
{
	updating_widgets_ = true;

	step2_tbar_box_->setVisible(chosen_mode_ == MFD_TARGET_TBAR);
	step2_scene_box_->setVisible(chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX ||
				     chosen_mode_ == MFD_TARGET_SINGLE_LAYER);
	step2_audio_box_->setVisible(chosen_mode_ == MFD_TARGET_AUDIO_VOLUME);
	step2_filter_box_->setVisible(chosen_mode_ == MFD_TARGET_FILTER_PARAM);

	if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX || chosen_mode_ == MFD_TARGET_SINGLE_LAYER) {
		const size_t min_layers = (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX) ? 2U : 1U;
		lbl_left_caption_->setText((chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX) ? QStringLiteral("Left:")
										       : QStringLiteral("Layer:"));
		right_row_widget_->setVisible(chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX);

		combo_scene_->clear();
		int selected_idx = -1;
		if (catalog_) {
			for (size_t i = 0U; i < catalog_->scene_count; ++i) {
				const struct mfd_ui_scene_entry &se = catalog_->scenes[i];
				if (se.layer_count < min_layers) {
					continue;
				}
				const QString sc_name = QString::fromUtf8(se.name);
				combo_scene_->addItem(sc_name, sc_name);
				if (sc_name == chosen_scene_) {
					selected_idx = combo_scene_->count() - 1;
				}
			}
		}
		if (combo_scene_->count() > 0) {
			if (selected_idx < 0) {
				selected_idx = 0;
			}
			combo_scene_->setCurrentIndex(selected_idx);
			chosen_scene_ = combo_scene_->currentData().toString();
			step2_empty_scene_note_->hide();
			combo_scene_->setEnabled(true);
		} else {
			chosen_scene_.clear();
			combo_scene_->setEnabled(false);
			step2_empty_scene_note_->setText(
				(chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX)
					? QStringLiteral(
						  "No scene has at least two layers yet. Add two sources to a scene in OBS, and they will appear here automatically.")
					: QStringLiteral(
						  "No scene has any layers yet. Add a source to a scene in OBS, and it will appear here automatically."));
			step2_empty_scene_note_->show();
		}
		repopulate_layer_combos_for_scene();
	} else if (chosen_mode_ == MFD_TARGET_AUDIO_VOLUME) {
		combo_audio_->clear();
		int selected_idx = -1;
		if (catalog_) {
			for (size_t i = 0U; i < catalog_->audio_count; ++i) {
				const struct mfd_ui_audio_entry &ae = catalog_->audio_sources[i];
				const QString nm = QString::fromUtf8(ae.name);
				QString db_str;
				if (!std::isfinite(ae.db)) {
					db_str = QStringLiteral("-inf dB");
				} else if (std::fabs(ae.db) < 0.05f) {
					db_str = QStringLiteral("0.0 dB");
				} else {
					db_str = QString::asprintf("%.1f dB", static_cast<double>(ae.db));
				}
				combo_audio_->addItem(QStringLiteral("%1  (%2)").arg(nm, db_str), nm);
				if (nm == chosen_audio_) {
					selected_idx = combo_audio_->count() - 1;
				}
			}
		}
		if (combo_audio_->count() > 0) {
			if (selected_idx < 0) {
				selected_idx = 0;
			}
			combo_audio_->setCurrentIndex(selected_idx);
			chosen_audio_ = combo_audio_->currentData().toString();
			combo_audio_->setEnabled(true);
			step2_empty_audio_note_->hide();
		} else {
			chosen_audio_.clear();
			combo_audio_->setEnabled(false);
			step2_empty_audio_note_->show();
		}
	} else if (chosen_mode_ == MFD_TARGET_FILTER_PARAM) {
		combo_filter_host_->clear();
		int selected_idx = -1;
		if (catalog_) {
			for (size_t i = 0U; i < catalog_->filter_host_count; ++i) {
				const struct mfd_ui_filter_host_summary &fh = catalog_->filter_hosts[i];
				if (fh.filter_count == 0U) {
					continue;
				}
				const QString nm = QString::fromUtf8(fh.name);
				const QString cnt_lbl = (fh.filter_count == 1U)
								? QStringLiteral("1 filter")
								: QStringLiteral("%1 filters").arg(fh.filter_count);
				const QString disp =
					fh.is_scene ? QStringLiteral("%1  (Scene \xC2\xB7 %2)").arg(nm, cnt_lbl)
						    : QStringLiteral("%1  (%2)").arg(nm, cnt_lbl);
				combo_filter_host_->addItem(disp, nm);
				if (nm == chosen_filter_host_) {
					selected_idx = combo_filter_host_->count() - 1;
				}
			}
		}
		chk_filter_bypass_->setChecked(chosen_filter_bypass_);
		if (combo_filter_host_->count() > 0) {
			const bool host_preserved = (selected_idx >= 0);
			if (selected_idx < 0) {
				selected_idx = 0;
			}
			combo_filter_host_->setCurrentIndex(selected_idx);
			chosen_filter_host_ = combo_filter_host_->currentData().toString();
			combo_filter_host_->setEnabled(true);
			step2_empty_filter_note_->hide();
			repopulate_filter_combos_for_host(host_preserved && chosen_filter_has_custom_range_);
		} else {
			chosen_filter_host_.clear();
			chosen_filter_name_.clear();
			chosen_filter_prop_.clear();
			combo_filter_host_->setEnabled(false);
			combo_filter_name_->clear();
			combo_filter_name_->setEnabled(false);
			combo_filter_prop_->clear();
			combo_filter_prop_->setEnabled(false);
			spin_filter_from_->setEnabled(false);
			spin_filter_to_->setEnabled(false);
			btn_use_current_from_->setEnabled(false);
			btn_use_current_to_->setEnabled(false);
			chk_filter_bypass_->setEnabled(false);
			lbl_filter_limits_note_->clear();
			step2_empty_filter_note_->setText(QStringLiteral(
				"No sources or scenes have filters yet. Right-click a source or scene in OBS, "
				"choose Filters, add an effect, and it will appear here automatically."));
			step2_empty_filter_note_->show();
		}
	}

	updating_widgets_ = false;
	update_step2_notes_and_validation();
}

void MfdSetupSheetDialog::repopulate_filter_combos_for_host(bool preserve_range)
{
	const bool prev_upd = updating_widgets_;
	updating_widgets_ = true;

	combo_filter_name_->clear();
	combo_filter_prop_->clear();

	if (!filter_cat_ || chosen_filter_host_.isEmpty() ||
	    !mfd_engine_get_source_filter_catalog(eng_, chosen_filter_host_.toUtf8().constData(), filter_cat_) ||
	    !filter_cat_->source_exists || filter_cat_->filter_count == 0U) {
		chosen_filter_name_.clear();
		chosen_filter_prop_.clear();
		combo_filter_name_->setEnabled(false);
		combo_filter_prop_->setEnabled(false);
		spin_filter_from_->setEnabled(false);
		spin_filter_to_->setEnabled(false);
		btn_use_current_from_->setEnabled(false);
		btn_use_current_to_->setEnabled(false);
		chk_filter_bypass_->setEnabled(false);
		lbl_filter_limits_note_->clear();
		step2_empty_filter_note_->setText(
			QStringLiteral("No filters were found on '%1'. Add a filter in OBS Filters first.")
				.arg(chosen_filter_host_));
		step2_empty_filter_note_->show();
		updating_widgets_ = prev_upd;
		return;
	}

	int selected_idx = -1;
	int first_with_props = -1;
	for (size_t i = 0U; i < filter_cat_->filter_count; ++i) {
		const struct mfd_filter_entry_info &fe = filter_cat_->filters[i];
		const QString fnm = QString::fromUtf8(fe.name);
		combo_filter_name_->addItem(fnm, fnm);
		if (first_with_props < 0 && fe.prop_count > 0U) {
			first_with_props = static_cast<int>(i);
		}
		if (fnm == chosen_filter_name_) {
			selected_idx = static_cast<int>(i);
		}
	}

	const bool filter_preserved = (selected_idx >= 0);
	if (selected_idx < 0) {
		selected_idx = (first_with_props >= 0) ? first_with_props : 0;
	}
	combo_filter_name_->setCurrentIndex(selected_idx);
	chosen_filter_name_ = combo_filter_name_->currentData().toString();
	combo_filter_name_->setEnabled(true);
	step2_empty_filter_note_->hide();

	repopulate_prop_combo_for_filter(preserve_range && filter_preserved);
	updating_widgets_ = prev_upd;
}

void MfdSetupSheetDialog::repopulate_prop_combo_for_filter(bool preserve_range)
{
	const bool prev_upd = updating_widgets_;
	updating_widgets_ = true;

	combo_filter_prop_->clear();
	const struct mfd_filter_entry_info *fe = find_selected_filter_entry();
	if (!fe || fe->prop_count == 0U) {
		chosen_filter_prop_.clear();
		combo_filter_prop_->setEnabled(false);
		spin_filter_from_->setEnabled(false);
		spin_filter_to_->setEnabled(false);
		btn_use_current_from_->setEnabled(false);
		btn_use_current_to_->setEnabled(false);
		chk_filter_bypass_->setEnabled(false);
		lbl_filter_limits_note_->clear();
		step2_empty_filter_note_->setText(
			QStringLiteral("Filter '%1' has no numeric settings that Midi Fadester can sweep.")
				.arg(chosen_filter_name_));
		step2_empty_filter_note_->show();
		updating_widgets_ = prev_upd;
		return;
	}

	int selected_idx = -1;
	for (size_t i = 0U; i < fe->prop_count; ++i) {
		const struct mfd_filter_prop_info &pi = fe->props[i];
		const QString pnm = QString::fromUtf8(pi.name);
		const QString plbl = (pi.label[0] != '\0') ? QString::fromUtf8(pi.label) : pnm;
		const QString disp = (plbl != pnm) ? QStringLiteral("%1  (%2)").arg(plbl, pnm) : pnm;
		combo_filter_prop_->addItem(disp, pnm);
		if (pnm == chosen_filter_prop_) {
			selected_idx = static_cast<int>(i);
		}
	}

	const bool prop_preserved = (selected_idx >= 0);
	if (selected_idx < 0) {
		selected_idx = 0;
	}
	combo_filter_prop_->setCurrentIndex(selected_idx);
	chosen_filter_prop_ = combo_filter_prop_->currentData().toString();
	combo_filter_prop_->setEnabled(true);
	step2_empty_filter_note_->hide();

	configure_spinboxes_for_prop(preserve_range && prop_preserved);
	updating_widgets_ = prev_upd;
}

void MfdSetupSheetDialog::configure_spinboxes_for_prop(bool preserve_range)
{
	const bool prev_upd = updating_widgets_;
	updating_widgets_ = true;

	const struct mfd_filter_prop_info *pi = find_selected_filter_prop();
	if (!pi) {
		spin_filter_from_->setEnabled(false);
		spin_filter_to_->setEnabled(false);
		btn_use_current_from_->setEnabled(false);
		btn_use_current_to_->setEnabled(false);
		chk_filter_bypass_->setEnabled(false);
		lbl_filter_limits_note_->clear();
		updating_widgets_ = prev_upd;
		return;
	}

	double pmin = 0.0;
	double pmax = 1.0;
	mfd_filter_normalize_bounds(pi->min_val, pi->max_val, &pmin, &pmax);
	const int dec = mfd_filter_decimals_for_step(pi->is_int, pi->step);
	const double step = pi->is_int ? 1.0 : ((std::isfinite(pi->step) && pi->step > 0.0) ? pi->step : 0.01);
	const QString sfx = QString::fromUtf8(pi->suffix);

	spin_filter_from_->setDecimals(dec);
	spin_filter_from_->setRange(pmin, pmax);
	spin_filter_from_->setSingleStep(step);
	spin_filter_from_->setSuffix(sfx);
	spin_filter_from_->setEnabled(true);

	spin_filter_to_->setDecimals(dec);
	spin_filter_to_->setRange(pmin, pmax);
	spin_filter_to_->setSingleStep(step);
	spin_filter_to_->setSuffix(sfx);
	spin_filter_to_->setEnabled(true);

	btn_use_current_from_->setEnabled(true);
	btn_use_current_to_->setEnabled(true);
	chk_filter_bypass_->setEnabled(true);

	if (!preserve_range || !chosen_filter_has_custom_range_) {
		chosen_filter_from_ = pmin;
		chosen_filter_to_ = pmax;
		chosen_filter_has_custom_range_ = true;
	}
	spin_filter_from_->setValue(chosen_filter_from_);
	spin_filter_to_->setValue(chosen_filter_to_);
	chosen_filter_from_ = spin_filter_from_->value();
	chosen_filter_to_ = spin_filter_to_->value();

	char cur_buf[64] = {0};
	char min_buf[64] = {0};
	char max_buf[64] = {0};
	mfd_filter_format_display_value(0.0f, pmin, pmax, pmin, pmax, pi->is_int, pi->step, pi->suffix, false, min_buf,
					sizeof(min_buf));
	mfd_filter_format_display_value(1.0f, pmin, pmax, pmin, pmax, pi->is_int, pi->step, pi->suffix, false, max_buf,
					sizeof(max_buf));
	mfd_filter_format_display_value(0.0f, pi->current_val, pi->current_val, pmin, pmax, pi->is_int, pi->step,
					pi->suffix, false, cur_buf, sizeof(cur_buf));
	lbl_filter_limits_note_->setText(
		QStringLiteral("Allowed range: %1 to %2  \xC2\xB7  Current OBS value: %3 (inverted ranges supported)")
			.arg(QString::fromUtf8(min_buf), QString::fromUtf8(max_buf), QString::fromUtf8(cur_buf)));

	updating_widgets_ = prev_upd;
}

void MfdSetupSheetDialog::repopulate_layer_combos_for_scene()
{
	const bool prev_upd = updating_widgets_;
	updating_widgets_ = true;

	combo_left_->clear();
	combo_right_->clear();

	const struct mfd_ui_scene_entry *se = find_catalog_scene(chosen_scene_);
	if (!se || se->layer_count == 0U) {
		chosen_left_.clear();
		chosen_right_.clear();
		combo_left_->setEnabled(false);
		combo_right_->setEnabled(false);
		updating_widgets_ = prev_upd;
		return;
	}

	int left_idx = -1;
	int right_idx = -1;
	for (size_t i = 0U; i < se->layer_count; ++i) {
		const QString lnm = QString::fromUtf8(se->layers[i]);
		combo_left_->addItem(lnm, lnm);
		combo_right_->addItem(lnm, lnm);
		if (lnm == chosen_left_) {
			left_idx = static_cast<int>(i);
		}
		if (lnm == chosen_right_) {
			right_idx = static_cast<int>(i);
		}
	}

	if (left_idx < 0) {
		left_idx = 0;
	}
	combo_left_->setCurrentIndex(left_idx);
	chosen_left_ = combo_left_->currentData().toString();
	combo_left_->setEnabled(true);

	if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX) {
		if (right_idx < 0 && se->layer_count >= 2U) {
			right_idx = (left_idx == 0) ? 1 : 0;
		} else if (right_idx < 0) {
			right_idx = 0;
		}
		combo_right_->setCurrentIndex(right_idx);
		chosen_right_ = combo_right_->currentData().toString();
		combo_right_->setEnabled(true);
	} else {
		chosen_right_.clear();
	}

	updating_widgets_ = prev_upd;
}

void MfdSetupSheetDialog::update_step2_notes_and_validation()
{
	if (chosen_mode_ == MFD_TARGET_TBAR) {
		const bool studio_on = snap_.studio_mode_active;
		if (studio_on) {
			step2_tbar_info_->setText(
				QStringLiteral("Studio Mode is on. Ready to fade Preview into Program."));
			step2_tbar_info_->setStyleSheet(
				QStringLiteral("color: #6ee7b7; font-size: 14px; font-weight: bold;"));
			step2_turn_on_studio_btn_->hide();
		} else {
			step2_tbar_info_->setText(QStringLiteral("Studio Mode is off."));
			step2_tbar_info_->setStyleSheet(
				QStringLiteral("color: #fde68a; font-size: 14px; font-weight: bold;"));
			step2_turn_on_studio_btn_->show();
		}
	}

	if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX || chosen_mode_ == MFD_TARGET_SINGLE_LAYER) {
		const bool dup = (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX && !chosen_left_.isEmpty() &&
				  chosen_left_ == chosen_right_);
		same_layer_refusal_->setVisible(dup);

		char live_buf[224] = {0};
		const QByteArray sc_utf8 = chosen_scene_.toUtf8();
		mfd_ui_format_live_scene_note(sc_utf8.constData(), live_buf, sizeof(live_buf));
		live_scene_note_->setText(QString::fromUtf8(live_buf));
		live_scene_note_->setVisible(live_buf[0] != '\0');

		/* Shared-layer notes across all other scenes in catalog_ */
		QStringList shared_notes;
		auto append_shared_for = [&](const QString &layer_q) {
			if (!catalog_ || layer_q.isEmpty() || chosen_scene_.isEmpty()) {
				return;
			}
			const QByteArray l_utf8 = layer_q.toUtf8();
			const char *other_ptrs[MFD_CATALOG_MAX_SCENES];
			size_t other_cnt = 0U;
			for (size_t si = 0U; si < catalog_->scene_count; ++si) {
				const struct mfd_ui_scene_entry &se = catalog_->scenes[si];
				if (std::strcmp(se.name, sc_utf8.constData()) == 0) {
					continue;
				}
				for (size_t li = 0U; li < se.layer_count; ++li) {
					if (std::strcmp(se.layers[li], l_utf8.constData()) == 0) {
						other_ptrs[other_cnt++] = se.name;
						break;
					}
				}
			}
			if (other_cnt > 0U) {
				char note_buf[256] = {0};
				mfd_ui_format_shared_layer_note(l_utf8.constData(), other_ptrs, other_cnt, note_buf,
								sizeof(note_buf));
				if (note_buf[0] != '\0') {
					shared_notes.append(QString::fromUtf8(note_buf));
				}
			}
		};

		append_shared_for(chosen_left_);
		if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX && chosen_right_ != chosen_left_) {
			append_shared_for(chosen_right_);
		}

		if (shared_notes.isEmpty()) {
			shared_layer_note_->clear();
			shared_layer_note_->hide();
		} else {
			shared_layer_note_->setText(shared_notes.join(QStringLiteral("\n")));
			shared_layer_note_->show();
		}
	}

	if (chosen_mode_ == MFD_TARGET_FILTER_PARAM) {
		if (!chosen_filter_host_.isEmpty() && !chosen_filter_name_.isEmpty() &&
		    !chosen_filter_prop_.isEmpty()) {
			struct mfd_setup_candidate cand = build_candidate();
			char rsn[192] = {0};
			if (!mfd_engine_validate_setup_candidate(eng_, &cand, rsn, sizeof(rsn)) && rsn[0] != '\0') {
				filter_validation_refusal_->setText(QString::fromUtf8(rsn));
				filter_validation_refusal_->show();
			} else {
				filter_validation_refusal_->clear();
				filter_validation_refusal_->hide();
			}
		} else {
			filter_validation_refusal_->clear();
			filter_validation_refusal_->hide();
		}
	}

	bool truncated = false;
	if (catalog_) {
		if (chosen_mode_ == MFD_TARGET_AUDIO_VOLUME) {
			truncated = catalog_->audio_truncated;
			catalog_truncated_note_->setText(QStringLiteral(
				"Showing the first 48 scenes/layers or 64 audio sources in this collection."));
		} else if (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX || chosen_mode_ == MFD_TARGET_SINGLE_LAYER) {
			const struct mfd_ui_scene_entry *se = find_catalog_scene(chosen_scene_);
			truncated = catalog_->scenes_truncated || (se && se->layers_truncated);
			catalog_truncated_note_->setText(QStringLiteral(
				"Showing the first 48 scenes/layers or 64 audio sources in this collection."));
		} else if (chosen_mode_ == MFD_TARGET_FILTER_PARAM) {
			const struct mfd_filter_entry_info *fe = find_selected_filter_entry();
			truncated = catalog_->filter_hosts_truncated ||
				    (filter_cat_ && filter_cat_->filters_truncated) || (fe && fe->props_truncated);
			catalog_truncated_note_->setText(QStringLiteral(
				"Showing the first 128 filter hosts, 32 filters, or 64 numeric settings."));
		}
	}
	catalog_truncated_note_->setVisible(truncated);
	update_footer_buttons();
}

void MfdSetupSheetDialog::update_step3_controls()
{
	updating_widgets_ = true;

	const struct mfd_fader_slot &live_slot = snap_.slots[slot_index_];
	char fin_buf[224] = {0};
	const QByteArray sc_utf8 = chosen_scene_.toUtf8();
	QByteArray sa_utf8;
	if (chosen_mode_ == MFD_TARGET_AUDIO_VOLUME) {
		sa_utf8 = chosen_audio_.toUtf8();
	} else if (chosen_mode_ == MFD_TARGET_FILTER_PARAM) {
		sa_utf8 = chosen_filter_host_.toUtf8();
	} else {
		sa_utf8 = chosen_left_.toUtf8();
	}
	const QByteArray sb_utf8 = chosen_right_.toUtf8();
	const QByteArray fn_utf8 = chosen_filter_name_.toUtf8();
	const struct mfd_filter_prop_info *sel_pi = find_selected_filter_prop();
	const QByteArray fp_lbl_utf8 = (sel_pi && sel_pi->label[0] != '\0') ? QByteArray(sel_pi->label)
									    : chosen_filter_prop_.toUtf8();
	mfd_ui_format_finished_sentence_ex(slot_index_, live_slot.label, chosen_mode_, sc_utf8.constData(),
					   sa_utf8.constData(), sb_utf8.constData(), fn_utf8.constData(),
					   fp_lbl_utf8.constData(), fin_buf, sizeof(fin_buf));
	finished_sentence_->setText(QString::fromUtf8(fin_buf));

	const int meter_pct = mfd_calibrated_percent(snap_.setup.candidate_raw_norm);
	candidate_meter_->setValue(meter_pct);
	if (chosen_mode_ == MFD_TARGET_FILTER_PARAM) {
		double pmin = -MFD_FILTER_BOUND_LIMIT;
		double pmax = MFD_FILTER_BOUND_LIMIT;
		bool is_int = false;
		double pstep = 0.01;
		const char *psfx = "";
		if (sel_pi) {
			mfd_filter_normalize_bounds(sel_pi->min_val, sel_pi->max_val, &pmin, &pmax);
			is_int = sel_pi->is_int;
			pstep = sel_pi->step;
			psfx = sel_pi->suffix;
		}
		char sim_disp[64] = {0};
		mfd_filter_format_display_value(snap_.setup.candidate_raw_norm, chosen_filter_from_, chosen_filter_to_,
						pmin, pmax, is_int, pstep, psfx, chosen_filter_bypass_, sim_disp,
						sizeof(sim_disp));
		if (snap_.setup.has_candidate_input) {
			candidate_meter_->setFormat(
				QString::asprintf("Hardware test: %d%% \xC2\xB7 %s", meter_pct, sim_disp));
		} else {
			candidate_meter_->setFormat(QString::asprintf(
				"Move control on your controller to test (%d%% \xC2\xB7 %s)", meter_pct, sim_disp));
		}
	} else if (snap_.setup.has_candidate_input) {
		candidate_meter_->setFormat(QString::asprintf("Hardware test: %d%%", meter_pct));
	} else {
		candidate_meter_->setFormat(
			QString::asprintf("Move the fader on your controller to test (%d%%)", meter_pct));
	}

	feel_box_->setVisible(chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX);
	const int feel_idx = combo_feel_->findData(static_cast<int>(chosen_curve_));
	if (feel_idx >= 0) {
		combo_feel_->setCurrentIndex(feel_idx);
	}
	feel_desc_label_->setText(QString::fromUtf8(mfd_ui_curve_plain_description(chosen_curve_)));

	const bool show_afv = (chosen_mode_ == MFD_TARGET_TWO_SOURCE_MIX || chosen_mode_ == MFD_TARGET_SINGLE_LAYER) &&
			      chosen_layers_have_audio();
	chk_also_fade_audio_->setVisible(show_afv);
	chk_also_fade_audio_->setChecked(chosen_afv_);

	volume_law_label_->setVisible(chosen_mode_ == MFD_TARGET_AUDIO_VOLUME);

	chk_reverse_dir_->setChecked(snap_.setup.candidate_invert);

	tbar_step3_box_->setVisible(chosen_mode_ == MFD_TARGET_TBAR);
	chk_tbar_bidir_->setChecked(chosen_tbar_bidir_);
	const bool can_finish_now = (live_slot.enabled && live_slot.target_type == MFD_TARGET_TBAR &&
				     live_slot.tbar_state == MFD_TBAR_THROWING && snap_.studio_mode_active);
	btn_step3_finish_now_->setEnabled(can_finish_now);

	candidate_addr_label_->setText(format_plain_midi_addr(snap_.setup.candidate_addr));
	if (snap_.setup.learning) {
		btn_setup_learn_->setText(QStringLiteral("Move control... (Cancel)"));
		btn_setup_learn_->setStyleSheet(
			QStringLiteral("QPushButton { background-color: #b45309; color: #ffffff; font-weight: bold; "
				       "border-radius: 5px; padding: 6px 12px; }"));
		hw_panel_->show();
	} else {
		btn_setup_learn_->setText(QStringLiteral("Learn"));
		btn_setup_learn_->setStyleSheet(QStringLiteral(
			"QPushButton { background-color: #1e293b; color: #f8fafc; border: 1px solid #475569; "
			"border-radius: 5px; padding: 6px 12px; font-weight: bold; }"));
	}

	int owners[MFD_MAX_FADER_SLOTS] = {0};
	const size_t owner_cnt = mfd_engine_find_addr_owners(eng_, &snap_.setup.candidate_addr, slot_index_, owners,
							     MFD_MAX_FADER_SLOTS);
	if (owner_cnt > 0U) {
		QStringList owner_titles;
		for (size_t oi = 0U; oi < owner_cnt; ++oi) {
			char ot[64] = {0};
			mfd_ui_strip_title(owners[oi], snap_.slots[owners[oi]].label, ot, sizeof(ot));
			owner_titles.append(QString::fromUtf8(ot));
		}
		collision_notice_->setText(
			QStringLiteral("Currently assigned to strip %1. Pressing Done will move it to this strip.")
				.arg(owner_titles.join(QStringLiteral(", "))));
		collision_notice_->show();
		hw_panel_->show();
	} else {
		collision_notice_->clear();
		collision_notice_->hide();
	}

	updating_widgets_ = false;
}

void MfdSetupSheetDialog::update_footer_buttons()
{
	char strip_title[64] = {0};
	mfd_ui_strip_title(slot_index_, snap_.slots[slot_index_].label, strip_title, sizeof(strip_title));
	const int page = stack_->currentIndex();
	title_label_->setText(
		QStringLiteral("Set up %1 \xE2\x80\x94 Step %2 of 3").arg(QString::fromUtf8(strip_title)).arg(page + 1));

	btn_back_->setVisible(page > 0);
	btn_next_->setVisible(page < 2);
	btn_done_->setVisible(page == 2);

	if (page == 0) {
		btn_next_->setEnabled(true);
	} else if (page == 1) {
		struct mfd_setup_candidate cand = build_candidate();
		char rsn[160] = {0};
		const bool valid = mfd_engine_validate_setup_candidate(eng_, &cand, rsn, sizeof(rsn));
		btn_next_->setEnabled(valid);
	} else {
		btn_done_->setEnabled(true);
	}
}

void MfdSetupSheetDialog::on_done_clicked()
{
	struct mfd_setup_candidate cand = build_candidate();
	char err_buf[192] = {0};
	if (mfd_engine_commit_setup_session(eng_, &cand, err_buf, sizeof(err_buf))) {
		committed_or_unassigned_ = true;
		accept();
		return;
	}

	mfd_engine_get_ui_snapshot(eng_, &snap_);
	if (catalog_) {
		mfd_engine_get_ui_catalog(eng_, catalog_);
		last_topology_rev_ = catalog_->config_topology_rev;
	}

	error_banner_->setText(QString::fromUtf8(err_buf));
	error_banner_->show();

	char val_rsn[160] = {0};
	if (!mfd_engine_validate_setup_candidate(eng_, &cand, val_rsn, sizeof(val_rsn))) {
		repopulate_step2_pickers();
		stack_->setCurrentIndex(1);
	} else {
		update_step3_controls();
	}
	update_footer_buttons();
}
