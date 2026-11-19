/*
 * Visual Sidechain - printf signature check unit tests (R-I18N-2)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/text-format.h"
#include "obs/text.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_equal_signatures(void)
{
	assert(vs_format_signature_equal("", ""));
	assert(vs_format_signature_equal("plain text", "texte simple"));
	assert(vs_format_signature_equal("Shader file not found: %s", "Shader-Datei nicht gefunden: %s"));
	assert(vs_format_signature_equal("%s%s -> %s / %s | %.4g to %.4g | Amount %.0f%% | Depth: %s",
					 "%s%s -> %s / %s | %.4g bis %.4g | Menge %.0f %% | Tiefe: %s"));
	assert(vs_format_signature_equal("100%% wet", "100 %% nass"));
	assert(vs_format_signature_equal("%d and %u, %.12f", "%d und %u, %.12f"));
	printf("  test_equal_signatures: PASS\n");
}

static void test_different_signatures(void)
{
	assert(!vs_format_signature_equal("%s", "%d"));
	assert(!vs_format_signature_equal("%.4g", "%.3g"));
	/* "%%" prints a percent sign and takes no argument, so a translation may drop or add one */
	assert(vs_format_signature_equal("%.0f%%", "%.0f"));
	assert(!vs_format_signature_equal("%s %s", "%s"));
	assert(!vs_format_signature_equal("%s", "%s %s"));
	assert(!vs_format_signature_equal("no conversion", "%s"));
	assert(!vs_format_signature_equal("%s: %.0f", "%.0f: %s"));
	assert(!vs_format_signature_equal("%5s", "%s"));
	assert(!vs_format_signature_equal("%ls", "%s"));
	assert(!vs_format_signature_equal("%*d", "%d"));
	/* a translation can never slip in a write (%n) or a positional argument, whatever its spacing */
	assert(!vs_format_signature_equal("%s", "%n"));
	assert(!vs_format_signature_equal("%s done", "% n%s"));
	assert(!vs_format_signature_equal("%s", "%1$s"));
	assert(!vs_format_signature_equal("%s %s", "%2$s %1$s"));
	/* "% to" reads as a conversion, so it can only pass where the English has exactly that */
	assert(!vs_format_signature_equal("%s", "%s from 0% to 100"));
	printf("  test_different_signatures: PASS\n");
}

static void test_invalid_strings(void)
{
	assert(!vs_format_signature_equal(NULL, ""));
	assert(!vs_format_signature_equal("", NULL));
	assert(!vs_format_signature_equal(NULL, NULL));
	/* an incomplete or unknown conversion is never safe, even when both sides have it */
	assert(!vs_format_signature_equal("50%", "50%"));
	assert(!vs_format_signature_equal("100% wet", "100% wet"));
	assert(!vs_format_signature_equal("%.4", "%.4"));
	assert(!vs_format_signature_equal("%y", "%y"));
	/* forms no English text uses are refused even against themselves: flags, widths, "*", length modifiers, %n */
	assert(!vs_format_signature_equal("%*d", "%*d"));
	assert(!vs_format_signature_equal("%5s", "%5s"));
	assert(!vs_format_signature_equal("%-s", "%-s"));
	assert(!vs_format_signature_equal("%'d", "%'d"));
	assert(!vs_format_signature_equal("%ld", "%ld"));
	assert(!vs_format_signature_equal("%n", "%n"));
	assert(!vs_format_signature_equal("%.s", "%.s"));
	assert(!vs_format_signature_equal("%.4s", "%.4s"));
	/* "%%" is skipped whole: "%.0%%f" is a broken "%.0" followed by "%f", not "%.0f" */
	assert(!vs_format_signature_equal("%.0f%%", "%.0%%f"));
	/* a stray % in a translation of a text that has none */
	assert(!vs_format_signature_equal("Effect mix", "Mix 50% Effekt"));
	printf("  test_invalid_strings: PASS\n");
}

/* obs_module_text for vs_text_format: a table of translations; a key it lacks comes back unchanged, as in OBS */
static const char *const translations[][2] = {
	{"Good", "Gut: %s bei %.0f %%"},
	{"Swapped", "%.0f: %s"},
	{"Stray", "%s zu 50% fertig"},
	{"Write", "%s%n"},
};

const char *obs_module_text(const char *key)
{
	for (size_t i = 0; i < sizeof(translations) / sizeof(translations[0]); i++) {
		if (strcmp(translations[i][0], key) == 0)
			return translations[i][1];
	}
	return key;
}

static void test_text_format_fallback(void)
{
	assert(strcmp(vs_text_format("Good", "Good: %s at %.0f%%"), "Gut: %s bei %.0f %%") == 0);
	assert(strcmp(vs_text_format("Swapped", "%s: %.0f"), "%s: %.0f") == 0);
	assert(strcmp(vs_text_format("Stray", "%s is 50%% done"), "%s is 50%% done") == 0);
	assert(strcmp(vs_text_format("Write", "%s"), "%s") == 0);
	/* a key no file has: OBS gives back the key, which has no conversions */
	assert(strcmp(vs_text_format("Missing.Key", "Missing: %s"), "Missing: %s") == 0);
	printf("  test_text_format_fallback: PASS\n");
}

int main(void)
{
	printf("test-text-format:\n");
	test_equal_signatures();
	test_different_signatures();
	test_invalid_strings();
	test_text_format_fallback();
	printf("test-text-format: all passed\n");
	return 0;
}
