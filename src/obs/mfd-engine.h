/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <util/threading.h>
#include <callback/signal.h>
#include "core/midi-types.h"
#include "core/midi-queue.h"
#include "core/midi-parser.h"
#include "core/smoother.h"
#include "core/takeover.h"
#include "core/tbar-logic.h"
#include "core/filter-math.h"
#include "core/apc40-profile.h"
#include "core/slot-messages.h"
#include "core/vj-pads.h"
#include "midi/winmm-device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_MAX_FADER_SLOTS 18
#define MFD_MAX_CAL_ENTRIES 64
#define MFD_PROFILE_NAME_MAX 32
#define MFD_SOURCE_NAME_MAX 256
#define MFD_SLOT_LABEL_MAX 64
#define MFD_SLOT_ID_MAX 32
#define MFD_SLOT_KIND_MAX 16
#define MFD_FILTER_PROP_NAME_MAX 128
#define MFD_FADER_EVENT_MIN_INTERVAL_SEC (1.0f / 30.0f)

#define MFD_CATALOG_MAX_SCENES 48
#define MFD_CATALOG_MAX_SCENE_LAYERS 48
#define MFD_CATALOG_MAX_AUDIO_SOURCES 64
#define MFD_CATALOG_MAX_FILTER_HOSTS 128
#define MFD_HOST_MAX_FILTERS 32
#define MFD_FILTER_MAX_PROPS 64

struct mfd_persisted_cal_entry {
	bool in_use;
	char profile_name[MFD_PROFILE_NAME_MAX];
	struct mfd_midi_addr addr;
	struct mfd_control_calibration cal;
};

enum mfd_mix_delivery {
	MFD_MIX_IDLE,
	MFD_MIX_WAITING,
	MFD_MIX_CONFIRMED,
	MFD_MIX_DECLINED,
	MFD_MIX_UNAVAILABLE,
	MFD_MIX_NO_REPLY,
	MFD_MIX_CONFLICT
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
	char filter_name[MFD_SOURCE_NAME_MAX];
	char filter_prop[MFD_FILTER_PROP_NAME_MAX];
	double filter_from_val;
	double filter_to_val;
	bool filter_bypass_at_zero;
	bool filter_pre_bypass_enabled;
	bool restore_persisted_pre_bypass;
	enum mfd_crossfade_curve curve;
	bool audio_follows_video;
	bool invert;
	/* APC40 mkII VJ buttons: a row-2 pad holds this layer at 100 % (flash) or, with Shift, 0 % (blackout); fader moves
	 * update the value but not OBS meanwhile. */
	bool vj_flash_held;
	/* Filled from the scene on air (strips 1..8) and refilled when Program changes, until the user sets the strip or
	 * pins it. Saved. */
	bool auto_follow;

	/* Cached filter property metadata (refreshed on signals, never on video tick) */
	bool filter_prop_valid;
	bool filter_prop_is_int;
	double filter_prop_min;
	double filter_prop_max;
	double filter_prop_step;
	char filter_prop_suffix[32];
	char filter_prop_label[MFD_FILTER_PROP_NAME_MAX];

	/* Runtime filter write & reset tracking */
	bool has_last_written_mapped;
	int64_t last_written_int;
	double last_written_float;
	bool writing_filter_settings;
	bool auto_fade_is_reset_fx;
	bool pending_clean_apply_on_avail;

	/* Runtime state */
	struct mfd_smoother smoother;
	struct mfd_takeover takeover;
	enum mfd_tbar_state tbar_state;
	bool tbar_midi_active;
	bool tbar_from_idle_band;
	char resolved_scene_name[MFD_SOURCE_NAME_MAX];
	bool a_is_above_b;
	bool cached_available;
	char cached_reason[160];          /* English, for logs and the API */
	struct mfd_msg cached_reason_msg; /* code + arguments; compared for changes, worded by the UI */
	float last_raw_norm;
	float last_applied_val;
	float last_alpha_a;
	float last_alpha_b;
	float last_audio_mul;
	float last_audio_db;
	char mix_request_id[40];
	enum mfd_mix_delivery mix_delivery;
	double mix_reported_db;
	char mix_reason[80];
	uint64_t mix_sent_ns;
	bool mix_pending; /* this slot's latest mix request, not yet sent */
	float mix_pending_db;
	uint64_t mix_pending_rev;
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
	bool layer_has_video[MFD_CATALOG_MAX_SCENE_LAYERS];
};

struct mfd_ui_audio_entry {
	char name[MFD_SOURCE_NAME_MAX];
	float db; /* finite dB or -INFINITY via mfd_mul_to_db */
};

struct mfd_ui_filter_host_summary {
	char name[MFD_SOURCE_NAME_MAX];
	bool is_scene;
	uint32_t filter_count;
};

struct mfd_ui_catalog {
	uint64_t config_topology_rev;
	bool scenes_truncated;
	bool audio_truncated;
	bool filter_hosts_truncated;
	size_t scene_count;
	struct mfd_ui_scene_entry scenes[MFD_CATALOG_MAX_SCENES];
	size_t audio_count;
	struct mfd_ui_audio_entry audio_sources[MFD_CATALOG_MAX_AUDIO_SOURCES];
	size_t filter_host_count;
	struct mfd_ui_filter_host_summary filter_hosts[MFD_CATALOG_MAX_FILTER_HOSTS];
};

struct mfd_filter_prop_info {
	char name[MFD_FILTER_PROP_NAME_MAX];
	char label[MFD_FILTER_PROP_NAME_MAX];
	bool is_int;
	double min_val;
	double max_val;
	double step;
	char suffix[32];
	double current_val;
};

