/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/ui-sentences.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void mfd_ui_strip_title(int slot_index, const char *slot_label, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';

	if (slot_index == 0) {
		if (!slot_label || slot_label[0] == '\0' || strcmp(slot_label, "Crossfader (A/B)") == 0 ||
		    strcmp(slot_label, "Fader 1") == 0) {
			snprintf(out, out_size, "Crossfader");
		} else {
			snprintf(out, out_size, "%s", slot_label);
		}
		return;
	}

	if (slot_index >= 1 && slot_index <= 8) {
		char def_track[32];
		char def_fader[32];
		snprintf(def_track, sizeof(def_track), "Track Fader %d", slot_index);
		snprintf(def_fader, sizeof(def_fader), "Fader %d", slot_index + 1);
		if (!slot_label || slot_label[0] == '\0' || strcmp(slot_label, def_track) == 0 ||
		    strcmp(slot_label, def_fader) == 0) {
			snprintf(out, out_size, "%d", slot_index);
		} else {
			snprintf(out, out_size, "%s", slot_label);
		}
		return;
	}

	if (slot_index == 9) {
		if (!slot_label || slot_label[0] == '\0' || strcmp(slot_label, "Master Fader") == 0 ||
		    strcmp(slot_label, "Fader 10") == 0) {
			snprintf(out, out_size, "Master");
		} else {
			snprintf(out, out_size, "%s", slot_label);
		}
		return;
	}

	if (slot_label && slot_label[0] != '\0') {
		snprintf(out, out_size, "%s", slot_label);
	} else {
		snprintf(out, out_size, "%d", slot_index + 1);
	}
}

void mfd_ui_format_controller_status(bool connected, const char *configured_name, const char *opened_name,
				     enum mfd_winmm_error_kind error_kind, struct mfd_ui_controller_presentation *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));

	const bool has_cfg = (configured_name && configured_name[0] != '\0');
	const bool has_open = (opened_name && opened_name[0] != '\0');
	const char *dev = "MIDI controller";
	if (connected && has_open) {
		dev = opened_name;
	} else if (has_cfg) {
		dev = configured_name;
	} else if (has_open) {
		dev = opened_name;
	}

	if (connected) {
		snprintf(out->display_name, sizeof(out->display_name), "%s", dev);
		out->dot_green = true;
		snprintf(out->status_sentence, sizeof(out->status_sentence), "Connected");
		out->show_reconnect = false;
		return;
	}

	snprintf(out->display_name, sizeof(out->display_name), "%s", (has_cfg || has_open) ? dev : "None");
	out->dot_green = false;
	out->show_reconnect = true;

	if (error_kind == MFD_WINMM_ERR_BUSY_ALLOCATED) {
		snprintf(out->status_sentence, sizeof(out->status_sentence),
			 "Another program has the %s open. Close it, then press Reconnect.", dev);
	} else if (error_kind == MFD_WINMM_ERR_CONFIGURED_NOT_FOUND || error_kind == MFD_WINMM_ERR_DISCONNECTED ||
		   (error_kind == MFD_WINMM_ERR_NO_DEVICES && has_cfg)) {
		snprintf(out->status_sentence, sizeof(out->status_sentence),
			 "%s not found. Plug it in, then Reconnect.", dev);
	} else if (error_kind == MFD_WINMM_ERR_NO_DEVICES) {
		snprintf(out->status_sentence, sizeof(out->status_sentence),
			 "No MIDI controller found. Plug one in, then press Reconnect.");
	} else {
		snprintf(out->status_sentence, sizeof(out->status_sentence),
			 "Could not open %s. Unplug and replug it, then press Reconnect.", dev);
	}
}

