/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/ui-sentences.h"
#include "core/apc40-profile.h"
#include "core/mfd-text.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Every sentence here is a whole locale text with %1..%4 filled in one pass (R-I18N-3); a missing name picks its own
 * sentence instead of a fallback word stitched into another one. */
static void say(char *out, size_t out_size, const char *text, const char *a1, const char *a2, const char *a3,
		const char *a4)
{
	const char *const values[4] = {a1, a2, a3, a4};
	mfd_text_fill(out, out_size, text, values, 4U);
}

static void percent_text(char *buf, size_t size, float norm_01)
{
	snprintf(buf, size, "%d", mfd_calibrated_percent(norm_01));
}

static bool has_text(const char *s)
{
	return s && s[0] != '\0';
}

static bool label_is_default(int slot_index, const char *slot_label)
{
	/* A default label is stored empty; older configs may still hold a profile's English default. */
	return !has_text(slot_label) ||
	       (slot_index >= 0 && slot_index < 18 && mfd_profile_is_default_slot_label(slot_index, slot_label));
}

void mfd_ui_strip_title(int slot_index, const char *slot_label, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';

	/* A default label shows the desk-printed short title, a label the user typed shows as typed (R-I18N-2). */
	if (!label_is_default(slot_index, slot_label)) {
		snprintf(out, out_size, "%s", slot_label);
		return;
	}
	char number[16];
	if (slot_index == 0) {
		say(out, out_size, mfd_text("StripTitle.Crossfader", "Crossfader"), NULL, NULL, NULL, NULL);
	} else if (slot_index >= 1 && slot_index <= 8) {
		snprintf(out, out_size, "%d", slot_index);
	} else if (slot_index == 9) {
		say(out, out_size, mfd_text("StripTitle.Master", "Master"), NULL, NULL, NULL, NULL);
	} else if (slot_index >= 10 && slot_index <= 17) {
		snprintf(number, sizeof(number), "%d", slot_index - 9);
		say(out, out_size, mfd_text("StripTitle.Fx", "FX %1"), number, NULL, NULL, NULL);
	} else {
		snprintf(out, out_size, "%d", slot_index + 1);
	}
}

void mfd_ui_strip_name(int slot_index, const char *slot_label, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	if (slot_index >= 1 && slot_index <= 8 && label_is_default(slot_index, slot_label)) {
		char number[16];
		snprintf(number, sizeof(number), "%d", slot_index);
		say(out, out_size, mfd_text("StripName.Fader", "Fader %1"), number, NULL, NULL, NULL);
		return;
	}
	mfd_ui_strip_title(slot_index, slot_label, out, out_size);
}

void mfd_ui_format_controller_status(bool connected, const char *configured_name, const char *opened_name,
				     enum mfd_winmm_error_kind error_kind, struct mfd_ui_controller_presentation *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));

	const bool has_cfg = has_text(configured_name);
	const bool has_open = has_text(opened_name);
	const char *dev = NULL;
	if (connected && has_open) {
		dev = opened_name;
	} else if (has_cfg) {
		dev = configured_name;
	} else if (has_open) {
		dev = opened_name;
	}

	if (connected) {
		if (dev) {
			snprintf(out->display_name, sizeof(out->display_name), "%s", dev);
		} else {
			say(out->display_name, sizeof(out->display_name),
			    mfd_text("Controller.Unnamed", "MIDI controller"), NULL, NULL, NULL, NULL);
		}
		out->dot_green = true;
		say(out->status_sentence, sizeof(out->status_sentence), mfd_text("Controller.Connected", "Connected"),
		    NULL, NULL, NULL, NULL);
		out->show_reconnect = false;
		return;
	}

	if (dev) {
		snprintf(out->display_name, sizeof(out->display_name), "%s", dev);
	} else {
		say(out->display_name, sizeof(out->display_name), mfd_text("Controller.None", "None"), NULL, NULL, NULL,
		    NULL);
	}
	out->dot_green = false;
	out->show_reconnect = true;

	char *s = out->status_sentence;
	const size_t n = sizeof(out->status_sentence);
	if (error_kind == MFD_WINMM_ERR_BUSY_ALLOCATED) {
		if (dev) {
			say(s, n,
			    mfd_text("Controller.Busy",
				     "Another program has the %1 open. Close it, then press Reconnect."),
			    dev, NULL, NULL, NULL);
		} else {
			say(s, n,
			    mfd_text("Controller.BusyUnnamed",
				     "Another program has the MIDI controller open. Close it, then press Reconnect."),
			    NULL, NULL, NULL, NULL);
		}
	} else if (error_kind == MFD_WINMM_ERR_CONFIGURED_NOT_FOUND || error_kind == MFD_WINMM_ERR_DISCONNECTED ||
		   (error_kind == MFD_WINMM_ERR_NO_DEVICES && has_cfg)) {
		if (dev) {
			say(s, n, mfd_text("Controller.NotFound", "%1 not found. Plug it in, then Reconnect."), dev,
			    NULL, NULL, NULL);
		} else {
			say(s, n,
			    mfd_text("Controller.NotFoundUnnamed",
				     "MIDI controller not found. Plug it in, then Reconnect."),
			    NULL, NULL, NULL, NULL);
		}
	} else if (error_kind == MFD_WINMM_ERR_NO_DEVICES) {
		say(s, n,
		    mfd_text("Controller.NoneFound", "No MIDI controller found. Plug one in, then press Reconnect."),
		    NULL, NULL, NULL, NULL);
	} else if (dev) {
		say(s, n,
		    mfd_text("Controller.OpenFailed", "Could not open %1. Unplug and replug it, then press Reconnect."),
		    dev, NULL, NULL, NULL);
	} else {
		say(s, n,
		    mfd_text("Controller.OpenFailedUnnamed",
			     "Could not open MIDI controller. Unplug and replug it, then press Reconnect."),
		    NULL, NULL, NULL, NULL);
	}
}

