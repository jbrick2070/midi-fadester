/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Visual Sidechain's part of Midi Fadester's module (its filters, hotkeys, Tape Deck and the "visual-sidechain"
 * obs-websocket vendor), called from obs_module_load / _post_load / _unload. */
bool vs_module_load(void);
void vs_module_post_load(void);
void vs_module_unload(void);

/* The old standalone Visual Sidechain plugin's path when OBS loaded it before Midi Fadester (Visual Sidechain then stays
 * off here and the old copy runs the effects), else NULL. */
const char *vs_module_conflict_path(void);
void vs_module_set_conflict_path(const char *path); /* vs-conflict.c */

#ifdef __cplusplus
}
#endif