static void format_mode_active_readout(const struct mfd_ui_strip_input *in, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!in) {
		return;
	}

	const char *sa = (in->source_a_name && in->source_a_name[0] != '\0') ? in->source_a_name : "Left";
	const char *sb = (in->source_b_name && in->source_b_name[0] != '\0') ? in->source_b_name : "Right";

	switch (in->target_type) {
	case MFD_TARGET_TBAR:
		if (in->tbar_state == MFD_TBAR_THROWING) {
			if (in->auto_fade_active) {
				snprintf(out, out_size, "Fading %d%%", mfd_calibrated_percent(in->applied_val));
			} else {
				snprintf(out, out_size, "Throwing %d%%", mfd_calibrated_percent(in->applied_val));
			}
		} else {
			snprintf(out, out_size, "Ready");
		}
		break;

	case MFD_TARGET_TWO_SOURCE_MIX:
		if (in->auto_fade_active) {
			snprintf(out, out_size, "Fading %d%%", mfd_calibrated_percent(in->applied_val));
		} else {
			snprintf(out, out_size, "%s %d%%, %s %d%%", sa, mfd_calibrated_percent(in->alpha_a), sb,
				 mfd_calibrated_percent(in->alpha_b));
		}
		break;

	case MFD_TARGET_SINGLE_LAYER:
		if (in->auto_fade_active) {
			snprintf(out, out_size, "Fading %d%%", mfd_calibrated_percent(in->applied_val));
		} else {
			snprintf(out, out_size, "%s %d%% visible", sa, mfd_calibrated_percent(in->alpha_a));
		}
		break;

	case MFD_TARGET_AUDIO_VOLUME:
		if (in->auto_fade_active) {
			snprintf(out, out_size, "Fading %d%%", mfd_calibrated_percent(in->applied_val));
		} else if (!isfinite(in->audio_db)) {
			snprintf(out, out_size, "-inf dB");
		} else if (fabsf(in->audio_db) < 0.05f) {
			snprintf(out, out_size, "0.0 dB");
		} else {
			snprintf(out, out_size, "%.1f dB", (double)in->audio_db);
		}
		break;

	case MFD_TARGET_UNASSIGNED:
	default:
		break;
	}
}

