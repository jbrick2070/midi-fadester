/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

/*
 * APC40 mkII VJ pads: the clip grid beside the faders, as a pure state machine (no OBS calls), so every press can be
 * tested. Column k is strip k's source (in the scene on air when the strip follows it). Only controls that are
 * momentary in the desk's default mode are used: Solo, Activator, Record Arm and A|B latch, and Track Select re-banks
 * the Device knobs onto other MIDI channels (design reviews 2026-11-18).
 *
 *   Bottom row pad k (note k, ch 1)         blend punch: the source between its own blend in the scene (its home
 *                                           blend, Normal or, say, Screen on an overlay) and its punch mode
 *   Shift + bottom row pad k                next punch mode for strip k (Lighten, Screen, Additive, Multiply,
 *                                           Subtract); a strip already blending switches to it at once
 *   Row 2 pad k (note 8 + k)                flash strip k's layer to 100 % while held; with Shift, blackout (0 %)
 *   Row 3 pad k (note 16 + k)               fade strip k out; again, fade it back in; with Shift, snap 0 % / 100 %
 *   Row 4 pad k (note 24 + k)               show or hide the source (its eye in OBS); with Shift, wake it: shown,
 *                                           100 %, blend Normal
 *   Row 5 pad 1 (note 32)                   cut: Preview to Program at once. Lit yellow.
 *   Row 5 pad k = 2..8 (note 31 + k)         scene fade: Preview to Program over 32 / 2^(k-2) beats (8 bars on pad 2,
 *                                           1/2 beat on pad 8) at Visual Sidechain's BPM, 120 when it has none.
 *                                           Lit in seven greens, dark (longest) to light. Shift changes nothing here.
 *   Clip Stop k (note 52, ch k)             column clean: the source back to its home blend, flash or blackout let go;
 *                                           with Shift, pin or unpin the column (a pinned column keeps its source when
 *                                           the scene on air changes; with Shift held, Clip Stop lights the pinned ones)
 *   Stop All Clips (note 81)                Clean / All FX Off, and every source back to its home blend
 *   Shift (note 98) + Stop All Clips        fade every layer strip out
 *
 * Scenes (the first five in OBS's Scenes list, top first):
 *
 *   Scene Launch n (notes 82..86)           put scene n in Preview; pressed again (it is in Preview), take it with
 *                                           the transition; with Shift, or outside Studio Mode, straight to Program
 *                                           (with the transition too)
 *   Session (note 102)                      Blackout: Program fades to black; again, back (blinks while black)
 *   Cue Level (CC 47, relative)             transition length, 50 ms a detent; with Shift, the pad fade time
 *
 * Visual Sidechain: Record (note 93) panics or restores; Metronome (note 90) taps the tempo, with Shift locks or
 * unlocks the BPM; Shift + Tap Tempo resyncs to the downbeat.
 *
 * The transport section sets the video speed through Fadester Companion and VJster (an OBS media source restarts when
 * its speed changes, so live video speed is VJster's): the engine adds the steps up and sends them as one vendor event.
 *
 *   Tempo knob (CC 13, relative)            each detent 0.05x faster or slower; with Shift held 0.25x
 *   Nudge - / Nudge + (notes 100 / 101)     0.25x slower / faster
 *   Tap Tempo (note 99)                     back to 1x (with Shift: Visual Sidechain downbeat resync)
 *   Play (note 91)                          play or pause the video
 *
 * Darken is left out of the punch modes: fading a Darken layer down turns its rectangle black.
 *
 * It also says what each LED should show. Scenes: Program red, Preview green pulsing, other scenes dim blue, no scene
 * off (Preview pulses between dim and bright green, so it stays lit if the desk does not animate). Sources: dim white
 * at their home blend, the blend colour when punched, bright white while flashing, amber when faded
 * out, at 0 % or blacked out, blinking dim / bright red when hidden, off without a source. Row 5: the cut yellow, then seven greens,
 * dark (the longest fade) to light (the shortest). Pulses and blinks run at the desk's slowest rate (1/2), sent on the animation's MIDI channel.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_VJ_STRIPS 8U
#define MFD_VJ_MAX_ACTIONS 8U
#define MFD_VJ_SCENES 5U
#define MFD_VJ_MAX_LEDS 64U

/* The APC40 mkII notes (Akai's protocol for the mkII), all on MIDI channel 1 (0 here). */
#define MFD_VJ_NOTE_ROW_BLEND 0U    /* bottom clip row: notes 0..7 */
#define MFD_VJ_NOTE_ROW_FLASH 8U    /* second row: notes 8..15 */
#define MFD_VJ_NOTE_ROW_FADE 16U    /* third row: notes 16..23 */
#define MFD_VJ_NOTE_ROW_SHOW 24U    /* fourth row: notes 24..31 */
#define MFD_VJ_NOTE_ROW_RESTART 32U /* top row: notes 32..39 */
#define MFD_VJ_NOTE_CLIP_STOP 52U   /* one per track: the track's MIDI channel (0..7) says which */
#define MFD_VJ_NOTE_STOP_ALL 81U
#define MFD_VJ_NOTE_SCENE 82U /* Scene Launch 1..5: notes 82..86, 82 at the top */
#define MFD_VJ_NOTE_METRONOME 90U
#define MFD_VJ_NOTE_RECORD 93U
#define MFD_VJ_NOTE_SESSION 102U
#define MFD_VJ_NOTE_SHIFT 98U
#define MFD_VJ_NOTE_PLAY 91U
#define MFD_VJ_NOTE_TAP_TEMPO 99U
#define MFD_VJ_NOTE_NUDGE_DOWN 100U
#define MFD_VJ_NOTE_NUDGE_UP 101U
#define MFD_VJ_CC_TEMPO 13U     /* endless knob, relative: 1..63 is +n detents, 64..127 is n - 128 */
#define MFD_VJ_CC_CUE_LEVEL 47U /* endless knob, relative like Tempo */
#define MFD_VJ_COARSE_STEPS 5   /* Shift + Tempo and Nudge: 5 steps of 0.05x = 0.25x */

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
	MFD_VJ_ACTION_SET_BLEND,       /* strip, blend */
	MFD_VJ_ACTION_FLASH_ON,        /* strip */
	MFD_VJ_ACTION_FLASH_OFF,       /* strip */
	MFD_VJ_ACTION_FADE_TOGGLE,     /* strip: out when it shows, back in when a pad faded it out */
	MFD_VJ_ACTION_CLEAN,           /* Clean / All FX Off and every strip's blend back to Normal */
	MFD_VJ_ACTION_FADE_ALL_OUT,    /* every layer strip out */
	MFD_VJ_ACTION_SPEED_STEPS,     /* steps: video speed by steps x 0.05x (through Fadester Companion) */
	MFD_VJ_ACTION_SPEED_RESET,     /* video back to 1x */
	MFD_VJ_ACTION_PLAY_PAUSE,      /* play or pause the video */
	MFD_VJ_ACTION_BLACKOUT_ON,     /* strip: hold the layer at 0 % (Shift + row 2) */
	MFD_VJ_ACTION_BLACKOUT_OFF,    /* strip */
	MFD_VJ_ACTION_SNAP,            /* strip: straight to 0 % when it shows, else back up (Shift + row 3) */
	MFD_VJ_ACTION_SHOW_TOGGLE,     /* strip: the source's eye on or off (row 4) */
	MFD_VJ_ACTION_WAKE,            /* strip: shown, 100 %, blend Normal (Shift + row 4) */
	MFD_VJ_ACTION_PIN_TOGGLE,      /* strip: pin the column to its source, or let it follow the scene again */
	MFD_VJ_ACTION_COLUMN_CLEAN,    /* strip: blend Normal, flash and blackout let go (Clip Stop) */
	MFD_VJ_ACTION_SCENE_PICK,      /* strip = scene 0..4: to Preview, or take it when it is in Preview */
	MFD_VJ_ACTION_SCENE_PROGRAM,   /* strip = scene 0..4: straight to Program, skipping Preview */
	MFD_VJ_ACTION_TAKE,            /* Preview to Program with the transition */
	MFD_VJ_ACTION_VS_PANIC,        /* Visual Sidechain panic, or restore when it is panicked */
	MFD_VJ_ACTION_VS_TAP,          /* Visual Sidechain tap tempo */
	MFD_VJ_ACTION_VS_LOCK,         /* Visual Sidechain BPM lock on or off */
	MFD_VJ_ACTION_VS_RESYNC,       /* Visual Sidechain downbeat resync */
	MFD_VJ_ACTION_CUE_STEPS,       /* steps: transition length by steps x 50 ms */
	MFD_VJ_ACTION_FADE_STEPS,      /* steps: pad fade time by steps x 50 ms */
	MFD_VJ_ACTION_BLACKOUT_TOGGLE, /* Program to black, or back (Session) */
	MFD_VJ_ACTION_SCENE_FADE,      /* strip = top-row pad 1..7: Preview to Program at that pad's length */
	MFD_VJ_ACTION_SCENE_CUT,       /* top-row pad 0 (yellow): Preview to Program at once */
};

