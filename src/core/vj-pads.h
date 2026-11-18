/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

/*
 * APC40 mkII VJ pads: the clip grid beside the faders, as a pure state machine (no OBS calls), so every press can be
 * tested. Only the clip pads, Shift and Stop All Clips are used: in the desk's default mode they are momentary, while
 * Solo latches and Track Select re-banks the Device knobs onto other MIDI channels (design review 2026-11-18).
 *
 *   Bottom row pad k (note k, ch 1)         blend punch: strip k's layer Normal <-> its punch mode (Screen at first)
 *   Shift + bottom row pad k                next punch mode for strip k (Lighten, Screen, Additive, Multiply,
 *                                           Subtract); a strip already blending switches to it at once
 *   Row 2 pad k (note 8 + k)                flash strip k's layer to 100 % while held
 *   Row 3 pad k (note 16 + k)               fade strip k out; again, fade it back in
 *   Stop All Clips (note 81)                Clean / All FX Off, and every strip's blend back to Normal
 *   Shift (note 98) + Stop All Clips        fade every layer strip out
 *
 * The transport section sets the video speed through Fadester Companion and VJster (an OBS media source restarts when
 * its speed changes, so live video speed is VJster's): the engine adds the steps up and sends them as one vendor event.
 *
 *   Tempo knob (CC 13, relative)            each detent 0.05x faster or slower; with Shift held 0.25x
 *   Nudge - / Nudge + (notes 100 / 101)     0.25x slower / faster
 *   Tap Tempo (note 99)                     back to 1x
 *   Play (note 91)                          play or pause the video
 *
 * Darken is left out of the punch modes: fading a Darken layer down turns its rectangle black.
 * It also says which colour each pad should show.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_VJ_STRIPS 8U
#define MFD_VJ_MAX_ACTIONS 8U
#define MFD_VJ_MAX_LEDS 24U

/* The APC40 mkII notes (Akai's protocol for the mkII), all on MIDI channel 1 (0 here). */
#define MFD_VJ_NOTE_ROW_BLEND 0U /* bottom clip row: notes 0..7 */
#define MFD_VJ_NOTE_ROW_FLASH 8U /* second row: notes 8..15 */
#define MFD_VJ_NOTE_ROW_FADE 16U /* third row: notes 16..23 */
#define MFD_VJ_NOTE_STOP_ALL 81U
#define MFD_VJ_NOTE_SHIFT 98U
#define MFD_VJ_NOTE_PLAY 91U
#define MFD_VJ_NOTE_TAP_TEMPO 99U
#define MFD_VJ_NOTE_NUDGE_DOWN 100U
#define MFD_VJ_NOTE_NUDGE_UP 101U
#define MFD_VJ_CC_TEMPO 13U   /* endless knob, relative: 1..63 is +n detents, 64..127 is n - 128 */
#define MFD_VJ_COARSE_STEPS 5 /* Shift + Tempo and Nudge: 5 steps of 0.05x = 0.25x */

enum mfd_vj_blend {
	MFD_VJ_BLEND_NORMAL = 0,
	MFD_VJ_BLEND_LIGHTEN = 1,
	MFD_VJ_BLEND_SCREEN = 2,
	MFD_VJ_BLEND_ADDITIVE = 3,
	MFD_VJ_BLEND_MULTIPLY = 4,
	MFD_VJ_BLEND_SUBTRACT = 5,
	MFD_VJ_BLEND_DARKEN = 6, /* read from OBS and shown, never offered as a punch mode */
	MFD_VJ_BLEND_COUNT = 7,
};

enum mfd_vj_action_kind {
	MFD_VJ_ACTION_NONE = 0,
	MFD_VJ_ACTION_SET_BLEND,    /* strip, blend */
	MFD_VJ_ACTION_FLASH_ON,     /* strip */
	MFD_VJ_ACTION_FLASH_OFF,    /* strip */
	MFD_VJ_ACTION_FADE_TOGGLE,  /* strip: out when it shows, back in when a pad faded it out */
	MFD_VJ_ACTION_CLEAN,        /* Clean / All FX Off and every strip's blend back to Normal */
	MFD_VJ_ACTION_FADE_ALL_OUT, /* every layer strip out */
	MFD_VJ_ACTION_SPEED_STEPS,  /* steps: video speed by steps x 0.05x (through Fadester Companion) */
	MFD_VJ_ACTION_SPEED_RESET,  /* video back to 1x */
	MFD_VJ_ACTION_PLAY_PAUSE,   /* play or pause the video */
};

