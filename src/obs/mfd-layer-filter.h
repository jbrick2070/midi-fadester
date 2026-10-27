/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_LAYER_FILTER_ID "midi_fadester_opacity_filter"
#define MFD_INTERNAL_OPACITY_FILTER_NAME "__midi_fadester_opacity__"

void mfd_register_layer_filter(void);

/*
 * Fast-path real-time setter used by the fader engine on the video tick thread.
 * Sets normalized opacity in [0.0f, 1.0f] without allocating obs_data_t per frame.
 */
void mfd_layer_filter_set_opacity(obs_source_t *filter, float opacity_01);

/*
 * Reads the current normalized opacity [0.0f, 1.0f] from a Midi Fadester layer filter.
 */
float mfd_layer_filter_get_opacity(obs_source_t *filter);

/*
 * Looks up or attaches the hidden internal "__midi_fadester_opacity__" filter on target_source.
 * Caller must release the returned obs_source_t reference via obs_source_release().
 */
obs_source_t *mfd_ensure_layer_opacity_filter(obs_source_t *target_source);

#ifdef __cplusplus
}
#endif
