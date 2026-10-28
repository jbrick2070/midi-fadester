/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <util/threading.h>
#include "core/midi-types.h"
#include "core/midi-queue.h"
#include "core/midi-parser.h"
#include "core/smoother.h"
#include "core/takeover.h"
#include "core/tbar-logic.h"
#include "core/apc40-profile.h"
#include "midi/winmm-device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_MAX_FADER_SLOTS 10
#define MFD_MAX_CAL_ENTRIES 64
#define MFD_PROFILE_NAME_MAX 32
#define MFD_SOURCE_NAME_MAX 256
#define MFD_SLOT_LABEL_MAX 64
#define MFD_SLOT_ID_MAX 32
#define MFD_SLOT_KIND_MAX 16
#define MFD_FADER_EVENT_MIN_INTERVAL_SEC (1.0f / 30.0f)

#define MFD_CATALOG_MAX_SCENES 48
#define MFD_CATALOG_MAX_SCENE_LAYERS 48
#define MFD_CATALOG_MAX_AUDIO_SOURCES 64

struct mfd_persisted_cal_entry {
	bool in_use;
	char profile_name[MFD_PROFILE_NAME_MAX];
	struct mfd_midi_addr addr;
	struct mfd_control_calibration cal;
};

struct mfd_fader_slot {
	bool enabled;
	char id[MFD_SLOT_ID_MAX];
	char kind[MFD_SLOT_KIND_MAX];
	char label[MFD_SLOT_LABEL_MAX];
	struct mfd_midi_addr addr;
	struct mfd_control_calibration cal;
	enum mfd_target_type target_type;
	char scene_name[MFD_SOURCE_NAME_MAX];
	char source_a_name[MFD_SOURCE_NAME_MAX];
	char source_b_name[MFD_SOURCE_NAME_MAX];
	enum mfd_crossfade_curve curve;
	bool audio_follows_video;
	bool invert;