struct mfd_filter_entry_info {
	char name[MFD_SOURCE_NAME_MAX];
	char type_id[128];
	bool enabled;
	bool props_truncated;
	size_t prop_count;
	struct mfd_filter_prop_info props[MFD_FILTER_MAX_PROPS];
};

struct mfd_source_filter_catalog {
	char source_name[MFD_SOURCE_NAME_MAX];
	bool source_exists;
	bool is_scene;
	bool filters_truncated;
	size_t filter_count;
	struct mfd_filter_entry_info filters[MFD_HOST_MAX_FILTERS];
};

struct mfd_setup_session {
	bool active;
	int slot_index;         /* 0..17 */
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
	char filter_name[MFD_SOURCE_NAME_MAX];
	char filter_prop[MFD_FILTER_PROP_NAME_MAX];
	double filter_from_val;
	double filter_to_val;
	bool filter_bypass_at_zero;
	enum mfd_crossfade_curve curve;
	bool audio_follows_video;
	bool apply_tbar_bidir;
	bool tbar_bidirectional_flip;
};

struct mfd_slot_config_edit {
	bool enabled;
	enum mfd_target_type target_type;
	bool preserve_scene;
	/* Built from the dialog's last snapshot with the layer untouched (a curve, invert or on/off change): a strip that
	 * follows the scene on air keeps its current source and keeps following. */
	bool keep_layer;
	char scene_name[MFD_SOURCE_NAME_MAX];
	char source_a_name[MFD_SOURCE_NAME_MAX];
	char source_b_name[MFD_SOURCE_NAME_MAX];
	char filter_name[MFD_SOURCE_NAME_MAX];
	char filter_prop[MFD_FILTER_PROP_NAME_MAX];
	bool has_filter_range;
	double filter_from_val;
	double filter_to_val;
	bool filter_bypass_at_zero;
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
	enum mfd_winmm_operation midi_error_operation;
	uint32_t midi_error_code;
	char midi_error_device_name[MFD_MIDI_DEV_NAME_MAX];
	char midi_last_error[MFD_MIDI_ERROR_MAX];
	char active_profile_name[MFD_PROFILE_NAME_MAX];
	enum mfd_controller_profile active_profile;
	enum mfd_profile_support_tier active_profile_tier;
	float smooth_tau_ms;
	bool tbar_bidirectional_flip;
	bool frontend_finished_loading;
	bool studio_mode_active;
	char preview_scene_name[MFD_SOURCE_NAME_MAX];
	char program_scene_name[MFD_SOURCE_NAME_MAX];
	bool first_run_offer_pending;
	bool stream_deck_companion_dismissed;
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
	char display_value[64];
	char origin[16];
};

struct mfd_filter_bypass_lease {
	bool in_use;
	char source_name[MFD_SOURCE_NAME_MAX];
	char filter_name[MFD_SOURCE_NAME_MAX];
	bool pre_mfd_enabled;
	bool last_mfd_enabled;
	bool has_written_enable;
	bool writing_enable; /* Midi Fadester is writing writing_value: an "enable" signal with that value is its echo */
	bool writing_value;
};

struct mfd_tracked_signal_entry;

typedef void (*mfd_ui_notify_fn)(void *user_data);

typedef bool (*mfd_tbar_apply_fn)(enum mfd_tbar_state state, float t_norm, bool force_complete, bool force_abort,
				  bool in_target_rail_zone, bool in_origin_rail_zone, bool in_start_deadband,
				  enum mfd_tbar_state *out_next_state, bool *out_completed_throw,
				  bool *out_aborted_throw, char *out_err, size_t err_size);
typedef void (*mfd_tbar_unlatch_fn)(void);
typedef bool (*mfd_source_exists_fn)(const char *source_name, void *user_data);
typedef void (*mfd_filter_host_cb)(const char *source_name, bool is_scene, void *param);
typedef void (*mfd_source_filter_cb)(const char *filter_name, const char *type_id, bool enabled, void *param);
typedef void (*mfd_numeric_prop_cb)(const struct mfd_filter_prop_info *info, void *param);

/* An APC40 desk command that has to run in OBS's UI thread, with no Midi Fadester or libobs lock held: queued by the
 * pads and carried out by mfd_engine_flush_pending_ui_work. */
enum mfd_vj_command_kind {
	MFD_VJ_CMD_PREVIEW = 0, /* scene to Preview */
	MFD_VJ_CMD_PROGRAM,     /* scene straight to Program, with the current transition */
	MFD_VJ_CMD_TAKE,        /* Preview to Program with the transition */
	MFD_VJ_CMD_SET_VISIBLE, /* scene, source, arg 1 shown / 0 hidden */
	MFD_VJ_CMD_VS_PANIC,    /* Visual Sidechain panic, or restore when panicked */
	MFD_VJ_CMD_VS_TAP,      /* Visual Sidechain tap tempo */
	MFD_VJ_CMD_VS_LOCK,     /* Visual Sidechain BPM lock toggled */
	MFD_VJ_CMD_VS_RESYNC,   /* Visual Sidechain downbeat resync */
	MFD_VJ_CMD_TRANSITION,  /* arg: transition length change in ms */
	MFD_VJ_CMD_SCENE_FADE,  /* scene (in Preview), arg: top-row pad 1..7 (32 / 2^(arg-1) beats), -1 a cut */
};

#define MFD_VJ_CMD_MAX 16

