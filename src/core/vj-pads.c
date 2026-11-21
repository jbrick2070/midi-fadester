/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/vj-pads.h"

#include <string.h>

/* APC40 mkII RGB palette velocities (Akai's protocol v1.2 palette). */
#define COLOR_OFF 0U
#define COLOR_WHITE_DIM 1U
#define COLOR_WHITE 3U
#define COLOR_RED 5U
#define COLOR_RED_DIM 6U
#define COLOR_AMBER 9U
#define COLOR_GREEN 21U
#define COLOR_GREEN_DIM 22U
#define COLOR_AZURE 37U
#define COLOR_BLUE_DIM 46U

/* The top row: the cut in yellow, then the scene fades, longest to shortest, in palette greens from dark to light
 * (white-lifted at the short end so all seven read as different on the desk). */
static const uint8_t k_fade_green[MFD_VJ_STRIPS] = {13U /* the cut: yellow */, 23U, 22U, 76U, 122U, 21U, 20U, 114U};

/* An RGB pad's MIDI channel picks its animation; the rate is the desk's slowest, 1/2 (a two-beat breath). */
#define CH_SOLID 0U
#define CH_PULSE 10U /* channel 11: pulsing, 1/2 */
#define CH_BLINK 15U /* channel 16: blinking, 1/2 */

/* A single-colour button: 0 off, 1 on (Clip Stop could also blink with 2; Record and Session cannot). */
#define LAMP_OFF 0U
#define LAMP_ON 1U

static const uint8_t k_blend_color[MFD_VJ_BLEND_COUNT] = {
	COLOR_WHITE_DIM, /* Normal: dim white, an idle source */
	COLOR_AZURE,     /* Lighten */
	13U,             /* Screen: yellow */
	9U,              /* Additive: orange */
	53U,             /* Multiply: violet */
	57U,             /* Subtract: magenta */
	45U,             /* Darken: blue */
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
		p->home[i] = MFD_VJ_BLEND_NORMAL;
		p->punch[i] = MFD_VJ_BLEND_SCREEN;
	}
}

bool mfd_vj_pads_is_button(uint8_t channel, uint8_t note)
{
	if (note == MFD_VJ_NOTE_CLIP_STOP) {
		return channel < MFD_VJ_STRIPS;
	}
	return channel == 0U &&
	       (note < MFD_VJ_NOTE_ROW_RESTART + MFD_VJ_STRIPS || note == MFD_VJ_NOTE_STOP_ALL ||
		(note >= MFD_VJ_NOTE_SCENE && note < MFD_VJ_NOTE_SCENE + MFD_VJ_SCENES) || note == MFD_VJ_NOTE_SHIFT ||
		note == MFD_VJ_NOTE_PLAY || note == MFD_VJ_NOTE_TAP_TEMPO || note == MFD_VJ_NOTE_NUDGE_DOWN ||
		note == MFD_VJ_NOTE_NUDGE_UP || note == MFD_VJ_NOTE_METRONOME || note == MFD_VJ_NOTE_RECORD ||
		note == MFD_VJ_NOTE_SESSION);
}

bool mfd_vj_pads_is_knob(uint8_t channel, uint8_t cc)
{
	return channel == 0U && (cc == MFD_VJ_CC_TEMPO || cc == MFD_VJ_CC_CUE_LEVEL);
}

static void push(struct mfd_vj_action *out, size_t max, size_t *count, enum mfd_vj_action_kind kind, int strip,
		 enum mfd_vj_blend blend)
{
	if (out && *count < max) {
		out[*count].kind = kind;
		out[*count].strip = strip;
		out[*count].blend = blend;
		out[*count].steps = 0;
		++*count;
	}
}

static void push_steps(struct mfd_vj_action *out, size_t max, size_t *count, enum mfd_vj_action_kind kind, int steps)
{
	push(out, max, count, kind, -1, MFD_VJ_BLEND_NORMAL);
	if (out && *count > 0 && out[*count - 1].kind == kind) {
		out[*count - 1].steps = steps;
	}
}

