/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/midi-types.h"
#include "core/midi-queue.h"
#include "core/midi-parser.h"
#include "core/smoother.h"
#include "core/takeover.h"
#include "core/apc40-profile.h"
#include "midi/winmm-device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_MAX_FADER_SLOTS 10
#define MFD_SOURCE_NAME_MAX 256
#define MFD_SLOT_LABEL_MAX 64
#define MFD_SLOT_ID_MAX 32
#define MFD_SLOT_KIND_MAX 16
#define MFD_FADER_EVENT_MIN_INTERVAL_SEC (1.0f / 30.0f)

struct mfd_fader_slot {
	bool enabled;
	char id[MFD_SLOT_ID_MAX];
	char kind[MFD_SLOT_KIND_MAX];
	char label[MFD_SLOT_LABEL_MAX];
	struct mfd_midi_addr addr;
	enum mfd_target_type target_type;
	char source_a_name[MFD_SOURCE_NAME_MAX];
	char source_b_name[MFD_SOURCE_NAME_MAX];
	enum mfd_crossfade_curve curve;
	bool audio_follows_video;
	bool invert;

	/* Runtime state */
	struct mfd_smoother smoother;
	struct mfd_takeover takeover;
	float last_raw_norm;
	float last_applied_val;
	float last_alpha_a;
	float last_alpha_b;
	float last_audio_mul;
	float last_audio_db;
	uint32_t output_write_count;

	/* Auto-fade state */
	bool auto_fade_active;
	float auto_fade_from;
	float auto_fade_to;
	float auto_fade_duration_sec;
	float auto_fade_elapsed_sec;

	/* Coalesced fader_changed event state (max 30 Hz per slot) */
	float sec_since_last_fader_event;
	bool fader_event_pending;
	char pending_origin[16];
};

typedef bool (*mfd_tbar_apply_fn)(float t_norm, bool *out_completed_throw, char *out_err, size_t err_size);
typedef bool (*mfd_source_exists_fn)(const char *source_name, void *user_data);

struct mfd_engine {
	struct mfd_midi_queue midi_queue;
	struct mfd_midi_parser midi_parser;
	struct mfd_winmm_device midi_dev;

	char configured_device_name[MFD_MIDI_DEV_NAME_MAX];
	float smooth_tau_ms;
	bool tbar_bidirectional_flip;

	struct mfd_fader_slot fader_slots[MFD_MAX_FADER_SLOTS];
	int learning_slot_index; /* -1 when not learning */
	bool has_last_midi;
	struct mfd_midi_decoded last_midi_decoded;

	mfd_tbar_apply_fn tbar_driver;
	char tbar_error[256];

	/* Optional test overrides when running outside live OBS */
	mfd_source_exists_fn source_exists_fn;
	void *source_exists_user_data;
	bool studio_mode_override_set;
	bool studio_mode_override;

	float health_check_accum_sec;
	bool initialized;
};

struct mfd_engine *mfd_engine_get(void);

bool mfd_engine_init(void);
void mfd_engine_shutdown(void);

void mfd_engine_set_tbar_driver(mfd_tbar_apply_fn driver_fn);

void mfd_engine_load_apc40_defaults(struct mfd_engine *eng);
bool mfd_engine_reconnect_midi(struct mfd_engine *eng, const char *device_name);
void mfd_engine_set_smooth_tau_ms(struct mfd_engine *eng, float tau_ms);
void mfd_engine_set_tbar_bidirectional_flip(struct mfd_engine *eng, bool flip);

void mfd_engine_start_learn(struct mfd_engine *eng, int slot_index);
void mfd_engine_cancel_learn(struct mfd_engine *eng);

void mfd_engine_save_global_config(struct mfd_engine *eng);
void mfd_engine_load_global_config(struct mfd_engine *eng);
bool mfd_engine_export_config_file(struct mfd_engine *eng, const char *filepath);
bool mfd_engine_import_config_file(struct mfd_engine *eng, const char *filepath);

bool mfd_engine_source_exists(const struct mfd_engine *eng, const char *source_name);
bool mfd_engine_is_studio_mode_active(const struct mfd_engine *eng);
void mfd_engine_get_slot_status(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, char *out_label,
				size_t label_size, bool *out_available, char *out_reason, size_t reason_size);

void mfd_engine_notify_fader_changed(struct mfd_engine *eng, int slot_index, const char *origin);
void mfd_engine_notify_state_changed(struct mfd_engine *eng);

/*
 * Directly applies a slot's normalized value [0.0f, 1.0f] to its bound OBS target(s).
 * Exposed so both the video tick and integration tests can invoke it deterministically.
 */
void mfd_engine_apply_slot_output(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val);
void mfd_engine_apply_slot_output_ex(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val,
				     bool force_release);

/*
 * Steps MIDI queue decoding, smoother, auto-fade ramps, and coalesced vendor events by `seconds`.
 */
void mfd_engine_tick(struct mfd_engine *eng, float seconds);

#ifdef __cplusplus
}
#endif
