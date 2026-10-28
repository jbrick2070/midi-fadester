# Midi Fadester 0.2.0 — Whole-Dialog UI Plan (`PLAN-UI.md`)

**Date:** 2026-10-28 (Hardened via 4-round `kibitz` campaign `r1 -> r2 -> r3 -> r4`, driver `antigravity` + `codex`)
**Platform Scope:** Windows x64 OBS Studio 33+ (`WinMM` MIDI In, `WM_DEVICECHANGE`, `Qt6Core` / `Qt6Gui` / `Qt6Widgets` / `Qt6Svg` — verified present in OBS 33 `bin\64bit`; `Qt6SvgWidgets.dll` and `qoffscreen.dll` are absent in OBS 33 / `obs-deps-qt6` and MUST NOT be depended upon).
**Baseline (`0.1.3`, `commit d7c6488`):** Per-mode precondition checks (`available` + canonical `reason`), `libobs` scene/source signals (`item_add`, `item_remove`, `reorder`, `source_create`, `source_remove`, `source_destroy`, `source_rename`), automatic `crossfade_ab` stack-order inversion (`a_is_above_b`), `6%` T-bar start band, exact calibrated readout (`mfd_calibrated_percent`), and mode-switch takeover reset (`mfd_engine_reset_slot_mode_state`).

---

## 1. Goals & Enforceable Invariants

