/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/vj-pads.h"

#include <string.h>

/* APC40 mkII RGB palette velocities. */
#define COLOR_OFF 0U
#define COLOR_WHITE_DIM 1U
#define COLOR_WHITE 3U
#define COLOR_RED 5U
#define COLOR_GREEN_DIM 23U

static const uint8_t k_blend_color[MFD_VJ_BLEND_COUNT] = {
	3U,  /* Normal: white */
	37U, /* Lighten: cyan */
	13U, /* Screen: yellow */
	9U,  /* Additive: orange */
	53U, /* Multiply: purple */
	57U, /* Subtract: magenta */
	45U, /* Darken: blue */
};

static const char *const k_blend_name[MFD_VJ_BLEND_COUNT] = {"Normal",   "Lighten",  "Screen", "Additive",
							     "Multiply", "Subtract", "Darken"};
static const char *const k_blend_id[MFD_VJ_BLEND_COUNT] = {"normal",   "lighten",  "screen", "additive",
							   "multiply", "subtract", "darken"};

static bool valid_strip(int strip)
{
	return strip >= 0 && strip < (int)MFD_VJ_STRIPS;
}

static uint8_t bit(int strip)
{
	return (uint8_t)(1U << (unsigned)strip);
}

bool mfd_vj_blend_is_punch(enum mfd_vj_blend blend)
{
	return blend > MFD_VJ_BLEND_NORMAL && blend < MFD_VJ_BLEND_DARKEN;
}

void mfd_vj_pads_init(struct mfd_vj_pads *p)
{
	if (!p) {
		return;
	}
	memset(p, 0, sizeof(*p));
	for (unsigned i = 0; i < MFD_VJ_STRIPS; ++i) {
		p->current[i] = MFD_VJ_BLEND_NORMAL;
		p->punch[i] = MFD_VJ_BLEND_SCREEN;
	}
}

bool mfd_vj_pads_is_button(uint8_t channel, uint8_t note)
{
	return channel == 0U && (note < MFD_VJ_NOTE_ROW_FADE + MFD_VJ_STRIPS || note == MFD_VJ_NOTE_STOP_ALL ||
				 note == MFD_VJ_NOTE_SHIFT);
}

static void push(struct mfd_vj_action *out, size_t max, size_t *count, enum mfd_vj_action_kind kind, int strip,
		 enum mfd_vj_blend blend)
{
	if (out && *count < max) {
		out[*count].kind = kind;
		out[*count].strip = strip;
		out[*count].blend = blend;
		++*count;
	}
}

/* The punch mode after this one: Lighten, Screen, Additive, Multiply, Subtract, round again. */
static enum mfd_vj_blend next_punch(enum mfd_vj_blend b)
{
	int n = (int)b + 1;
	return n >= (int)MFD_VJ_BLEND_DARKEN ? MFD_VJ_BLEND_LIGHTEN : (enum mfd_vj_blend)n;
}

bool mfd_vj_pads_feed(struct mfd_vj_pads *p, bool note_on, uint8_t channel, uint8_t note, struct mfd_vj_action *out,
		      size_t max, size_t *out_count)
{
	size_t count = 0;
	if (out_count) {
		*out_count = 0;
	}
	if (!p || !mfd_vj_pads_is_button(channel, note)) {
		return false;
	}

	if (note == MFD_VJ_NOTE_SHIFT) {
		p->shift = note_on;
	} else if (note == MFD_VJ_NOTE_STOP_ALL) {
		if (note_on) {
			if (p->shift) {
				push(out, max, &count, MFD_VJ_ACTION_FADE_ALL_OUT, -1, MFD_VJ_BLEND_NORMAL);
			} else {
				for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
					if (p->has_layer & bit(i)) {
						p->current[i] = MFD_VJ_BLEND_NORMAL;
					}
				}
				push(out, max, &count, MFD_VJ_ACTION_CLEAN, -1, MFD_VJ_BLEND_NORMAL);
			}
		}
	} else if (note >= MFD_VJ_NOTE_ROW_FADE) {
		if (note_on) {
			push(out, max, &count, MFD_VJ_ACTION_FADE_TOGGLE, (int)(note - MFD_VJ_NOTE_ROW_FADE),
			     MFD_VJ_BLEND_NORMAL);
		}
	} else if (note >= MFD_VJ_NOTE_ROW_FLASH) {
		int s = (int)(note - MFD_VJ_NOTE_ROW_FLASH);
		if (note_on && !(p->flashing & bit(s))) {
			p->flashing |= bit(s);
			push(out, max, &count, MFD_VJ_ACTION_FLASH_ON, s, MFD_VJ_BLEND_NORMAL);
		} else if (!note_on && (p->flashing & bit(s))) {
			p->flashing &= (uint8_t)~bit(s);
			push(out, max, &count, MFD_VJ_ACTION_FLASH_OFF, s, MFD_VJ_BLEND_NORMAL);
		}
	} else if (note_on) {
		/* A bottom-row pad. A strip without a layer has nothing to blend, so nothing changes. */
		int s = (int)(note - MFD_VJ_NOTE_ROW_BLEND);
		if (p->has_layer & bit(s)) {
			if (p->shift) {
				p->punch[s] = next_punch(p->punch[s]);
				/* Only a strip already blending changes on air: from one blend mode to the next. */
				if (p->current[s] != MFD_VJ_BLEND_NORMAL) {
					p->current[s] = p->punch[s];
					push(out, max, &count, MFD_VJ_ACTION_SET_BLEND, s, p->punch[s]);
				}
			} else {
				enum mfd_vj_blend b = p->current[s] != MFD_VJ_BLEND_NORMAL ? MFD_VJ_BLEND_NORMAL
											   : p->punch[s];
				p->current[s] = b;
				push(out, max, &count, MFD_VJ_ACTION_SET_BLEND, s, b);
			}
		}
	}

	if (out_count) {
		*out_count = count;
	}
	return true;
}

