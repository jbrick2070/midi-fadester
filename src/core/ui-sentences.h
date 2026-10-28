/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/midi-types.h"
#include "core/takeover.h"
#include "core/tbar-logic.h"
#include "midi/winmm-device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mfd_ui_severity {
	MFD_UI_SEVERITY_NONE = 0,
	MFD_UI_SEVERITY_AMBER,
	MFD_UI_SEVERITY_RED,
};

enum mfd_ui_strip_action {
	MFD_UI_ACTION_SETUP = 0,
	MFD_UI_ACTION_TURN_ON_STUDIO_MODE,
};

enum mfd_ui_pickup_arrow {
	MFD_UI_ARROW_NONE = 0,
	MFD_UI_ARROW_UP,    /* ↑ (vertical fader) */
	MFD_UI_ARROW_DOWN,  /* ↓ (vertical fader) */
	MFD_UI_ARROW_RIGHT, /* → (horizontal crossfader) */
	MFD_UI_ARROW_LEFT,  /* ← (horizontal crossfader) */
};

struct mfd_ui_controller_presentation {
	char display_name[64];
	bool dot_green;
	char status_sentence[192];
	bool show_reconnect;
};

struct mfd_ui_strip_input {
	int slot_index;
	const char *slot_label;
	bool is_horizontal;
	bool enabled;
	enum mfd_midi_msg_type addr_type;
	enum mfd_target_type target_type;
	const char *scene_name;
	const char *source_a_name;
	const char *source_b_name;
	bool available;
	const char *reason;
	float raw_norm;
	float applied_val;
	float alpha_a;
	float alpha_b;
	float audio_db;
	bool auto_fade_active;
	enum mfd_tbar_state tbar_state;
	enum mfd_takeover_state takeover_state;
	enum mfd_takeover_hint takeover_hint;
	bool has_last_fader;
	bool polarity_reversed;
	float target_obs;
};

struct mfd_ui_strip_presentation {
	char title[64];
	bool dimmed;
	int bar_percent;
	bool show_pickup_marker;
	int pickup_target_percent;
	enum mfd_ui_pickup_arrow arrow;
	char arrow_glyph[8];
	char state_readout[128];
	char assignment_sentence[192];
	char svg_short_label[96];
	char status_sentence[192];
	enum mfd_ui_severity severity;
	bool show_finish_now;
	enum mfd_ui_strip_action bottom_action;
	char bottom_button_text[48];
};

void mfd_ui_strip_title(int slot_index, const char *slot_label, char *out, size_t out_size);

void mfd_ui_format_controller_status(bool connected, const char *configured_name, const char *opened_name,
				     enum mfd_winmm_error_kind error_kind, struct mfd_ui_controller_presentation *out);

void mfd_ui_format_strip_presentation(const struct mfd_ui_strip_input *in, struct mfd_ui_strip_presentation *out);

void mfd_ui_format_finished_sentence(int slot_index, const char *slot_label, enum mfd_target_type target_type,
				     const char *scene_name, const char *source_a_name, const char *source_b_name,
				     char *out, size_t out_size);

void mfd_ui_format_live_scene_note(const char *scene_name, char *out, size_t out_size);

void mfd_ui_format_shared_layer_note(const char *layer_name, const char *const *other_scenes, size_t other_scene_count,
				     char *out, size_t out_size);

void mfd_ui_format_last_move(int last_moved_slot_index, const char *slot_label, bool has_last_midi,
			     const struct mfd_midi_decoded *last_midi, char *out, size_t out_size);

void mfd_ui_format_zone3_status(bool studio_mode_active, const char *preview_scene, const char *program_scene,
				const char *last_move_text, char *out, size_t out_size);

const char *mfd_ui_target_plain_name(enum mfd_target_type target_type);
const char *mfd_ui_curve_plain_name(enum mfd_crossfade_curve curve);
const char *mfd_ui_curve_plain_description(enum mfd_crossfade_curve curve);

#ifdef __cplusplus
}
#endif
