/*
 * Midi Fadester — test-ui-sentences
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/ui-sentences.h"
#include "core/filter-math.h"
#include "core/midi-parser.h"
#include "core/mfd-text.h"
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

/* --- Marked lookup: en-US.ini's texts wrapped in guillemets, so a sentence that bypasses the hook shows up --- */
#define MARK_MAX_KEYS 1024
static char g_mark_keys[MARK_MAX_KEYS][64];
static char g_mark_texts[MARK_MAX_KEYS][320];
static size_t g_mark_count;

static void load_marked_locale(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f) && g_mark_count < MARK_MAX_KEYS) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq || eq[1] != '"') {
			continue;
		}
		*eq = '\0';
		snprintf(g_mark_keys[g_mark_count], sizeof(g_mark_keys[0]), "%s", line);
		char *src = eq + 2;
		char *dst = g_mark_texts[g_mark_count];
		size_t used = 0U;
		dst[used++] = '\xC2';
		dst[used++] = '\xAB';
		for (; *src && *src != '\r' && *src != '\n' && used + 4U < sizeof(g_mark_texts[0]); ++src) {
			if (src[0] == '\\' && src[1] != '\0') {
				++src;
				dst[used++] = (*src == 'n') ? '\n' : *src;
				continue;
			}
			if (src[0] == '"') {
				break;
			}
			dst[used++] = *src;
		}
		dst[used++] = '\xC2';
		dst[used++] = '\xBB';
		dst[used] = '\0';
		++g_mark_count;
	}
	fclose(f);
}

static const char *marked_text(const char *key)
{
	for (size_t i = 0U; i < g_mark_count; ++i) {
		if (strcmp(g_mark_keys[i], key) == 0) {
			return g_mark_texts[i];
		}
	}
	return NULL;
}

static int is_marked(const char *text)
{
	if (!text || !strstr(text, "\xC2\xAB")) {
		fprintf(stderr, "not through the text hook: '%s'\n", text ? text : "(null)");
		return 0;
	}
	/* every placeholder filled */
	for (const char *c = strchr(text, '%'); c; c = strchr(c + 1, '%')) {
		if (c[1] >= '1' && c[1] <= '9') {
			fprintf(stderr, "placeholder left: '%s'\n", text);
			return 0;
		}
	}
	return 1;
}