1. **Under-a-minute first fade (`G-1`):** On a fresh install with an Akai APC40 mkII, `Crossfader` already switches scenes out of the box; opening **Tools → Midi Fadester** presents a one-click First-Run Offer (`[Yes]` / `[I'll set it up]`) and plain-English strips with zero MIDI jargon on the primary screen.
2. **Total plain-English state & recovery taxonomy (`G-2`):** Every controller state and every strip state maps via a single normative precedence table to a complete sentence naming what is happening and the button that fixes it. While a strip has an unmet precondition (`!available`), moving the physical control moves its live calibrated bar (`0..100%`) while writing **nothing** to OBS.
3. **Invalid assignments cannot be built or committed (`G-3`):** The modeless 3-step **Set up** sheet filters pickers from an atomic heap-allocated catalog snapshot, refreshes live on OBS topology/Studio Mode changes, checks `slot_config_rev[slot_index]` against concurrent edits, and re-runs the engine's canonical precondition evaluator atomically inside **Done**—so no picker sequence or mid-dialog mutation can commit an unavailable slot.
4. **Plain words first, full power & wire contract preserved (`G-4`):** Plain words replace internal/MIDI terms across **Strips** and **Set up**, the 10-column configuration table stays intact as the **Advanced** tab, and `obs-websocket` `api_version: 1` fields (`target_mode`, `curve`, `afv`, `slots[i].label`, `slots[i].reason`) remain 100% backward-compatible.
5. **No independent timers in production code (`G-5`, Standing Rule):** Zero `QTimer`, `QBasicTimer`, `QObject::startTimer`, Win32 `SetTimer`, or production sleep/polling loops across `src/core/`, `src/midi/`, `src/obs/mfd-engine.*`, `src/obs/mfd-vendor-api.*`, and `src/ui/` (enforced by `test-shader-syntax`; `mfd-tbar.cpp`'s `#ifdef MFD_ENABLE_SELF_TEST` subprocess harness is the sole test-only exception).

---

## 2. Concurrency, Engine Data Contracts & Pure-C UI Formatting

### 2.1 Thread-Safety, Lock-Free SPSC Atomics & Module Lifecycle (`src/core/midi-queue.*`, `src/midi/winmm-device.*`, `src/obs/mfd-engine.*`, `src/obs/mfd-vendor-api.*`, `src/obs/mfd-tbar.cpp`, `src/plugin-main.c`)
- **Real-time SPSC & WinMM atomics (`<util/threading.h>`, C17 + C++20 compatible on MSVC):**
  - `struct mfd_midi_queue` uses `volatile long write_idx; volatile long read_idx; volatile long dropped_count;` accessed via `os_atomic_load_long`, `os_atomic_store_long`, and `os_atomic_inc_long` (preserving unsigned `(uint32_t)w - (uint32_t)r` ring indexing with acquire/release barriers between WinMM's `midiInProc` and `mfd_engine_tick`).
  - `struct mfd_winmm_device` accesses `device_closed_unexpectedly` via `os_atomic_load_bool` / `os_atomic_store_bool` and records `enum mfd_winmm_error_kind { MFD_WINMM_OK = 0, MFD_WINMM_ERR_NO_DEVICES, MFD_WINMM_ERR_CONFIGURED_NOT_FOUND, MFD_WINMM_ERR_BUSY_ALLOCATED, MFD_WINMM_ERR_DISCONNECTED, MFD_WINMM_ERR_OPEN_FAILED }`.
- **Recursive `state_mutex` & outermost-unlock drain (`R-THR-1`):**
  1. `struct mfd_engine` adds `pthread_mutex_t state_mutex; bool state_mutex_initialized; uint32_t lock_depth; bool pending_ui_notify; bool pending_state_event; bool pending_config_save;` plus a bounded per-slot pending `fader_changed` drain buffer.
  2. `mfd_engine_init_mutex` (`pthread_mutex_init_recursive`) runs immediately after `memset` in `mfd_engine_init()` (propagating `false` on failure); every recursive `mfd_engine_lock(eng)` calls `pthread_mutex_lock` (when `state_mutex_initialized`) and increments `lock_depth`; every `mfd_engine_unlock(eng)` decrements `lock_depth` and calls `pthread_mutex_unlock`. Stack-zeroed single-threaded test instances work unchanged, while `mfd_engine_init_mutex` / `mfd_engine_destroy_mutex` enable multi-threaded concurrency tests in `test-vendor-api.c`.
  3. **Complete lock coverage:** Every engine read/write entry point—including `mfd_engine_tick`, `mfd_engine_get_ui_snapshot`, `mfd_engine_get_ui_catalog`, `on_frontend_event`, `on_obs_*` signal callbacks, `on_save_callback`, legacy `start_learn`/`cancel_learn`, config import/export, and all `mfd-vendor-api.c` WebSocket & proc-bridge handlers—acquires `mfd_engine_lock(eng)` and releases via `mfd_engine_unlock(eng)`.
  4. **Outermost-unlock drain (zero external callbacks or render-thread disk I/O while locked):**
     - `mfd_engine_mark_ui_dirty(eng, bool is_topology)` increments `config_topology_rev` (when `is_topology`) or `telemetry_rev`, and sets `pending_ui_notify = true`. It is called on **every** snapshot-visible change: accepted MIDI samples (before any `!writable` `continue`), smoother ticks, `auto_fade` ticks, precondition re-evaluations, setup-session changes, and `device_closed_unexpectedly`.
     - When `mfd_engine_unlock(eng)` reaches `lock_depth == 0`: snapshots pending `state_changed` / `fader_changed` payloads and `(ui_notify_fn, ui_notify_user_data)` (`should_wake_ui = pending_ui_notify || pending_config_save`), clears `pending_ui_notify = false` (leaving `pending_config_save` set until the UI thread flushes it!), calls `pthread_mutex_unlock`, and then emits `fader_changed` / `state_changed` and calls `ui_notify_fn(ud)` strictly outside `state_mutex`.
- **Strict UI-thread affinity & lossless UI-dispatcher coalescing:**
  - `on_frontend_event` (UI thread) caches `frontend_finished_loading`, `cached_studio_mode`, `cached_preview_scene`, `cached_program_scene`, and `apc40_controller_detected` under `state_mutex`.
  - `mfd-tbar.cpp` caches `static QPointer<QWidget> g_main_win` on the UI thread in `mfd_tbar_init()` / `on_tbar_frontend_event` so `mfd_tbar_driver_apply` and `mfd_tbar_is_studio_mode_active` never call `obs_frontend_get_main_window()` on the video render thread.
  - Persistent UI-thread `MfdUiDispatcher : public QObject` (created in `mfd_ui_init()`, destroyed in `mfd_ui_shutdown()`):
    - Producer (`ui_notify_bridge_cb`, protected by `static std::mutex g_ui_bridge_mu` + `static std::atomic<bool> g_ui_refresh_queued{false}`): if `g_ui_dispatcher` is alive and `!g_ui_refresh_queued.exchange(true)`, posts `QMetaObject::invokeMethod(g_ui_dispatcher, &MfdUiDispatcher::on_drain, Qt::QueuedConnection)` (resetting `g_ui_refresh_queued` to `false` if `invokeMethod` fails).
    - Consumer (`MfdUiDispatcher::on_drain()` on Qt UI thread): clears `g_ui_refresh_queued.store(false)` **first**, calls `mfd_engine_flush_pending_ui_work(eng)` (which atomically exchanges `eng->pending_config_save` to `false` and saves `global.json`, and runs `mfd_engine_poll_midi_health_on_ui_thread(eng)` if `os_atomic_load_bool(&eng->midi_dev.device_closed_unexpectedly)` is true), and then refreshes `g_open_dialog` if open.
- **Deterministic load/unload fencing (`src/plugin-main.c`, `src/obs/mfd-vendor-api.*`, `src/obs/mfd-tbar.cpp`, `src/ui/mfd-settings-dialog.cpp`):**
  - Qt6 (`Widgets`, `Core`, `Gui`, `Svg`) is required unconditionally in `CMakeLists.txt` (matching `mfd-tbar.cpp`'s existing unconditional Qt6 dependency).
  - `obs_module_load`: `mfd_register_layer_filter()`; `if (!mfd_engine_init()) return false;`; `mfd_tbar_init()`; `mfd_ui_init()`; `return true;`.
  - `obs_module_unload`:
    1. `mfd_vendor_api_shutdown(mfd_engine_get())`: sets `os_atomic_store_bool(&g_vendor_accepting, false)`, unregisters all 6 WebSocket requests via `obs_websocket_vendor_unregister_request(g_vendor, "...")`, acquires-and-releases `mfd_engine_lock(eng)` as an in-flight barrier for any executing vendor or `proc_handler` bridge call (each checks `os_atomic_load_bool(&g_vendor_accepting) && eng->initialized` under `mfd_engine_lock`), and clears `g_vendor`, `g_bridge_ph`, `g_bridge_sh`, `g_event_sink`.
    2. `mfd_ui_shutdown()`: calls `mfd_engine_set_ui_notify_fn(eng, nullptr, nullptr)`, removes and deletes the `WM_DEVICECHANGE` `QAbstractNativeEventFilter`, closes/deletes open dialogs, and deletes `g_ui_dispatcher` (purging queued Qt events).
    3. `mfd_tbar_shutdown()`: under `#ifdef MFD_ENABLE_SELF_TEST`, pumps `QCoreApplication::processEvents` while `g_selftest_running` is true so any in-flight `BlockingQueuedConnection` completes without deadlock, then joins `g_selftest_thread`; removes frontend callback and clears T-bar driver pointers.
    4. `mfd_engine_shutdown()`: marks `initialized = false`, disconnects OBS signals/callbacks, saves `global.json`, closes WinMM, destroys `state_mutex`.

### 2.2 Exact Engine Structs, Heap-Allocated Atomic Catalog & Setup Transaction (`src/obs/mfd-engine.h`)
```c
#define MFD_CATALOG_MAX_SCENES 48
#define MFD_CATALOG_MAX_SCENE_LAYERS 48
#define MFD_CATALOG_MAX_AUDIO_SOURCES 64

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
    int slot_index;                       /* 0..9 */
    uint64_t base_slot_rev;               /* captured from eng->slot_config_rev[slot_index] */
    struct mfd_midi_addr candidate_addr;
    struct mfd_control_calibration candidate_cal;
    bool candidate_invert;                /* single authoritative Reverse direction owner during Set up */
    bool learning;
    float candidate_raw_norm;             /* 0..1 calibrated meter for Step 3 */
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
```
- **Heap-only Catalog & Snapshot APIs:**
  - `struct mfd_ui_catalog *mfd_ui_catalog_create(void);` / `void mfd_ui_catalog_destroy(struct mfd_ui_catalog *cat);` (prevents `~600 KB` stack frames on Windows' `1 MB` stack).
  - `void mfd_engine_get_ui_catalog(struct mfd_engine *eng, struct mfd_ui_catalog *out);` (sorts scene names and audio source names case-insensitively while preserving `bottom → top` stacking order in `scenes[i].layers[]`; sets `scenes_truncated` / `layers_truncated` / `audio_truncated` when limits are exceeded, surfaced as an inline notice in `Set up` Step 2).
  - `void mfd_engine_get_ui_snapshot(struct mfd_engine *eng, struct mfd_ui_snapshot *out);`
- **Exhaustive `slot_config_rev` Contract:**
  - One central helper `bump_slot_config_rev(struct mfd_engine *eng, int slot_index)` increments `eng->slot_config_rev[slot_index]` and `eng->config_topology_rev` across **every** slot-config mutation path: (1) `mfd_engine_commit_setup_session`, (2) `mfd_engine_unassign_slot_preserve_addr`, (3) `mfd_engine_update_slot_config`, (4) `mfd_engine_set_slot_label`, (5) `mfd_engine_clear_slot_binding`, (6) `mfd_engine_set_slot_calibration`, (7) legacy `Learn` capture in `mfd_engine_tick`, (8) `mfd_vendor_handle_set_slot`, (9) `mfd_vendor_handle_load_profile`, (10) `mfd_engine_load_apc40_defaults`, (11) `deserialize_slots` (config import & scene-collection load), (12) `mfd_engine_accept_first_run_usual_layout`, (13) `mfd_engine_decline_first_run_usual_layout`, and (14) `mfd_engine_handle_source_rename`.
- **Complete Setup Transaction Signatures:**
  - `void mfd_engine_begin_setup_session(struct mfd_engine *eng, int slot_index);` (cancels `learning_slot_index`, stops `auto_fade_active` and resets smoother to `last_applied_val` on `slot_index`, captures `base_slot_rev`, initializes `candidate_addr`/`candidate_cal`/`candidate_invert`/`candidate_raw_norm`, marks `active = true`).
  - `void mfd_engine_start_setup_learn(struct mfd_engine *eng);` / `void mfd_engine_cancel_setup_learn(struct mfd_engine *eng);` (mutually exclusive with `mfd_engine_start_learn` / `mfd_engine_cancel_learn` on `eng->midi_parser`).
  - `void mfd_engine_set_setup_invert(struct mfd_engine *eng, bool invert);`
  - `void mfd_engine_cancel_setup_session(struct mfd_engine *eng);`
  - `bool mfd_engine_unassign_slot_preserve_addr(struct mfd_engine *eng, int slot_index);` (used by Step 1 `[Not used]`: sets `target_type = MFD_TARGET_UNASSIGNED`, clears `scene_name`/`source_a_name`/`source_b_name`, keeps `enabled = true` and `slot->addr` / `slot->cal` intact so APC40 fader bars remain live, cancels `setup_session`, bumps `slot_config_rev`, saves, and notifies).
  - `bool mfd_engine_validate_setup_candidate(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand, char *out_reason, size_t reason_size);`
  - `bool mfd_engine_commit_setup_session(struct mfd_engine *eng, const struct mfd_setup_candidate *cand, char *out_err, size_t err_size);`
  - In `mfd_engine_tick`: while `setup.active`, (a) if `setup.learning` and `mfd_midi_parser_pop_learned` succeeds, populates `setup.candidate_addr` + `setup.candidate_cal`, clears `setup.learning = false`, and marks topology+telemetry dirty; (b) every non-Note-Off MIDI sample matching `setup.candidate_addr` updates `setup.candidate_raw_norm` (with `setup.candidate_invert`) and `setup.has_candidate_input = true`, marking telemetry dirty without writing to OBS on `setup.slot_index`.
- **First-Run Offer Discriminator & Button Semantics:**
  - Before `mfd_engine_load_global_config`, `eng->first_run_offer_dismissed = false`. If `global.json` does **not** exist on disk (`!os_file_exists(cfg_path)`), `first_run_offer_dismissed` stays `false` (fresh install). If `global.json` exists, `mfd_engine_import_config_file` reads `"first_run_offer_dismissed"` if `obs_data_has_user_value(data, "first_run_offer_dismissed")`, otherwise sets `first_run_offer_dismissed = true` (`0.1.x` upgrade). Any non-default slot in scene-collection `on_save_callback(saving=false)` also sets `first_run_offer_dismissed = true`.
  - `[Yes]` (`mfd_engine_accept_first_run_usual_layout`): loads APC40 defaults (`Crossfader` = `Switch scenes (T-bar)`, `1..8` = `Not used` with APC40 CCs bound), binds `Master` to `Volume` on `"Desktop Audio"` (case-preserved) iff an audio-capable source matching `"Desktop Audio"` case-insensitively exists (does not auto-enable Studio Mode; the amber Studio Mode banner offers `[Turn on Studio Mode]` if Studio Mode is off), sets `first_run_offer_dismissed = true`, saves `global.json`.
  - `[I'll set it up]` (`mfd_engine_decline_first_run_usual_layout`): keeps all 10 APC40 MIDI addresses bound (`enabled = true`, live bars active) with all 10 `target_type = MFD_TARGET_UNASSIGNED`, sets `first_run_offer_dismissed = true`, saves `global.json`, and remains on `Strips`.

### 2.3 Normative Sentence & Action Precedence Table (`src/core/ui-sentences.{h,c}`)

#### Controller Bar (`mfd_ui_format_controller_status`, evaluated top-to-bottom)
| Priority | Condition | Dot | Sentence | `[Reconnect]` |
|---|---|---|---|---|
| 1 | `connected == true` | Green | `Connected` | Hidden |
| 2 | `error_kind == MFD_WINMM_ERR_BUSY_ALLOCATED` | Red | `Another program has the <Dev> open. Close it, then press Reconnect.` | Shown |
| 3 | `error_kind == MFD_WINMM_ERR_CONFIGURED_NOT_FOUND` or `DISCONNECTED` or (`NO_DEVICES` with `configured_name != ""`) | Red | `<Dev> not found. Plug it in, then Reconnect.` | Shown |
| 4 | `error_kind == MFD_WINMM_ERR_NO_DEVICES` (`configured_name == ""`) | Red | `No MIDI controller found. Plug one in, then press Reconnect.` | Shown |
| 5 | `error_kind == MFD_WINMM_ERR_OPEN_FAILED` (or fallback) | Red | `Could not open <Dev>. Unplug and replug it, then press Reconnect.` | Shown |

#### Strip Card (`mfd_ui_format_strip_presentation`, evaluated top-to-bottom; `bar_percent` is always live `0..100%`)
| Priority | Condition | Dimmed | `state_readout` | `status_sentence` (`severity`) | Bottom Button |
|---|---|---|---|---|---|
| 1 | `!enabled` or `target_type == MFD_TARGET_UNASSIGNED` | Yes | `""` | `""` (`NONE`; assignment shows `Not used.` / `Click to set up.`) | `Set up` |
| 2 | `!available` and `target_type == MFD_TARGET_TBAR` and `reason == "Studio Mode is off."` | No | `""` | `Studio Mode is off.` (`AMBER`) | `Turn on Studio Mode` |
| 3 | `!available` (any other unmet precondition: `<Layer> is no longer in scene <Scene>.`, `Scene <Scene> was deleted.`, `<A> and <B> are not in the same scene.`, `<Layer> is not in any scene.`, `<Source> was deleted.`, `<Source> has no audio.`, `No MIDI control bound.`, or `tbar_error`) | No | `""` | `in->reason` verbatim (`RED`) | `Set up` |
| 4 | `available` and `takeover_state == MFD_TAKEOVER_PARKED` | No | `At end.` | `""` (`NONE`) | `Set up` |
| 5 | `available` and `has_last_fader` and `takeover_state == MFD_TAKEOVER_WAITING` and `takeover_hint != MFD_HINT_NONE` | No | Mode value (`Ready`, `<L> <A>%, <R> <B>%`, `<L> <A>% visible`, `<X> dB`) | `Move the fader up to <T>% to pick up.` or `Move the fader down to <T>% to pick up.` (`AMBER`, plus `↑`/`↓` or `→`/`←`) | `Set up` |
| 6 | `available` and `target_type == MFD_TARGET_TBAR` and `tbar_state == MFD_TBAR_THROWING` | No | `Throwing <P>%` | `""` (`NONE`, `[Finish now]` mini-button shown) | `Set up` |
| 7 | `available` (engaged / ready) | No | `Ready` (`tbar`), `<L> <A>%, <R> <B>%` (`crossfade_ab`), `<L> <A>% visible` (`layer_opacity`), `<-X.X> dB` / `-inf dB` (`audio_volume`) | `""` (`NONE`) | `Set up` |

---

## 3. Qt6 Widget Architecture (`src/ui/`)

### 3.1 CMake Wiring & Files
- `CMakeLists.txt`: unconditionally requires `find_package(Qt6 COMPONENTS Widgets Core Gui Svg REQUIRED)` and links `Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg`; lists `src/core/ui-sentences.{h,c}`, `src/ui/mfd-svg-canvas.hpp`, `src/ui/mfd-controller-svg.{hpp,cpp}`, `src/ui/mfd-setup-sheet.{hpp,cpp}`, and `src/ui/mfd-settings-dialog.{hpp,cpp}` in `target_sources`.
- `src/ui/mfd-settings-dialog.hpp` exposes `QDialog *mfd_ui_create_settings_dialog(struct mfd_engine *eng, QWidget *parent = nullptr);` (`#ifdef __cplusplus`).
- Any `obs_module_text("...")` keys remain synchronized with `data/locale/en-US.ini` (`check-locale-keys` CTest).

### 3.2 Three-Zone `Strips` View, `Advanced` Tab, `[Show controller]` & Modeless `Set up` Sheet
- **Top Banners:** First-Run Offer banner (`"mfdFirstRunBanner"`, `[Yes]` / `[I'll set it up]`) and amber Studio Mode banner (`"mfdStudioNote"`, `[Turn on Studio Mode]`).
- **Zone 1 (`"mfdControllerBar"`):** Controller name, green/red dot, status sentence, `[Reconnect]`, `[Change controller]`, `[Show controller]` (`MfdControllerViewDialog` SVG desk diagram).
- **Zone 2 (`"mfdStrip_0"`..`"mfdStrip_9"`):** `Crossfader` (horizontal bar) + `1..8` & `Master` (vertical bars) in a frameless horizontal `QScrollArea`, showing title, live calibrated bar + pickup marker/arrow, `state_readout`, clickable assignment sentence, wrapped amber/red `status_sentence`, `[Finish now]` mini-button when throwing, and `[Turn on Studio Mode]` or `[Set up]`.
- **Zone 3 (`"mfdZone3StatusLabel"`):** Studio Mode state, `Preview → Program`, `Last move` (plus dropped-MIDI diagnostic tooltip if `midi_dropped_count > 0`), and `[Advanced]` switch (cancelling any open `MfdSetupSheetDialog` on tab switch).
- **`Set up` Sheet (`MfdSetupSheetDialog`, uses `mfd_ui_catalog_create` / `mfd_ui_catalog_destroy`):**
  - **Step 1:** 4 SVG-illustrated cards + `[Not used]` (`mfd_engine_unassign_slot_preserve_addr`).
  - **Step 2:** Filtered dropdowns from `mfd_ui_catalog`, `[Turn on Studio Mode]` when needed, inline `"Left and Right cannot be the same layer."` refusal, live-scene note, shared-layer notes (`mfd_ui_format_shared_layer_note`), and truncation note if `scenes_truncated` / `audio_truncated`; `Next` gated on `mfd_engine_validate_setup_candidate`.
  - **Step 3:** Finished sentence, live `candidate_raw_norm` meter, mode-specific options (`Feel`, `"Also fade audio"` from `layer_has_audio`, cubic law note, `"Reverse direction"` via `mfd_engine_set_setup_invert`, `"T-bar works in both directions"`, and `"Finish now"` — enabled iff the live slot's committed mode is `MFD_TARGET_TBAR` and `tbar_state == MFD_TBAR_THROWING`), `"Which hardware control?"` (`Learn` + displaced-strip collision notice), and **Done** (`mfd_engine_commit_setup_session`, bouncing back to Step 2 with `out_err` on mid-dialog invalidation or `slot_config_rev` conflict).

---

## 4. Vocabulary Table (`Strips`, `Set up`, and `Advanced` Column Headers)

| Stop Saying | Say Instead (`0.2.0`) |
|---|---|
| `Target Mode` | `What it does` |
| `Studio Mode T-Bar` | `Switch scenes (T-bar)` |
| `Two-Source Crossfade (A ↔ B)` | `Mix two layers` |
| `Single Layer Opacity` | `Show or hide a layer` |
| `Audio Volume (Cubic dB)` | `Volume` |
| `Source A (Bottom / Primary)`, `Source B (Top Layer)` | `Left`, `Right` |
| `Inv` | `Reverse direction` |
| `AFV` | `Also fade audio` |
| `Smoothing (τ)` | `Smoothing (ms)` (in `Advanced`) |
| `Bidirectional T-Bar (0→1 then 1→0)` | `T-bar works in both directions` |
| `Dip-Free Dissolve (Video)` | `Dip-free` |
| `48% (A:100% B:48%)` | `Color 100%, Color 2 48%` / `Color 2 48% visible` / `-12.0 dB` |

---

## 5. Step-by-Step Build & Test Order (`ctest` Green After Every Step)

1. **Step 1 — SPSC/WinMM Atomics, Recursive Mutex & Outermost-Unlock Drain, Heap Catalog, Setup Transaction, First-Run & Teardown Fencing:**
   - Update `midi-queue.{h,c}`, `winmm-device.{h,c}`, `plugin-main.c`, `mfd-tbar.cpp`, `mfd-vendor-api.{h,c}`, and `mfd-engine.{h,c}`.
   - Extend `tests/test-midi-queue.c` and `tests/test-vendor-api.c` (setup session isolation, `[Not used]` address preservation, Learn transition + mutual exclusion + multi-slot collision transfer, `slot_config_rev` conflict checks across vendor edit / rename / import / profile load, mid-setup precondition invalidation on commit, clean-dir vs `0.1.3` First-Run migration tests, deleted-audio vs no-audio preconditions, bounded 4-mode candidate-validator equivalence matrix, and multi-threaded `pthread_t` concurrency stress test).
2. **Step 2 — Pure-C Sentence & Presentation Formatter (`src/core/ui-sentences.{h,c}`) + Headless Tests (`tests/test-ui-sentences.c`):**
   - Wire `src/core/ui-sentences.{h,c}` into `CMakeLists.txt` and `test-ui-sentences` in `tests/CMakeLists.txt`, testing every row of the controller and strip precedence tables, `has_last_fader` startup pickup suppression, reversed-polarity pickup target percentages, shared-layer notes (1 and `>= 2` scenes), and `Last move` copy.
3. **Step 3 — Qt6 `Strips` + `Advanced` Dialog, `[Show controller]` SVG Desk View, Modeless `Set up` Sheet, `WM_DEVICECHANGE` & Zero-Timer Guard:**
   - Wire unconditional `Qt6::Gui` + `Qt6::Svg` and all `src/ui/*` files in `CMakeLists.txt`.
   - Implement `mfd-svg-canvas.hpp`, `mfd-controller-svg.{hpp,cpp}`, `mfd-setup-sheet.{hpp,cpp}`, and `mfd-settings-dialog.{hpp,cpp}` with zero `QTimer`s.
   - Extend `tests/test-shader-syntax.c` + `tests/CMakeLists.txt` to scan all production `src/core/*`, `src/midi/*`, `src/obs/mfd-engine.*`, `src/obs/mfd-vendor-api.*`, and `src/ui/*` files for UTF-8 BOMs and forbidden timer/sleep tokens (`QTimer`, `QBasicTimer`, `startTimer`, `SetTimer`, `sleep_for`, `Sleep(`).
4. **Step 4 — Headless `qwindows` (`WA_DontShowOnScreen`) End-to-End Acceptance & Screenshot Suite (`tests/test-ui-dialog.cpp`) + `TESTING.md`:**
   - Add `test-ui-dialog` in `tests/CMakeLists.txt` (deriving `_qt_plugins_dir` from `${_prefix}/plugins` where `platforms/qwindows.dll` exists, setting `QT_PLUGIN_PATH`, asserting `QGuiApplication::platformName() == "windows"`, and using `Qt::WA_DontShowOnScreen` + `show()` + `grab()`).
   - Verify end-to-end First-Run `[Yes]` & `[I'll set it up]`, Studio Mode recovery, 3-step `Mix two layers` setup, duplicate-layer inline refusal, shared-layer notes, mid-dialog invalidation bounce-back, `slot_config_rev` conflict handling, `[Show controller]`, and `Advanced` tab round-trip.
   - During normal `ctest`, validate rendered screenshots in `${CMAKE_CURRENT_BINARY_DIR}/ui_screenshots` via semantic widget assertions + non-empty image checks; run `test-ui-dialog --export-screenshots C:/midi-fadester/docs/screenshots` once to export the UI state screenshots documented in `TESTING.md`.
5. **Step 5 — Minor Version Bump to `0.2.0` & Live Portable OBS 33 + `loopMIDI` Verification:**
   - Bump `buildspec.json` to `"0.2.0"` and update `tests/test-vendor-api.c:491` to `"0.2.0"`.
   - Format with `clang-format` / `gersemi`, rebuild `RelWithDebInfo` & `Release`, pass full `ctest` on both, run live Portable OBS 33 + `loopMIDI` (`MFD_SELF_TEST=1` using `RelWithDebInfo\midi-fadester.dll`, which enables `MFD_ENABLE_SELF_TEST=1`) verifying all pixel checks, scene-signal stack-order inversion, `loopMIDI` Cases 1–8, and `0` memory leaks, and deploy `Release\midi-fadester.dll` as the final artifact.
