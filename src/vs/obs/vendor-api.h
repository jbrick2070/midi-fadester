/*
 * Visual Sidechain - obs-websocket 5 Vendor API & In-Process Signal Bridge
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "core/fader-bridge.h"

#include <obs-data.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_VENDOR_NAME "visual-sidechain"

bool vs_vendor_api_register(void);
void vs_vendor_api_on_tick(uint64_t video_frame_ts, float dt_sec, const struct vs_audio_frame *frame,
			   bool is_primary_track);

void vs_vendor_handle_get_signals(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

#ifdef __cplusplus
}
#endif
