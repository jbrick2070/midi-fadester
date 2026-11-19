/*
 * Midi Fadester — test-vj-pads
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/vj-pads.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static struct mfd_vj_action acts[MFD_VJ_MAX_ACTIONS];
static size_t n;

static bool feed(struct mfd_vj_pads *p, bool on, uint8_t ch, uint8_t note)
{
	return mfd_vj_pads_feed(p, on, ch, note, acts, MFD_VJ_MAX_ACTIONS, &n);
}

/* The LED for a note (and, for Clip Stop, its track channel): its velocity, and its channel in *ch. */
static uint8_t led_at(const struct mfd_vj_pads *p, uint8_t note, int track, uint8_t *ch)
{
	struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
	size_t c = mfd_vj_pads_leds(p, leds, MFD_VJ_MAX_LEDS);
	for (size_t i = 0; i < c; ++i) {
		if (leds[i].note == note && (track < 0 || leds[i].channel == (uint8_t)track)) {
			if (ch) {
				*ch = leds[i].channel;
			}
			return leds[i].velocity;
		}
	}
	return 255U;
}

static uint8_t led_velocity(const struct mfd_vj_pads *p, uint8_t note)
{
	uint8_t ch = 0;
	uint8_t v = led_at(p, note, -1, &ch);
	return ch == 0U ? v : 254U; /* an animated pad is not a plain colour */
}