void mfd_ui_format_db(float db, char *out, size_t out_size)
{
	if (!isfinite(db)) {
		say(out, out_size, mfd_text("Readout.DbSilent", "-inf dB"), NULL, NULL, NULL, NULL);
		return;
	}
	char number[32];
	/* the decimal point stays ".", as in OBS's own mixer */
	snprintf(number, sizeof(number), "%.1f", (fabsf(db) < 0.05f) ? 0.0 : (double)db);
	say(out, out_size, mfd_text("Readout.Db", "%1 dB"), number, NULL, NULL, NULL);
}

void mfd_ui_format_percent(int percent, char *out, size_t out_size)
{
	char number[16];
	snprintf(number, sizeof(number), "%d", percent);
	say(out, out_size, mfd_text("Readout.Percent", "%1%"), number, NULL, NULL, NULL);
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

	const bool has_a = has_text(in->source_a_name);
	const bool has_b = has_text(in->source_b_name);
	char applied[16];
	char pct_a[16];
	char pct_b[16];
	percent_text(applied, sizeof(applied), in->applied_val);
	percent_text(pct_a, sizeof(pct_a), in->alpha_a);
	percent_text(pct_b, sizeof(pct_b), in->alpha_b);
	const char *fading = mfd_text("Readout.Fading", "Fading %1%");

	switch (in->target_type) {
	case MFD_TARGET_TBAR:
		if (in->tbar_state == MFD_TBAR_THROWING) {
			say(out, out_size, in->auto_fade_active ? fading : mfd_text("Readout.Throwing", "Throwing %1%"),
			    applied, NULL, NULL, NULL);
		} else {
			say(out, out_size, mfd_text("Readout.Ready", "Ready"), NULL, NULL, NULL, NULL);
		}
		break;

	case MFD_TARGET_TWO_SOURCE_MIX:
		if (in->auto_fade_active) {
			say(out, out_size, fading, applied, NULL, NULL, NULL);
		} else if (has_a && has_b) {
			say(out, out_size, mfd_text("Readout.Mix", "%1 %2%, %3 %4%"), in->source_a_name, pct_a,
			    in->source_b_name, pct_b);
		} else if (has_b) {
			say(out, out_size, mfd_text("Readout.MixNoLeft", "Left %1%, %2 %3%"), pct_a, in->source_b_name,
			    pct_b, NULL);
		} else if (has_a) {
			say(out, out_size, mfd_text("Readout.MixNoRight", "%1 %2%, Right %3%"), in->source_a_name,
			    pct_a, pct_b, NULL);
		} else {
			say(out, out_size, mfd_text("Readout.MixNoLayers", "Left %1%, Right %2%"), pct_a, pct_b, NULL,
			    NULL);
		}
		break;

	case MFD_TARGET_SINGLE_LAYER:
		if (in->auto_fade_active) {
			say(out, out_size, fading, applied, NULL, NULL, NULL);
		} else if (has_a) {
			say(out, out_size, mfd_text("Readout.LayerVisible", "%1 %2% visible"), in->source_a_name, pct_a,
			    NULL, NULL);
		} else {
			say(out, out_size, mfd_text("Readout.LayerVisibleUnnamed", "Layer %1% visible"), pct_a, NULL,
			    NULL, NULL);
		}
		break;

	case MFD_TARGET_AUDIO_VOLUME:
		if (in->auto_fade_active) {
			say(out, out_size, fading, applied, NULL, NULL, NULL);
		} else {
			mfd_ui_format_db(in->audio_db, out, out_size);
		}
		break;

	case MFD_TARGET_MIX_MASTER:
		snprintf(out, out_size, "%s",
			 has_text(in->display_value)
				 ? in->display_value
				 : mfd_text("Readout.MixMasterIdle", "Fadester mix provider required"));
		break;
	case MFD_TARGET_FILTER_PARAM:
		if (in->auto_fade_active) {
			if (has_text(in->display_value)) {
				say(out, out_size, mfd_text("Readout.FadingValue", "Fading %1"), in->display_value,
				    NULL, NULL, NULL);
			} else {
				say(out, out_size, fading, applied, NULL, NULL, NULL);
			}
		} else if (has_text(in->display_value)) {
			snprintf(out, out_size, "%s", in->display_value);
		} else {
			mfd_ui_format_percent(mfd_calibrated_percent(in->applied_val), out, out_size);
		}
		break;

	case MFD_TARGET_UNASSIGNED:
	default:
		break;
	}
}

