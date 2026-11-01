/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/slot-messages.h"
#include "core/mfd-text.h"

#include <stdio.h>
#include <string.h>

struct msg_entry {
	const char *slug;
	const char *english; /* %1..%5 are the arguments */
};

static const struct msg_entry k_messages[MFD_MSG_CODE_COUNT] = {
	[MFD_MSG_NONE] = {"", ""},
	[MFD_MSG_NOT_SET_UP] = {"not_set_up", "Not used. Click to set up."},
	[MFD_MSG_STUDIO_MODE_OFF] = {"studio_mode_off", "Studio Mode is off."},
	[MFD_MSG_TBAR_SLIDER_MISSING] = {"tbar_slider_missing",
					 "CRITICAL: Studio Mode 'slider-tbar' widget not found in this OBS version. "
					 "T-bar sync disabled."},
	[MFD_MSG_SAME_LAYER] = {"same_layer", "Left and Right cannot be the same layer."},
	[MFD_MSG_SCENE_DELETED] = {"scene_deleted", "Scene %1 was deleted."},
	[MFD_MSG_LAYER_NOT_IN_SCENE] = {"layer_not_in_scene", "%1 is no longer in scene %2."},
	[MFD_MSG_LAYERS_NOT_IN_SAME_SCENE] = {"layers_not_in_same_scene", "%1 and %2 are not in the same scene."},
	[MFD_MSG_LAYER_NOT_IN_ANY_SCENE] = {"layer_not_in_any_scene", "%1 is not in any scene."},
	[MFD_MSG_SOURCE_DELETED] = {"source_deleted", "%1 was deleted."},
	[MFD_MSG_NO_AUDIO] = {"no_audio", "%1 has no audio."},
	[MFD_MSG_FILTER_RESERVED] = {"filter_reserved",
				     "Filter '%1' is reserved by Midi Fadester and cannot be bound directly."},
	[MFD_MSG_FILTER_REMOVED] = {"filter_removed",
				    "Filter '%1' was removed from %2. Re-add it in OBS Filters or click Set up."},
	[MFD_MSG_SETTING_UNAVAILABLE] = {"setting_unavailable",
					 "Setting '%1' is no longer available on '%2'. Click Set up to choose another "
					 "setting."},
	[MFD_MSG_SETTING_NOT_NUMERIC] =
		{"setting_not_numeric",
		 "Setting '%1' on '%2' is not a numeric setting. Choose a float or integer filter "
		 "property."},
	[MFD_MSG_RANGE_OUTSIDE_LIMITS] = {"range_outside_limits",
					  "Range [%1, %2] is outside '%3' limits [%4, %5]. Click Set up to update the "
					  "range."},
	[MFD_MSG_INVALID_STRIP] = {"invalid_strip", "Invalid strip index."},
	[MFD_MSG_MISSING_SOURCE_A] = {"missing_source_a", "Missing required 'source_a' for filter_param."},
	[MFD_MSG_MISSING_FILTER_NAME] = {"missing_filter_name", "Missing required 'filter_name' for filter_param."},
	[MFD_MSG_MISSING_FILTER_PROP] = {"missing_filter_prop", "Missing required 'filter_prop' for filter_param."},
	[MFD_MSG_SETTING_ALREADY_CONTROLLED] = {"setting_already_controlled",
						"Setting '%1' on '%2' is already controlled by %3."},
	[MFD_MSG_SOURCE_NOT_FOUND] = {"source_not_found",
				      "Source '%1' not found. Create the source in OBS first or check its spelling."},
	[MFD_MSG_FILTER_NOT_FOUND] = {"filter_not_found",
				      "Filter '%1' not found on '%2'. Add the filter in OBS Filters first."},
	[MFD_MSG_FILTER_NO_PROPERTIES] = {"filter_no_properties",
					  "Filter '%1' on '%2' exposes no configurable properties."},
	[MFD_MSG_SETTING_NOT_FOUND] = {"setting_not_found",
				       "Setting '%1' not found on filter '%2'. Query get_catalog to list numeric "
				       "settings."},
	[MFD_MSG_RANGE_NOT_FINITE] = {"range_not_finite", "'from_val' and 'to_val' must be finite numbers."},
	[MFD_MSG_RANGE_EMPTY] = {"range_empty",
				 "'from_val' and 'to_val' must differ so the control has a range to sweep."},
	[MFD_MSG_CHOOSE_SCENE_TWO_LAYERS] = {"choose_scene_two_layers", "Choose a scene with at least two layers."},
	[MFD_MSG_CHOOSE_SCENE_ONE_LAYER] = {"choose_scene_one_layer", "Choose a scene with at least one layer."},
	[MFD_MSG_SETUP_NOT_ACTIVE] = {"setup_not_active", "Setup session is no longer active for this strip."},
	[MFD_MSG_CHANGED_EXTERNALLY] = {"changed_externally",
					"This strip was changed externally while Set up was open. Review and press Done "
					"again."},
	[MFD_MSG_STRIP_PAUSED_IN_SETUP] = {"strip_paused_in_setup", "Strip is currently paused while Set up is open."},
	[MFD_MSG_ENGINE_NOT_INITIALIZED] = {"engine_not_initialized", "Engine is not initialized."},
	[MFD_MSG_INVALID_FADE_MS] = {"invalid_fade_ms",
				     "Duration 'ms' must be 0 (instant) or between 50 and 20000 ms."},
};

static const struct msg_entry *entry_for(enum mfd_msg_code code)
{
	if ((int)code < 0 || code >= MFD_MSG_CODE_COUNT || !k_messages[code].slug) {
		return &k_messages[MFD_MSG_NONE];
	}
	return &k_messages[code];
}

void mfd_msg_clear(struct mfd_msg *msg)
{
	if (msg) {
		memset(msg, 0, sizeof(*msg));
	}
}

void mfd_msg_set(struct mfd_msg *msg, enum mfd_msg_code code, const char *a1, const char *a2, const char *a3,
		 const char *a4, const char *a5)
{
	if (!msg) {
		return;
	}
	mfd_msg_clear(msg);
	msg->code = code;
	const char *const values[MFD_MSG_MAX_ARGS] = {a1, a2, a3, a4, a5};
	for (size_t i = 0U; i < MFD_MSG_MAX_ARGS; ++i) {
		if (values[i]) {
			snprintf(msg->args[i], sizeof(msg->args[i]), "%s", values[i]);
		}
	}
}

bool mfd_msg_equal(const struct mfd_msg *a, const struct mfd_msg *b)
{
	if (!a || !b) {
		return a == b;
	}
	if (a->code != b->code) {
		return false;
	}
	for (size_t i = 0U; i < MFD_MSG_MAX_ARGS; ++i) {
		if (strcmp(a->args[i], b->args[i]) != 0) {
			return false;
		}
	}
	return true;
}

const char *mfd_msg_slug(enum mfd_msg_code code)
{
	return entry_for(code)->slug;
}

void mfd_msg_fill(const struct mfd_msg *msg, const char *text, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	const char *values[MFD_MSG_MAX_ARGS];
	for (size_t i = 0U; i < MFD_MSG_MAX_ARGS; ++i) {
		values[i] = msg ? msg->args[i] : "";
	}
	mfd_text_fill(out, out_size, text, values, MFD_MSG_MAX_ARGS);
}

void mfd_msg_format_english(const struct mfd_msg *msg, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!msg) {
		return;
	}
	mfd_msg_fill(msg, entry_for(msg->code)->english, out, out_size);
}
