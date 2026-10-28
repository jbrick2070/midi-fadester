/*
 * Midi Fadester — test-ui-sentences
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/ui-sentences.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	/* 1. Desk-printed strip titles vs custom labels */
	char title[64];
	mfd_ui_strip_title(0, "Crossfader (A/B)", title, sizeof(title));
	REQUIRE(strcmp(title, "Crossfader") == 0);
	for (int i = 1; i <= 8; ++i) {
		char def_lbl[32];
		char exp_lbl[16];
		snprintf(def_lbl, sizeof(def_lbl), "Track Fader %d", i);
		snprintf(exp_lbl, sizeof(exp_lbl), "%d", i);
		mfd_ui_strip_title(i, def_lbl, title, sizeof(title));
		REQUIRE(strcmp(title, exp_lbl) == 0);
	}
	mfd_ui_strip_title(9, "Master Fader", title, sizeof(title));
	REQUIRE(strcmp(title, "Master") == 0);
	mfd_ui_strip_title(2, "Guest Cam", title, sizeof(title));
	REQUIRE(strcmp(title, "Guest Cam") == 0);

	/* 2. Controller bar 5-row precedence table */
	struct mfd_ui_controller_presentation ctrl;

	mfd_ui_format_controller_status(true, "APC40 mkII", "APC40 mkII", MFD_WINMM_OK, &ctrl);
	REQUIRE(ctrl.dot_green);
	REQUIRE(!ctrl.show_reconnect);
	REQUIRE(strcmp(ctrl.display_name, "APC40 mkII") == 0);
	REQUIRE(strcmp(ctrl.status_sentence, "Connected") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_BUSY_ALLOCATED, &ctrl);
	REQUIRE(!ctrl.dot_green);
	REQUIRE(ctrl.show_reconnect);
	REQUIRE(strcmp(ctrl.status_sentence,
		       "Another program has the APC40 mkII open. Close it, then press Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_CONFIGURED_NOT_FOUND, &ctrl);
	REQUIRE(!ctrl.dot_green);
	REQUIRE(ctrl.show_reconnect);
	REQUIRE(strcmp(ctrl.status_sentence, "APC40 mkII not found. Plug it in, then Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_DISCONNECTED, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence, "APC40 mkII not found. Plug it in, then Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_NO_DEVICES, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence, "APC40 mkII not found. Plug it in, then Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "", "", MFD_WINMM_ERR_NO_DEVICES, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence, "No MIDI controller found. Plug one in, then press Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_OPEN_FAILED, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence,
		       "Could not open APC40 mkII. Unplug and replug it, then press Reconnect.") == 0);

	/* 3. Strip presentation 7-row precedence table */
	struct mfd_ui_strip_input in;
	struct mfd_ui_strip_presentation sp;

	/* Row 1: Unassigned strip is dimmed, shows "Not used. Click to set up.", live bar still moves */
	memset(&in, 0, sizeof(in));
	in.slot_index = 2;
	in.slot_label = "Track Fader 2";
	in.enabled = true;
	in.addr_type = MFD_MIDI_CC;
	in.target_type = MFD_TARGET_UNASSIGNED;
	in.raw_norm = 51.0f / 127.0f;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.title, "2") == 0);
	REQUIRE(sp.dimmed);
	REQUIRE(sp.bar_percent == 40);
	REQUIRE(strcmp(sp.assignment_sentence, "Not used. Click to set up.") == 0);
	REQUIRE(sp.status_sentence[0] == '\0');
	REQUIRE(sp.state_readout[0] == '\0');
	REQUIRE(sp.severity == MFD_UI_SEVERITY_NONE);
	REQUIRE(sp.bottom_action == MFD_UI_ACTION_SETUP);
	REQUIRE(strcmp(sp.bottom_button_text, "Set up") == 0);

	/* Row 2: Switch scenes with Studio Mode off -> Amber + "Turn on Studio Mode" button + live bar */
	memset(&in, 0, sizeof(in));
	in.slot_index = 0;
	in.slot_label = "Crossfader (A/B)";
	in.is_horizontal = true;
	in.enabled = true;
	in.addr_type = MFD_MIDI_CC;
	in.target_type = MFD_TARGET_TBAR;
	in.available = false;
	in.reason = "Studio Mode is off.";
	in.raw_norm = 0.72f;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(!sp.dimmed);
	REQUIRE(sp.bar_percent == 72);
	REQUIRE(sp.state_readout[0] == '\0');
	REQUIRE(strcmp(sp.assignment_sentence, "Switches scenes (T-bar)") == 0);
	REQUIRE(strcmp(sp.status_sentence, "Studio Mode is off.") == 0);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_AMBER);
	REQUIRE(sp.bottom_action == MFD_UI_ACTION_TURN_ON_STUDIO_MODE);
	REQUIRE(strcmp(sp.bottom_button_text, "Turn on Studio Mode") == 0);

	/* Row 3: Every red unmet precondition sentence + unbound MIDI hardware control */
	const char *red_reasons[] = {
		"Color 2 is no longer in scene Red.",
		"Scene Red was deleted.",
		"Color and Color 2 are not in the same scene.",
		"Color is not in any scene.",
		"Mic/Aux was deleted.",
		"Mic/Aux has no audio.",
	};
	for (size_t ri = 0U; ri < sizeof(red_reasons) / sizeof(red_reasons[0]); ++ri) {
		memset(&in, 0, sizeof(in));
		in.slot_index = 1;
		in.slot_label = "Track Fader 1";
		in.enabled = true;
		in.addr_type = MFD_MIDI_CC;
		in.target_type = MFD_TARGET_TWO_SOURCE_MIX;
		in.scene_name = "Red";
		in.source_a_name = "Color";
		in.source_b_name = "Color 2";
		in.available = false;
		in.reason = red_reasons[ri];
		in.raw_norm = 0.35f;
		mfd_ui_format_strip_presentation(&in, &sp);
		REQUIRE(!sp.dimmed);
		REQUIRE(sp.bar_percent == 35);
		REQUIRE(sp.state_readout[0] == '\0');
		REQUIRE(strcmp(sp.status_sentence, red_reasons[ri]) == 0);
		REQUIRE(sp.severity == MFD_UI_SEVERITY_RED);
		REQUIRE(sp.bottom_action == MFD_UI_ACTION_SETUP);
		REQUIRE(strcmp(sp.bottom_button_text, "Set up") == 0);
	}

	/* Row 3b: Assigned & available in engine, but hardware CC cleared ("No MIDI control bound.") */
	in.available = true;
	in.reason = "";
	in.addr_type = MFD_MIDI_NONE;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.status_sentence, "No MIDI control bound.") == 0);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_RED);

	/* Row 4: At end. (parked at rail) */
	memset(&in, 0, sizeof(in));
	in.slot_index = 0;
	in.is_horizontal = true;
	in.enabled = true;
	in.addr_type = MFD_MIDI_CC;
	in.target_type = MFD_TARGET_TBAR;
	in.available = true;
	in.raw_norm = 1.0f;
	in.tbar_state = MFD_TBAR_PARKED;
	in.takeover_state = MFD_TAKEOVER_PARKED;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "At end.") == 0);
	REQUIRE(sp.status_sentence[0] == '\0');
	REQUIRE(!sp.show_finish_now);

	/* Row 5a: Startup pickup suppression when has_last_fader == false (no phantom pickup warning!) */
	memset(&in, 0, sizeof(in));
	in.slot_index = 9;
	in.enabled = true;
	in.addr_type = MFD_MIDI_CC;
	in.target_type = MFD_TARGET_AUDIO_VOLUME;
	in.source_a_name = "Desktop Audio";
	in.available = true;
	in.raw_norm = 1.0f;
	in.applied_val = 1.0f;
	in.audio_db = 0.0f;
	in.takeover_state = MFD_TAKEOVER_WAITING;
	in.takeover_hint = MFD_HINT_MOVE_UP;
	in.has_last_fader = false;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(sp.status_sentence[0] == '\0');
	REQUIRE(!sp.show_pickup_marker);
	REQUIRE(strcmp(sp.state_readout, "0.0 dB") == 0);
	REQUIRE(strcmp(sp.assignment_sentence, "Volume: Desktop Audio") == 0);

	/* Row 5b: Pickup pending on vertical fader ("Move the fader down to 12% to pick up." + ↓) */
	in.has_last_fader = true;
	in.raw_norm = 0.80f;
	in.target_obs = 0.12f;
	in.audio_db = -12.0f;
	in.takeover_hint = MFD_HINT_MOVE_DOWN;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(sp.show_pickup_marker);
	REQUIRE(sp.pickup_target_percent == 12);
	REQUIRE(sp.arrow == MFD_UI_ARROW_DOWN);
	REQUIRE(strcmp(sp.arrow_glyph, "\xE2\x86\x93") == 0);
	REQUIRE(strcmp(sp.status_sentence, "Move the fader down to 12% to pick up.") == 0);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_AMBER);
	REQUIRE(strcmp(sp.state_readout, "-12.0 dB") == 0);

	/* Row 5c: Reversed-polarity pickup on horizontal crossfader (target_obs = 0.40 -> physical 60%, move down/left) */
	memset(&in, 0, sizeof(in));
	in.slot_index = 0;
	in.is_horizontal = true;
	in.enabled = true;
	in.addr_type = MFD_MIDI_CC;
	in.target_type = MFD_TARGET_TBAR;
	in.available = true;
	in.raw_norm = 1.0f;
	in.applied_val = 0.40f;
	in.tbar_state = MFD_TBAR_THROWING;
	in.takeover_state = MFD_TAKEOVER_WAITING;
	in.takeover_hint = MFD_HINT_MOVE_UP; /* throw-space UP == physical DOWN when polarity_reversed */
	in.has_last_fader = true;
	in.polarity_reversed = true;
	in.target_obs = 0.40f;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(sp.show_pickup_marker);
	REQUIRE(sp.pickup_target_percent == 60);
	REQUIRE(sp.arrow == MFD_UI_ARROW_LEFT);
	REQUIRE(strcmp(sp.arrow_glyph, "\xE2\x86\x90") == 0);
	REQUIRE(strcmp(sp.status_sentence, "Move the fader down to 60% to pick up.") == 0);

	/* Row 6: Throwing 63% + Finish now mini-button (and Fading 63% during auto_fade) */
	in.takeover_state = MFD_TAKEOVER_ENGAGED;
	in.takeover_hint = MFD_HINT_NONE;
	in.applied_val = 0.63f;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "Throwing 63%") == 0);
	REQUIRE(sp.show_finish_now);
	REQUIRE(sp.status_sentence[0] == '\0');

	in.auto_fade_active = true;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "Fading 63%") == 0);

	/* Row 7: Engaged / Ready readouts across all 4 modes */
	in.auto_fade_active = false;
	in.tbar_state = MFD_TBAR_IDLE;
	in.applied_val = 0.0f;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "Ready") == 0);
	REQUIRE(!sp.show_finish_now);

	memset(&in, 0, sizeof(in));
	in.slot_index = 1;
	in.enabled = true;
	in.addr_type = MFD_MIDI_CC;
	in.target_type = MFD_TARGET_TWO_SOURCE_MIX;
	in.scene_name = "Red";
	in.source_a_name = "Color";
	in.source_b_name = "Color 2";
	in.available = true;
	in.alpha_a = 1.0f;
	in.alpha_b = 0.48f;
	in.takeover_state = MFD_TAKEOVER_ENGAGED;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "Color 100%, Color 2 48%") == 0);
	REQUIRE(strcmp(sp.assignment_sentence, "Mixes Color and Color 2 in Red") == 0);

	in.target_type = MFD_TARGET_SINGLE_LAYER;
	in.source_a_name = "Color 2";
	in.alpha_a = 0.48f;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "Color 2 48% visible") == 0);
	REQUIRE(strcmp(sp.assignment_sentence, "Shows or hides Color 2 in Red") == 0);

	in.target_type = MFD_TARGET_AUDIO_VOLUME;
	in.source_a_name = "Mic/Aux";
	in.audio_db = -INFINITY;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "-inf dB") == 0);

	/* 4. Step 3 finished sentences, Step 2 live/shared-layer notes, Zone 3 status & vocabulary */
	char sentence[192];
	mfd_ui_format_finished_sentence(0, "Crossfader (A/B)", MFD_TARGET_TWO_SOURCE_MIX, "Red", "Color", "Color 2",
					sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "Crossfader mixes Color and Color 2 in scene Red") == 0);

	mfd_ui_format_live_scene_note("Red", sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "You will see this when scene Red is live.") == 0);

	const char *others[] = {"Blue", "Green", "Yellow"};
	mfd_ui_format_shared_layer_note("Color 2", others, 0U, sentence, sizeof(sentence));
	REQUIRE(sentence[0] == '\0');
	mfd_ui_format_shared_layer_note("Color 2", others, 1U, sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "Color 2 is also in scene Blue; it will fade there too.") == 0);
	mfd_ui_format_shared_layer_note("Color 2", others, 2U, sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "Color 2 is also in scenes Blue and Green; it will fade there too.") == 0);
	mfd_ui_format_shared_layer_note("Color 2", others, 3U, sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "Color 2 is also in scene Blue and 2 other scenes; it will fade there too.") == 0);

	char lm[96];
	mfd_ui_format_last_move(0, "Crossfader (A/B)", true, NULL, lm, sizeof(lm));
	REQUIRE(strcmp(lm, "Last move: Crossfader") == 0);

	mfd_ui_format_zone3_status(true, "Blue", "Red", lm, sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence,
		       "Studio Mode on  \xE2\x80\xA2  Preview: Blue \xE2\x86\x92 Program: Red  \xE2\x80\xA2  Last move: Crossfader") ==
		0);

	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_TBAR), "Switch scenes (T-bar)") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_TWO_SOURCE_MIX), "Mix two layers") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_SINGLE_LAYER), "Show or hide a layer") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_AUDIO_VOLUME), "Volume") == 0);
	REQUIRE(strcmp(mfd_ui_curve_plain_name(MFD_CURVE_DIP_FREE), "Dip-free") == 0);
	REQUIRE(strcmp(mfd_ui_curve_plain_name(MFD_CURVE_COSINE), "Equal power") == 0);

	printf("test-ui-sentences: PASS\n");
	return 0;
}