static void format_assignment(const struct mfd_ui_strip_input *in, struct mfd_ui_strip_presentation *out)
{
	char *as = out->assignment_sentence;
	const size_t as_size = sizeof(out->assignment_sentence);
	char *sl = out->svg_short_label;
	const size_t sl_size = sizeof(out->svg_short_label);
	const char *sc = in->scene_name;
	const char *sa = in->source_a_name;
	const char *sb = in->source_b_name;
	const char *fp = has_text(in->filter_prop_label) ? in->filter_prop_label : in->filter_prop;

	if (!in->enabled || in->target_type == MFD_TARGET_UNASSIGNED) {
		say(as, as_size, mfd_text("Assign.NotUsed", "Not used. Click to set up."), NULL, NULL, NULL, NULL);
		say(sl, sl_size, mfd_text("Svg.NotUsed", "Not used"), NULL, NULL, NULL, NULL);
		return;
	}
	switch (in->target_type) {
	case MFD_TARGET_TBAR:
		say(as, as_size, mfd_text("Assign.Tbar", "Switches scenes (T-bar)"), NULL, NULL, NULL, NULL);
		say(sl, sl_size, mfd_text("Svg.Tbar", "Switches scenes"), NULL, NULL, NULL, NULL);
		break;
	case MFD_TARGET_TWO_SOURCE_MIX:
		if (has_text(sa) && has_text(sb)) {
			if (has_text(sc)) {
				say(as, as_size, mfd_text("Assign.MixInScene", "Mixes %1 and %2 in %3"), sa, sb, sc,
				    NULL);
			} else {
				say(as, as_size, mfd_text("Assign.Mix", "Mixes %1 and %2"), sa, sb, NULL, NULL);
			}
			say(sl, sl_size, mfd_text("Svg.Mix", "%1 / %2"), sa, sb, NULL, NULL);
		} else if (has_text(sa) || has_text(sb)) {
			/* one layer chosen: name it, and "Left" or "Right" for the other */
			const char *named = has_text(sa) ? sa : sb;
			if (has_text(sc)) {
				say(as, as_size,
				    has_text(sa) ? mfd_text("Assign.MixNoRightInScene", "Mixes %1 and Right in %2")
						 : mfd_text("Assign.MixNoLeftInScene", "Mixes Left and %1 in %2"),
				    named, sc, NULL, NULL);
			} else {
				say(as, as_size,
				    has_text(sa) ? mfd_text("Assign.MixNoRight", "Mixes %1 and Right")
						 : mfd_text("Assign.MixNoLeft", "Mixes Left and %1"),
				    named, NULL, NULL, NULL);
			}
			say(sl, sl_size,
			    has_text(sa) ? mfd_text("Svg.MixNoRight", "%1 / Right")
					 : mfd_text("Svg.MixNoLeft", "Left / %1"),
			    named, NULL, NULL, NULL);
		} else {
			if (has_text(sc)) {
				say(as, as_size, mfd_text("Assign.MixUnnamedInScene", "Mixes Left and Right in %1"), sc,
				    NULL, NULL, NULL);
			} else {
				say(as, as_size, mfd_text("Assign.MixUnnamed", "Mixes Left and Right"), NULL, NULL,
				    NULL, NULL);
			}
			say(sl, sl_size, mfd_text("Svg.MixUnnamed", "Left / Right"), NULL, NULL, NULL, NULL);
		}
		break;
	case MFD_TARGET_SINGLE_LAYER:
		if (has_text(sa)) {
			if (has_text(sc)) {
				say(as, as_size, mfd_text("Assign.LayerInScene", "Shows or hides %1 in %2"), sa, sc,
				    NULL, NULL);
			} else {
				say(as, as_size, mfd_text("Assign.Layer", "Shows or hides %1"), sa, NULL, NULL, NULL);
			}
			say(sl, sl_size, mfd_text("Svg.Layer", "Layer: %1"), sa, NULL, NULL, NULL);
		} else {
			if (has_text(sc)) {
				say(as, as_size, mfd_text("Assign.LayerUnnamedInScene", "Shows or hides a layer in %1"),
				    sc, NULL, NULL, NULL);
			} else {
				say(as, as_size, mfd_text("Assign.LayerUnnamed", "Shows or hides a layer"), NULL, NULL,
				    NULL, NULL);
			}
			say(sl, sl_size, mfd_text("Svg.LayerUnnamed", "Layer"), NULL, NULL, NULL, NULL);
		}
		break;
	case MFD_TARGET_AUDIO_VOLUME:
		if (has_text(sa)) {
			say(as, as_size, mfd_text("Assign.Volume", "Volume: %1"), sa, NULL, NULL, NULL);
			say(sl, sl_size, mfd_text("Svg.Volume", "Vol: %1"), sa, NULL, NULL, NULL);
		} else {
			say(as, as_size, mfd_text("Assign.VolumeUnnamed", "Volume"), NULL, NULL, NULL, NULL);
			say(sl, sl_size, mfd_text("Svg.VolumeUnnamed", "Vol"), NULL, NULL, NULL, NULL);
		}
		break;
	case MFD_TARGET_MIX_MASTER:
		say(as, as_size, mfd_text("Target.MixMaster", "Fadester mix master"), NULL, NULL, NULL, NULL);
		say(sl, sl_size, mfd_text("Svg.MixMaster", "Mix master"), NULL, NULL, NULL, NULL);
		break;
	case MFD_TARGET_FILTER_PARAM:
		if (has_text(in->filter_name) && has_text(fp)) {
			if (has_text(sa)) {
				say(as, as_size, mfd_text("Assign.FilterOnSource", "%1: %2 (%3)"), sa, in->filter_name,
				    fp, NULL);
			} else {
				say(as, as_size, mfd_text("Assign.Filter", "%1 (%2)"), in->filter_name, fp, NULL, NULL);
			}
		} else {
			say(as, as_size, mfd_text("Assign.FilterUnnamed", "Filter setting"), NULL, NULL, NULL, NULL);
		}
		if (has_text(in->filter_name)) {
			say(sl, sl_size, mfd_text("Svg.Filter", "FX: %1"), in->filter_name, NULL, NULL, NULL);
		} else {
			say(sl, sl_size, mfd_text("Svg.FilterUnnamed", "FX"), NULL, NULL, NULL, NULL);
		}
		break;
	default:
		say(as, as_size, mfd_text("Assign.NotUsed", "Not used. Click to set up."), NULL, NULL, NULL, NULL);
		say(sl, sl_size, mfd_text("Svg.NotUsed", "Not used"), NULL, NULL, NULL, NULL);
		break;
	}
}

