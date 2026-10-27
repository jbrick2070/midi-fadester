/*
 * Midi Fadester test fixture
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

const char *fixture_missing_locale_key(void)
{
	return obs_module_text("LocaleKeyThatIsNotInTheLocaleFile");
}