	/* Runtime state */
	struct mfd_smoother smoother;
	struct mfd_takeover takeover;
	enum mfd_tbar_state tbar_state;
	bool tbar_midi_active;
	bool tbar_from_idle_band;
	char resolved_scene_name[MFD_SOURCE_NAME_MAX];
	bool a_is_above_b;
	bool cached_available;
	char cached_reason[160];
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

struct mfd_ui_scene_entry {
	char name[MFD_SOURCE_NAME_MAX];
	size_t layer_count;
	bool layers_truncated;
	char layers[MFD_CATALOG_MAX_SCENE_LAYERS][MFD_SOURCE_NAME_MAX]; /* bottom -> top, deduplicated */
	bool layer_has_audio[MFD_CATALOG_MAX_SCENE_LAYERS];
};

struct mfd_ui_audio_entry {
	char name[MFD_SOURCE_NAME_MAX];
	float db; /* finite dB or -INFINITY via mfd_mul_to_db */
};

struct mfd_ui_catalog {
	uint64_t config_topology_rev;
	bool scenes_truncated;
	bool audio_truncated;
	size_t scene_count;
	struct mfd_ui_scene_entry scenes[MFD_CATALOG_MAX_SCENES];
	size_t audio_count;
	struct mfd_ui_audio_entry audio_sources[MFD_CATALOG_MAX_AUDIO_SOURCES];
};

struct mfd_setup_session {
	bool active;
	int slot_index;         /* 0..9 */
	uint64_t base_slot_rev; /* captured from eng->slot_config_rev[slot_index] */
	struct mfd_midi_addr candidate_addr;
	struct mfd_control_calibration candidate_cal;
	bool candidate_invert; /* single authoritative Reverse direction owner during Set up */
	bool learning;
	float candidate_raw_norm; /* 0..1 calibrated meter for Step 3 */
	bool has_candidate_input;
};

struct mfd_setup_candidate {
	int slot_index;
	bool enabled;
	enum mfd_target_type target_type;
	char scene_name[MFD_SOURCE_NAME_MAX];
	char source_a_name[MFD_SOURCE_NAME_MAX];
	char source_b_name[MFD_SOURCE_NAME_MAX];
	enum mfd_crossfade_curve curve;
	bool audio_follows_video;
	bool apply_tbar_bidir;
	bool tbar_bidirectional_flip;
};

struct mfd_slot_config_edit {
	bool enabled;
	enum mfd_target_type target_type;
	bool preserve_scene;
	char scene_name[MFD_SOURCE_NAME_MAX];
	char source_a_name[MFD_SOURCE_NAME_MAX];
	char source_b_name[MFD_SOURCE_NAME_MAX];
	enum mfd_crossfade_curve curve;
	bool audio_follows_video;
	bool invert;
};

struct mfd_ui_snapshot {
	uint64_t config_topology_rev;
	uint64_t telemetry_rev;
	uint64_t slot_config_rev[MFD_MAX_FADER_SLOTS];
	char configured_device_name[MFD_MIDI_DEV_NAME_MAX];
	char opened_device_name[MFD_MIDI_DEV_NAME_MAX];
	bool midi_connected;
	enum mfd_winmm_error_kind midi_error_kind;
	char midi_last_error[MFD_MIDI_ERROR_MAX];
	char active_profile_name[MFD_PROFILE_NAME_MAX];
	float smooth_tau_ms;
	bool tbar_bidirectional_flip;
	bool frontend_finished_loading;
	bool studio_mode_active;
	char preview_scene_name[MFD_SOURCE_NAME_MAX];
	char program_scene_name[MFD_SOURCE_NAME_MAX];
	bool first_run_offer_pending;
	int learning_slot_index;
	bool has_last_midi;
	struct mfd_midi_decoded last_midi_decoded;
	int last_moved_slot_index;
	uint32_t midi_dropped_count;
	struct mfd_setup_session setup;
	struct mfd_fader_slot slots[MFD_MAX_FADER_SLOTS];
};

struct mfd_pending_fader_emit {
	bool pending;
	char slot_id[MFD_SLOT_ID_MAX];
	float value;
	char origin[16];
};

typedef void (*mfd_ui_notify_fn)(void *user_data);

typedef bool (*mfd_tbar_apply_fn)(enum mfd_tbar_state state, float t_norm, bool force_complete, bool force_abort,
				  bool in_target_rail_zone, bool in_origin_rail_zone, bool in_start_deadband,
				  enum mfd_tbar_state *out_next_state, bool *out_completed_throw,
				  bool *out_aborted_throw, char *out_err, size_t err_size);
typedef void (*mfd_tbar_unlatch_fn)(void);
typedef bool (*mfd_source_exists_fn)(const char *source_name, void *user_data);

struct mfd_scene_graph_ops {
	bool (*source_exists)(const char *source_name, void *user_data);
	bool (*scene_exists)(const char *scene_name, void *user_data);
	bool (*source_has_audio)(const char *source_name, void *user_data);
	int (*scene_item_stack_index)(const char *scene_name, const char *source_name, void *user_data);
	bool (*find_scene_with_layer)(const char *source_name, const char *exclude_scene, char *out_scene,
				      size_t out_size, void *user_data);
	bool (*find_scene_with_pair)(const char *source_a, const char *source_b, char *out_scene, size_t out_size,
				     int *out_idx_a, int *out_idx_b, void *user_data);
	void (*apply_opacity)(const char *source_name, float alpha_01, bool afv, void *user_data);
	void (*apply_volume)(const char *source_name, float mul, void *user_data);
	void (*fill_catalog)(struct mfd_ui_catalog *out, void *user_data);
	void (*set_studio_mode)(bool enabled, void *user_data);
};

struct mfd_engine {
	pthread_mutex_t state_mutex;
	bool state_mutex_initialized;
	uint32_t lock_depth;
	bool pending_ui_notify;
	bool pending_state_event;
	bool pending_config_save;
	struct mfd_pending_fader_emit pending_fader_emits[MFD_MAX_FADER_SLOTS];

