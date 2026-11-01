/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

/* Slot reasons and engine errors as codes plus arguments (R-I18N-2). The engine stores and compares these, the
 * obs-websocket API sends the English text and the code's slug (R-I18N-1), and the UI words them in the language OBS
 * runs in. Pure C17 with no OBS headers (R-TST-1). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_MSG_MAX_ARGS 5
#define MFD_MSG_ARG_SIZE 256

enum mfd_msg_code {
	MFD_MSG_NONE = 0,

	/* Slot reasons (why a strip is unavailable) */
	MFD_MSG_NOT_SET_UP,
	MFD_MSG_STUDIO_MODE_OFF,
	MFD_MSG_TBAR_SLIDER_MISSING,
	MFD_MSG_SAME_LAYER,
	MFD_MSG_SCENE_DELETED,            /* %1 scene */
	MFD_MSG_LAYER_NOT_IN_SCENE,       /* %1 layer, %2 scene */
	MFD_MSG_LAYERS_NOT_IN_SAME_SCENE, /* %1 left layer, %2 right layer */
	MFD_MSG_LAYER_NOT_IN_ANY_SCENE,   /* %1 layer */
	MFD_MSG_SOURCE_DELETED,           /* %1 source */
	MFD_MSG_NO_AUDIO,                 /* %1 source */
	MFD_MSG_FILTER_RESERVED,          /* %1 filter */
	MFD_MSG_FILTER_REMOVED,           /* %1 filter, %2 source */
	MFD_MSG_SETTING_UNAVAILABLE,      /* %1 setting, %2 filter */
	MFD_MSG_SETTING_NOT_NUMERIC,      /* %1 setting, %2 filter */
	MFD_MSG_RANGE_OUTSIDE_LIMITS,     /* %1 from, %2 to, %3 setting, %4 min, %5 max */

	/* Engine errors (why an edit was refused) */
	MFD_MSG_INVALID_STRIP,
	MFD_MSG_MISSING_SOURCE_A,
	MFD_MSG_MISSING_FILTER_NAME,
	MFD_MSG_MISSING_FILTER_PROP,
	MFD_MSG_SETTING_ALREADY_CONTROLLED, /* %1 setting, %2 filter, %3 strip label */
	MFD_MSG_SOURCE_NOT_FOUND,           /* %1 source */
	MFD_MSG_FILTER_NOT_FOUND,           /* %1 filter, %2 source */
	MFD_MSG_FILTER_NO_PROPERTIES,       /* %1 filter, %2 source */
	MFD_MSG_SETTING_NOT_FOUND,          /* %1 setting, %2 filter */
	MFD_MSG_RANGE_NOT_FINITE,
	MFD_MSG_RANGE_EMPTY,
	MFD_MSG_CHOOSE_SCENE_TWO_LAYERS,
	MFD_MSG_CHOOSE_SCENE_ONE_LAYER,
	MFD_MSG_SETUP_NOT_ACTIVE,
	MFD_MSG_CHANGED_EXTERNALLY,
	MFD_MSG_STRIP_PAUSED_IN_SETUP,
	MFD_MSG_ENGINE_NOT_INITIALIZED,
	MFD_MSG_INVALID_FADE_MS,

	MFD_MSG_CODE_COUNT
};

struct mfd_msg {
	enum mfd_msg_code code;
	char args[MFD_MSG_MAX_ARGS][MFD_MSG_ARG_SIZE];
};

/* Sets code and up to five arguments (NULL for none); arguments longer than MFD_MSG_ARG_SIZE - 1 are cut. */
void mfd_msg_set(struct mfd_msg *msg, enum mfd_msg_code code, const char *a1, const char *a2, const char *a3,
		 const char *a4, const char *a5);
void mfd_msg_clear(struct mfd_msg *msg);
bool mfd_msg_equal(const struct mfd_msg *a, const struct mfd_msg *b);

/* Stable snake_case id for the API's reason_code / error_code ("" for MFD_MSG_NONE). */
const char *mfd_msg_slug(enum mfd_msg_code code);

/* The English sentence for logs and the obs-websocket API; "" for MFD_MSG_NONE. */
void mfd_msg_format_english(const struct mfd_msg *msg, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