static int check_marked_sentences(void)
{
	load_marked_locale(MFD_TEST_LOCALE_FILE);
	REQUIRE(g_mark_count > 100U);
	mfd_text_set_lookup(marked_text);
	char out[512];

	/* strip titles that are words, and strip names */
	mfd_ui_strip_title(0, "", out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_strip_title(9, "", out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_strip_title(12, "", out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_strip_name(3, "", out, sizeof(out));
	REQUIRE(is_marked(out));

	/* controller bar, every kind, named and unnamed */
	const enum mfd_winmm_error_kind kinds[] = {MFD_WINMM_ERR_NO_DEVICES, MFD_WINMM_ERR_CONFIGURED_NOT_FOUND,
						   MFD_WINMM_ERR_BUSY_ALLOCATED, MFD_WINMM_ERR_DISCONNECTED,
						   MFD_WINMM_ERR_OPEN_FAILED};
	struct mfd_ui_controller_presentation ctrl;
	mfd_ui_format_controller_status(true, "", "", MFD_WINMM_OK, &ctrl);
	REQUIRE(is_marked(ctrl.status_sentence) && is_marked(ctrl.display_name));
	for (size_t k = 0U; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
		mfd_ui_format_controller_status(false, "APC40 mkII", "", kinds[k], &ctrl);
		REQUIRE(is_marked(ctrl.status_sentence));
		mfd_ui_format_controller_status(false, "", "", kinds[k], &ctrl);
		REQUIRE(is_marked(ctrl.status_sentence) && is_marked(ctrl.display_name));
		mfd_ui_format_device_error(kinds[k], MFD_WINMM_OP_OPEN, 4U, "APC40 mkII", out, sizeof(out));
		REQUIRE(is_marked(out));
		mfd_ui_format_device_error(kinds[k], MFD_WINMM_OP_START, 4U, "", out, sizeof(out));
		REQUIRE(is_marked(out));
	}

	/* strips: every target, named and unnamed, in each state */
	const enum mfd_target_type targets[] = {MFD_TARGET_UNASSIGNED,     MFD_TARGET_TBAR,
						MFD_TARGET_TWO_SOURCE_MIX, MFD_TARGET_SINGLE_LAYER,
						MFD_TARGET_AUDIO_VOLUME,   MFD_TARGET_FILTER_PARAM};
	for (size_t t = 0U; t < sizeof(targets) / sizeof(targets[0]); ++t) {
		for (int named = 0; named < 2; ++named) {
			for (int with_scene = 0; with_scene < 2; ++with_scene) {
				struct mfd_ui_strip_input in;
				struct mfd_ui_strip_presentation sp;
				memset(&in, 0, sizeof(in));
				in.slot_index = (targets[t] == MFD_TARGET_FILTER_PARAM) ? 10 : 2;
				in.enabled = true;
				in.addr_type = MFD_MIDI_CC;
				in.target_type = targets[t];
				in.scene_name = with_scene ? "Red" : "";
				in.source_a_name = named ? "Cam" : "";
				in.source_b_name = named ? "Loop" : "";
				if (named && with_scene && targets[t] == MFD_TARGET_TWO_SOURCE_MIX) {
					struct mfd_ui_strip_presentation half;
					in.source_b_name = "";
					mfd_ui_format_strip_presentation(&in, &half);
					REQUIRE(is_marked(half.assignment_sentence) && is_marked(half.svg_short_label));
					in.source_a_name = "";
					in.source_b_name = "Loop";
					mfd_ui_format_strip_presentation(&in, &half);
					REQUIRE(is_marked(half.assignment_sentence) && is_marked(half.svg_short_label));
					mfd_ui_format_finished_sentence(2, "", targets[t], "Red", "Cam", "", out,
									sizeof(out));
					REQUIRE(is_marked(out));
					mfd_ui_format_finished_sentence(2, "", targets[t], "", "", "Loop", out,
									sizeof(out));
					REQUIRE(is_marked(out));
					in.source_a_name = "Cam";
				}
				in.filter_name = named ? "Blur" : "";
				in.filter_prop_label = named ? "Radius" : "";
				in.available = true;
				in.has_last_fader = true;
				in.audio_db = -6.0f;
				mfd_ui_format_strip_presentation(&in, &sp);
				REQUIRE(is_marked(sp.assignment_sentence) && is_marked(sp.svg_short_label));
				REQUIRE(is_marked(sp.bottom_button_text));
				if (targets[t] != MFD_TARGET_UNASSIGNED && targets[t] != MFD_TARGET_FILTER_PARAM) {
					REQUIRE(is_marked(sp.state_readout));
				}
				in.auto_fade_active = true;
				mfd_ui_format_strip_presentation(&in, &sp);
				if (targets[t] != MFD_TARGET_UNASSIGNED &&
				    !(targets[t] == MFD_TARGET_TBAR && in.tbar_state != MFD_TBAR_THROWING)) {
					REQUIRE(is_marked(sp.state_readout));
				}
				in.auto_fade_active = false;
				in.takeover_state = MFD_TAKEOVER_WAITING;
				in.takeover_hint = MFD_HINT_MOVE_UP;
				for (int horizontal = 0; horizontal < 2; ++horizontal) {
					in.is_horizontal = horizontal != 0;
					mfd_ui_format_strip_presentation(&in, &sp);
					if (targets[t] != MFD_TARGET_UNASSIGNED) {
						REQUIRE(is_marked(sp.status_sentence));
					}
				}
				in.takeover_hint = MFD_HINT_MOVE_DOWN;
				for (int horizontal = 0; horizontal < 2; ++horizontal) {
					in.is_horizontal = horizontal != 0;
					mfd_ui_format_strip_presentation(&in, &sp);
					if (targets[t] != MFD_TARGET_UNASSIGNED) {
						REQUIRE(is_marked(sp.status_sentence));
					}
				}
				in.is_horizontal = false;
				in.takeover_state = MFD_TAKEOVER_PARKED;
				mfd_ui_format_strip_presentation(&in, &sp);
				if (targets[t] != MFD_TARGET_UNASSIGNED) {
					REQUIRE(is_marked(sp.state_readout) && sp.at_end);
				}
				in.takeover_state = MFD_TAKEOVER_ENGAGED;
				in.addr_type = MFD_MIDI_NONE;
				mfd_ui_format_strip_presentation(&in, &sp);
				if (targets[t] != MFD_TARGET_UNASSIGNED && targets[t] != MFD_TARGET_FILTER_PARAM) {
					REQUIRE(is_marked(sp.status_sentence));
				}

				mfd_ui_format_finished_sentence_ex(in.slot_index, "", targets[t], in.scene_name,
								   in.source_a_name, in.source_b_name, in.filter_name,
								   in.filter_prop_label, out, sizeof(out));
				REQUIRE(is_marked(out));
			}
		}
		REQUIRE(is_marked(mfd_ui_target_plain_name(targets[t])));
	}

	/* reasons and errors: every code, with the Studio Mode action */
	for (int code = 1; code < (int)MFD_MSG_CODE_COUNT; ++code) {
		struct mfd_msg msg;
		mfd_msg_set(&msg, (enum mfd_msg_code)code, "A", "B", "C", "3", "E");
		mfd_ui_format_msg(&msg, out, sizeof(out));
		REQUIRE(is_marked(out));
		char english[512];
		mfd_msg_format_english(&msg, english, sizeof(english));
		REQUIRE(!strstr(english, "\xC2\xAB")); /* the API stays English */

		struct mfd_ui_strip_input in;
		struct mfd_ui_strip_presentation sp;
		memset(&in, 0, sizeof(in));
		in.slot_index = 0;
		in.enabled = true;
		in.addr_type = MFD_MIDI_CC;
		in.target_type = MFD_TARGET_TBAR;
		in.available = false;
		in.reason = english;
		in.reason_msg = &msg;
		mfd_ui_format_strip_presentation(&in, &sp);
		REQUIRE(is_marked(sp.status_sentence) && is_marked(sp.bottom_button_text));
	}

	/* notes, last move, zone 3, curves, values */
	const char *others[] = {"Blue", "Green", "Yellow"};
	mfd_ui_format_live_scene_note("Red", out, sizeof(out));
	REQUIRE(is_marked(out));
	for (size_t n = 1U; n <= 3U; ++n) {
		mfd_ui_format_shared_layer_note("Cam", others, n, out, sizeof(out));
		REQUIRE(is_marked(out));
	}
	struct mfd_midi_decoded ev;
	memset(&ev, 0, sizeof(ev));
	const enum mfd_midi_msg_type types[] = {MFD_MIDI_CC, MFD_MIDI_PITCH_BEND, MFD_MIDI_NOTE_ON, MFD_MIDI_NOTE_OFF};
	for (size_t ti = 0U; ti < sizeof(types) / sizeof(types[0]); ++ti) {
		ev.addr.type = types[ti];
		mfd_ui_format_last_move(-1, NULL, true, &ev, out, sizeof(out));
		REQUIRE(is_marked(out));
	}
	mfd_ui_format_last_move(-1, NULL, false, NULL, out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_format_last_move(4, "", false, NULL, out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_format_zone3_status(true, "", "", "", out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_format_zone3_status(false, "", "Red", "", out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_format_zone3_status(false, "", "", "", out, sizeof(out));
	REQUIRE(is_marked(out));
	const enum mfd_crossfade_curve curves[] = {MFD_CURVE_DIP_FREE, MFD_CURVE_COSINE, MFD_CURVE_LINEAR,
						   MFD_CURVE_CUT, MFD_CURVE_FULL_CENTER};
	for (size_t ci = 0U; ci < sizeof(curves) / sizeof(curves[0]); ++ci) {
		REQUIRE(is_marked(mfd_ui_curve_plain_name(curves[ci])));
		REQUIRE(is_marked(mfd_ui_curve_plain_description(curves[ci])));
	}
	mfd_ui_format_db(-INFINITY, out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_format_db(-6.0f, out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_ui_format_percent(40, out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_filter_format_display_value_ex(0.0f, 0.0, 10.0, 0.0, 10.0, false, 0.1, " px", true, true, out, sizeof(out));
	REQUIRE(is_marked(out));
	mfd_filter_format_display_value(0.0f, 0.0, 10.0, 0.0, 10.0, false, 0.1, " px", true, out, sizeof(out));
	REQUIRE(strcmp(out, "0.0 px (Off)") == 0); /* the API path stays English */
	for (size_t pi = 0U; pi < mfd_profile_count(); ++pi) {
		const struct mfd_controller_profile_def *def = mfd_profile_get_by_index(pi);
		REQUIRE(is_marked(mfd_profile_ui_spec_note(def)));
		if (def->id == MFD_PROFILE_GENERIC) {
			REQUIRE(is_marked(mfd_profile_ui_display_name(def)) &&
				is_marked(mfd_profile_ui_short_name(def)));
		}
	}
	mfd_format_last_midi_readout(false, NULL, out, sizeof(out));
	REQUIRE(is_marked(out));
	for (size_t ti = 0U; ti < sizeof(types) / sizeof(types[0]); ++ti) {
		ev.addr.type = types[ti];
		mfd_format_last_midi_readout(true, &ev, out, sizeof(out));
		REQUIRE(is_marked(out));
	}
	REQUIRE(is_marked(mfd_learn_button_text(true)) && is_marked(mfd_learn_button_text(false)));

	/* a translation may reorder placeholders; a name holding "%1" is never filled again */
	mfd_ui_format_shared_layer_note("Cam %2", others, 2U, out, sizeof(out));
	REQUIRE(strstr(out, "Cam %2") != NULL && strstr(out, "Blue") != NULL);

	mfd_text_set_lookup(NULL);
	return 0;
}

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
	for (int k = 0; k < 8; ++k) {
		char def_fx[16];
		char def_fader[16];
		snprintf(def_fx, sizeof(def_fx), "FX %d", k + 1);
		snprintf(def_fader, sizeof(def_fader), "Fader %d", 11 + k);
		mfd_ui_strip_title(10 + k, def_fx, title, sizeof(title));
		REQUIRE(strcmp(title, def_fx) == 0);
		mfd_ui_strip_title(10 + k, def_fader, title, sizeof(title));
		REQUIRE(strcmp(title, def_fx) == 0);
	}
	mfd_ui_strip_title(10, "Cam Blur", title, sizeof(title));
	REQUIRE(strcmp(title, "Cam Blur") == 0);

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
	REQUIRE(strcmp(ctrl.status_sentence, "APC40 mkII not found. Plug it in, then press Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_DISCONNECTED, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence, "APC40 mkII not found. Plug it in, then press Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_NO_DEVICES, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence, "APC40 mkII not found. Plug it in, then press Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "", "", MFD_WINMM_ERR_NO_DEVICES, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence, "No MIDI controller found. Plug one in, then press Reconnect.") == 0);

	mfd_ui_format_controller_status(false, "APC40 mkII", "", MFD_WINMM_ERR_OPEN_FAILED, &ctrl);
	REQUIRE(strcmp(ctrl.status_sentence,
		       "Could not open APC40 mkII. Unplug and replug it, then press Reconnect.") == 0);

	/* 2b. The Advanced banner says a device error in the controller bar's words, with no WinMM code */
	char dev_err[320];
	mfd_ui_format_device_error(MFD_WINMM_ERR_BUSY_ALLOCATED, MFD_WINMM_OP_OPEN, 4U, "APC40 mkII", dev_err,
				   sizeof(dev_err));
	REQUIRE(strcmp(dev_err, "Another program has the APC40 mkII open. Close it, then press Reconnect.") == 0);
	mfd_ui_format_device_error(MFD_WINMM_ERR_BUSY_ALLOCATED, MFD_WINMM_OP_OPEN, 4U, "", dev_err, sizeof(dev_err));
	REQUIRE(strcmp(dev_err, "Another program has the MIDI controller open. Close it, then press Reconnect.") == 0);
	mfd_ui_format_device_error(MFD_WINMM_ERR_DISCONNECTED, MFD_WINMM_OP_OPEN, 0U, "APC40 mkII", dev_err,
				   sizeof(dev_err));
	REQUIRE(strcmp(dev_err, "APC40 mkII not found. Plug it in, then press Reconnect.") == 0);
	mfd_ui_format_device_error(MFD_WINMM_ERR_CONFIGURED_NOT_FOUND, MFD_WINMM_OP_OPEN, 0U, "", dev_err,
				   sizeof(dev_err));
	REQUIRE(strcmp(dev_err, "No MIDI controller found. Plug one in, then press Reconnect.") == 0);
	mfd_ui_format_device_error(MFD_WINMM_ERR_NO_DEVICES, MFD_WINMM_OP_OPEN, 0U, "", dev_err, sizeof(dev_err));
	REQUIRE(strcmp(dev_err, "No MIDI controller found. Plug one in, then press Reconnect.") == 0);
	mfd_ui_format_device_error(MFD_WINMM_ERR_OPEN_FAILED, MFD_WINMM_OP_START, 4U, "APC40 mkII", dev_err,
				   sizeof(dev_err));
	REQUIRE(strcmp(dev_err, "Could not open APC40 mkII. Unplug and replug it, then press Reconnect.") == 0);
	mfd_ui_format_device_error(MFD_WINMM_OK, MFD_WINMM_OP_OPEN, 0U, "APC40 mkII", dev_err, sizeof(dev_err));
	REQUIRE(dev_err[0] == '\0');

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
	struct mfd_msg studio_off;
	mfd_msg_set(&studio_off, MFD_MSG_STUDIO_MODE_OFF, NULL, NULL, NULL, NULL, NULL);
	in.reason_msg = &studio_off;
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

	/* The amber action comes from the reason code, not the English text (R-I18N-2). */
	in.reason_msg = NULL;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_RED);
	REQUIRE(sp.bottom_action == MFD_UI_ACTION_SETUP);
	in.reason = "Translated reason text";
	in.reason_msg = &studio_off;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_AMBER);
	REQUIRE(sp.bottom_action == MFD_UI_ACTION_TURN_ON_STUDIO_MODE);
	REQUIRE(strcmp(mfd_msg_slug(MFD_MSG_STUDIO_MODE_OFF), "studio_mode_off") == 0);

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

	/* Row 3b: Assigned & available in engine, but hardware CC cleared ("No MIDI control assigned. ...") */
	in.available = true;
	in.reason = "";
	in.addr_type = MFD_MIDI_NONE;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.status_sentence, "No MIDI control assigned. Click Set up. In step 3, press Learn.") == 0);
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
	REQUIRE(sp.at_end);
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
	REQUIRE(strcmp(sp.status_sentence, "Move the crossfader left to 60% to pick up.") == 0);

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

	/* Row 7b: FX bank filter_param with no MIDI CC bound (knobless profile / Stream Deck + only):
	 * no red "No MIDI control assigned." error, bar reflects applied_val, readout shows display_value. */
	memset(&in, 0, sizeof(in));
	in.slot_index = 10;
	in.slot_label = "FX 1";
	in.enabled = true;
	in.addr_type = MFD_MIDI_NONE;
	in.target_type = MFD_TARGET_FILTER_PARAM;
	in.source_a_name = "Cam 1";
	in.filter_name = "Blur";
	in.filter_prop = "radius";
	in.filter_prop_label = "Radius";
	in.display_value = "+12.4 px";
	in.available = true;
	in.raw_norm = 0.0f;
	in.applied_val = 0.65f;
	in.takeover_state = MFD_TAKEOVER_ENGAGED;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.title, "FX 1") == 0);
	REQUIRE(!sp.dimmed);
	REQUIRE(sp.bar_percent == 65);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_NONE);
	REQUIRE(sp.status_sentence[0] == '\0');
	REQUIRE(strcmp(sp.state_readout, "+12.4 px") == 0);
	REQUIRE(strcmp(sp.assignment_sentence, "Cam 1: Blur (Radius)") == 0);
	REQUIRE(strcmp(sp.svg_short_label, "FX: Blur") == 0);

	in.auto_fade_active = true;
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "Fading +12.4 px") == 0);

	in.auto_fade_active = false;
	in.display_value = "";
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(strcmp(sp.state_readout, "65%") == 0);

	/* Physical fader strip (slot 1) bound to filter_param WITHOUT MIDI CC still flags red "No MIDI control assigned." */
	in.slot_index = 1;
	in.slot_label = "Track Fader 1";
	in.display_value = "+12.4 px";
	mfd_ui_format_strip_presentation(&in, &sp);
	REQUIRE(sp.severity == MFD_UI_SEVERITY_RED);
	REQUIRE(strcmp(sp.status_sentence, "No MIDI control assigned. Click Set up. In step 3, press Learn.") == 0);

	/* Filter precondition failure sentences on FX 1 */
	const char *filter_red_reasons[] = {
		"Cam 1 was deleted.",
		"Filter 'Blur' is no longer on Cam 1.",
		"Setting 'radius' is missing or not numeric on filter 'Blur'.",
		"Range [0, 200] is outside filter 'Blur' setting 'Radius' limits [0, 100].",
	};
	for (size_t fri = 0U; fri < sizeof(filter_red_reasons) / sizeof(filter_red_reasons[0]); ++fri) {
		in.slot_index = 10;
		in.slot_label = "FX 1";
		in.available = false;
		in.reason = filter_red_reasons[fri];
		mfd_ui_format_strip_presentation(&in, &sp);
		REQUIRE(sp.severity == MFD_UI_SEVERITY_RED);
		REQUIRE(strcmp(sp.status_sentence, filter_red_reasons[fri]) == 0);
		REQUIRE(sp.state_readout[0] == '\0');
	}

	/* 4. Step 3 finished sentences, Step 2 live/shared-layer notes, Zone 3 status & vocabulary */
	char sentence[192];
	mfd_ui_format_finished_sentence(0, "Crossfader (A/B)", MFD_TARGET_TWO_SOURCE_MIX, "Red", "Color", "Color 2",
					sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "Crossfader mixes Color and Color 2 in scene Red") == 0);

	mfd_ui_format_finished_sentence_ex(10, "FX 1", MFD_TARGET_FILTER_PARAM, "", "Cam 1", "", "Blur", "Radius",
					   sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence, "FX 1 controls Blur (Radius) on Cam 1") == 0);

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
	mfd_ui_format_last_move(10, "FX 1", true, NULL, lm, sizeof(lm));
	REQUIRE(strcmp(lm, "Last move: FX 1") == 0);

	mfd_ui_format_zone3_status(true, "Blue", "Red", lm, sentence, sizeof(sentence));
	REQUIRE(strcmp(sentence,
		       "Studio Mode on  \xE2\x80\xA2  Preview: Blue \xE2\x86\x92 Program: Red  \xE2\x80\xA2  Last move: FX 1") ==
		0);

	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_TBAR), "Switch scenes (T-bar)") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_TWO_SOURCE_MIX), "Mix two layers") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_SINGLE_LAYER), "Show or hide a layer") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_AUDIO_VOLUME), "Volume") == 0);
	REQUIRE(strcmp(mfd_ui_target_plain_name(MFD_TARGET_FILTER_PARAM), "Filter or effect setting") == 0);
	REQUIRE(strcmp(mfd_ui_curve_plain_name(MFD_CURVE_DIP_FREE), "Dip-free") == 0);
	REQUIRE(strcmp(mfd_ui_curve_plain_name(MFD_CURVE_COSINE), "Equal power") == 0);

	/* Reason messages: single-pass fill, so a scene named "%1" or "%2" is never filled again (R-I18N-3). */
	struct mfd_msg layer_gone;
	mfd_msg_set(&layer_gone, MFD_MSG_LAYER_NOT_IN_SCENE, "Cam %2", "Scene %1", NULL, NULL, NULL);
	char reason_text[160];
	mfd_msg_format_english(&layer_gone, reason_text, sizeof(reason_text));
	REQUIRE(strcmp(reason_text, "Cam %2 is no longer in scene Scene %1.") == 0);
	struct mfd_msg same_args;
	mfd_msg_set(&same_args, MFD_MSG_LAYER_NOT_IN_SCENE, "Cam %2", "Scene %1", NULL, NULL, NULL);
	REQUIRE(mfd_msg_equal(&layer_gone, &same_args));
	mfd_msg_set(&same_args, MFD_MSG_LAYER_NOT_IN_SCENE, "Cam %2", "Scene 2", NULL, NULL, NULL);
	REQUIRE(!mfd_msg_equal(&layer_gone, &same_args));
	mfd_msg_clear(&same_args);
	mfd_msg_format_english(&same_args, reason_text, sizeof(reason_text));
	REQUIRE(reason_text[0] == '\0');
	REQUIRE(mfd_msg_slug(MFD_MSG_NONE)[0] == '\0');
	for (int code = 1; code < (int)MFD_MSG_CODE_COUNT; ++code) {
		REQUIRE(mfd_msg_slug((enum mfd_msg_code)code)[0] != '\0');
	}

	/* Default labels: stored empty (or a legacy English default) show the short title; a typed label as typed */
	mfd_ui_strip_title(0, "", title, sizeof(title));
	REQUIRE(strcmp(title, "Crossfader") == 0);
	mfd_ui_strip_title(3, NULL, title, sizeof(title));
	REQUIRE(strcmp(title, "3") == 0);
	mfd_ui_strip_title(12, "", title, sizeof(title));
	REQUIRE(strcmp(title, "FX 3") == 0);
	REQUIRE(mfd_profile_is_default_slot_label(9, "Master Fader"));
	REQUIRE(!mfd_profile_is_default_slot_label(9, "Master"));
	char default_label[64];
	mfd_profile_default_slot_label(MFD_PROFILE_APC40_MK2, 1, default_label, sizeof(default_label));
	REQUIRE(strcmp(default_label, "Track Fader 1") == 0);

	REQUIRE(check_marked_sentences() == 0);

	printf("test-ui-sentences: PASS\n");
	return 0;
}
