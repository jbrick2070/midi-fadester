/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/slot-messages.h"
#include "core/mfd-text.h"

#include <stdio.h>
#include <string.h>

static const char *const k_slugs[MFD_MSG_CODE_COUNT] = {
	[MFD_MSG_NONE] = "",
	[MFD_MSG_NOT_SET_UP] = "not_set_up",
	[MFD_MSG_STUDIO_MODE_OFF] = "studio_mode_off",
	[MFD_MSG_TBAR_SLIDER_MISSING] = "tbar_slider_missing",
	[MFD_MSG_SAME_LAYER] = "same_layer",
	[MFD_MSG_SCENE_DELETED] = "scene_deleted",
	[MFD_MSG_LAYER_NOT_IN_SCENE] = "layer_not_in_scene",
	[MFD_MSG_LAYERS_NOT_IN_SAME_SCENE] = "layers_not_in_same_scene",
	[MFD_MSG_LAYER_NOT_IN_ANY_SCENE] = "layer_not_in_any_scene",
	[MFD_MSG_SOURCE_DELETED] = "source_deleted",
	[MFD_MSG_NO_AUDIO] = "no_audio",
	[MFD_MSG_FILTER_RESERVED] = "filter_reserved",
	[MFD_MSG_FILTER_REMOVED] = "filter_removed",
	[MFD_MSG_SETTING_UNAVAILABLE] = "setting_unavailable",
	[MFD_MSG_SETTING_NOT_NUMERIC] = "setting_not_numeric",
	[MFD_MSG_RANGE_OUTSIDE_LIMITS] = "range_outside_limits",
	[MFD_MSG_INVALID_STRIP] = "invalid_strip",
	[MFD_MSG_MISSING_SOURCE_A] = "missing_source_a",
	[MFD_MSG_MISSING_FILTER_NAME] = "missing_filter_name",
	[MFD_MSG_MISSING_FILTER_PROP] = "missing_filter_prop",
	[MFD_MSG_SETTING_ALREADY_CONTROLLED] = "setting_already_controlled",
	[MFD_MSG_SOURCE_NOT_FOUND] = "source_not_found",
	[MFD_MSG_FILTER_NOT_FOUND] = "filter_not_found",
	[MFD_MSG_FILTER_NO_PROPERTIES] = "filter_no_properties",
	[MFD_MSG_SETTING_NOT_FOUND] = "setting_not_found",
	[MFD_MSG_RANGE_NOT_FINITE] = "range_not_finite",
	[MFD_MSG_RANGE_EMPTY] = "range_empty",
	[MFD_MSG_CHOOSE_SCENE_TWO_LAYERS] = "choose_scene_two_layers",
	[MFD_MSG_CHOOSE_SCENE_ONE_LAYER] = "choose_scene_one_layer",
	[MFD_MSG_SETUP_NOT_ACTIVE] = "setup_not_active",
	[MFD_MSG_CHANGED_EXTERNALLY] = "changed_externally",
	[MFD_MSG_STRIP_PAUSED_IN_SETUP] = "strip_paused_in_setup",
	[MFD_MSG_ENGINE_NOT_INITIALIZED] = "engine_not_initialized",
	[MFD_MSG_INVALID_FADE_MS] = "invalid_fade_ms",
};

/* The template for a code: English for logs and the API, or the locale text for the UI. %1..%5 are the
 * arguments. */