/* The punch mode after this one: Lighten, Screen, Additive, Multiply, Subtract, round again. */
static enum mfd_vj_blend next_punch(enum mfd_vj_blend b)
{
	int n = (int)b + 1;
	return n >= (int)MFD_VJ_BLEND_DARKEN ? MFD_VJ_BLEND_LIGHTEN : (enum mfd_vj_blend)n;
}

/* The blend a punch switches the strip to: its punch mode, or the next one when that is the home blend already. */
static enum mfd_vj_blend punch_target(const struct mfd_vj_pads *p, int s)
{
	return p->punch[s] == p->home[s] ? next_punch(p->punch[s]) : p->punch[s];
}

/* What Shift + the pad sets the punch mode to: the next mode after the one on air (or the saved one at home), never
 * the home blend itself. */
static enum mfd_vj_blend shift_target(const struct mfd_vj_pads *p, int s)
{
	enum mfd_vj_blend from = p->current[s] != p->home[s] ? p->current[s] : p->punch[s];
	enum mfd_vj_blend nb = next_punch(from);
	return nb == p->home[s] ? next_punch(nb) : nb;
}

/* A bottom-row pad: the source between its home blend and its punch mode. A strip without a layer has nothing to blend,
 * so nothing changes. */
static void feed_blend(struct mfd_vj_pads *p, int s, struct mfd_vj_action *out, size_t max, size_t *count)
{
	if (!(p->has_layer & bit(s))) {
		return;
	}
	if (p->shift) {
		p->punch[s] = shift_target(p, s);
		/* Only a strip already punched changes on air: from one blend mode to the next. */
		if (p->current[s] != p->home[s]) {
			p->current[s] = p->punch[s];
			push(out, max, count, MFD_VJ_ACTION_SET_BLEND, s, p->punch[s]);
		}
	} else {
		enum mfd_vj_blend b = p->current[s] != p->home[s] ? p->home[s] : punch_target(p, s);
		p->current[s] = b;
		push(out, max, count, MFD_VJ_ACTION_SET_BLEND, s, b);
	}
}

/* A row-2 pad: flash while held, or with Shift a blackout while held. Its note off lets go of whichever it holds. */
static void feed_hold(struct mfd_vj_pads *p, bool note_on, int s, struct mfd_vj_action *out, size_t max, size_t *count)
{
	if (note_on) {
		if ((p->flashing | p->blackout) & bit(s)) {
			return; /* already held: a second note on changes nothing */
		}
		if (p->shift) {
			p->blackout |= bit(s);
			push(out, max, count, MFD_VJ_ACTION_BLACKOUT_ON, s, MFD_VJ_BLEND_NORMAL);
		} else {
			p->flashing |= bit(s);
			push(out, max, count, MFD_VJ_ACTION_FLASH_ON, s, MFD_VJ_BLEND_NORMAL);
		}
	} else if (p->flashing & bit(s)) {
		p->flashing &= (uint8_t)~bit(s);
		push(out, max, count, MFD_VJ_ACTION_FLASH_OFF, s, MFD_VJ_BLEND_NORMAL);
	} else if (p->blackout & bit(s)) {
		p->blackout &= (uint8_t)~bit(s);
		push(out, max, count, MFD_VJ_ACTION_BLACKOUT_OFF, s, MFD_VJ_BLEND_NORMAL);
	}
}

