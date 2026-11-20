/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Blackout: Program fades to black and back (Session on the APC40 mkII, Blackout on the Fadester Desk dock). It needs
 * nothing in the user's scenes: one private black colour source (never in a scene, never saved) is drawn on an OBS output
 * channel above the main one, so the main video and everything made from it (stream, recording, the Program view, the
 * virtual camera in Program mode) go black while Preview does not. Its opacity is animated from the video tick.
 *
 * Threads: toggle / is_on / level from any thread (one small lock, never held while calling OBS); tick from the video
 * tick with no engine lock held; ui and reset only on the UI thread with no engine lock held (they create, attach,
 * detach and release the source).
 */

/* Why the last blackout that was asked for could not go on air (F-13). The dock shows it; a new press clears it. */
enum mfd_blackout_failure {
	MFD_BLACKOUT_OK = 0,
	MFD_BLACKOUT_NO_COLOR_SOURCE, /* OBS has no colour source to make the black from */
	MFD_BLACKOUT_NO_CHANNEL,      /* every output channel above the scene is held by other plugins */
};
enum mfd_blackout_failure mfd_blackout_last_failure(void);

/* The fade itself, apart from OBS (tested, H-03). The speed of a fade of fade_ms (never faster than 300 ms), in
 * level per second; and the level one step later, on its way to black (1) or to clear (0), never past it. */
float mfd_blackout_rate_for_ms(int fade_ms);
float mfd_blackout_next_level(float level, bool to_black, float rate_per_sec, float seconds);

void mfd_blackout_init(void);
void mfd_blackout_shutdown(void); /* UI thread, module unload */

/* Press: fade to black, or back from it (from wherever the fade is, it turns round smoothly). fade_ms below 300 is
 * raised to 300. */
void mfd_blackout_toggle(int fade_ms);

/* Black or on the way to or from it (the Session lamp blinks, the dock shows its line). */
bool mfd_blackout_is_on(void);

/* 0 = clear, 1 = fully black. */
float mfd_blackout_level(void);

/* Video tick, no engine lock held: moves the opacity on and writes it to the source. */
void mfd_blackout_tick(float seconds);

/* UI thread, no engine lock held: creates and attaches the source when a blackout is asked for, detaches and releases it
 * once clear again, and re-attaches it when OBS or another plugin cleared its channel. */
void mfd_blackout_ui(void);

/* UI thread: back to clear at once and the source gone (scene collection changing, OBS exiting). */
void mfd_blackout_reset(void);

#ifdef __cplusplus
}
#endif
