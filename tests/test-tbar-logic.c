/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/tbar-logic.h"
#include <stdio.h>

#define ASSERT_TRUE(cond)                                                               \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	struct mfd_tbar_action act;

	/*
	 * 1. Studio Mode OFF guard:
	 * Must never call obs_frontend_get_tbar_position / set_tbar_position / release_tbar
	 * when Studio Mode is inactive (OBSBasic::tBar is uninitialized or dangling).
	 */
	mfd_tbar_compute_action(false, false, 0.5f, 0, &act);
	ASSERT_TRUE(!act.allow_frontend_calls);
	ASSERT_TRUE(!act.report_missing_slider_error);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 2. Studio Mode ON, but Qt "slider-tbar" missing (future OBS version breakage check):
	 * Must flag report_missing_slider_error = true and block unsafe calls.
	 */
	mfd_tbar_compute_action(true, false, 0.5f, 0, &act);
	ASSERT_TRUE(!act.allow_frontend_calls);
	ASSERT_TRUE(act.report_missing_slider_error);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 3. Studio Mode ON and "slider-tbar" found: mid-throw at t = 0.5 -> pos = 512
	 */
	mfd_tbar_compute_action(true, true, 0.5f, 0, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(!act.report_missing_slider_error);
	ASSERT_TRUE(act.target_pos_1024 == 512);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 4. Just below 90% completion threshold (t = 0.89 -> pos = 910 < 921) does not release T-bar
	 */
	ASSERT_TRUE(MFD_TBAR_COMPLETE_POS == 921);
	mfd_tbar_compute_action(true, true, 0.89f, 512, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 910);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 5. Reaching 90% completion threshold (t = 0.90 -> pos = 921 from 910) triggers release_tbar once
	 */
	mfd_tbar_compute_action(true, true, 0.90f, 910, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 921);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);

	/*
	 * 6. Further travel in the completion zone (pos = 972 or 1023 when last_pos_1024 >= 921)
	 * neither writes the slider nor re-fires release_tbar
	 */
	mfd_tbar_compute_action(true, true, 0.95f, 921, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	mfd_tbar_compute_action(true, true, 1.0f, 1023, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 6. Amber Studio Mode note banner visibility (Item 3):
	 * Shown only when Studio Mode is OFF and at least one T-bar slot is enabled; hidden when Studio Mode is ON.
	 */
	ASSERT_TRUE(mfd_should_show_studio_mode_note(false, 1U));
	ASSERT_TRUE(!mfd_should_show_studio_mode_note(true, 1U));
	ASSERT_TRUE(!mfd_should_show_studio_mode_note(false, 0U));

	printf("test-tbar-logic: PASS\n");
	return 0;
}