void mfd_ui_format_strip_presentation(const struct mfd_ui_strip_input *in, struct mfd_ui_strip_presentation *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!in) {
		return;
	}

	mfd_ui_strip_title(in->slot_index, in->slot_label, out->title, sizeof(out->title));
	out->bar_percent = mfd_calibrated_percent(in->raw_norm);
	out->bottom_action = MFD_UI_ACTION_SETUP;
	snprintf(out->bottom_button_text, sizeof(out->bottom_button_text), "Set up");

	const char *sc = (in->scene_name && in->scene_name[0] != '\0') ? in->scene_name : "";
	const char *sa = (in->source_a_name && in->source_a_name[0] != '\0') ? in->source_a_name : "";
	const char *sb = (in->source_b_name && in->source_b_name[0] != '\0') ? in->source_b_name : "";

	/* Assignment sentence + SVG short label */
	if (!in->enabled || in->target_type == MFD_TARGET_UNASSIGNED) {
		snprintf(out->assignment_sentence, sizeof(out->assignment_sentence), "Not used. Click to set up.");
		snprintf(out->svg_short_label, sizeof(out->svg_short_label), "Not used");
	} else {
		switch (in->target_type) {
		case MFD_TARGET_TBAR:
			snprintf(out->assignment_sentence, sizeof(out->assignment_sentence), "Switches scenes (T-bar)");
			snprintf(out->svg_short_label, sizeof(out->svg_short_label), "Switches scenes");
			break;
		case MFD_TARGET_TWO_SOURCE_MIX:
			if (sc[0] != '\0') {
				snprintf(out->assignment_sentence, sizeof(out->assignment_sentence),
					 "Mixes %s and %s in %s", sa[0] ? sa : "Left", sb[0] ? sb : "Right", sc);
			} else {
				snprintf(out->assignment_sentence, sizeof(out->assignment_sentence), "Mixes %s and %s",
					 sa[0] ? sa : "Left", sb[0] ? sb : "Right");
			}
			snprintf(out->svg_short_label, sizeof(out->svg_short_label), "%s / %s", sa[0] ? sa : "Left",
				 sb[0] ? sb : "Right");
			break;
		case MFD_TARGET_SINGLE_LAYER:
			if (sc[0] != '\0') {
				snprintf(out->assignment_sentence, sizeof(out->assignment_sentence),
					 "Shows or hides %s in %s", sa[0] ? sa : "layer", sc);
			} else {
				snprintf(out->assignment_sentence, sizeof(out->assignment_sentence),
					 "Shows or hides %s", sa[0] ? sa : "layer");
			}
			snprintf(out->svg_short_label, sizeof(out->svg_short_label), "Layer: %s", sa[0] ? sa : "layer");
			break;
		case MFD_TARGET_AUDIO_VOLUME:
			snprintf(out->assignment_sentence, sizeof(out->assignment_sentence), "Volume: %s",
				 sa[0] ? sa : "Audio");
			snprintf(out->svg_short_label, sizeof(out->svg_short_label), "Vol: %s", sa[0] ? sa : "Audio");
			break;
		default:
			snprintf(out->assignment_sentence, sizeof(out->assignment_sentence),
				 "Not used. Click to set up.");
			snprintf(out->svg_short_label, sizeof(out->svg_short_label), "Not used");
			break;
		}
	}

	/* Priority 1: Unassigned / disabled */
	if (!in->enabled || in->target_type == MFD_TARGET_UNASSIGNED) {
		out->dimmed = true;
		return;
	}

	/* Priority 2: Switch scenes with Studio Mode off */
	if (!in->available && in->target_type == MFD_TARGET_TBAR && in->reason &&
	    strcmp(in->reason, "Studio Mode is off.") == 0) {
		snprintf(out->status_sentence, sizeof(out->status_sentence), "Studio Mode is off.");
		out->severity = MFD_UI_SEVERITY_AMBER;
		out->bottom_action = MFD_UI_ACTION_TURN_ON_STUDIO_MODE;
		snprintf(out->bottom_button_text, sizeof(out->bottom_button_text), "Turn on Studio Mode");
		return;
	}

	/* Priority 3: Any other unmet precondition or unbound MIDI hardware control */
	if (!in->available) {
		snprintf(out->status_sentence, sizeof(out->status_sentence), "%s",
			 (in->reason && in->reason[0] != '\0') ? in->reason : "Not used. Click to set up.");
		out->severity = MFD_UI_SEVERITY_RED;
		return;
	}
	if (in->addr_type == MFD_MIDI_NONE) {
		snprintf(out->status_sentence, sizeof(out->status_sentence), "No MIDI control bound.");
		out->severity = MFD_UI_SEVERITY_RED;
		return;
	}

	/* Priority 4: At a rail, parked */
	if (in->takeover_state == MFD_TAKEOVER_PARKED ||
	    (in->target_type == MFD_TARGET_TBAR && in->tbar_state == MFD_TBAR_PARKED)) {
		snprintf(out->state_readout, sizeof(out->state_readout), "At end.");
		return;
	}

	/* Priority 5: Pickup pending (only after physical fader position has been observed) */
	if (in->has_last_fader && in->takeover_state == MFD_TAKEOVER_WAITING && in->takeover_hint != MFD_HINT_NONE) {
		float phys_target = in->polarity_reversed ? (1.0f - in->target_obs) : in->target_obs;
		int tgt_pct = mfd_calibrated_percent(phys_target);
		bool phys_move_up = in->polarity_reversed ? (in->takeover_hint == MFD_HINT_MOVE_DOWN)
							  : (in->takeover_hint == MFD_HINT_MOVE_UP);
		out->show_pickup_marker = true;
		out->pickup_target_percent = tgt_pct;
		if (in->is_horizontal) {
			out->arrow = phys_move_up ? MFD_UI_ARROW_RIGHT : MFD_UI_ARROW_LEFT;
			snprintf(out->arrow_glyph, sizeof(out->arrow_glyph), "%s",
				 phys_move_up ? "\xE2\x86\x92" : "\xE2\x86\x90");
		} else {
			out->arrow = phys_move_up ? MFD_UI_ARROW_UP : MFD_UI_ARROW_DOWN;
			snprintf(out->arrow_glyph, sizeof(out->arrow_glyph), "%s",
				 phys_move_up ? "\xE2\x86\x91" : "\xE2\x86\x93");
		}
		format_mode_active_readout(in, out->state_readout, sizeof(out->state_readout));
		snprintf(out->status_sentence, sizeof(out->status_sentence), "Move the fader %s to %d%% to pick up.",
			 phys_move_up ? "up" : "down", tgt_pct);
		out->severity = MFD_UI_SEVERITY_AMBER;
		return;
	}

	/* Priority 6: T-bar actively throwing */
	if (in->target_type == MFD_TARGET_TBAR && in->tbar_state == MFD_TBAR_THROWING) {
		format_mode_active_readout(in, out->state_readout, sizeof(out->state_readout));
		out->show_finish_now = true;
		return;
	}

	/* Priority 7: Engaged / ready */
	format_mode_active_readout(in, out->state_readout, sizeof(out->state_readout));
}

