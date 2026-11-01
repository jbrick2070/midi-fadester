/*
 * Midi Fadester test fixture
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* A wrapped call whose English differs from en-US.ini: the check must read it and fail. */
/* the layout is what this fixture tests, so clang-format keeps out */
/* clang-format off */
const char *fixture_multi_line_changed(bool translate)
{
	return translate ? mfdText(
				   "PluginDisplayName",
				   "Midi "
				   "Fadestr")
			 : mfd_text_or_english(translate, "PluginDisplayName", "Midi \x46" "adester");
}
/* clang-format on */
