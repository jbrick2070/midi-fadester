/*
 * Tubester - a tape path becomes a file:/// address for the deck page
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest path accepted, in bytes of UTF-8 without the terminator. Windows allows 32,767 characters with the long-path
 * prefix; a tape longer than this is refused rather than truncated. */
#define TUB_PATH_MAX 32000

/* True when the path can name a tape (R-TAP-3): absolute on a drive (C:\... or C:/..., with something after the root)
 * or a UNC share (\\server\share\...), no control characters, none of ? * < > | ", not longer than TUB_PATH_MAX.
 * Device paths such as \\.\ and \\?\ are refused. Whether the file exists is not checked here. */
bool tub_path_is_usable(const char *path);

/* Writes "file:///C:/a/b%20c.mp4" for a drive path, or "file://server/share/x.mp4" for a UNC path. Every byte outside
 * the unreserved set and '/' is percent-encoded, so the page sees exactly the path given (R-TAP-3). Returns the number
 * of bytes written without the terminator, or 0 with out set to "" when the path is not usable or the buffer is too
 * small. */
size_t tub_path_to_file_url(const char *path, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