void mfd_ui_format_finished_sentence(int slot_index, const char *slot_label, enum mfd_target_type target_type,
				     const char *scene_name, const char *source_a_name, const char *source_b_name,
				     char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	char title[64] = {0};
	mfd_ui_strip_title(slot_index, slot_label, title, sizeof(title));
	const char *sc = (scene_name && scene_name[0] != '\0') ? scene_name : "";
	const char *sa = (source_a_name && source_a_name[0] != '\0') ? source_a_name : "";
	const char *sb = (source_b_name && source_b_name[0] != '\0') ? source_b_name : "";

	switch (target_type) {
	case MFD_TARGET_TBAR:
		snprintf(out, out_size, "%s switches scenes (T-bar)", title);
		break;
	case MFD_TARGET_TWO_SOURCE_MIX:
		if (sc[0] != '\0') {
			snprintf(out, out_size, "%s mixes %s and %s in scene %s", title, sa[0] ? sa : "Left",
				 sb[0] ? sb : "Right", sc);
		} else {
			snprintf(out, out_size, "%s mixes %s and %s", title, sa[0] ? sa : "Left", sb[0] ? sb : "Right");
		}
		break;
	case MFD_TARGET_SINGLE_LAYER:
		if (sc[0] != '\0') {
			snprintf(out, out_size, "%s shows or hides %s in scene %s", title, sa[0] ? sa : "layer", sc);
		} else {
			snprintf(out, out_size, "%s shows or hides %s", title, sa[0] ? sa : "layer");
		}
		break;
	case MFD_TARGET_AUDIO_VOLUME:
		snprintf(out, out_size, "%s controls %s volume", title, sa[0] ? sa : "audio");
		break;
	case MFD_TARGET_UNASSIGNED:
	default:
		snprintf(out, out_size, "%s is not used", title);
		break;
	}
}

void mfd_ui_format_live_scene_note(const char *scene_name, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	if (!scene_name || scene_name[0] == '\0') {
		out[0] = '\0';
		return;
	}
	snprintf(out, out_size, "You will see this when scene %s is live.", scene_name);
}

void mfd_ui_format_shared_layer_note(const char *layer_name, const char *const *other_scenes, size_t other_scene_count,
				     char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!layer_name || layer_name[0] == '\0' || !other_scenes || other_scene_count == 0U) {
		return;
	}
	if (other_scene_count == 1U) {
		snprintf(out, out_size, "%s is also in scene %s; it will fade there too.", layer_name, other_scenes[0]);
	} else if (other_scene_count == 2U) {
		snprintf(out, out_size, "%s is also in scenes %s and %s; it will fade there too.", layer_name,
			 other_scenes[0], other_scenes[1]);
	} else {
		snprintf(out, out_size, "%s is also in scene %s and %u other scenes; it will fade there too.",
			 layer_name, other_scenes[0], (unsigned)(other_scene_count - 1U));
	}
}