static const char *template_for(enum mfd_msg_code code, bool translate)
{
	switch (code) {
	case MFD_MSG_NOT_SET_UP:
		return mfd_text_or_english(translate, "Msg.NotSetUp", "Not used. Click to set up.");
	case MFD_MSG_STUDIO_MODE_OFF:
		return mfd_text_or_english(translate, "Msg.StudioModeOff", "Studio Mode is off.");
	case MFD_MSG_TBAR_SLIDER_MISSING:
		return mfd_text_or_english(
			translate, "Msg.TbarSliderMissing",
			"Midi Fadester cannot find the T-bar in this OBS version. Switch scenes does not work. Everything "
			"else still works. Look for a newer version of Midi Fadester.");
	case MFD_MSG_SAME_LAYER:
		return mfd_text_or_english(translate, "Msg.SameLayer", "Left and Right cannot be the same layer.");
	case MFD_MSG_SCENE_DELETED:
		return mfd_text_or_english(translate, "Msg.SceneDeleted", "Scene %1 was deleted.");
	case MFD_MSG_LAYER_NOT_IN_SCENE:
		return mfd_text_or_english(translate, "Msg.LayerNotInScene", "%1 is no longer in scene %2.");
	case MFD_MSG_LAYERS_NOT_IN_SAME_SCENE:
		return mfd_text_or_english(translate, "Msg.LayersNotInSameScene",
					   "%1 and %2 are not in the same scene.");
	case MFD_MSG_LAYER_NOT_IN_ANY_SCENE:
		return mfd_text_or_english(translate, "Msg.LayerNotInAnyScene", "%1 is not in any scene.");
	case MFD_MSG_SOURCE_DELETED:
		return mfd_text_or_english(translate, "Msg.SourceDeleted", "%1 was deleted.");
	case MFD_MSG_NO_AUDIO:
		return mfd_text_or_english(translate, "Msg.NoAudio", "%1 has no audio.");
	case MFD_MSG_FILTER_RESERVED:
		return mfd_text_or_english(translate, "Msg.FilterReserved",
					   "Filter '%1' is reserved by Midi Fadester and cannot be bound directly.");
	case MFD_MSG_FILTER_REMOVED:
		return mfd_text_or_english(
			translate, "Msg.FilterRemoved",
			"Filter '%1' was removed from %2. Re-add it in OBS Filters or click Set up.");
	case MFD_MSG_SETTING_UNAVAILABLE:
		return mfd_text_or_english(
			translate, "Msg.SettingUnavailable",
			"Setting '%1' is no longer available on '%2'. Click Set up to choose another setting.");
	case MFD_MSG_SETTING_NOT_NUMERIC:
		return mfd_text_or_english(
			translate, "Msg.SettingNotNumeric",
			"Setting '%1' on '%2' is not a numeric setting. Choose a float or integer filter property.");
	case MFD_MSG_RANGE_OUTSIDE_LIMITS:
		return mfd_text_or_english(
			translate, "Msg.RangeOutsideLimits",
			"Range [%1, %2] is outside '%3' limits [%4, %5]. Click Set up to update the range.");
	case MFD_MSG_INVALID_STRIP:
		return mfd_text_or_english(translate, "Msg.InvalidStrip", "Invalid strip index.");
	case MFD_MSG_MISSING_SOURCE_A:
		return mfd_text_or_english(translate, "Msg.MissingSourceA",
					   "Missing required 'source_a' for filter_param.");
	case MFD_MSG_MISSING_FILTER_NAME:
		return mfd_text_or_english(translate, "Msg.MissingFilterName",
					   "Missing required 'filter_name' for filter_param.");
	case MFD_MSG_MISSING_FILTER_PROP:
		return mfd_text_or_english(translate, "Msg.MissingFilterProp",
					   "Missing required 'filter_prop' for filter_param.");
	case MFD_MSG_SETTING_ALREADY_CONTROLLED:
		return mfd_text_or_english(translate, "Msg.SettingAlreadyControlled",
					   "Setting '%1' on '%2' is already controlled by %3.");
	case MFD_MSG_SOURCE_NOT_FOUND:
		return mfd_text_or_english(
			translate, "Msg.SourceNotFound",
			"Source '%1' not found. Create the source in OBS first or check its spelling.");
	case MFD_MSG_FILTER_NOT_FOUND:
		return mfd_text_or_english(translate, "Msg.FilterNotFound",
					   "Filter '%1' not found on '%2'. Add the filter in OBS Filters first.");
	case MFD_MSG_FILTER_NO_PROPERTIES:
		return mfd_text_or_english(translate, "Msg.FilterNoProperties",
					   "Filter '%1' on '%2' exposes no configurable properties.");
	case MFD_MSG_SETTING_NOT_FOUND:
		return mfd_text_or_english(
			translate, "Msg.SettingNotFound",
			"Setting '%1' not found on filter '%2'. Query get_catalog to list numeric settings.");
	case MFD_MSG_RANGE_NOT_FINITE:
		return mfd_text_or_english(translate, "Msg.RangeNotFinite",
					   "'from_val' and 'to_val' must be finite numbers.");
	case MFD_MSG_RANGE_EMPTY:
		return mfd_text_or_english(translate, "Msg.RangeEmpty",
					   "'from_val' and 'to_val' must differ so the control has a range to sweep.");
	case MFD_MSG_CHOOSE_SCENE_TWO_LAYERS:
		return mfd_text_or_english(translate, "Msg.ChooseSceneTwoLayers",
					   "Choose a scene with at least two layers.");
	case MFD_MSG_CHOOSE_SCENE_ONE_LAYER:
		return mfd_text_or_english(translate, "Msg.ChooseSceneOneLayer",
					   "Choose a scene with at least one layer.");
	case MFD_MSG_SETUP_NOT_ACTIVE:
		return mfd_text_or_english(translate, "Msg.SetupNotActive",
					   "Setup session is no longer active for this strip.");
	case MFD_MSG_CHANGED_EXTERNALLY:
		return mfd_text_or_english(
			translate, "Msg.ChangedExternally",
			"This strip was changed externally while Set up was open. Review and press Done again.");
	case MFD_MSG_STRIP_PAUSED_IN_SETUP:
		return mfd_text_or_english(translate, "Msg.StripPausedInSetup",
					   "Strip is currently paused while Set up is open.");
	case MFD_MSG_ENGINE_NOT_INITIALIZED:
		return mfd_text_or_english(translate, "Msg.EngineNotInitialized", "Engine is not initialized.");
	case MFD_MSG_INVALID_FADE_MS:
		return mfd_text_or_english(translate, "Msg.InvalidFadeMs",
					   "Duration 'ms' must be 0 (instant) or between 50 and 20000 ms.");
	case MFD_MSG_NONE:
	case MFD_MSG_CODE_COUNT:
	default:
		return "";
	}
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
	if ((int)code < 0 || code >= MFD_MSG_CODE_COUNT || !k_slugs[code]) {
		return "";
	}
	return k_slugs[code];
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

void mfd_msg_format(const struct mfd_msg *msg, bool translate, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!msg) {
		return;
	}
	mfd_msg_fill(msg, template_for(msg->code, translate), out, out_size);
}

void mfd_msg_format_english(const struct mfd_msg *msg, char *out, size_t out_size)
{
	mfd_msg_format(msg, false, out, out_size);
}
