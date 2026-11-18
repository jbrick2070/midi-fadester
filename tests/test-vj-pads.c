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

static uint8_t led_velocity(const struct mfd_vj_pads *p, uint8_t note)
{
	struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
	size_t c = mfd_vj_pads_leds(p, leds, MFD_VJ_MAX_LEDS);
	for (size_t i = 0; i < c; ++i) {
		if (leds[i].channel == 0U && leds[i].note == note) {
			return leds[i].velocity;
		}
	}
	return 255U;
}

int main(void)
{
	struct mfd_vj_pads p;
	mfd_vj_pads_init(&p);
	for (int i = 0; i < 8; ++i) {
		mfd_vj_pads_set_has_layer(&p, i, i < 3);
	}

	/* Not a VJ pad: Solo, Track Select, upper rows and other channels are left alone. */
	REQUIRE(!feed(&p, true, 0, 32)); /* top clip row */
	REQUIRE(!feed(&p, true, 0, 24)); /* fourth row */
	REQUIRE(!feed(&p, true, 1, 3));  /* bottom-row note on another channel */
	REQUIRE(!feed(&p, true, 0, 49)); /* Solo */
	REQUIRE(!feed(&p, true, 1, 51)); /* Track Select */
	REQUIRE(mfd_vj_pads_is_button(0, 23) && mfd_vj_pads_is_button(0, 98) && mfd_vj_pads_is_button(0, 81));

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
	REQUIRE(led_velocity(&p, 16) == 23U);
	mfd_vj_pads_set_faded(&p, 0, true);
	REQUIRE(led_velocity(&p, 16) == 5U);

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
	REQUIRE(mfd_vj_pads_leds(&p, leds, MFD_VJ_MAX_LEDS) == 24U);

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

	printf("test-vj-pads: all checks passed\n");
	return 0;
}