static void feed_transport(struct mfd_vj_pads *p, uint8_t note, struct mfd_vj_action *out, size_t max, size_t *count)
{
	if (note == MFD_VJ_NOTE_PLAY) {
		push(out, max, count, MFD_VJ_ACTION_PLAY_PAUSE, -1, MFD_VJ_BLEND_NORMAL);
	} else if (note == MFD_VJ_NOTE_TAP_TEMPO) {
		push(out, max, count, p->shift ? MFD_VJ_ACTION_VS_RESYNC : MFD_VJ_ACTION_SPEED_RESET, -1,
		     MFD_VJ_BLEND_NORMAL);
	} else if (note == MFD_VJ_NOTE_NUDGE_DOWN || note == MFD_VJ_NOTE_NUDGE_UP) {
		push_steps(out, max, count, MFD_VJ_ACTION_SPEED_STEPS,
			   note == MFD_VJ_NOTE_NUDGE_UP ? MFD_VJ_COARSE_STEPS : -MFD_VJ_COARSE_STEPS);
	} else if (note == MFD_VJ_NOTE_METRONOME) {
		push(out, max, count, p->shift ? MFD_VJ_ACTION_VS_LOCK : MFD_VJ_ACTION_VS_TAP, -1, MFD_VJ_BLEND_NORMAL);
	} else if (note == MFD_VJ_NOTE_RECORD) {
		push(out, max, count, MFD_VJ_ACTION_VS_PANIC, -1, MFD_VJ_BLEND_NORMAL);
	} else if (note == MFD_VJ_NOTE_SESSION) {
		push(out, max, count, MFD_VJ_ACTION_BLACKOUT_TOGGLE, -1, MFD_VJ_BLEND_NORMAL);
	}
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
	} else if (note == MFD_VJ_NOTE_CLIP_STOP) {
		if (note_on && p->shift) {
			push(out, max, &count, MFD_VJ_ACTION_PIN_TOGGLE, (int)channel, MFD_VJ_BLEND_NORMAL);
		} else if (note_on && (p->has_layer & bit((int)channel))) {
			push(out, max, &count, MFD_VJ_ACTION_COLUMN_CLEAN, (int)channel, MFD_VJ_BLEND_NORMAL);
		}
	} else if (note >= MFD_VJ_NOTE_SCENE && note < MFD_VJ_NOTE_SCENE + MFD_VJ_SCENES) {
		if (note_on) {
			push(out, max, &count, p->shift ? MFD_VJ_ACTION_SCENE_PROGRAM : MFD_VJ_ACTION_SCENE_PICK,
			     (int)(note - MFD_VJ_NOTE_SCENE), MFD_VJ_BLEND_NORMAL);
		}
	} else if (note == MFD_VJ_NOTE_PLAY || note == MFD_VJ_NOTE_TAP_TEMPO || note == MFD_VJ_NOTE_NUDGE_DOWN ||
		   note == MFD_VJ_NOTE_NUDGE_UP || note == MFD_VJ_NOTE_METRONOME || note == MFD_VJ_NOTE_RECORD ||
		   note == MFD_VJ_NOTE_SESSION) {
		if (note_on) {
			feed_transport(p, note, out, max, &count);
		}
	} else if (note == MFD_VJ_NOTE_STOP_ALL) {
		if (note_on) {
			if (p->shift) {
				push(out, max, &count, MFD_VJ_ACTION_FADE_ALL_OUT, -1, MFD_VJ_BLEND_NORMAL);
			} else {
				for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
					if (p->has_layer & bit(i)) {
						p->current[i] = p->home[i];
					}
				}
				push(out, max, &count, MFD_VJ_ACTION_CLEAN, -1, MFD_VJ_BLEND_NORMAL);
			}
		}
	} else if (note >= MFD_VJ_NOTE_ROW_RESTART) {
		int s = (int)(note - MFD_VJ_NOTE_ROW_RESTART);
		if (note_on) { /* the top row: the cut on the left, then scene fades from long to short; Shift or not */
			push(out, max, &count, s == 0 ? MFD_VJ_ACTION_SCENE_CUT : MFD_VJ_ACTION_SCENE_FADE, s,
			     MFD_VJ_BLEND_NORMAL);
		}
	} else if (note >= MFD_VJ_NOTE_ROW_SHOW) {
		int s = (int)(note - MFD_VJ_NOTE_ROW_SHOW);
		if (note_on && (p->has_layer & bit(s))) {
			push(out, max, &count, p->shift ? MFD_VJ_ACTION_WAKE : MFD_VJ_ACTION_SHOW_TOGGLE, s,
			     MFD_VJ_BLEND_NORMAL);
		}
	} else if (note >= MFD_VJ_NOTE_ROW_FADE) {
		if (note_on) {
			push(out, max, &count, p->shift ? MFD_VJ_ACTION_SNAP : MFD_VJ_ACTION_FADE_TOGGLE,
			     (int)(note - MFD_VJ_NOTE_ROW_FADE), MFD_VJ_BLEND_NORMAL);
		}
	} else if (note >= MFD_VJ_NOTE_ROW_FLASH) {
		feed_hold(p, note_on, (int)(note - MFD_VJ_NOTE_ROW_FLASH), out, max, &count);
	} else if (note_on) {
		feed_blend(p, (int)(note - MFD_VJ_NOTE_ROW_BLEND), out, max, &count);
	}

	if (out_count) {
		*out_count = count;
	}
	return true;
}