struct mfd_vj_command {
	enum mfd_vj_command_kind kind;
	int arg;
	int strip; /* the pad column it came from (show / hide), -1 otherwise */
	char scene[MFD_SOURCE_NAME_MAX];
	char source[MFD_SOURCE_NAME_MAX];
};

/* What "Fill empty faders from the scene on air" did. */
enum mfd_fill_status {
	MFD_FILL_OK = 0,          /* at least one strip filled */
	MFD_FILL_NO_SCENE,        /* no scene on air, or the scene asked for is not there */
	MFD_FILL_NO_EMPTY_STRIPS, /* every strip 1..8 has a job of its own */
	MFD_FILL_NO_VIDEO_LAYERS, /* the scene has no video source left for them */
};

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

	/* Optional filter callbacks for headless filter_param & get_catalog testing */
	void (*enum_filter_hosts)(mfd_filter_host_cb cb, void *cb_param, void *user_data);
	bool (*enum_source_filters)(const char *source_name, bool *out_is_scene, mfd_source_filter_cb cb,
				    void *cb_param, void *user_data);
	obs_properties_t *(*create_filter_properties)(const char *source_name, const char *filter_name,
						      void *user_data);
	obs_data_t *(*get_filter_settings)(const char *source_name, const char *filter_name, void *user_data);
	void (*update_filter_settings)(const char *source_name, const char *filter_name, obs_data_t *settings,
				       void *user_data);
	bool (*get_filter_enabled)(const char *source_name, const char *filter_name, bool *out_enabled,
				   void *user_data);
	void (*set_filter_enabled)(const char *source_name, const char *filter_name, bool enabled, void *user_data);

	/* Optional: the name of the source on an OBS output channel (1 = Desktop Audio), so the first-run layout finds
	 * desktop audio whatever language OBS names it in. */
	bool (*get_output_source_name)(uint32_t channel, char *out_name, size_t out_size, void *user_data);

	/* Optional: read a source's volume multiplier (headless tests). */
	bool (*get_volume)(const char *source_name, float *out_mul, void *user_data);

	/* Optional: check whether Midi Fadester's internal opacity filter is attached (headless tests). */
	bool (*source_has_internal_opacity)(const char *source_name, void *user_data);

	/* Optional: read / write a scene item's blend mode (an obs_blending_type value; headless tests). */
	bool (*get_blend_mode)(const char *scene_name, const char *source_name, int *out_mode, void *user_data);
	bool (*set_blend_mode)(const char *scene_name, const char *source_name, int mode, void *user_data);

	/* Optional: whether a scene item is shown (its eye; headless tests). */
	bool (*get_item_visible)(const char *scene_name, const char *source_name, bool *out_visible, void *user_data);
	/* Optional: carry out an APC40 desk command instead of calling OBS (headless tests). */
	void (*run_vj_command)(const struct mfd_vj_command *cmd, void *user_data);
};

/*
 * Scene-graph changes reported by libobs signals. libobs emits them while it may hold its own locks (the sources
 * mutex, a scene's lock: obs_sceneitem_remove fires item_remove inside full_lock), so the callbacks never wait for the
 * engine lock: they queue the change here, and mfd_engine_process_scene_graph_changes() applies it later from a
 * context that holds no libobs lock (the engine's video tick, a frontend event, the UI thread).
 */
enum mfd_graph_change_kind {
	MFD_GRAPH_RESCAN = 0,      /* a source or filter appeared: connect signals, then re-check */
	MFD_GRAPH_RECHECK,         /* scene items or a filter's properties changed: re-check */
	MFD_GRAPH_SOURCE_GONE,     /* a: the removed or destroyed source's name; flag: it was a filter or transition */
	MFD_GRAPH_SOURCE_RENAMED,  /* a -> b */
	MFD_GRAPH_FILTER_REMOVED,  /* a: host, b: filter */
	MFD_GRAPH_FILTER_RENAMED,  /* a: host, b -> c */
	MFD_GRAPH_FILTER_ENABLED,  /* a: host, b: filter, flag: enabled */
	MFD_GRAPH_FILTER_SETTINGS, /* a: host, b: filter */
};

/*
 * Queued changes kept between two drains. Settings and enable changes fold by (host, filter), keeping the last value,
 * but never across a later rename or removal. Past this many entries further settings changes are dropped and a full
 * rescan and re-check stand in. Enable changes, renames, removals and removed filters are never dropped: the queue
 * grows past the cap for them (removals also fold by name).
 */
#define MFD_GRAPH_CHANGE_MAX 4096

struct mfd_graph_change {
	enum mfd_graph_change_kind kind;
	bool flag;
	char a[MFD_SOURCE_NAME_MAX];
	char b[MFD_SOURCE_NAME_MAX];
	char c[MFD_SOURCE_NAME_MAX];
};

struct mfd_engine {
	pthread_mutex_t state_mutex;
	bool state_mutex_initialized;
	uint32_t lock_depth;
	volatile long lock_owner; /* thread holding state_mutex, 0 when free (written only by the holder) */