int main(void)
{
	struct mfd_vj_pads p;
	mfd_vj_pads_init(&p);
	for (int i = 0; i < 8; ++i) {
		mfd_vj_pads_set_has_layer(&p, i, i < 3);
	}

	/* Not a VJ pad: Solo, Track Select, Activator, A|B, a ninth Clip Stop and other channels are left alone. */
	REQUIRE(!feed(&p, true, 1, 3));  /* bottom-row note on another channel */
	REQUIRE(!feed(&p, true, 0, 49)); /* Solo */
	REQUIRE(!feed(&p, true, 1, 51)); /* Track Select */
	REQUIRE(!feed(&p, true, 0, 50)); /* Activator */
	REQUIRE(!feed(&p, true, 0, 66)); /* Crossfade A|B */
	REQUIRE(!feed(&p, true, 8, 52)); /* Clip Stop on a ninth channel */
	REQUIRE(!feed(&p, true, 0, 87)); /* Pan */
	REQUIRE(mfd_vj_pads_is_button(0, 23) && mfd_vj_pads_is_button(0, 98) && mfd_vj_pads_is_button(0, 81));
	REQUIRE(mfd_vj_pads_is_button(0, 39) && mfd_vj_pads_is_button(7, 52) && mfd_vj_pads_is_button(0, 86));
	REQUIRE(!mfd_vj_pads_is_button(0, 40) && !mfd_vj_pads_is_button(0, 87) && !mfd_vj_pads_is_button(1, 82));

	/* 1. Blend punch: Normal -> Screen (the first punch mode) -> Normal. Release does nothing. */
	REQUIRE(feed(&p, true, 0, 1) && n == 1);
	REQUIRE(acts[0].kind == MFD_VJ_ACTION_SET_BLEND && acts[0].strip == 1 && acts[0].blend == MFD_VJ_BLEND_SCREEN);
	REQUIRE(feed(&p, false, 0, 1) && n == 0);
	REQUIRE(led_velocity(&p, 1) == mfd_vj_blend_color(MFD_VJ_BLEND_SCREEN));
	REQUIRE(feed(&p, true, 0, 1) && n == 1 && acts[0].blend == MFD_VJ_BLEND_NORMAL);
	REQUIRE(led_velocity(&p, 1) == mfd_vj_blend_color(MFD_VJ_BLEND_NORMAL));

	/* A strip without a layer: the pad is taken (consumed) but nothing happens and its LED stays dark. */
	REQUIRE(feed(&p, true, 0, 6) && n == 0 && p.current[6] == MFD_VJ_BLEND_NORMAL);
	REQUIRE(led_velocity(&p, 6) == 0U && led_velocity(&p, 14) == 0U && led_velocity(&p, 22) == 0U);

	/* 2. Shift + pad: next punch mode. A strip at Normal stays at Normal on air; the LED shows the punch mode. */
	REQUIRE(feed(&p, true, 0, 98) && n == 0);
	REQUIRE(led_velocity(&p, 1) == mfd_vj_blend_color(MFD_VJ_BLEND_SCREEN));
	REQUIRE(feed(&p, true, 0, 1) && n == 0 && p.punch[1] == MFD_VJ_BLEND_ADDITIVE);
	REQUIRE(led_velocity(&p, 1) == mfd_vj_blend_color(MFD_VJ_BLEND_ADDITIVE));
	REQUIRE(feed(&p, false, 0, 98));
	REQUIRE(led_velocity(&p, 1) == mfd_vj_blend_color(MFD_VJ_BLEND_NORMAL));
	REQUIRE(feed(&p, true, 0, 1) && acts[0].blend == MFD_VJ_BLEND_ADDITIVE);

	/* A strip blending switches live from one punch mode to the next; the cycle skips Normal and Darken. */
	REQUIRE(feed(&p, true, 0, 98));
	REQUIRE(feed(&p, true, 0, 1) && n == 1 && acts[0].blend == MFD_VJ_BLEND_MULTIPLY);
	REQUIRE(feed(&p, true, 0, 1) && acts[0].blend == MFD_VJ_BLEND_SUBTRACT);
	REQUIRE(feed(&p, true, 0, 1) && acts[0].blend == MFD_VJ_BLEND_LIGHTEN);
	REQUIRE(feed(&p, false, 0, 98));

	/* 3. Flash (row 2): momentary, never repeated while held; release turns it off. */
	REQUIRE(feed(&p, true, 0, 10) && n == 1 && acts[0].kind == MFD_VJ_ACTION_FLASH_ON && acts[0].strip == 2);
	REQUIRE(feed(&p, true, 0, 10) && n == 0);
	REQUIRE(led_velocity(&p, 10) == 3U);
	REQUIRE(feed(&p, false, 0, 10) && n == 1 && acts[0].kind == MFD_VJ_ACTION_FLASH_OFF);
	REQUIRE(feed(&p, false, 0, 10) && n == 0);
	REQUIRE(led_velocity(&p, 10) == 1U);

	/* 4. Fade (row 3): a toggle on press only; the engine reports the faded state for the LED. */
	REQUIRE(feed(&p, true, 0, 16) && n == 1 && acts[0].kind == MFD_VJ_ACTION_FADE_TOGGLE && acts[0].strip == 0);
	REQUIRE(feed(&p, false, 0, 16) && n == 0);
	REQUIRE(led_velocity(&p, 16) == 1U); /* dim white: a source at rest */
	mfd_vj_pads_set_faded(&p, 0, true);
	REQUIRE(led_velocity(&p, 16) == 9U); /* amber: faded out */

	/* 5. Stop All Clips: Clean (FX off, every blend Normal); with Shift held, fade every layer out. */
	REQUIRE(feed(&p, true, 0, 0) && acts[0].blend == MFD_VJ_BLEND_SCREEN);
	REQUIRE(feed(&p, true, 0, 81) && n == 1 && acts[0].kind == MFD_VJ_ACTION_CLEAN);
	REQUIRE(p.current[0] == MFD_VJ_BLEND_NORMAL && p.current[1] == MFD_VJ_BLEND_NORMAL);
	REQUIRE(feed(&p, true, 0, 98) && n == 0);
	REQUIRE(feed(&p, true, 0, 81) && n == 1 && acts[0].kind == MFD_VJ_ACTION_FADE_ALL_OUT);
	REQUIRE(feed(&p, false, 0, 98));
	REQUIRE(feed(&p, true, 0, 81) && acts[0].kind == MFD_VJ_ACTION_CLEAN);

	/* 6. Disconnect: a held flash is let go, Shift forgotten. */
	REQUIRE(feed(&p, true, 0, 8) && acts[0].kind == MFD_VJ_ACTION_FLASH_ON);
	REQUIRE(feed(&p, true, 0, 98));
	n = mfd_vj_pads_release_all(&p, acts, MFD_VJ_MAX_ACTIONS);
	REQUIRE(n == 1 && acts[0].kind == MFD_VJ_ACTION_FLASH_OFF && acts[0].strip == 0);
	REQUIRE(p.flashing == 0 && !p.shift);

	/* 7. LEDs: the OBS state wins over the requested one; Darken from OBS is shown though never offered. */
	mfd_vj_pads_set_current(&p, 2, MFD_VJ_BLEND_DARKEN);
	REQUIRE(led_velocity(&p, 2) == mfd_vj_blend_color(MFD_VJ_BLEND_DARKEN));
	struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
	REQUIRE(mfd_vj_pads_leds(&p, leds, MFD_VJ_MAX_LEDS) ==
		55U); /* 40 pads, 8 Clip Stops, 5 scenes, Record, Session */

	/* 8. Saved punch modes: Normal and Darken are refused. */
	mfd_vj_pads_set_punch(&p, 4, MFD_VJ_BLEND_MULTIPLY);
	mfd_vj_pads_set_punch(&p, 4, MFD_VJ_BLEND_DARKEN);
	mfd_vj_pads_set_punch(&p, 4, MFD_VJ_BLEND_NORMAL);
	REQUIRE(p.punch[4] == MFD_VJ_BLEND_MULTIPLY);
	REQUIRE(!mfd_vj_blend_is_punch(MFD_VJ_BLEND_DARKEN) && mfd_vj_blend_is_punch(MFD_VJ_BLEND_SUBTRACT));

	/* 9. Names and ids. */
	enum mfd_vj_blend b;
	REQUIRE(strcmp(mfd_vj_blend_name(MFD_VJ_BLEND_SUBTRACT), "Subtract") == 0);
	REQUIRE(mfd_vj_blend_from_id("lighten", &b) && b == MFD_VJ_BLEND_LIGHTEN);
	REQUIRE(!mfd_vj_blend_from_id("overlay", &b));
	REQUIRE(strcmp(mfd_vj_blend_id(MFD_VJ_BLEND_DARKEN), "darken") == 0);

	/* 10. Transport: Tempo detents (relative; 64..127 is negative, 0 no turn), Shift x5, Nudge +-5, Tap Tempo, Play. */
	struct mfd_vj_pads t;
	mfd_vj_pads_init(&t);
	REQUIRE(!mfd_vj_pads_feed_cc(&t, 0, 14, 1, acts, MFD_VJ_MAX_ACTIONS, &n)); /* master fader */
	REQUIRE(!mfd_vj_pads_feed_cc(&t, 1, 13, 1, acts, MFD_VJ_MAX_ACTIONS, &n)); /* another channel */
	REQUIRE(mfd_vj_pads_feed_cc(&t, 0, 13, 1, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1);
	REQUIRE(acts[0].kind == MFD_VJ_ACTION_SPEED_STEPS && acts[0].steps == 1);
	REQUIRE(mfd_vj_pads_feed_cc(&t, 0, 13, 127, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1 && acts[0].steps == -1);
	REQUIRE(mfd_vj_pads_feed_cc(&t, 0, 13, 64, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1 && acts[0].steps == -64);
	REQUIRE(mfd_vj_pads_feed_cc(&t, 0, 13, 63, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1 && acts[0].steps == 63);
	REQUIRE(mfd_vj_pads_feed_cc(&t, 0, 13, 0, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 0);
	REQUIRE(feed(&t, true, 0, 98) && n == 0);
	REQUIRE(mfd_vj_pads_feed_cc(&t, 0, 13, 2, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1 && acts[0].steps == 10);
	REQUIRE(feed(&t, false, 0, 98) && !t.shift);
	REQUIRE(feed(&t, true, 0, 101) && n == 1 && acts[0].kind == MFD_VJ_ACTION_SPEED_STEPS && acts[0].steps == 5);
	REQUIRE(feed(&t, false, 0, 101) && n == 0);
	REQUIRE(feed(&t, true, 0, 100) && n == 1 && acts[0].steps == -5);
	REQUIRE(feed(&t, true, 0, 99) && n == 1 && acts[0].kind == MFD_VJ_ACTION_SPEED_RESET);
	REQUIRE(feed(&t, true, 0, 91) && n == 1 && acts[0].kind == MFD_VJ_ACTION_PLAY_PAUSE);
	REQUIRE(feed(&t, false, 0, 91) && n == 0);
	REQUIRE(!feed(&t, true, 1, 91) && !feed(&t, true, 0, 92)); /* Play on another channel; Stop */

	/* 11. Rows 4 and 5, Clip Stop, blackout and snap. */
	struct mfd_vj_pads q;
	mfd_vj_pads_init(&q);
	mfd_vj_pads_set_has_layer(&q, 0, true);
	mfd_vj_pads_set_has_layer(&q, 1, true);
	REQUIRE(feed(&q, true, 0, 24) && n == 1 && acts[0].kind == MFD_VJ_ACTION_SHOW_TOGGLE && acts[0].strip == 0);
	REQUIRE(feed(&q, false, 0, 24) && n == 0);
	REQUIRE(feed(&q, true, 0, 26) && n == 0); /* no source on strip 3: nothing to show or hide */
	REQUIRE(feed(&q, true, 0, 33) && n == 1 && acts[0].kind == MFD_VJ_ACTION_RESTART && acts[0].strip == 1);
	REQUIRE(feed(&q, true, 3, 52) && n == 0); /* Clip Stop on a strip without a source */
	REQUIRE(feed(&q, true, 1, 52) && n == 1 && acts[0].kind == MFD_VJ_ACTION_COLUMN_CLEAN && acts[0].strip == 1);
	REQUIRE(feed(&q, true, 0, 98));
	REQUIRE(feed(&q, true, 0, 25) && n == 1 && acts[0].kind == MFD_VJ_ACTION_WAKE && acts[0].strip == 1);
	REQUIRE(feed(&q, true, 0, 37) && n == 1 && acts[0].kind == MFD_VJ_ACTION_PIN_TOGGLE && acts[0].strip == 5);
	REQUIRE(feed(&q, true, 0, 17) && n == 1 && acts[0].kind == MFD_VJ_ACTION_SNAP && acts[0].strip == 1);
	REQUIRE(feed(&q, true, 0, 9) && n == 1 && acts[0].kind == MFD_VJ_ACTION_BLACKOUT_ON && acts[0].strip == 1);
	REQUIRE(led_velocity(&q, 9) == 9U);     /* amber while blacked out */
	REQUIRE(led_at(&q, 52, 1, NULL) == 1U); /* Clip Stop lit while the column holds a blackout */
	REQUIRE(feed(&q, false, 0, 98));
	REQUIRE(feed(&q, true, 0, 9) && n == 0); /* already held: no flash on top of the blackout */
	REQUIRE(feed(&q, false, 0, 9) && n == 1 && acts[0].kind == MFD_VJ_ACTION_BLACKOUT_OFF);
	REQUIRE(led_at(&q, 52, 1, NULL) == 0U && led_velocity(&q, 9) == 1U);
	REQUIRE(feed(&q, true, 0, 98) && feed(&q, true, 0, 8) && acts[0].kind == MFD_VJ_ACTION_BLACKOUT_ON);
	n = mfd_vj_pads_release_all(&q, acts, MFD_VJ_MAX_ACTIONS);
	REQUIRE(n == 1 && acts[0].kind == MFD_VJ_ACTION_BLACKOUT_OFF && q.blackout == 0 && !q.shift);

	/* 12. Row 4 and row 5 lights: hidden blinks red (channel 16), 0 % is amber, a pinned column is azure. */
	uint8_t ch = 0;
	REQUIRE(led_velocity(&q, 24) == 1U && led_velocity(&q, 32) == 1U);
	mfd_vj_pads_set_hidden(&q, 0, true);
	REQUIRE(led_at(&q, 24, -1, &ch) == 5U && ch == 15U);
	mfd_vj_pads_set_hidden(&q, 0, false);
	mfd_vj_pads_set_zero(&q, 0, true);
	REQUIRE(led_velocity(&q, 24) == 9U);
	mfd_vj_pads_set_pinned(&q, 0, true);
	REQUIRE(led_velocity(&q, 32) == 37U);
	REQUIRE(led_velocity(&q, 26) == 0U && led_velocity(&q, 34) == 0U); /* no source: dark */

	/* 13. Scenes: Scene Launch picks (Shift: straight to Program); Session takes. Lights: Program red, Preview green
	 * pulsing (channel 11), idle dim blue, no scene off. */
	REQUIRE(feed(&q, true, 0, 82) && n == 1 && acts[0].kind == MFD_VJ_ACTION_SCENE_PICK && acts[0].strip == 0);
	REQUIRE(feed(&q, false, 0, 82) && n == 0);
	REQUIRE(feed(&q, true, 0, 86) && n == 1 && acts[0].strip == 4);
	REQUIRE(feed(&q, true, 0, 102) && n == 1 && acts[0].kind == MFD_VJ_ACTION_TAKE);
	REQUIRE(feed(&q, true, 0, 98));
	REQUIRE(feed(&q, true, 0, 83) && n == 1 && acts[0].kind == MFD_VJ_ACTION_SCENE_PROGRAM && acts[0].strip == 1);
	REQUIRE(feed(&q, true, 0, 102) && n == 1 && acts[0].kind == MFD_VJ_ACTION_TAKE);
	REQUIRE(feed(&q, false, 0, 98));
	mfd_vj_pads_set_scene(&q, 0, MFD_VJ_SCENE_PROGRAM);
	mfd_vj_pads_set_scene(&q, 1, MFD_VJ_SCENE_PREVIEW);
	mfd_vj_pads_set_scene(&q, 2, MFD_VJ_SCENE_IDLE);
	REQUIRE(led_velocity(&q, 82) == 5U);
	REQUIRE(led_at(&q, 83, -1, &ch) == 21U && ch == 10U);
	REQUIRE(led_velocity(&q, 84) == 46U && led_velocity(&q, 85) == 0U);
	q.take_ready = true;
	q.vs_panic = true;
	REQUIRE(led_at(&q, 102, 0, NULL) == 1U && led_at(&q, 93, 0, NULL) == 1U); /* Record: on or off only */
	struct mfd_vj_led ql[MFD_VJ_MAX_LEDS];
	size_t qc = mfd_vj_pads_leds(&q, ql, MFD_VJ_MAX_LEDS);
	for (size_t i = 0; i < qc; ++i) {
		bool rgb = ql[i].note < 40U || (ql[i].note >= 82U && ql[i].note <= 86U);
		REQUIRE(ql[i].rgb == rgb);
	}

	/* 13b. Animated lights start from a dim colour, so they stay lit if the desk does not animate. */
	{
		struct mfd_vj_led al[MFD_VJ_MAX_LEDS];
		size_t ac = mfd_vj_pads_leds(&q, al, MFD_VJ_MAX_LEDS);
		bool seen_preview = false;
		for (size_t i = 0; i < ac; ++i) {
			if (al[i].note == 83U) {
				REQUIRE(al[i].channel == 10U && al[i].velocity == 21U && al[i].primary == 22U);
				seen_preview = true;
			} else if (al[i].channel == 0U) {
				REQUIRE(al[i].primary == 0U);
			}
		}
		REQUIRE(seen_preview);
		mfd_vj_pads_set_hidden(&q, 1, true);
		ac = mfd_vj_pads_leds(&q, al, MFD_VJ_MAX_LEDS);
		for (size_t i = 0; i < ac; ++i) {
			if (al[i].note == 25U) {
				REQUIRE(al[i].channel == 15U && al[i].velocity == 5U && al[i].primary == 6U);
			}
		}
		mfd_vj_pads_set_hidden(&q, 1, false);
	}

	/* 13c. A source whose own blend in the scene is Screen: the pad punches it to another mode and back to Screen
	 * (never to Normal, which would make a shape on black a black wall); Clean and the lights follow its home. */
	{
		struct mfd_vj_pads h;
		mfd_vj_pads_init(&h);
		mfd_vj_pads_set_has_layer(&h, 0, true);
		mfd_vj_pads_set_home(&h, 0, MFD_VJ_BLEND_SCREEN);
		mfd_vj_pads_set_current(&h, 0, MFD_VJ_BLEND_SCREEN);
		REQUIRE(led_velocity(&h, 0) == 1U &&
			led_at(&h, 52, 0, NULL) == 0U); /* at home: dim white, Clip Stop dark */
		REQUIRE(feed(&h, true, 0, 0) && n == 1 &&
			acts[0].blend == MFD_VJ_BLEND_ADDITIVE); /* Screen is home: next */
		REQUIRE(led_velocity(&h, 0) == mfd_vj_blend_color(MFD_VJ_BLEND_ADDITIVE) &&
			led_at(&h, 52, 0, NULL) == 1U);
		REQUIRE(feed(&h, true, 0, 0) && n == 1 && acts[0].blend == MFD_VJ_BLEND_SCREEN);
		REQUIRE(feed(&h, true, 0, 98));
		REQUIRE(feed(&h, true, 0, 0) && n == 0 && h.punch[0] == MFD_VJ_BLEND_ADDITIVE); /* Screen skipped */
		REQUIRE(feed(&h, true, 0, 0) && n == 0 && h.punch[0] == MFD_VJ_BLEND_MULTIPLY);
		REQUIRE(feed(&h, false, 0, 98));
		REQUIRE(feed(&h, true, 0, 0) && acts[0].blend == MFD_VJ_BLEND_MULTIPLY);
		/* Punched (Multiply) with Shift: straight on to the next mode on air, Subtract. */
		REQUIRE(feed(&h, true, 0, 98));
		REQUIRE(feed(&h, true, 0, 0) && n == 1 && acts[0].blend == MFD_VJ_BLEND_SUBTRACT);
		/* From Subtract the cycle wraps to Lighten (Screen, the home, is skipped on the way). */
		REQUIRE(feed(&h, true, 0, 0) && n == 1 && acts[0].blend == MFD_VJ_BLEND_LIGHTEN);
		REQUIRE(feed(&h, true, 0, 0) && n == 1 && acts[0].blend == MFD_VJ_BLEND_ADDITIVE);
		REQUIRE(feed(&h, false, 0, 98));
		REQUIRE(feed(&h, true, 0, 81) && n == 1 && acts[0].kind == MFD_VJ_ACTION_CLEAN &&
			h.current[0] == MFD_VJ_BLEND_SCREEN);
	}

	/* 14. Visual Sidechain: Record panics, Metronome taps (Shift: lock), Shift + Tap Tempo resyncs. Cue Level: the
	 * transition length, with Shift the pad fade time. */
	REQUIRE(feed(&q, true, 0, 93) && n == 1 && acts[0].kind == MFD_VJ_ACTION_VS_PANIC);
	REQUIRE(feed(&q, true, 0, 90) && n == 1 && acts[0].kind == MFD_VJ_ACTION_VS_TAP);
	REQUIRE(feed(&q, true, 0, 98));
	REQUIRE(feed(&q, true, 0, 90) && n == 1 && acts[0].kind == MFD_VJ_ACTION_VS_LOCK);
	REQUIRE(feed(&q, true, 0, 99) && n == 1 && acts[0].kind == MFD_VJ_ACTION_VS_RESYNC);
	REQUIRE(mfd_vj_pads_feed_cc(&q, 0, 47, 3, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1);
	REQUIRE(acts[0].kind == MFD_VJ_ACTION_FADE_STEPS && acts[0].steps == 3);
	REQUIRE(feed(&q, false, 0, 98));
	REQUIRE(mfd_vj_pads_feed_cc(&q, 0, 47, 126, acts, MFD_VJ_MAX_ACTIONS, &n) && n == 1);
	REQUIRE(acts[0].kind == MFD_VJ_ACTION_CUE_STEPS && acts[0].steps == -2);
	REQUIRE(!mfd_vj_pads_feed_cc(&q, 1, 47, 1, acts, MFD_VJ_MAX_ACTIONS, &n));

	printf("test-vj-pads: all checks passed\n");
	return 0;
}
