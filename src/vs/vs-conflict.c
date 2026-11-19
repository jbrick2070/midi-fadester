/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Whether the old standalone Visual Sidechain plugin was found loaded (vs-module.c sets it at load, the dock reads it on
 * the UI thread afterwards). Kept apart from vs-module.c so the dock and its UI test need none of Visual Sidechain. */

#include "vs-module.h"

#include <stdio.h>

static char g_conflict_path[512];

void vs_module_set_conflict_path(const char *path)
{
	snprintf(g_conflict_path, sizeof(g_conflict_path), "%s", path ? path : "");
}

const char *vs_module_conflict_path(void)
{
	return g_conflict_path[0] ? g_conflict_path : NULL;
}