	/* Queued scene-graph changes, guarded by graph_mutex: a leaf lock, never held across any other lock or call
	 * into libobs or Midi Fadester. Rescan and re-check are flags; the name-bearing changes keep their order. */
	pthread_mutex_t graph_mutex;
	bool graph_mutex_initialized;
	bool graph_rescan;
	bool graph_recheck;
	bool graph_overflowed;
	struct mfd_graph_change *graph_items;
	size_t graph_count;
	size_t graph_cap;
	bool graph_draining;             /* written only by the engine-lock holder */
	bool frontend_callbacks_removed; /* UI thread: taken away at OBS_FRONTEND_EVENT_EXIT, or at unload */
	volatile long graph_pending; /* 1 while anything is queued: a lock-free hint so an idle tick takes no lock */
	bool pending_ui_notify;
	bool pending_state_event;
	bool pending_config_save;
	struct mfd_pending_fader_emit pending_fader_emits[MFD_MAX_FADER_SLOTS];
	/* Paces mix requests; each mix-master slot keeps its own latest one. The OBS call runs outside the slot lock. */
	float mix_send_wait;
	char mix_provider_id[64];
	char last_mix_sent_id[64];
	uint64_t last_mix_sent_ns;
	bool mix_provider_conflict;

	uint64_t config_topology_rev;
	uint64_t telemetry_rev;
	uint64_t slot_config_rev[MFD_MAX_FADER_SLOTS];

	struct mfd_midi_queue midi_queue;
	struct mfd_midi_parser midi_parser;
	struct mfd_winmm_device midi_dev;
	bool midi_disconnect_handled;

	char configured_device_name[MFD_MIDI_DEV_NAME_MAX];
	char active_profile_name[MFD_PROFILE_NAME_MAX];
	float smooth_tau_ms;
	bool tbar_bidirectional_flip;

	bool first_run_offer_dismissed;
	bool stream_deck_companion_dismissed;
	bool frontend_finished_loading;
	bool apc40_controller_detected;
	bool cached_studio_mode;
	char cached_preview_scene[MFD_SOURCE_NAME_MAX];
	char cached_program_scene[MFD_SOURCE_NAME_MAX];
	/* The scene a Scene Launch press sent to Preview, until that command has run on the UI thread: a second press
	 * before then compares with it, not with the Preview OBS still shows. */
	char vj_preview_asked[MFD_SOURCE_NAME_MAX];
	/* The first scenes in OBS's Scenes list, top first: the APC40's Scene Launch buttons. */
	char cached_scenes[MFD_VJ_SCENES][MFD_SOURCE_NAME_MAX];
	int cached_scene_count;
	bool scene_list_override_set; /* tests: the list above is set by hand */

	struct mfd_persisted_cal_entry cal_entries[MFD_MAX_CAL_ENTRIES];
	struct mfd_fader_slot fader_slots[MFD_MAX_FADER_SLOTS];
	struct mfd_filter_bypass_lease filter_leases[MFD_MAX_FADER_SLOTS];
	struct mfd_tracked_signal_entry *tracked_signals_head;
	bool pruning_tracked_signals; /* written only by the engine-lock holder: a prune is walking the list */
	bool prune_tracked_again;     /* a prune was asked for while one was walking: walk once more */
	const char *removing_filter_host;
	const char *removing_filter_name;
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