	uint64_t config_topology_rev;
	uint64_t telemetry_rev;
	uint64_t slot_config_rev[MFD_MAX_FADER_SLOTS];

	struct mfd_midi_queue midi_queue;
	struct mfd_midi_parser midi_parser;
	struct mfd_winmm_device midi_dev;

	char configured_device_name[MFD_MIDI_DEV_NAME_MAX];
	char active_profile_name[MFD_PROFILE_NAME_MAX];
	float smooth_tau_ms;
	bool tbar_bidirectional_flip;

	bool first_run_offer_dismissed;
	bool frontend_finished_loading;
	bool apc40_controller_detected;
	bool cached_studio_mode;
	char cached_preview_scene[MFD_SOURCE_NAME_MAX];
	char cached_program_scene[MFD_SOURCE_NAME_MAX];

	struct mfd_persisted_cal_entry cal_entries[MFD_MAX_CAL_ENTRIES];
	struct mfd_fader_slot fader_slots[MFD_MAX_FADER_SLOTS];
	struct mfd_setup_session setup_session;
	int learning_slot_index; /* -1 when not learning */
	bool has_last_midi;
	struct mfd_midi_decoded last_midi_decoded;
	int last_moved_slot_index;

	mfd_ui_notify_fn ui_notify_fn;
	void *ui_notify_user_data;

	mfd_tbar_apply_fn tbar_driver;
	mfd_tbar_unlatch_fn tbar_unlatch;
	char tbar_error[256];

	/* Optional test overrides when running outside live OBS */
	const struct mfd_scene_graph_ops *scene_graph_ops;
	void *scene_graph_user_data;
	mfd_source_exists_fn source_exists_fn;
	void *source_exists_user_data;
	bool studio_mode_override_set;
	bool studio_mode_override;