bool mfd_vj_pads_feed_cc(struct mfd_vj_pads *p, uint8_t channel, uint8_t cc, uint8_t value, struct mfd_vj_action *out,
			 size_t max, size_t *out_count)
{
	size_t count = 0;
	if (out_count) {
		*out_count = 0;
	}
	if (!p || !mfd_vj_pads_is_knob(channel, cc)) {
		return false;
	}
	value &= 0x7FU;
	int detents = value < 64U ? (int)value : (int)value - 128; /* 0 is no turn */
	if (detents != 0) {
		if (cc == MFD_VJ_CC_CUE_LEVEL) {
			push_steps(out, max, &count, p->shift ? MFD_VJ_ACTION_FADE_STEPS : MFD_VJ_ACTION_CUE_STEPS,
				   detents);
		} else {
			push_steps(out, max, &count, MFD_VJ_ACTION_SPEED_STEPS,
				   p->shift ? detents * MFD_VJ_COARSE_STEPS : detents);
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
		if (p->blackout & bit(i)) {
			push(out, max, &count, MFD_VJ_ACTION_BLACKOUT_OFF, i, MFD_VJ_BLEND_NORMAL);
		}
	}
	p->flashing = 0;
	p->blackout = 0;
	p->shift = false;
	return count;
}

void mfd_vj_pads_set_home(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend)
{
	if (p && valid_strip(strip) && (int)blend >= 0 && blend < MFD_VJ_BLEND_COUNT) {
		p->home[strip] = blend;
	}
}

void mfd_vj_pads_set_current(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend)
{
	if (p && valid_strip(strip) && (int)blend >= 0 && blend < MFD_VJ_BLEND_COUNT) {
		p->current[strip] = blend;
	}
}

static void set_bit(uint8_t *bits, int strip, bool on)
{
	if (on) {
		*bits |= bit(strip);
	} else {
		*bits &= (uint8_t)~bit(strip);
	}
}

void mfd_vj_pads_set_has_layer(struct mfd_vj_pads *p, int strip, bool has_layer)
{
	if (p && valid_strip(strip)) {
		set_bit(&p->has_layer, strip, has_layer);
	}
}

void mfd_vj_pads_set_faded(struct mfd_vj_pads *p, int strip, bool faded)
{
	if (p && valid_strip(strip)) {
		set_bit(&p->faded, strip, faded);
	}
}

void mfd_vj_pads_set_hidden(struct mfd_vj_pads *p, int strip, bool hidden)
{
	if (p && valid_strip(strip)) {
		set_bit(&p->hidden, strip, hidden);
	}
}

void mfd_vj_pads_set_zero(struct mfd_vj_pads *p, int strip, bool zero)
{
	if (p && valid_strip(strip)) {
		set_bit(&p->zero, strip, zero);
	}
}

void mfd_vj_pads_set_pinned(struct mfd_vj_pads *p, int strip, bool pinned)
{
	if (p && valid_strip(strip)) {
		set_bit(&p->pinned, strip, pinned);
	}
}

void mfd_vj_pads_set_scene(struct mfd_vj_pads *p, int scene, enum mfd_vj_scene_state state)
{
	if (p && scene >= 0 && scene < (int)MFD_VJ_SCENES && (int)state >= 0 && state <= MFD_VJ_SCENE_PROGRAM) {
		p->scene[scene] = state;
	}
}

void mfd_vj_pads_set_punch(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend)
{
	if (p && valid_strip(strip) && mfd_vj_blend_is_punch(blend)) {
		p->punch[strip] = blend;
	}
}

static void led(struct mfd_vj_led *out, size_t max, size_t *count, uint8_t channel, uint8_t note, uint8_t velocity,
		bool rgb)
{
	if (*count < max) {
		out[*count].channel = channel;
		out[*count].note = note;
		out[*count].velocity = velocity;
		out[*count].rgb = rgb;
		out[*count].primary = 0U;
		++*count;
	}
}

static void pad(struct mfd_vj_led *out, size_t max, size_t *count, uint8_t note, uint8_t velocity)
{
	led(out, max, count, CH_SOLID, note, velocity, true);
}

size_t mfd_vj_pads_leds(const struct mfd_vj_pads *p, struct mfd_vj_led *out, size_t max)
{
	size_t count = 0;
	if (!p || !out) {
		return 0;
	}
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		uint8_t b = bit(i);
		bool layer = (p->has_layer & b) != 0;
		/* Bottom row: dim white at the home blend, the punched blend's colour; with Shift held, the armed punch mode
		 * (the one on air when punched). */
		pad(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_BLEND + (unsigned)i),
		    !layer     ? COLOR_OFF
		    : p->shift ? k_blend_color[p->current[i] != p->home[i] ? p->current[i] : punch_target(p, i)]
		    : p->current[i] == p->home[i] ? COLOR_WHITE_DIM
						  : k_blend_color[p->current[i]]);
		pad(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_FLASH + (unsigned)i),
		    !layer              ? COLOR_OFF
		    : (p->flashing & b) ? COLOR_WHITE
		    : (p->blackout & b) ? COLOR_AMBER
					: COLOR_WHITE_DIM);
		pad(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_FADE + (unsigned)i),
		    !layer           ? COLOR_OFF
		    : (p->faded & b) ? COLOR_AMBER
				     : COLOR_WHITE_DIM);
		uint8_t show = (uint8_t)(MFD_VJ_NOTE_ROW_SHOW + (unsigned)i);
		if (layer && (p->hidden & b)) {
			led(out, max, &count, CH_BLINK, show, COLOR_RED, true);
			out[count - 1U].primary = COLOR_RED_DIM; /* lit even if the desk does not blink */
		} else {
			pad(out, max, &count, show, !layer ? COLOR_OFF : (p->zero & b) ? COLOR_AMBER : COLOR_WHITE_DIM);
		}
		/* the cut yellow, then the scene fades in seven greens, darkest (longest) to lightest (shortest) */
		pad(out, max, &count, (uint8_t)(MFD_VJ_NOTE_ROW_RESTART + (unsigned)i), k_fade_green[i]);
		led(out, max, &count, (uint8_t)i, MFD_VJ_NOTE_CLIP_STOP,
		    p->shift ? ((p->pinned & b) ? LAMP_ON : LAMP_OFF) /* Shift held: which columns are pinned */
		    : layer && (p->current[i] != p->home[i] || ((p->flashing | p->blackout) & b)) ? LAMP_ON
												  : LAMP_OFF,
		    false);
	}
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		uint8_t note = (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)j);
		switch (p->scene[j]) {
		case MFD_VJ_SCENE_PROGRAM:
			pad(out, max, &count, note, COLOR_RED);
			break;
		case MFD_VJ_SCENE_PREVIEW:
			led(out, max, &count, CH_PULSE, note, COLOR_GREEN, true);
			out[count - 1U].primary =
				COLOR_GREEN_DIM; /* dim to bright, so it stays lit if the desk does not pulse */
			break;
		case MFD_VJ_SCENE_IDLE:
			pad(out, max, &count, note, COLOR_BLUE_DIM);
			break;
		case MFD_VJ_SCENE_NONE:
		default:
			pad(out, max, &count, note, COLOR_OFF);
			break;
		}
	}
	led(out, max, &count, 0U, MFD_VJ_NOTE_RECORD, p->vs_panic ? LAMP_ON : LAMP_OFF, false);
	led(out, max, &count, 0U, MFD_VJ_NOTE_SESSION, p->blackout_lamp ? LAMP_ON : LAMP_OFF, false);
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
