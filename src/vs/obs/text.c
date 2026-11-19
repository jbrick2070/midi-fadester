/*
 * Visual Sidechain - translated printf formats
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "text.h"

#include "core/text-format.h"

#include <obs-module.h>

const char *vs_text_format(const char *key, const char *english)
{
	/* obs_module_text gives back the key itself when no file has it; a key has no conversions, so it is refused
	 * here like any other mismatch. Only immutable lookup data is read, so any thread may call this. */
	const char *text = obs_module_text(key);
	return vs_format_signature_equal(text, english) ? text : english;
}