void mfd_ui_format_last_move(int last_moved_slot_index, const char *slot_label, bool has_last_midi,
			     const struct mfd_midi_decoded *last_midi, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	if (last_moved_slot_index >= 0 && last_moved_slot_index < 10) {
		char title[64] = {0};
		mfd_ui_strip_title(last_moved_slot_index, slot_label, title, sizeof(title));
		snprintf(out, out_size, "Last move: %s", title);
		return;
	}
	if (has_last_midi && last_midi) {
		unsigned ch = (unsigned)(last_midi->addr.channel + 1U);
		switch (last_midi->addr.type) {
		case MFD_MIDI_CC:
			snprintf(out, out_size, "Last move: Unmapped CC %u (Ch %u)", (unsigned)last_midi->addr.number,
				 ch);
			return;
		case MFD_MIDI_PITCH_BEND:
			snprintf(out, out_size, "Last move: Unmapped Pitch Bend (Ch %u)", ch);
			return;
		case MFD_MIDI_NOTE_ON:
		case MFD_MIDI_NOTE_OFF:
			snprintf(out, out_size, "Last move: Unmapped Note %u (Ch %u)", (unsigned)last_midi->addr.number,
				 ch);
			return;
		default:
			break;
		}
	}
	snprintf(out, out_size, "Last move: None yet");
}

void mfd_ui_format_zone3_status(bool studio_mode_active, const char *preview_scene, const char *program_scene,
				const char *last_move_text, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	const char *lm = (last_move_text && last_move_text[0] != '\0') ? last_move_text : "Last move: None yet";
	const char *prev = (preview_scene && preview_scene[0] != '\0') ? preview_scene : "(none)";
	const char *prog = (program_scene && program_scene[0] != '\0') ? program_scene : "(none)";

	if (studio_mode_active) {
		snprintf(out, out_size,
			 "Studio Mode on  \xE2\x80\xA2  Preview: %s \xE2\x86\x92 Program: %s  \xE2\x80\xA2  %s", prev,
			 prog, lm);
	} else if (program_scene && program_scene[0] != '\0') {
		snprintf(out, out_size, "Studio Mode off  \xE2\x80\xA2  Program: %s  \xE2\x80\xA2  %s", prog, lm);
	} else {
		snprintf(out, out_size, "Studio Mode off  \xE2\x80\xA2  %s", lm);
	}
}

const char *mfd_ui_target_plain_name(enum mfd_target_type target_type)
{
	switch (target_type) {
	case MFD_TARGET_TBAR:
		return "Switch scenes (T-bar)";
	case MFD_TARGET_TWO_SOURCE_MIX:
		return "Mix two layers";
	case MFD_TARGET_SINGLE_LAYER:
		return "Show or hide a layer";
	case MFD_TARGET_AUDIO_VOLUME:
		return "Volume";
	case MFD_TARGET_UNASSIGNED:
	default:
		return "Not used";
	}
}

const char *mfd_ui_curve_plain_name(enum mfd_crossfade_curve curve)
{
	switch (curve) {
	case MFD_CURVE_DIP_FREE:
		return "Dip-free";
	case MFD_CURVE_COSINE:
		return "Equal power";
	case MFD_CURVE_LINEAR:
		return "Linear";
	case MFD_CURVE_CUT:
		return "Cut";
	case MFD_CURVE_FULL_CENTER:
		return "Full at center";
	default:
		return "Dip-free";
	}
}

const char *mfd_ui_curve_plain_description(enum mfd_crossfade_curve curve)
{
	switch (curve) {
	case MFD_CURVE_DIP_FREE:
		return "Default; the picture never darkens in the middle.";
	case MFD_CURVE_COSINE:
		return "Smooth sine/cosine crossfade for matched-energy sources.";
	case MFD_CURVE_LINEAR:
		return "Straight 0-100% blend on both layers.";
	case MFD_CURVE_CUT:
		return "Instant switch at the halfway mark (50%).";
	case MFD_CURVE_FULL_CENTER:
		return "Both layers reach 100% at the center mark.";
	default:
		return "Default; the picture never darkens in the middle.";
	}
}
