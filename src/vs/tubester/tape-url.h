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
 * prefix (macOS far fewer); a tape longer than this is refused rather than truncated. */
#define TUB_PATH_MAX 32000

/* Which system's paths a function reads. The plugin always uses the host's (TUB_PATH_STYLE_HOST); the tests check
 * both styles on every platform. */
enum tub_path_style {
	TUB_PATH_STYLE_WINDOWS,
	TUB_PATH_STYLE_POSIX,
};

#ifdef _WIN32
#define TUB_PATH_STYLE_HOST TUB_PATH_STYLE_WINDOWS
#else
#define TUB_PATH_STYLE_HOST TUB_PATH_STYLE_POSIX
#endif

/* True when the path can name a tape (R-TAP-3), no control characters, none of ? * < > | ", not longer than
 * TUB_PATH_MAX. Whether the file exists is not checked here.
 * Windows: absolute on a drive (C:\... or C:/..., with something after the root) or a UNC share
 * (\\server\share\...); device paths such as \\.\ and \\?\ are refused.
 * POSIX (macOS): absolute (/Users/...), with something after the root; a backslash is an ordinary character. */
bool tub_path_is_usable_as(const char *path, enum tub_path_style style);

/* Windows: "file:///C:/a/b%20c.mp4" for a drive path, or "file://server/share/x.mp4" for a UNC path.
 * POSIX: "file:///Users/a/b%20c.mp4", with runs of '/' written once.
 * Every byte outside the unreserved set and the separators is percent-encoded, so the page sees exactly the path given
 * (R-TAP-3). Returns the number of bytes written without the terminator, or 0 with out set to "" when the path is not
 * usable or the buffer is too small. */
size_t tub_path_to_file_url_as(const char *path, enum tub_path_style style, char *out, size_t out_size);

/* The same, for the paths of the system the plugin runs on */
bool tub_path_is_usable(const char *path);
size_t tub_path_to_file_url(const char *path, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