	/* APC40 mkII VJ buttons (core/vj-pads.h): blend punch and pick, flash, fades, Clean FX. On unless turned off. */
	bool vj_disabled;
	/* The Fadester Desk dock shows its video speed rows (VJster through Fadester Companion). Off on a new install:
	 * they do nothing for most people. A settings file written before this key keeps them on, so nothing disappears
	 * from a dock that is in use when the plugin is updated. */
	bool desk_video_controls;
	int vj_fade_ms; /* 0 = the default, 1000 ms */
	struct mfd_vj_pads vj;
	float vj_fade_restore[MFD_VJ_STRIPS];  /* the level a Clip Stop faded out from */
	uint16_t vj_led_sent[MFD_VJ_MAX_LEDS]; /* channel << 8 | velocity last sent per LED index; 0xFFFF = unknown */
	bool vj_refresh_pending;               /* re-read the strips' layers and blend modes on the next tick */
	bool vj_was_active;                    /* the pads applied last tick (to turn the lights off when they stop) */
	bool vj_state_dirty;                   /* a pad changed something: tell obs-websocket clients once */
	char vj_said[MFD_VJ_STRIPS][96];       /* the last "why not" line per strip, said once */
	float vj_blink_sec;                    /* Record blinks in software: the desk's Record lamp is on or off only */
	int cached_transition_ms;              /* OBS's transition length, read on the UI thread (Blackout's fade) */
	char vj_home_key[MFD_VJ_STRIPS][2 * MFD_SOURCE_NAME_MAX + 2]; /* scene + source the home blend was read for */
	/* Punched sources saved with the scene collection (home blend per scene + source), put back once it has loaded:
	 * OBS saves the scenes before Midi Fadester hears that it is closing, and an autosave or a crash keeps a punch. */
	char vj_load_restore_key[MFD_VJ_STRIPS][2 * MFD_SOURCE_NAME_MAX + 2];
	enum mfd_vj_blend vj_load_restore_home[MFD_VJ_STRIPS];
	int vj_load_restore_count;
	float vj_vs_poll_sec;                  /* time since Visual Sidechain's panic state was last asked */
	bool vj_vs_poll_pending;               /* ask it from the UI thread */
	uint32_t vj_hold_epoch[MFD_VJ_STRIPS]; /* bumps on every change of a strip's flash / blackout hold (desk dock) */
	uint32_t vj_cmd_drops;                 /* desk commands dropped on a full queue (the dock reports them) */
	uint8_t vj_hold_seen;                  /* the hold bits vj_hold_epoch last saw */
	bool desk_visible;                     /* the Fadester Desk dock is on screen: Visual Sidechain is polled */
	float desk_frame_sec;                  /* time since the dock was last asked to repaint */
	bool vs_panic_known;      /* Visual Sidechain has answered get_safety or panic since it last failed */
	bool vs_last_failed;      /* the last Visual Sidechain command got no answer */
	uint8_t vj_punched;       /* strips whose blend was off its own at this press, read before the feed */
	uint8_t vj_eye_hold;      /* strips whose eye change is queued: not re-read until it is done */
	bool vj_autofill_done;    /* the eight empty strips were filled from the scene once (saved) */
	bool vj_autofill_pending; /* try that fill from the UI thread (the desk opened, pads turned on) */
	bool vj_follow_pending;   /* refill following strips from Program when the T-bar is at rest */
	bool vs_panic;            /* Visual Sidechain said it is panicked */
	int vj_cue_ms;            /* Cue Level turns not sent yet, in ms of transition length */
	struct mfd_vj_command vj_cmds[MFD_VJ_CMD_MAX];
	int vj_cmd_count;
	/* APC40 video speed for Fadester Companion: Tempo / Nudge steps added up, and Tap Tempo / Play pressed, sent as
	 * vendor events by mfd_engine_unlock after the lock is let go (steps at most every 50 ms). */
#define MFD_VJ_SPEED_RESET_BIT 1U
#define MFD_VJ_SPEED_PLAY_BIT 2U
#define MFD_VJ_SPEED_SET_BIT 4U          /* a preset speed from the dock: vj_speed_set_rate */
#define MFD_VJ_SPEED_MAX_STEPS 80        /* 0.25x..4x is 75 steps of 0.05x */
#define MFD_VJ_SPEED_SEND_NS 50000000ULL /* 20 sends a second at most */
	double vj_speed_set_rate;        /* the preset the dock asked for (MFD_VJ_SPEED_SET_BIT) */
	int vj_speed_steps;
	unsigned vj_speed_actions; /* MFD_VJ_SPEED_RESET_BIT | MFD_VJ_SPEED_PLAY_BIT */
	uint64_t vj_speed_sent_ns;

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
bool mfd_engine_held_by_current_thread(const struct mfd_engine *eng);
/* Called from libobs signal callbacks: applied at once when this thread already holds the engine lock (Midi
 * Fadester's own write caused the signal), queued otherwise. Never waits for the engine lock. */
void mfd_engine_report_scene_graph_change(struct mfd_engine *eng, enum mfd_graph_change_kind kind, const char *a,
					  const char *b, const char *c, bool flag);
/* Applies queued changes; call only where no libobs lock is held. Returns how many were applied. */
size_t mfd_engine_process_scene_graph_changes(struct mfd_engine *eng);
/* Drops every queued change (scene collection change, cleanup and exit). */
void mfd_engine_discard_scene_graph_changes(struct mfd_engine *eng);
/* Tests: connect the libobs signal callbacks to a test signal handler, as they are connected to OBS's own. */
void mfd_engine_connect_signal_callbacks_for_test(struct mfd_engine *eng, signal_handler_t *sh);
/* Tests: run the frontend event callback (event is an enum obs_frontend_event). */
void mfd_engine_frontend_event_for_test(struct mfd_engine *eng, int event);
/* Tests: write and read the strips as global.json and the scene collection keep them. */
void mfd_engine_serialize_slots_for_test(struct mfd_engine *eng, obs_data_t *root);
void mfd_engine_deserialize_slots_for_test(struct mfd_engine *eng, obs_data_t *root);
/* Tests: the scene-collection save callback (saving true: write; false: read). */
void mfd_engine_collection_save_callback_for_test(struct mfd_engine *eng, obs_data_t *save_data, bool saving);
void mfd_engine_bump_slot_rev(struct mfd_engine *eng, int slot_index);

void mfd_engine_set_ui_notify_fn(struct mfd_engine *eng, mfd_ui_notify_fn fn, void *user_data);
/* UI thread, no lock held: saves, MIDI health, APC40 desk commands, fills and refills from the scene on air. */
void mfd_engine_flush_pending_ui_work(struct mfd_engine *eng);

/*
 * Fills strips 1..8 from a scene's video sources, bottom source first (strip 1 = the lowest layer), as Show or hide a
 * layer jobs that follow the scene on air. include_empty: strips with no job are filled too (the button, the first
 * start); otherwise only strips already following are refilled. replace_layers: Show or hide a layer strips the user
 * set follow too (the obs-websocket "all" option). Other strips the user set, or pinned, never change, and a source
 * one of them uses is skipped. scene NULL = Program. No lock held. Returns the strips now on a source. The change is
 * saved from the UI thread.
 */
int mfd_engine_fill_strips_from_scene(struct mfd_engine *eng, const char *scene, bool include_empty,
				      bool replace_layers, enum mfd_fill_status *out_status, char *out_scene,
				      size_t out_scene_size);
/* Tests: the Scenes list as OBS would give it, top first. */
void mfd_engine_set_test_scene_list(struct mfd_engine *eng, const char *const *names, int count);
/* Whether strip 1..8 follows the scene on air. */
bool mfd_engine_slot_follows_scene(struct mfd_engine *eng, int slot_index);
/* Visual Sidechain's panic state as it answered (the Record LED). */
void mfd_engine_set_vs_panic(struct mfd_engine *eng, bool panic);

struct mfd_ui_catalog *mfd_ui_catalog_create(void);
void mfd_ui_catalog_destroy(struct mfd_ui_catalog *cat);
void mfd_engine_get_ui_catalog(struct mfd_engine *eng, struct mfd_ui_catalog *out);
void mfd_engine_get_ui_snapshot(struct mfd_engine *eng, struct mfd_ui_snapshot *out);

struct mfd_source_filter_catalog *mfd_source_filter_catalog_create(void);
void mfd_source_filter_catalog_destroy(struct mfd_source_filter_catalog *cat);
bool mfd_engine_get_source_filter_catalog(struct mfd_engine *eng, const char *source_name,
					  struct mfd_source_filter_catalog *out);
bool mfd_engine_fill_vendor_catalog_locked(struct mfd_engine *eng, const char *source_name, obs_data_t *response_data,
					   char *out_err, size_t err_size);
bool mfd_engine_fill_vendor_catalog_msg_locked(struct mfd_engine *eng, const char *source_name,
					       obs_data_t *response_data, struct mfd_msg *out_msg);
bool mfd_is_internal_filter(const char *filter_name, const char *type_id);
void mfd_collect_filter_numeric_props(obs_properties_t *props, obs_data_t *settings, mfd_numeric_prop_cb cb,
				      void *param);
void mfd_engine_format_slot_display_value(const struct mfd_fader_slot *slot, float norm_01, char *out, size_t out_size);
/* translate: the UI's words (" (Off)", "dB", "%") in the language OBS runs in; false is the API's English. */
void mfd_engine_format_slot_display_value_ex(const struct mfd_fader_slot *slot, float norm_01, bool translate,
					     char *out, size_t out_size);
bool mfd_engine_validate_filter_param_binding_locked(const struct mfd_engine *eng, int slot_index, bool enabled,
						     const char *source_a, const char *filter_name,
						     const char *filter_prop, bool has_from_to, double *inout_from_val,
						     double *inout_to_val, struct mfd_filter_prop_info *out_prop_info,
						     char *out_err, size_t err_size);
bool mfd_engine_validate_filter_param_binding_msg_locked(const struct mfd_engine *eng, int slot_index, bool enabled,
							 const char *source_a, const char *filter_name,
							 const char *filter_prop, bool has_from_to,
							 double *inout_from_val, double *inout_to_val,
							 struct mfd_filter_prop_info *out_prop_info,
							 struct mfd_msg *out_msg);
void mfd_engine_detach_filter_binding_locked(struct mfd_engine *eng, int slot_index);
/* Fader slot 1..8 (an APC40 strip) is about to change its binding: a punched blend and a held flash or blackout
 * are let go on the source it has now. */
void mfd_engine_vj_let_go_locked(struct mfd_engine *eng, int slot_index);

/* --- Fadester Desk dock (0.8.0): what the dock shows and what its buttons do --- */

enum mfd_desk_pads_state {
	MFD_DESK_PADS_ON = 0,        /* the pads act: an APC40 mkII is open on its profile */
	MFD_DESK_PADS_BUTTONS_OFF,   /* the APC40 buttons are switched off in Settings */
	MFD_DESK_PADS_NO_APC40,      /* no APC40 mkII is open */
	MFD_DESK_PADS_OTHER_PROFILE, /* the APC40 is open on another profile */
};

enum mfd_desk_press_result {
	MFD_DESK_REJECTED = 0, /* not a desk button, the pads are off, Learn / Set up, or a fader learned that button */
	MFD_DESK_ACCEPTED,
	MFD_DESK_HOLD_STARTED, /* a flash or blackout hold began with this press: *hold_epoch says which */
};

/* Per strip: the order of mfd_vj_pads_leds' six LEDs for it. */
enum mfd_desk_cell {
	MFD_DESK_BLEND = 0,
	MFD_DESK_FLASH,
	MFD_DESK_FADE,
	MFD_DESK_SHOW,
	MFD_DESK_RESTART,
	MFD_DESK_CLIP_STOP,
	MFD_DESK_CELLS
};

struct mfd_desk_strip {
	char scene[MFD_SOURCE_NAME_MAX];
	char source[MFD_SOURCE_NAME_MAX]; /* the strip's layer, "" when it has none */
	char label[MFD_SOURCE_NAME_MAX];  /* what its fader drives when it is not a layer (audio, a filter), else "" */
	float level;                      /* the fader's value, 0..1 */
	bool pickup;                      /* the fader waits: it must travel to pickup_target before it acts */
	bool pickup_up;                   /* ... by moving up (else down) */
	float pickup_target;              /* where the physical fader picks up, 0..1 */
	bool has_layer;                   /* a Layer Opacity or Two-Source Mix layer the pads act on */
	bool single_layer;                /* a Layer Opacity strip: flash, blackout, fade and snap work */
	bool source_missing;              /* a Layer Opacity strip whose source is not available now */
	bool follows;                     /* follows the scene on air */
	bool can_pin_toggle;              /* Shift + top pad would pin or unpin it */
	bool owned[MFD_DESK_CELLS];       /* a fader learned that button's MIDI address */
};

struct mfd_desk_view {
	bool finished_loading;
	enum mfd_desk_pads_state pads;
	bool learn_or_setup; /* MIDI Learn or Set up is running: the pads wait */
	bool desk_connected; /* an APC40 mkII is open */
	bool studio_mode;
	bool take_ready;
	bool tbar_busy;
	bool desk_shift; /* Shift held on the desk */
	int fade_ms;     /* the pads' fade time */
	char program[MFD_SOURCE_NAME_MAX];
	char preview[MFD_SOURCE_NAME_MAX];
	int scene_count;
	char scenes[MFD_VJ_SCENES][MFD_SOURCE_NAME_MAX];
	bool scene_owned[MFD_VJ_SCENES];
	bool stop_all_owned;
	bool session_owned;
	struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
	size_t led_count;
	struct mfd_desk_strip strips[MFD_VJ_STRIPS];
	float crossfader;
	bool vs_panic;
	bool vs_panic_known;
	bool vs_last_failed;
	bool blackout;       /* Program black or fading to or from black */
	bool video_controls; /* show the video speed rows (the Advanced tab's switch) */
};

/* A snapshot for the dock, copied under the engine lock (no OBS calls). The LEDs are worked out with Shift held when
 * dock_shift is set, so a latched dock Shift shows the Shift colours. */
void mfd_engine_get_desk_view(struct mfd_engine *eng, bool dock_shift, struct mfd_desk_view *out);

/*
 * A dock button pressed (on) or released, as the desk button at that MIDI address would be. Shift is the dock's own
 * for this press only: the desk's Shift is put back afterwards, and the Shift note itself is refused. A release of a
 * flash pad passes the epoch MFD_DESK_HOLD_STARTED gave; the hold is only let go while that same hold is on, so the dock
 * never ends a hold the desk started later.
 */
enum mfd_desk_press_result mfd_engine_desk_press(struct mfd_engine *eng, uint8_t channel, uint8_t note, bool on,
						 bool shift, uint32_t *hold_epoch);

/* The dock's Fade button: Preview to Program with OBS's transition (Studio Mode), with or without an APC40. */
void mfd_engine_desk_fade(struct mfd_engine *eng);

/* The dock's Blackout button: Program to black, or back, with or without an APC40 (always reachable, so a
 * blackout can always be ended). */
void mfd_engine_desk_blackout(struct mfd_engine *eng);

/* A Visual Sidechain button on the dock (MFD_VJ_CMD_VS_PANIC, _TAP, _LOCK or _RESYNC): queued for the UI thread, with
 * or without an APC40. Returns false when the command queue is full or the kind is not one of those. */
bool mfd_engine_desk_vs(struct mfd_engine *eng, enum mfd_vj_command_kind kind);

/* Visual Sidechain answered a safety poll (from_poll) or a desk command of this kind, or did not: keeps vs_panic,
 * vs_panic_known and vs_last_failed (what the dock shows). */
void mfd_engine_vs_result(struct mfd_engine *eng, bool from_poll, enum mfd_vj_command_kind kind, bool answered,
			  bool panic);

/* The dock's VJster buttons: video speed steps (n x 0.05x, signed), back to 1x, or play / pause, sent to Fadester
 * Companion as the desk's Tempo, Nudge, Tap Tempo and Play are (vendor event vjster_action, added up and paced the
 * same way). Works without the APC40; nothing answers back. */
void mfd_engine_desk_vjster(struct mfd_engine *eng, int steps, bool reset, bool play_pause);

/* A preset speed button on the dock (0.25..4): one vjster_action {"action":"vjster.speed.set","rate":r}, which
 * overtakes steps and a 1x still waiting (Fadester Companion 0.1.17 or later; older ones ignore it). */
void mfd_engine_desk_vjster_rate(struct mfd_engine *eng, double rate);

/* The dock is shown or hidden: while shown, Visual Sidechain's panic state is asked every 2 s (at once on show). */
void mfd_engine_set_desk_visible(struct mfd_engine *eng, bool visible);
void mfd_engine_restore_all_bypass_leases(struct mfd_engine *eng);
void mfd_engine_handle_filter_rename(struct mfd_engine *eng, const char *parent_source_name, const char *prev_name,
				     const char *new_name);
void mfd_engine_handle_filter_removed(struct mfd_engine *eng, const char *parent_source_name, const char *filter_name);
void mfd_engine_handle_filter_enable(struct mfd_engine *eng, const char *parent_source_name, const char *filter_name,
				     bool enabled);
void mfd_engine_handle_filter_settings_updated(struct mfd_engine *eng, const char *parent_source_name,
					       const char *filter_name);
void mfd_engine_handle_filter_properties_updated(struct mfd_engine *eng, const char *parent_source_name,
						 const char *filter_name);
/* Clean FX targets enabled filter bindings in any bank, never audio or layer controls. */
bool mfd_slot_is_reset_fx_eligible(const struct mfd_fader_slot *slot);
/* Caller holds the engine lock; refresh cached deflection without writing to the filter. */
void mfd_engine_sync_filter_slot_from_live_locked(struct mfd_engine *eng, int slot_index);
bool mfd_engine_reset_fx(struct mfd_engine *eng, int ms, int *out_eligible_count, int *out_reset_count,
			 int *out_queued_unavailable_count, char *out_err, size_t err_size);
bool mfd_engine_reset_fx_msg(struct mfd_engine *eng, int ms, int *out_eligible_count, int *out_reset_count,
			     int *out_queued_unavailable_count, struct mfd_msg *out_msg);

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
bool mfd_engine_validate_setup_candidate_msg(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					     struct mfd_msg *out_msg);
bool mfd_engine_commit_setup_session(struct mfd_engine *eng, const struct mfd_setup_candidate *cand, char *out_err,
				     size_t err_size);
bool mfd_engine_commit_setup_session_msg(struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					 struct mfd_msg *out_msg);

void mfd_engine_accept_first_run_usual_layout(struct mfd_engine *eng);
void mfd_engine_decline_first_run_usual_layout(struct mfd_engine *eng);
void mfd_engine_set_stream_deck_companion_dismissed(struct mfd_engine *eng, bool dismissed);

bool mfd_engine_update_slot_config(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit);
bool mfd_engine_update_slot_config_ex(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit,
				      char *out_err, size_t err_size);
bool mfd_engine_update_slot_config_msg(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit,
				       struct mfd_msg *out_msg);
/* An empty label, or any profile's English default, stores an empty label (shown as the translated default). */
void mfd_engine_set_slot_label(struct mfd_engine *eng, int slot_index, const char *label);
/* The slot's label, or its English default when the label is empty (API, logs, saved config). */
void mfd_engine_slot_english_label(const struct mfd_engine *eng, int slot_index, char *out, size_t out_size);
void mfd_engine_clear_slot_binding(struct mfd_engine *eng, int slot_index);
bool mfd_engine_set_slot_value(struct mfd_engine *eng, int slot_index, float value_01, bool release, const char *origin,
			       char *out_err, size_t err_size);
bool mfd_engine_set_slot_value_msg(struct mfd_engine *eng, int slot_index, float value_01, bool release,
				   const char *origin, struct mfd_msg *out_msg);
bool mfd_engine_finish_tbar_throw(struct mfd_engine *eng, int slot_index);

void mfd_engine_set_tbar_driver(mfd_tbar_apply_fn driver_fn);
void mfd_engine_set_tbar_unlatch_fn(mfd_tbar_unlatch_fn unlatch_fn);
void mfd_engine_unlatch_tbar(struct mfd_engine *eng);
void mfd_engine_on_external_tbar_reset(struct mfd_engine *eng);
void mfd_engine_poll_midi_health_on_ui_thread(struct mfd_engine *eng);

enum mfd_controller_profile mfd_engine_get_active_profile(const struct mfd_engine *eng);
bool mfd_engine_load_profile_defaults(struct mfd_engine *eng, enum mfd_controller_profile prof);
bool mfd_engine_select_profile(struct mfd_engine *eng, enum mfd_controller_profile prof, bool reset_targets);
void mfd_engine_load_apc40_defaults(struct mfd_engine *eng);
void mfd_engine_reset_slot_mode_state(struct mfd_engine *eng, struct mfd_fader_slot *slot,
				      enum mfd_target_type new_target);
void mfd_engine_set_slot_calibration(struct mfd_engine *eng, int slot_index, int cal_min, int cal_max);
bool mfd_engine_lookup_persisted_calibration(const struct mfd_engine *eng, const char *profile_name,
					     const struct mfd_midi_addr *addr, struct mfd_control_calibration *out_cal);
bool mfd_engine_reconnect_midi(struct mfd_engine *eng, const char *device_name);
void mfd_engine_set_smooth_tau_ms(struct mfd_engine *eng, float tau_ms);
void mfd_engine_set_tbar_bidirectional_flip(struct mfd_engine *eng, bool flip);

/* APC40 mkII VJ buttons: on/off and the Clip Stop fade time (50..20000 ms). Persisted with the global settings. */
void mfd_engine_set_vj_buttons(struct mfd_engine *eng, bool enabled, int fade_ms);
bool mfd_engine_vj_buttons_enabled(const struct mfd_engine *eng);
/* The dock's video speed rows, shown or hidden. Persisted with the global settings. */
void mfd_engine_set_desk_video_controls(struct mfd_engine *eng, bool shown);
bool mfd_engine_desk_video_controls(const struct mfd_engine *eng);
int mfd_engine_vj_fade_ms(const struct mfd_engine *eng);

void mfd_engine_start_learn(struct mfd_engine *eng, int slot_index);
void mfd_engine_cancel_learn(struct mfd_engine *eng);

bool mfd_engine_save_global_config(struct mfd_engine *eng);
void mfd_engine_load_global_config(struct mfd_engine *eng);
bool mfd_engine_export_config_file(struct mfd_engine *eng, const char *filepath);
bool mfd_engine_import_config_file(struct mfd_engine *eng, const char *filepath);
bool mfd_engine_import_config_file_and_reopen(struct mfd_engine *eng, const char *filepath);

bool mfd_engine_source_exists(const struct mfd_engine *eng, const char *source_name);
bool mfd_engine_scene_exists(const struct mfd_engine *eng, const char *scene_name);
bool mfd_engine_source_has_audio(const struct mfd_engine *eng, const char *source_name);
bool mfd_engine_source_has_internal_opacity(const struct mfd_engine *eng, const char *source_name);
bool mfd_engine_find_other_scene_with_layer(const struct mfd_engine *eng, const char *source_name,
					    const char *exclude_scene, char *out_scene, size_t out_size);
bool mfd_engine_is_studio_mode_active(const struct mfd_engine *eng);
bool mfd_engine_is_slot_writable(const struct mfd_engine *eng, const struct mfd_fader_slot *slot);
void mfd_engine_reevaluate_slots(struct mfd_engine *eng, bool emit_if_changed);
void mfd_engine_handle_source_rename(struct mfd_engine *eng, const char *prev_name, const char *new_name);
void mfd_engine_get_slot_status(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, char *out_label,
				size_t label_size, bool *out_available, char *out_reason, size_t reason_size);
/* As mfd_engine_get_slot_status, plus the reason's code and arguments (out_msg may be NULL). */
void mfd_engine_get_slot_status_msg(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, char *out_label,
				    size_t label_size, bool *out_available, char *out_reason, size_t reason_size,
				    struct mfd_msg *out_msg);

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
/* Caller holds the engine lock; call before overwriting an old T-bar binding. */
void mfd_engine_abort_tbar_for_config_change_locked(struct mfd_engine *eng, struct mfd_fader_slot *slot);

/*
 * Steps MIDI queue decoding, smoother, auto-fade ramps, and coalesced vendor events by `seconds`.
 */
void mfd_engine_tick(struct mfd_engine *eng, float seconds);

#ifdef __cplusplus
}
#endif