static void format_pickup_sentence(const struct mfd_ui_strip_input *in, bool phys_move_up, int target_percent,
				   char *out, size_t out_size)
{
	char pct[16];
	snprintf(pct, sizeof(pct), "%d", target_percent);
	const bool is_knob = (in->slot_index >= 10 && in->slot_index < 18);
	const char *text;
	if (is_knob) {
		text = phys_move_up ? mfd_text("Pickup.KnobRight", "Turn the knob right to %1% to pick up.")
				    : mfd_text("Pickup.KnobLeft", "Turn the knob left to %1% to pick up.");
	} else if (in->is_horizontal) {
		text = phys_move_up ? mfd_text("Pickup.CrossfaderRight", "Move the crossfader right to %1% to pick up.")
				    : mfd_text("Pickup.CrossfaderLeft", "Move the crossfader left to %1% to pick up.");
	} else {
		text = phys_move_up ? mfd_text("Pickup.FaderUp", "Move the fader up to %1% to pick up.")
				    : mfd_text("Pickup.FaderDown", "Move the fader down to %1% to pick up.");
	}
	say(out, out_size, text, pct, NULL, NULL, NULL);
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

	const bool is_fx_bank = (in->slot_index >= 10 && in->slot_index < 18);
	mfd_ui_strip_title(in->slot_index, in->slot_label, out->title, sizeof(out->title));
	if (is_fx_bank && (in->addr_type == MFD_MIDI_NONE || !in->has_last_fader) &&
	    in->target_type != MFD_TARGET_UNASSIGNED) {
		out->bar_percent = mfd_calibrated_percent(in->applied_val);
	} else {
		out->bar_percent = mfd_calibrated_percent(in->raw_norm);
	}
	out->bottom_action = MFD_UI_ACTION_SETUP;
	say(out->bottom_button_text, sizeof(out->bottom_button_text), mfd_text("Strip.SetUp", "Set up"), NULL, NULL,
	    NULL, NULL);

	format_assignment(in, out);

	/* Priority 1: Unassigned / disabled */
	if (!in->enabled || in->target_type == MFD_TARGET_UNASSIGNED) {
		out->dimmed = true;
		return;
	}

	/* Priority 2: Switch scenes with Studio Mode off */
	if (!in->available && in->target_type == MFD_TARGET_TBAR && in->reason_msg &&
	    in->reason_msg->code == MFD_MSG_STUDIO_MODE_OFF) {
		mfd_ui_format_msg(in->reason_msg, out->status_sentence, sizeof(out->status_sentence));
		out->severity = MFD_UI_SEVERITY_AMBER;
		out->bottom_action = MFD_UI_ACTION_TURN_ON_STUDIO_MODE;
		say(out->bottom_button_text, sizeof(out->bottom_button_text),
		    mfd_text("Strip.TurnOnStudioMode", "Turn on Studio Mode"), NULL, NULL, NULL, NULL);
		return;
	}

	/* Priority 3: Any other unmet precondition or unbound MIDI hardware control (fader strips 0..9) */
	if (!in->available) {
		if (in->reason_msg && in->reason_msg->code != MFD_MSG_NONE) {
			mfd_ui_format_msg(in->reason_msg, out->status_sentence, sizeof(out->status_sentence));
		} else if (has_text(in->reason)) {
			snprintf(out->status_sentence, sizeof(out->status_sentence), "%s", in->reason);
		} else {
			say(out->status_sentence, sizeof(out->status_sentence),
			    mfd_text("Assign.NotUsed", "Not used. Click to set up."), NULL, NULL, NULL, NULL);
		}
		out->severity = MFD_UI_SEVERITY_RED;
		return;
	}
	if (in->addr_type == MFD_MIDI_NONE && !is_fx_bank) {
		say(out->status_sentence, sizeof(out->status_sentence),
		    mfd_text("Strip.NoMidiControl", "No MIDI control bound."), NULL, NULL, NULL, NULL);
		out->severity = MFD_UI_SEVERITY_RED;
		return;
	}

	/* Priority 4: At a rail, parked */
	if (in->takeover_state == MFD_TAKEOVER_PARKED ||
	    (in->target_type == MFD_TARGET_TBAR && in->tbar_state == MFD_TBAR_PARKED)) {
		say(out->state_readout, sizeof(out->state_readout), mfd_text("Readout.AtEnd", "At end."), NULL, NULL,
		    NULL, NULL);
		out->at_end = true;
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
		format_pickup_sentence(in, phys_move_up, tgt_pct, out->status_sentence, sizeof(out->status_sentence));
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

void mfd_ui_format_finished_sentence_ex(int slot_index, const char *slot_label, enum mfd_target_type target_type,
					const char *scene_name, const char *source_a_name, const char *source_b_name,
					const char *filter_name, const char *filter_prop_label, char *out,
					size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	char title[64] = {0};
	mfd_ui_strip_title(slot_index, slot_label, title, sizeof(title));
	const char *sc = scene_name;
	const char *sa = source_a_name;
	const char *sb = source_b_name;

	switch (target_type) {
	case MFD_TARGET_TBAR:
		say(out, out_size, mfd_text("Finished.Tbar", "%1 switches scenes (T-bar)"), title, NULL, NULL, NULL);
		break;
	case MFD_TARGET_TWO_SOURCE_MIX:
		if (has_text(sa) && has_text(sb)) {
			if (has_text(sc)) {
				say(out, out_size, mfd_text("Finished.MixInScene", "%1 mixes %2 and %3 in scene %4"),
				    title, sa, sb, sc);
			} else {
				say(out, out_size, mfd_text("Finished.Mix", "%1 mixes %2 and %3"), title, sa, sb, NULL);
			}
		} else if (has_text(sa) || has_text(sb)) {
			const char *named = has_text(sa) ? sa : sb;
			if (has_text(sc)) {
				say(out, out_size,
				    has_text(sa)
					    ? mfd_text("Finished.MixNoRightInScene",
						       "%1 mixes %2 and Right in scene %3")
					    : mfd_text("Finished.MixNoLeftInScene", "%1 mixes Left and %2 in scene %3"),
				    title, named, sc, NULL);
			} else {
				say(out, out_size,
				    has_text(sa) ? mfd_text("Finished.MixNoRight", "%1 mixes %2 and Right")
						 : mfd_text("Finished.MixNoLeft", "%1 mixes Left and %2"),
				    title, named, NULL, NULL);
			}
		} else if (has_text(sc)) {
			say(out, out_size,
			    mfd_text("Finished.MixUnnamedInScene", "%1 mixes Left and Right in scene %2"), title, sc,
			    NULL, NULL);
		} else {
			say(out, out_size, mfd_text("Finished.MixUnnamed", "%1 mixes Left and Right"), title, NULL,
			    NULL, NULL);
		}
		break;
	case MFD_TARGET_SINGLE_LAYER:
		if (has_text(sa)) {
			if (has_text(sc)) {
				say(out, out_size,
				    mfd_text("Finished.LayerInScene", "%1 shows or hides %2 in scene %3"), title, sa,
				    sc, NULL);
			} else {
				say(out, out_size, mfd_text("Finished.Layer", "%1 shows or hides %2"), title, sa, NULL,
				    NULL);
			}
		} else if (has_text(sc)) {
			say(out, out_size,
			    mfd_text("Finished.LayerUnnamedInScene", "%1 shows or hides a layer in scene %2"), title,
			    sc, NULL, NULL);
		} else {
			say(out, out_size, mfd_text("Finished.LayerUnnamed", "%1 shows or hides a layer"), title, NULL,
			    NULL, NULL);
		}
		break;
	case MFD_TARGET_AUDIO_VOLUME:
		if (has_text(sa)) {
			say(out, out_size, mfd_text("Finished.Volume", "%1 controls %2 volume"), title, sa, NULL, NULL);
		} else {
			say(out, out_size, mfd_text("Finished.VolumeUnnamed", "%1 controls audio volume"), title, NULL,
			    NULL, NULL);
		}
		break;
	case MFD_TARGET_MIX_MASTER:
		say(out, out_size,
		    mfd_text("Finished.MixMaster", "%1 requests shared gain from the Fadester mix provider"), title,
		    NULL, NULL, NULL);
		break;
	case MFD_TARGET_FILTER_PARAM:
		if (has_text(filter_name) && has_text(filter_prop_label)) {
			if (has_text(sa)) {
				say(out, out_size, mfd_text("Finished.Filter", "%1 controls %2 (%3) on %4"), title,
				    filter_name, filter_prop_label, sa);
			} else {
				say(out, out_size, mfd_text("Finished.FilterNoSource", "%1 controls %2 (%3)"), title,
				    filter_name, filter_prop_label, NULL);
			}
		} else {
			say(out, out_size, mfd_text("Finished.FilterUnnamed", "%1 controls a filter setting"), title,
			    NULL, NULL, NULL);
		}
		break;
	case MFD_TARGET_UNASSIGNED:
	default:
		say(out, out_size, mfd_text("Finished.NotUsed", "%1 is not used"), title, NULL, NULL, NULL);
		break;
	}
}

void mfd_ui_format_finished_sentence(int slot_index, const char *slot_label, enum mfd_target_type target_type,
				     const char *scene_name, const char *source_a_name, const char *source_b_name,
				     char *out, size_t out_size)
{
	mfd_ui_format_finished_sentence_ex(slot_index, slot_label, target_type, scene_name, source_a_name,
					   source_b_name, NULL, NULL, out, out_size);
}

void mfd_ui_format_live_scene_note(const char *scene_name, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	if (!has_text(scene_name)) {
		out[0] = '\0';
		return;
	}
	say(out, out_size, mfd_text("Note.LiveScene", "You will see this when scene %1 is live."), scene_name, NULL,
	    NULL, NULL);
}

void mfd_ui_format_shared_layer_note(const char *layer_name, const char *const *other_scenes, size_t other_scene_count,
				     char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!has_text(layer_name) || !other_scenes || other_scene_count == 0U) {
		return;
	}
	if (other_scene_count == 1U) {
		say(out, out_size, mfd_text("Note.SharedLayerOne", "%1 is also in scene %2; it will fade there too."),
		    layer_name, other_scenes[0], NULL, NULL);
	} else if (other_scene_count == 2U) {
		say(out, out_size,
		    mfd_text("Note.SharedLayerTwo", "%1 is also in scenes %2 and %3; it will fade there too."),
		    layer_name, other_scenes[0], other_scenes[1], NULL);
	} else {
		/* a translation words the count so it needs no plural agreement (R-I18N-3) */
		char more[16];
		snprintf(more, sizeof(more), "%u", (unsigned)(other_scene_count - 1U));
		say(out, out_size,
		    mfd_text("Note.SharedLayerMany",
			     "%1 is also in scene %2 and %3 other scenes; it will fade there too."),
		    layer_name, other_scenes[0], more, NULL);
	}
}

void mfd_ui_format_last_move(int last_moved_slot_index, const char *slot_label, bool has_last_midi,
			     const struct mfd_midi_decoded *last_midi, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	if (last_moved_slot_index >= 0 && last_moved_slot_index < 18) {
		char title[64] = {0};
		mfd_ui_strip_title(last_moved_slot_index, slot_label, title, sizeof(title));
		say(out, out_size, mfd_text("LastMove.Strip", "Last move: %1"), title, NULL, NULL, NULL);
		return;
	}
	if (has_last_midi && last_midi) {
		char ch[16];
		char number[16];
		snprintf(ch, sizeof(ch), "%u", (unsigned)(last_midi->addr.channel + 1U));
		snprintf(number, sizeof(number), "%u", (unsigned)last_midi->addr.number);
		switch (last_midi->addr.type) {
		case MFD_MIDI_CC:
			say(out, out_size, mfd_text("LastMove.UnmappedCc", "Last move: Unmapped CC %1 (Ch %2)"), number,
			    ch, NULL, NULL);
			return;
		case MFD_MIDI_PITCH_BEND:
			say(out, out_size,
			    mfd_text("LastMove.UnmappedPitchBend", "Last move: Unmapped Pitch Bend (Ch %1)"), ch, NULL,
			    NULL, NULL);
			return;
		case MFD_MIDI_NOTE_ON:
		case MFD_MIDI_NOTE_OFF:
			say(out, out_size, mfd_text("LastMove.UnmappedNote", "Last move: Unmapped Note %1 (Ch %2)"),
			    number, ch, NULL, NULL);
			return;
		default:
			break;
		}
	}
	say(out, out_size, mfd_text("LastMove.None", "Last move: None yet"), NULL, NULL, NULL, NULL);
}

void mfd_ui_format_zone3_status(bool studio_mode_active, const char *preview_scene, const char *program_scene,
				const char *last_move_text, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	char none_yet[96];
	if (!has_text(last_move_text)) {
		say(none_yet, sizeof(none_yet), mfd_text("LastMove.None", "Last move: None yet"), NULL, NULL, NULL,
		    NULL);
	}
	const char *lm = has_text(last_move_text) ? last_move_text : none_yet;
	const char *no_scene = mfd_text("Zone3.NoScene", "(none)");
	const char *prev = has_text(preview_scene) ? preview_scene : no_scene;
	const char *prog = has_text(program_scene) ? program_scene : no_scene;

	if (studio_mode_active) {
		say(out, out_size,
		    mfd_text("Zone3.StudioOn",
			     "Studio Mode on  \xE2\x80\xA2  Preview: %1 \xE2\x86\x92 Program: %2  \xE2\x80\xA2  %3"),
		    prev, prog, lm, NULL);
	} else if (has_text(program_scene)) {
		say(out, out_size,
		    mfd_text("Zone3.StudioOff", "Studio Mode off  \xE2\x80\xA2  Program: %1  \xE2\x80\xA2  %2"), prog,
		    lm, NULL, NULL);
	} else {
		say(out, out_size, mfd_text("Zone3.StudioOffNoProgram", "Studio Mode off  \xE2\x80\xA2  %1"), lm, NULL,
		    NULL, NULL);
	}
}

const char *mfd_ui_target_plain_name(enum mfd_target_type target_type)
{
	switch (target_type) {
	case MFD_TARGET_TBAR:
		return mfd_text("Target.Tbar", "Switch scenes (T-bar)");
	case MFD_TARGET_TWO_SOURCE_MIX:
		return mfd_text("Target.Mix", "Mix two layers");
	case MFD_TARGET_SINGLE_LAYER:
		return mfd_text("Target.Layer", "Show or hide a layer");
	case MFD_TARGET_AUDIO_VOLUME:
		return mfd_text("Target.Volume", "Volume");
	case MFD_TARGET_MIX_MASTER:
		return mfd_text("Target.MixMaster", "Fadester mix master");
	case MFD_TARGET_FILTER_PARAM:
		return mfd_text("Target.Filter", "Filter or effect setting");
	case MFD_TARGET_UNASSIGNED:
	default:
		return mfd_text("Target.NotUsed", "Not used");
	}
}

const char *mfd_ui_curve_plain_name(enum mfd_crossfade_curve curve)
{
	switch (curve) {
	case MFD_CURVE_COSINE:
		return mfd_text("Curve.EqualPower", "Equal power");
	case MFD_CURVE_LINEAR:
		return mfd_text("Curve.Linear", "Linear");
	case MFD_CURVE_CUT:
		return mfd_text("Curve.Cut", "Cut");
	case MFD_CURVE_FULL_CENTER:
		return mfd_text("Curve.FullCenter", "Full at center");
	case MFD_CURVE_DIP_FREE:
	default:
		return mfd_text("Curve.DipFree", "Dip-free");
	}
}

const char *mfd_ui_curve_plain_description(enum mfd_crossfade_curve curve)
{
	switch (curve) {
	case MFD_CURVE_COSINE:
		return mfd_text("Curve.EqualPower.Description",
				"Smooth sine/cosine crossfade for matched-energy sources.");
	case MFD_CURVE_LINEAR:
		return mfd_text("Curve.Linear.Description", "Straight 0-100% blend on both layers.");
	case MFD_CURVE_CUT:
		return mfd_text("Curve.Cut.Description", "Instant switch at the halfway mark (50%).");
	case MFD_CURVE_FULL_CENTER:
		return mfd_text("Curve.FullCenter.Description", "Both layers reach 100% at the center mark.");
	case MFD_CURVE_DIP_FREE:
	default:
		return mfd_text("Curve.DipFree.Description", "Default; the picture never darkens in the middle.");
	}
}

void mfd_ui_format_device_error(enum mfd_winmm_error_kind kind, enum mfd_winmm_operation operation, uint32_t error_code,
				const char *device_name, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	char code[16];
	snprintf(code, sizeof(code), "%u", (unsigned)error_code);
	const char *dev = has_text(device_name) ? device_name : "";
	switch (kind) {
	case MFD_WINMM_ERR_NO_DEVICES:
		say(out, out_size,
		    mfd_text("Device.NoDevices",
			     "No MIDI input devices detected. Connect your MIDI controller (e.g. Akai "
			     "APC40 mkII) and click Reconnect."),
		    NULL, NULL, NULL, NULL);
		break;
	case MFD_WINMM_ERR_CONFIGURED_NOT_FOUND:
		/* with no device name the configured device was missing because no device was there at all */
		if (has_text(device_name)) {
			say(out, out_size,
			    mfd_text("Device.NotConnected", "Configured MIDI device '%1' is not connected."), dev, NULL,
			    NULL, NULL);
		} else {
			say(out, out_size,
			    mfd_text("Device.NoDevices", "No MIDI input devices detected. Connect your MIDI controller "
							 "(e.g. Akai APC40 mkII) and click Reconnect."),
			    NULL, NULL, NULL, NULL);
		}
		break;
	case MFD_WINMM_ERR_BUSY_ALLOCATED:
		say(out, out_size,
		    mfd_text("Device.Busy",
			     "MIDI port '%1' is busy (MMSYSERR_ALLOCATED). Windows WinMM allows only one "
			     "application at a time \xE2\x80\x94 close Ableton Live, MIDI-OX, or your DAW and "
			     "click Reconnect."),
		    dev, NULL, NULL, NULL);
		break;
	case MFD_WINMM_ERR_DISCONNECTED:
		say(out, out_size,
		    mfd_text("Device.Disconnected",
			     "MIDI device '%1' was disconnected. Reconnect USB cable and click Reconnect."),
		    dev, NULL, NULL, NULL);
		break;
	case MFD_WINMM_ERR_OPEN_FAILED:
		if (operation == MFD_WINMM_OP_START) {
			say(out, out_size,
			    mfd_text("Device.StartFailed",
				     "Failed to start MIDI input stream on '%1' (WinMM error code %2)."),
			    dev, code, NULL, NULL);
		} else {
			say(out, out_size,
			    mfd_text("Device.OpenFailed", "Failed to open MIDI port '%1' (WinMM error code %2)."), dev,
			    code, NULL, NULL);
		}
		break;
	case MFD_WINMM_OK:
	default:
		break;
	}
}

void mfd_ui_format_msg(const struct mfd_msg *msg, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!msg) {
		return;
	}
	struct mfd_msg shown = *msg;
	/* the owning strip by its translated name, not its English label (%4 carries its slot index) */
	if (shown.code == MFD_MSG_SETTING_ALREADY_CONTROLLED && shown.args[3][0] != '\0') {
		int owner = -1;
		if (sscanf(shown.args[3], "%d", &owner) == 1 && owner >= 0 && owner < 18) {
			char owner_name[MFD_MSG_ARG_SIZE];
			mfd_ui_strip_name(owner, msg->args[2], owner_name, sizeof(owner_name));
			snprintf(shown.args[2], sizeof(shown.args[2]), "%s", owner_name);
		}
	}
	mfd_msg_format(&shown, true, out, out_size);
}
