/*
 * Midi Fadester test fixture
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

const char *fixture_runtime_key(const char *key_from_somewhere)
{
	return mfd_text(key_from_somewhere, "A text whose key the check cannot see");
}