/* What a Scene Launch button shows. */
enum mfd_vj_scene_state {
	MFD_VJ_SCENE_NONE = 0, /* no scene at this place in the list */
	MFD_VJ_SCENE_IDLE,
	MFD_VJ_SCENE_PREVIEW,
	MFD_VJ_SCENE_PROGRAM,
};

struct mfd_vj_action {
	enum mfd_vj_action_kind kind;
	int strip; /* 0..7, -1 when the action is not about one strip */
	enum mfd_vj_blend blend;
	int steps; /* SPEED_STEPS, CUE_STEPS and FADE_STEPS only, signed */
};

/* One LED: the note and channel the desk lights it by, and the velocity (an RGB pad's colour). */
struct mfd_vj_led {
	uint8_t channel; /* an RGB pad's channel is its animation: 0 solid, 10 pulsing, 15 blinking (both at 1/2) */
	uint8_t note;
	uint8_t velocity;
	bool rgb;        /* a clip pad or Scene Launch pad: turned off (and its animation stopped) on channel 0 */
	uint8_t primary; /* an animated RGB pad: the colour sent first on channel 0, the other end of the animation */
};

struct mfd_vj_pads {
	bool shift;
	uint8_t flashing;  /* a bit per strip held at 100 % by its row-2 pad */
	uint8_t faded;     /* a bit per strip a row-3 pad faded out */
	uint8_t has_layer; /* a bit per strip that drives a layer the pads can act on */
	uint8_t blackout;  /* a bit per strip held at 0 % by Shift + its row-2 pad */
	uint8_t hidden;    /* a bit per strip whose source's eye is off in its scene */
	uint8_t zero;      /* a bit per strip whose layer is at 0 % */
	uint8_t pinned;    /* a bit per strip that keeps its source when the scene on air changes */
	enum mfd_vj_blend current[MFD_VJ_STRIPS];
	enum mfd_vj_blend home[MFD_VJ_STRIPS]; /* the blend each source has in its scene: Clean goes back to it */
	enum mfd_vj_blend punch[MFD_VJ_STRIPS];
	enum mfd_vj_scene_state scene[MFD_VJ_SCENES];
	bool vs_panic;      /* Record lamp on (the engine blinks it while Visual Sidechain is panicked) */
	bool take_ready;    /* Studio Mode with another scene in Preview (a fade is ready) */
	bool blackout_lamp; /* Session lamp: the engine blinks it while Program is black */
};

