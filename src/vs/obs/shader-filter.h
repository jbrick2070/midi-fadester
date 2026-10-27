/*
 * Visual Sidechain - OBS 33 FFT-Reactive GPU Shader Filter
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void vs_register_shader_filter(void);
void vs_shader_filter_apply_panic(obs_source_t *source);
void vs_shader_filter_apply_restore(obs_source_t *source);

#ifdef __cplusplus
}
#endif
