/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

/* User-visible text by locale key (R-I18N-1, R-I18N-3). Shared code asks for its texts here, so the plugin shows them
 * in the language OBS runs in, while the ctest programs, which load no locale, get the English given. Only text a user
 * reads goes through here; log lines and the obs-websocket API stay English. Pure C17 with no OBS headers (R-TST-1).
 * The lookup is set in obs_module_load before any consumer starts and cleared in obs_module_unload after they stop;
 * texts are looked up on use, on the Qt thread, never on the video tick or a websocket thread (R-THR-1). */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The locale's text for key, or NULL when it has none. The plugin passes a wrapper of obs_module_get_string. */
typedef const char *(*mfd_text_lookup_fn)(const char *key);

/* NULL goes back to English. */
void mfd_text_set_lookup(mfd_text_lookup_fn lookup);

/* The text for key in the current locale, else english (also for an empty translation). */
const char *mfd_text(const char *key, const char *english);

/* mfd_text when translate is true, else english: one table serves the UI (translated) and the API (English). */
const char *mfd_text_or_english(bool translate, const char *key, const char *english);

/* Writes text to out with "%1" to "%9" replaced by values[0] to values[8], in one pass: a value is never scanned
 * again, so a name holding "%2" stays as it is. A placeholder with no value (its number past count, or a NULL value)
 * is left out, and any other "%" stays, so a translation can never make this read past the values; it is never a
 * printf format (R-I18N-3). The result is cut to out_size - 1 bytes at a UTF-8 character boundary and always ends in
 * NUL. Returns the length written. */
size_t mfd_text_fill(char *out, size_t out_size, const char *text, const char *const *values, size_t count);

#ifdef __cplusplus
}
#endif