void mfd_vj_pads_init(struct mfd_vj_pads *p);

/* Whether this note address is one of the VJ pads or buttons. */
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
 * Feeds one control change. Returns false when it is not the Tempo or Cue Level knob (nothing changes). A Tempo detent
 * (or a fast turn's several) becomes one SPEED_STEPS action; Shift held makes each detent MFD_VJ_COARSE_STEPS steps.
 * Cue Level becomes CUE_STEPS, or FADE_STEPS with Shift held.
 */
bool mfd_vj_pads_feed_cc(struct mfd_vj_pads *p, uint8_t channel, uint8_t cc, uint8_t value, struct mfd_vj_action *out,
			 size_t max, size_t *out_count);

/* Whether this control change address is the Tempo or Cue Level knob. */
bool mfd_vj_pads_is_knob(uint8_t channel, uint8_t cc);

/* The MIDI controller went away: actions that let go of every held pad (flash and blackout off), Shift cleared. */
size_t mfd_vj_pads_release_all(struct mfd_vj_pads *p, struct mfd_vj_action *out, size_t max);

void mfd_vj_pads_set_current(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend);
/* The blend the strip's source has in its scene (read when the strip takes the source). */
void mfd_vj_pads_set_home(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend);
void mfd_vj_pads_set_has_layer(struct mfd_vj_pads *p, int strip, bool has_layer);
void mfd_vj_pads_set_faded(struct mfd_vj_pads *p, int strip, bool faded);
void mfd_vj_pads_set_hidden(struct mfd_vj_pads *p, int strip, bool hidden);
void mfd_vj_pads_set_zero(struct mfd_vj_pads *p, int strip, bool zero);
void mfd_vj_pads_set_pinned(struct mfd_vj_pads *p, int strip, bool pinned);
void mfd_vj_pads_set_scene(struct mfd_vj_pads *p, int scene, enum mfd_vj_scene_state state);
/* A saved punch mode; Normal and Darken are not punch modes and are ignored. */
void mfd_vj_pads_set_punch(struct mfd_vj_pads *p, int strip, enum mfd_vj_blend blend);

/* Every VJ LED and what it should show now, always in the same order (out holds at least MFD_VJ_MAX_LEDS). */
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