	bool signals_connected;
	float health_check_accum_sec;
	bool initialized;
};

struct mfd_engine *mfd_engine_get(void);

bool mfd_engine_init(void);
void mfd_engine_shutdown(void);

bool mfd_engine_init_mutex(struct mfd_engine *eng);
void mfd_engine_destroy_mutex(struct mfd_engine *eng);
void mfd_engine_lock(struct mfd_engine *eng);
void mfd_engine_unlock(struct mfd_engine *eng);
void mfd_engine_mark_ui_dirty(struct mfd_engine *eng, bool is_topology);
void mfd_engine_bump_slot_rev(struct mfd_engine *eng, int slot_index);

void mfd_engine_set_ui_notify_fn(struct mfd_engine *eng, mfd_ui_notify_fn fn, void *user_data);
void mfd_engine_flush_pending_ui_work(struct mfd_engine *eng);

struct mfd_ui_catalog *mfd_ui_catalog_create(void);
void mfd_ui_catalog_destroy(struct mfd_ui_catalog *cat);
void mfd_engine_get_ui_catalog(struct mfd_engine *eng, struct mfd_ui_catalog *out);
void mfd_engine_get_ui_snapshot(struct mfd_engine *eng, struct mfd_ui_snapshot *out);

void mfd_engine_set_test_frontend_state(struct mfd_engine *eng, bool finished_loading, bool studio_mode,
					const char *preview_scene, const char *program_scene, bool apc40_detected);
void mfd_engine_request_studio_mode(struct mfd_engine *eng, bool enabled);

void mfd_engine_begin_setup_session(struct mfd_engine *eng, int slot_index);
void mfd_engine_start_setup_learn(struct mfd_engine *eng);
void mfd_engine_cancel_setup_learn(struct mfd_engine *eng);
void mfd_engine_set_setup_invert(struct mfd_engine *eng, bool invert);
void mfd_engine_cancel_setup_session(struct mfd_engine *eng);
bool mfd_engine_unassign_slot_preserve_addr(struct mfd_engine *eng, int slot_index);
size_t mfd_engine_find_addr_owners(struct mfd_engine *eng, const struct mfd_midi_addr *addr, int exclude_slot,
				   int *out_slots, size_t max_slots);
bool mfd_engine_validate_setup_candidate(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					 char *out_reason, size_t reason_size);
bool mfd_engine_commit_setup_session(struct mfd_engine *eng, const struct mfd_setup_candidate *cand, char *out_err,
				     size_t err_size);

void mfd_engine_accept_first_run_usual_layout(struct mfd_engine *eng);
void mfd_engine_decline_first_run_usual_layout(struct mfd_engine *eng);

bool mfd_engine_update_slot_config(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit);
void mfd_engine_set_slot_label(struct mfd_engine *eng, int slot_index, const char *label);
void mfd_engine_clear_slot_binding(struct mfd_engine *eng, int slot_index);
bool mfd_engine_set_slot_value(struct mfd_engine *eng, int slot_index, float value_01, bool release, const char *origin,
			       char *out_err, size_t err_size);
bool mfd_engine_finish_tbar_throw(struct mfd_engine *eng, int slot_index);

void mfd_engine_set_tbar_driver(mfd_tbar_apply_fn driver_fn);
void mfd_engine_set_tbar_unlatch_fn(mfd_tbar_unlatch_fn unlatch_fn);
void mfd_engine_unlatch_tbar(struct mfd_engine *eng);
void mfd_engine_on_external_tbar_reset(struct mfd_engine *eng);
void mfd_engine_poll_midi_health_on_ui_thread(struct mfd_engine *eng);

void mfd_engine_load_apc40_defaults(struct mfd_engine *eng);
void mfd_engine_reset_slot_mode_state(struct mfd_engine *eng, struct mfd_fader_slot *slot,
				      enum mfd_target_type new_target);
void mfd_engine_set_slot_calibration(struct mfd_engine *eng, int slot_index, int cal_min, int cal_max);
bool mfd_engine_lookup_persisted_calibration(const struct mfd_engine *eng, const char *profile_name,
					     const struct mfd_midi_addr *addr, struct mfd_control_calibration *out_cal);
bool mfd_engine_reconnect_midi(struct mfd_engine *eng, const char *device_name);
void mfd_engine_set_smooth_tau_ms(struct mfd_engine *eng, float tau_ms);
void mfd_engine_set_tbar_bidirectional_flip(struct mfd_engine *eng, bool flip);

void mfd_engine_start_learn(struct mfd_engine *eng, int slot_index);
void mfd_engine_cancel_learn(struct mfd_engine *eng);

bool mfd_engine_save_global_config(struct mfd_engine *eng);
void mfd_engine_load_global_config(struct mfd_engine *eng);
bool mfd_engine_export_config_file(struct mfd_engine *eng, const char *filepath);
bool mfd_engine_import_config_file(struct mfd_engine *eng, const char *filepath);

bool mfd_engine_source_exists(const struct mfd_engine *eng, const char *source_name);
bool mfd_engine_scene_exists(const struct mfd_engine *eng, const char *scene_name);
bool mfd_engine_source_has_audio(const struct mfd_engine *eng, const char *source_name);
bool mfd_engine_find_other_scene_with_layer(const struct mfd_engine *eng, const char *source_name,
					    const char *exclude_scene, char *out_scene, size_t out_size);
bool mfd_engine_is_studio_mode_active(const struct mfd_engine *eng);
bool mfd_engine_is_slot_writable(const struct mfd_engine *eng, const struct mfd_fader_slot *slot);
void mfd_engine_reevaluate_slots(struct mfd_engine *eng, bool emit_if_changed);
void mfd_engine_handle_source_rename(struct mfd_engine *eng, const char *prev_name, const char *new_name);
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
void mfd_engine_apply_tbar_abort(struct mfd_engine *eng, struct mfd_fader_slot *slot);

/*
 * Steps MIDI queue decoding, smoother, auto-fade ramps, and coalesced vendor events by `seconds`.
 */
void mfd_engine_tick(struct mfd_engine *eng, float seconds);

#ifdef __cplusplus
}
#endif
