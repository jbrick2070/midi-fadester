/*
 * Midi Fadester test fixture
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* A call as clang-format may wrap it: key and English on their own lines, the English in two literals. */
/* the layout is what this fixture tests, so clang-format keeps out */
/* clang-format off */
const char *fixture_multi_line(bool translate)
{
	return translate ? mfdText(
				   "PluginDisplayName",
				   "Midi "
				   "Fadester")
			 : mfd_text_or_english(translate, "PluginDisplayName", "Midi \x46" "adester");
}
/* clang-format on */