struct mfd_vj_action {
	enum mfd_vj_action_kind kind;
	int strip; /* 0..7, -1 when the action is not about one strip */
	enum mfd_vj_blend blend;
	int steps; /* SPEED_STEPS only, signed */
};

/* One LED: the note and channel the desk lights it by, and the velocity (an RGB pad's colour). */
struct mfd_vj_led {
	uint8_t channel;
	uint8_t note;
	uint8_t velocity;
};

struct mfd_vj_pads {
	bool shift;
	uint8_t flashing;  /* a bit per strip held at 100 % by its row-2 pad */
	uint8_t faded;     /* a bit per strip a row-3 pad faded out */
	uint8_t has_layer; /* a bit per strip that drives a layer the pads can act on */
	enum mfd_vj_blend current[MFD_VJ_STRIPS];
	enum mfd_vj_blend punch[MFD_VJ_STRIPS];
};

void mfd_vj_pads_init(struct mfd_vj_pads *p);

/* Whether this note address is one of the VJ pads. */
bool mfd_vj_pads_is_button(uint8_t channel, uint8_t note);

/*
 * Feeds one note on (note_on) or note off. Returns false when it is not a VJ pad (nothing changes). Writes the actions
 * to carry out to out (at most max), their number to *out_count. A strip without a layer gets no blend action. The blend
 * a SET_BLEND asks for is recorded as the strip's current blend at once; the engine corrects it with
 * mfd_vj_pads_set_current when OBS says otherwise.
 */
bool mfd_vj_pads_feed(struct mfd_vj_pads *p, bool note_on, uint8_t channel, uint8_t note, struct mfd_vj_action *out,
		      size_t max, size_t *out_count);

/*
 * Feeds one control change. Returns false when it is not the Tempo knob (nothing changes). A detent (or a fast turn's
 * several) becomes one SPEED_STEPS action; Shift held makes each detent MFD_VJ_COARSE_STEPS steps.
 */
bool mfd_vj_pads_feed_cc(struct mfd_vj_pads *p, uint8_t channel, uint8_t cc, uint8_t value, struct mfd_vj_action *out,
			 size_t max, size_t *out_count);

/* Whether this control change address is the Tempo knob. */
bool mfd_vj_pads_is_knob(uint8_t channel, uint8_t cc);

/* The MIDI controller went away: actions that let go of every held pad (flash off), Shift cleared. */
size_t mfd_vj_pads_release_all(struct mfd_vj_pads *p, struct mfd_vj_action *out, size_t max);

void mfd_vj_pads_set_current(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend);
void mfd_vj_pads_set_has_layer(struct mfd_vj_pads *p, int strip, bool has_layer);
void mfd_vj_pads_set_faded(struct mfd_vj_pads *p, int strip, bool faded);
/* A saved punch mode; Normal and Darken are not punch modes and are ignored. */
void mfd_vj_pads_set_punch(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend);

/* Every VJ pad LED and the colour it should show now (out holds at least MFD_VJ_MAX_LEDS). */
size_t mfd_vj_pads_leds(const struct mfd_vj_pads *p, struct mfd_vj_led *out, size_t max);

/* An RGB pad colour (APC40 mkII palette velocity) for a blend mode. */
uint8_t mfd_vj_blend_color(enum mfd_vj_blend blend);

/* English name, for logs ("Normal", "Screen", ...). */
const char *mfd_vj_blend_name(enum mfd_vj_blend blend);

/* The saved and API id ("normal", "screen", ...), and back; false for an unknown id. */
const char *mfd_vj_blend_id(enum mfd_vj_blend blend);
bool mfd_vj_blend_from_id(const char *id, enum mfd_vj_blend *out);

/* Whether a blend mode can be a punch mode (not Normal, not Darken). */
bool mfd_vj_blend_is_punch(enum mfd_vj_blend blend);

#ifdef __cplusplus
}
#endif