size_t mfd_vj_pads_release_all(struct mfd_vj_pads *p, struct mfd_vj_action *out, size_t max)
{
	size_t count = 0;
	if (!p) {
		return 0;
	}
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		if (p->flashing & bit(i)) {
			push(out, max, &count, MFD_VJ_ACTION_FLASH_OFF, i, MFD_VJ_BLEND_NORMAL);
		}
	}
	p->flashing = 0;
	p->shift = false;
	return count;
}

void mfd_vj_pads_set_current(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend)
{
	if (p && valid_strip(strip) && (int)blend >= 0 && blend < MFD_VJ_BLEND_COUNT) {
		p->current[strip] = blend;
	}
}

void mfd_vj_pads_set_has_layer(struct mfd_vj_pads *p, int strip, bool has_layer)
{
	if (p && valid_strip(strip)) {
		if (has_layer) {
			p->has_layer |= bit(strip);
		} else {
			p->has_layer &= (uint8_t)~bit(strip);
		}
	}
}

void mfd_vj_pads_set_faded(struct mfd_vj_pads *p, int strip, bool faded)
{
	if (p && valid_strip(strip)) {
		if (faded) {
			p->faded |= bit(strip);
		} else {
			p->faded &= (uint8_t)~bit(strip);
		}
	}
}

void mfd_vj_pads_set_punch(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend)
{
	if (p && valid_strip(strip) && mfd_vj_blend_is_punch(blend)) {
		p->punch[strip] = blend;
	}
}

static void led(struct mfd_vj_led *out, size_t max, size_t *count, uint8_t note, uint8_t velocity)
{
	if (*count < max) {
		out[*count].channel = 0U;
		out[*count].note = note;
		out[*count].velocity = velocity;
		++*count;
	}
}

size_t mfd_vj_pads_leds(const struct mfd_vj_pads *p, struct mfd_vj_led *out, size_t max)
{
	size_t count = 0;
	if (!p || !out) {
		return 0;
	}
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		bool layer = (p->has_layer & bit(i)) != 0;
		/* Bottom row: the blend on air; with Shift held, the punch mode the pad would switch to. */
		uint8_t blend = !layer ? COLOR_OFF : k_blend_color[p->shift ? p->punch[i] : p->current[i]];
		led(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_BLEND + (unsigned)i), blend);
		led(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_FLASH + (unsigned)i),
		    !layer                   ? COLOR_OFF
		    : (p->flashing & bit(i)) ? COLOR_WHITE
					     : COLOR_WHITE_DIM);
		led(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_FADE + (unsigned)i),
		    !layer                ? COLOR_OFF
		    : (p->faded & bit(i)) ? COLOR_RED
					  : COLOR_GREEN_DIM);
	}
	return count;
}

uint8_t mfd_vj_blend_color(enum mfd_vj_blend blend)
{
	return (int)blend >= 0 && blend < MFD_VJ_BLEND_COUNT ? k_blend_color[blend] : COLOR_OFF;
}

const char *mfd_vj_blend_name(enum mfd_vj_blend blend)
{
	return (int)blend >= 0 && blend < MFD_VJ_BLEND_COUNT ? k_blend_name[blend] : "Normal";
}

const char *mfd_vj_blend_id(enum mfd_vj_blend blend)
{
	return (int)blend >= 0 && blend < MFD_VJ_BLEND_COUNT ? k_blend_id[blend] : "normal";
}

bool mfd_vj_blend_from_id(const char *id, enum mfd_vj_blend *out)
{
	if (!id) {
		return false;
	}
	for (int i = 0; i < (int)MFD_VJ_BLEND_COUNT; ++i) {
		if (strcmp(id, k_blend_id[i]) == 0) {
			if (out) {
				*out = (enum mfd_vj_blend)i;
			}
			return true;
		}
	}
	return false;
}
