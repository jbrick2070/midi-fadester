# Fadester Ecosystem Plan — Dedicated FX Bank, Numeric Filter Controls & Live Performance Safety (`PLAN-FX.md`)

**Date:** 2026-10-28 (Post-Round-4 Converged & Build-Ready)
**Target Releases:**
1. **`C:\midi-fadester` `0.4.0`** (Windows x64 OBS Studio 33+ C17/C++20 plugin, baseline `0.3.0` at `commit 642d6b7`)
2. **`C:\streamdeck-fadester` `0.2.0`** (`Fadester for OBS` TypeScript `@elgato/streamdeck` `3.0.3` plugin + shared `API-CONTRACT.md` draft 2, `api_version: 1`)

---

## 1. Goals, Live-Performance Requirements & Hard Invariants (`S1`)

### 1.1 Operator & Live-Show Goals
1. **Dedicated FX / Knob Bank (`G-FX-1`):** Add 8 independent continuous FX slots (`fx_1 .. fx_8`, `kind = "fx"`, indices `10..17`) alongside the existing 10 mixer strips (`crossfader`, `fader_1 .. fader_8`, `master`, indices `0..9`; `18` total slots) so modulating filters, shader uniforms, or audio-reactivity trim never steals channel faders (`V2`, `V3`, `V4` synchronized story layers in *Fabel / Earthsearch*) or the A/B crossfader, and works identically from hardware MIDI knobs, Stream Deck + rotary dials, `Auto-fade` keys, or Stream Deck-only setups with no MIDI controller connected.
2. **Any Numeric Filter Parameter (`G-FX-2`, `R-FADE-4`):** Allow any slot (`0..17`) to drive any `OBS_PROPERTY_FLOAT` or `OBS_PROPERTY_INT` setting (recursing into nested `OBS_PROPERTY_GROUP` containers, excluding non-numeric and `OBS_PROPERTY_LIST` combo properties) on any user filter attached to a source or whole scene — including OBS Color Correction, Blur, Chroma/Luma/Color Key, Transform/Shader plugins, and `Visual Sidechain` (`VisualSidechain.AudioModulator` `slot_N_amount_pct` reactivity trim and Shader Filter `.shader` uniforms).
3. **Neutral-at-Zero Sub-Range Mapping & Shared-Safe GPU Bypass (`G-FX-3`):** Map normalized slot value $t \in [0.0, 1.0]$ linearly across an operator-chosen `[from_val, to_val]` range ($\text{val} = \text{from\_val} + (\text{to\_val} - \text{from\_val}) \cdot t$), supporting inverted sweeps (`from_val > to_val`, e.g. Saturation `0.0 -> -1.0` monochrome) and bipolar neutral origins (`0% = 0.0`, `100% = +0.35`), plus an optional `bypass_at_zero` flag with deterministic multi-slot lease ownership and pre-bypass `enabled` restoration across unbind, rename, removal, and shutdown.
4. **`CLEAN / ALL FX OFF` Live Performance Panic (`G-FX-4`, Fabel Brief):** Return every enabled `filter_param` binding to `0.0` (`from_val` neutral + aggregated `bypass_at_zero`, instant `ms = 0` or timed `ms in 50..20000`), including filters on ordinary faders. Temporarily unavailable filter bindings queue a clean for when their source/filter returns. Non-filter `tbar`, `crossfade_ab`, `layer_opacity`, and `audio_volume` controls are excluded in every bank, including `fx_1..fx_8`; media playback is not restarted.

### 1.2 Hard Standing Invariants
- **Zero timers / zero polling loops (`G-INV-1`):** No `QTimer`, `startTimer`, `SetTimer`, or background polling anywhere in `C:\midi-fadester\src\` (enforced by `test-shader-syntax`). All filter/source/property precondition updates run strictly on `libobs` signals; all smoothing runs on `obs_add_tick_callback(seconds)`.
- **Zero `obs_source_properties()` calls on the video render tick (`G-INV-2`):** `obs_source_properties()` may be queried on explicit discovery/UI calls (`get_catalog`, `mfd_engine_get_source_filter_catalog`) and on signal-driven `mfd_engine_reevaluate_slots` (which refreshes each bound slot's cached `filter_prop_*` metadata), **never** inside `mfd_engine_tick` / `apply_filter_param_output_locked`.
- **Backward-compatible `api_version: 1` (`G-INV-3`):** Verified directly against `C:\streamdeck-fadester` (Appendix A): `SurfaceStore` dynamically indexes `state.slots` by `slot.id` with zero fixed-10 assumptions, and unknown `target_mode` values fall through to the standard `%` fader cell, whereas bumping `api_version > 1` forces existing `0.1.x` Stream Deck clients to reject the connection (`R-API-5`).
- **Universal single-writer property ownership & no silent fallbacks (`G-INV-4`):** One shared validation helper (`mfd_engine_validate_filter_param_binding_locked`) enforces that no two enabled slots bind the same `(source_a, filter_name, filter_prop)` across **every** entry point (`set_slot`, `Set up` commit, `Advanced` `update_slot_config`, disabled-to-enabled toggle, and JSON import/deserialization). Missing source, removed filter, disappeared property, or dynamically incompatible property range sets `available: false` with a complete human sentence naming the fix and suppresses OBS writes while keeping the live physical control bar responsive.
- **No `"OBS"` prefix on any plugin/action/symbol name; UTF-8 without BOM; version bump on every deployed build.**

---

## 2. Atomic Rollout Sequence & Mixed-Version Compatibility Matrix (`S2`)

### 2.1 Ordered Delivery Steps & Release Gates
1. **Step 0 — Shared Wire Contract:** Draft `C:\streamdeck-fadester\API-CONTRACT.md` (draft 2, `api_version: 1`) documenting `fx_1..fx_8`, `filter_param`, `display_value`, `origin: "obs"`, `get_catalog`, and `reset_fx`.
2. **Step 1 — Provider Release & Live Gate (`C:\midi-fadester` `0.4.0`):** Implement engine, profile knob maps, weak-ref tracked filter signals, lease detachment/rename/shutdown lifecycle, vendor API (`get_catalog`, `reset_fx`), and OBS UI. **Mandatory release gate before Step 2 ships:** `15/15` `ctest` pass + live Portable OBS 33 self-test (`scripts/run-live-selftest.ps1` -> `src/obs/mfd-tbar.cpp:run_live_selftest_worker`) proves `get_catalog`, `set_slot(filter_param)`, pixel-verified filter sweep, `reset_fx`, lease restore, and `0` memory leaks + GitHub Actions CI green.
3. **Step 2 — Surface Release (`C:\streamdeck-fadester` `0.2.0`):** Commit `API-CONTRACT.md` draft 2 + implement grouped `Faders` / `FX & Knobs` slot selection, `display_value` + cyan-amber FX dial/key rendering, and `Clean FX` (`reset_fx`) across `FadesterClient`, `SurfaceStore`, `FaderDialInstanceController`, `AutoFadeKeyInstanceController`, and `com.jbrick2070.fadester-for-obs.sdPlugin\ui\`; pass Vitest + live OBS `0.4.0` conformance; push & verify CI green.

*(Note: `R-MIX-3` scene-item blend modes, Luma Key, and Wipes remain deferred to `0.5.0`; hardware-desk SVG FX knob rings in `mfd-controller-svg.cpp` are omitted from `0.4.0`.)*

### 2.2 `2 x 2` Mixed-Version Compatibility Matrix
| Midi Fadester (OBS) | Fadester for OBS (Stream Deck) | Behavior |
|---|---|---|
| `0.4.0` (18 slots) | `0.1.x` (existing) | Connects cleanly (`api_version == 1`). `SurfaceStore` loads all 18 slots (`crossfader..master`, `fx_1..fx_8`); `fx_1..fx_8` appear in the Slot dropdown and render as standard `0..100%` fader cells via `svg-renderer.ts` fallback branch #7. |
| `0.3.0` (10 slots) | `0.2.0` (new) | Connects cleanly (`api_version == 1`). `Faders` group shows 10 slots; `FX & Knobs` group is omitted when no `kind === "fx"` slots exist; `display_value` falls back to `${Math.round(value * 100)}%`; any key configured to `__all_fx__` renders status `"Update Midi Fadester to 0.4.0"`. |
| `0.4.0` (18 slots) | `0.2.0` (new) | Full feature set: grouped `Faders` + `FX & Knobs`, unit-formatted `display_value` (`"14 px"`, `"-40%"`, `"0 (Off)"`), cyan-amber FX cells, and `Clean FX` (`reset_fx`). |

---

## 3. `API-CONTRACT.md` Wire Specification (`api_version: 1`, Additive) (`S3`)

### 3.1 Slot IDs & Slot JSON (`get_state`, `state_changed`, `set_slot`)
- Valid `slot` IDs (`18` stable strings in fixed index order):
  - `0..9`: `"crossfader"`, `"fader_1"` .. `"fader_8"`, `"master"` (`kind`: `"crossfader" | "fader" | "master"`).
  - `10..17`: `"fx_1"` .. `"fx_8"` (`kind`: `"fx"`, default `name`: `"FX 1"` .. `"FX 8"`).
- **Profile independence of `fx_1..fx_8`:** `fx_1..fx_8` remain **`enabled: true`** across all 6 controller profiles (`apc40mk2`, `apcminimk2`, `midimix`, `nanokontrol2`, `launchcontrolxl`, `generic`) even when `fx_knob_count == 0` (`apcminimk2`, `generic`), so Stream Deck + dials and MIDI Learn can always use all 8 FX slots.
- **Slot JSON object:**
  ```json
  {
    "id": "fx_1",
    "name": "FX 1",
    "kind": "fx",
    "enabled": true,
    "target_mode": "unassigned | tbar | crossfade_ab | layer_opacity | audio_volume | filter_param",
    "scene": "",
    "source_a": "V2 - Variety",
    "source_b": "",
    "filter_name": "Bloom Glow",
    "filter_prop": "bloom_intensity",
    "from_val": 0.0,
    "to_val": 0.35,
    "bypass_at_zero": true,
    "curve": "dip_free | full_center | equal_power | linear | cut",
    "afv": false,
    "value": 0.50,
    "display_value": "0.18",
    "cal_min": 0,
    "cal_max": 127,
    "takeover_pending": "none | up | down",
    "parked": false,
    "label": "V2 - Variety: Bloom Glow (bloom_intensity)",
    "available": true,
    "reason": ""
  }
  ```
- **`display_value` formatting (pure C in `src/core/filter-math.c` & `src/core/ui-sentences.c`):**
  - `filter_param`: `mapped = from_val + (to_val - from_val) * value` clamped to `[prop_min, prop_max]`. If `prop_is_int`: `"%lld%s"` (`llround(mapped)`, `prop_suffix`). If float: decimal places derived from `prop_step` (`step >= 1.0 -> 0`, `step >= 0.1 -> 1`, `step >= 0.01 -> 2`, else `3`, with `-0.0` normalized to `0.0`) + `prop_suffix`. When `bypass_at_zero && value <= 0.001f`, appends `" (Off)"`.
  - `audio_volume`: `"-6.0 dB"` or `"-inf dB"`.
  - `tbar`, `crossfade_ab`, `layer_opacity`, `unassigned`: `"%d%%"` (`round(value * 100)`).
- **`fader_changed` event & in-process signal:**
  - `obs-websocket` event: `{ "slot": "fx_1", "value": 0.50, "display_value": "0.18", "origin": "midi" | "ui" | "api" | "auto_fade" | "obs" }`.
  - In-process `midi_fadester_fader_changed` signal (`src/obs/mfd-vendor-api.c:139,375`): registered as `"void midi_fadester_fader_changed(string slot, float value, string origin, string display_value)"`, preserving `calldata_set_float(&cd, "value", (double)clamped)` while additively setting `calldata_set_string(&cd, "display_value", disp)`.
- **Uniform vendor error convention (`set_error_comment` in `src/obs/mfd-vendor-api.c:38-45`):**
  - Every validation or lookup error across `set_slot`, `get_catalog`, and `reset_fx` **must** set both `"error": <sentence>` and `"comment": <sentence>` via `set_error_comment(response_data, sentence)` (matching `ObsVendorClient` in `C:\streamdeck-fadester\packages\streamdeck-fadester-shared\src\obs\obs-vendor-client.ts:250-261`).

### 3.2 Extended `set_slot` & Shared Single-Writer Validation (`mfd_engine_validate_filter_param_binding_locked`)
- Request fields: `{ slot, enabled?, target_mode?, scene?, source_a?, source_b?, filter_name?, filter_prop?, from_val?, to_val?, bypass_at_zero?, curve?, afv?, cal_min?, cal_max? }`.
- **`enabled` semantics:** An explicit boolean overrides automatic enablement from `target_mode`; when absent, target-mode edits still enable the slot and other partial edits preserve its current enabled state. Non-booleans return `"Parameter 'enabled' must be a boolean."`. Enabling validates the binding and single-writer ownership before any mutation. An explicit disable without target/source/filter/range/bypass edits can disable an unavailable binding while retaining its configuration. Disabling releases filter ownership and cancels active fades and queued cleans; re-enabling reacquires ownership without an unsolicited output write. Actual binding/range edits remain validated.
- **Coordinated surface follow-up:** This correction keeps `api_version: 1`. Matching Stream Deck contract, reset-count, T-bar caller, and inspector changes are held for reconciliation with its concurrent `0.3.0` editor work; the earlier bank-or-filter reset clause is superseded by the filter-only target set below.
- One shared engine helper `mfd_engine_validate_filter_param_binding_locked(eng, slot_index, enabled, source_a, filter_name, filter_prop, has_from_to, &from_val, &to_val, out_prop_info, out_err, err_size)` is invoked by mutation paths that validate filter bindings (`mfd_vendor_handle_set_slot`, `mfd_engine_validate_setup_candidate` / `mfd_engine_commit_setup_session`, `mfd_engine_update_slot_config` including disabled-to-enabled toggles, and `deserialize_slots` / config import):
  1. `source_a` (non-empty live source or scene), `filter_name` (non-empty user filter attached to `source_a`; `"__midi_fadester_opacity__"` rejected), and `filter_prop` (`OBS_PROPERTY_FLOAT` or `OBS_PROPERTY_INT` on `filter_name`, including inside nested `OBS_PROPERTY_GROUP`s) are required.
  2. **Single-writer uniqueness check:** If `enabled` is true and another enabled slot `j != slot_index` already has `target_type == MFD_TARGET_FILTER_PARAM` with identical `(source_a, filter_name, filter_prop)`, fails with: `"Setting '<filter_prop>' on '<filter_name>' is already controlled by <other_slot.label>."`
  3. **Range validation:** Finite bounds `prop_min = mfd_filter_clamp_bound(raw_min, false)`, `prop_max = mfd_filter_clamp_bound(raw_max, true)` (`[-1e6, 1e6]` clamp; fallback `[-1e6, 1e6]` if `prop_min >= prop_max`). Default `from_val = prop_min`, `to_val = prop_max` when binding a new `(source_a, filter_name, filter_prop)` without explicit values. Both `from_val` and `to_val` must be finite numbers within `[prop_min, prop_max]` and `fabs(to_val - from_val) >= 1e-6` (rejecting equal endpoints with `"'from_val' and 'to_val' must differ so the control has a range to sweep."`).

### 3.3 New Vendor Request 7: `get_catalog` (Fully Dynamic Source-First Discovery)
- **Request:** `{ "source"?: string }`.
- **Implementation:** Builds `obs_data_array_t` directly from live OBS enumerations (`obs_enum_scenes`, `obs_enum_sources`, `obs_source_enum_filters`, `obs_source_properties` via shared recursive `mfd_collect_filter_numeric_props`) or fake `scene_graph_ops` in unit tests with **zero fixed C-struct array caps**.
- **When `"source"` is omitted or empty:**
  `{ "scenes": [{ "name": "Live Mix", "layers": ["V2", "V3"] }], "audio_sources": ["Desktop Audio"], "filter_hosts": [{ "source": "Live Mix", "is_scene": true, "filter_count": 2 }] }`.
- **When `"source"` is provided:** If `!mfd_engine_source_exists(eng, source)`, calls `set_error_comment(response_data, "Source '<source>' not found. Create the source in OBS first or check its spelling.")`; otherwise enumerates that host's user filters (excluding `"__midi_fadester_opacity__"`) and each filter's `OBS_PROPERTY_FLOAT` / `OBS_PROPERTY_INT` properties (recursing arbitrarily deep into `OBS_PROPERTY_GROUP`s):
  `{ "source": "Live Mix", "is_scene": true, "filters": [{ "name": "Color Correction", "type_id": "color_filter_v2", "enabled": true, "properties": [{ "name": "saturation", "label": "Saturation", "type": "float", "min": -1.0, "max": 5.0, "step": 0.01, "suffix": "", "current": 0.0 }] }] }`.

### 3.4 New Vendor Request 8: `reset_fx` (`CLEAN / ALL FX OFF`)
- **Request:** `{ "ms"?: int }` (`0` default for instant clean snap, or `50..20000` for timed wash-out; `1..49`, `< 0`, or `> 20000` calls `set_error_comment(response_data, "Duration 'ms' must be 0 (instant) or between 50 and 20000 ms.")`).
- **Target set (`eligible_count`):** Every slot `i in 0..17` where `slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM`. `mfd_slot_is_reset_fx_eligible` is shared by the engine and OBS dialog active-button styling; FX strip styling remains independent of reset eligibility.
- **Behavior (including unavailable-slot panic recovery):**
  - **Unwritable slot (`!mfd_engine_is_slot_writable(eng, slot)`):** For both `ms == 0` and `ms >= 50`, cancels `auto_fade_active`, resets `smoother` and `last_applied_val = 0.0f`, clears `has_last_written_mapped = false`, sets `slot->pending_clean_apply_on_avail = true`, calls `mfd_takeover_sync_external(&slot->takeover, 0.0f)`, emits `fader_changed` (`value: 0.0`, `origin: "api"`), and increments `queued_unavailable_count`. When availability returns (`prev_avail == false -> new_avail == true`), `pending_clean_apply_on_avail` immediately applies `0.0f` (`from_val` + bypass) and clears the flag.
  - **Writable slot (`ms == 0`):** Cancels `auto_fade_active`, resets `smoother` to `0.0f`, clears `has_last_written_mapped` to force an authoritative `0.0f` write (`from_val` + aggregated `bypass_at_zero`), calls `mfd_takeover_sync_external(&slot->takeover, 0.0f)`, emits `fader_changed` (`origin: "api"`), and increments `reset_count`.
  - **Writable slot (`ms in 50..20000`):** If `last_applied_val <= 0.0005f`, performs the authoritative `0.0f` snap + takeover sync above (`reset_count++`); otherwise starts `auto_fade` (`from = last_applied_val`, `to = 0.0f`, `duration = ms / 1000.0f`, `auto_fade_is_reset_fx = true`, `reset_count++`). If a slot with `auto_fade_is_reset_fx == true` becomes unavailable mid-ramp, `mfd_engine_tick` snaps `last_applied_val = 0.0f`, `pending_clean_apply_on_avail = true`, and `mfd_takeover_sync_external(..., 0.0f)`.
  - Emits `state_changed` + UI dirty notification.
- **Response:** `{ "eligible_count": <int>, "reset_count": <int>, "queued_unavailable_count": <int>, "ms": <int> }`.

---

## 4. Midi Fadester (`0.4.0`) Technical Design (`C:\midi-fadester`) (`S4`)

### 4.1 Profile Registry, Hardware Knob Pre-Maps & Exact 18-Slot Branching (`src/core/apc40-profile.{h,c}`, `src/obs/mfd-engine.c`)
- `#define MFD_FADER_STRIP_COUNT 10U`, `#define MFD_FX_SLOT_COUNT 8U`, `#define MFD_APC40_DEFAULT_SLOT_COUNT 18U` (`MFD_MAX_FADER_SLOTS = 18`).
- `enum mfd_target_type` adds `MFD_TARGET_FILTER_PARAM = 5` (`mfd_target_mode_to_string` <-> `"filter_param"`).
- `struct mfd_controller_profile_def` adds `int fx_knob_count;` and defines slots `10..17` (`"fx_1" .. "fx_8"`, `"FX 1" .. "FX 8"`, `kind = "fx"`, `default_target = MFD_TARGET_UNASSIGNED`):
  - **`apc40mk2` (Verified, `fx_knob_count = 8`):** Device Knobs 1–8 (`Ch 1 CC 16..23`, matching *Fabel* `APC40_INSTRUMENT_PLAN.md:52`).
  - **`nanokontrol2` (Beta, `fx_knob_count = 8`):** Knobs 1–8 (`Ch 1 CC 16..23`).
  - **`midimix` (Beta, `fx_knob_count = 8`):** Bottom-Row Channel Knobs 1–8 (`Ch 1 CC 18, 22, 26, 30, 48, 52, 56, 60`).
  - **`launchcontrolxl` (Beta, `fx_knob_count = 8`):** Bottom-Row Knobs 1–8 (`Ch 1 CC 49..56`, Factory User Template 1).
  - **`apcminimk2` (Beta, `fx_knob_count = 0`) & `generic` (Custom, `fx_knob_count = 0`):** `addr = {MFD_MIDI_NONE, 0, 0, false}`.
- `mfd_profile_has_physical_slot(prof, idx)`: `0..9` unchanged; `10..17` returns `(idx - 10) < def->fx_knob_count` (`false` on `apcminimk2` and `generic`).
- `bool mfd_slot_is_fx_bank(int slot_index)`: returns `slot_index >= 10 && slot_index < 18`.
- **Exact 18-Slot Branching:**
  1. `mfd_engine_load_profile_defaults`: detaches all active filter bypass leases first; for `i in 0..17`, sets `s->enabled = (mfd_slot_is_fx_bank((int)i) || def->id == MFD_PROFILE_GENERIC) ? true : (mfd_profile_has_physical_slot(def->id, (int)i) && def->slots[i].addr.type != MFD_MIDI_NONE)` and `s->filter_pre_bypass_enabled = true`.
  2. `mfd_engine_select_profile(..., reset_targets=false)`: for `i in 0..9`, detaches filter lease if disabled; for `i in 10..17` (`mfd_slot_is_fx_bank(i)`), keeps `s->enabled = true` and preserves `target_type` + filter/source fields while updating `s->addr = def->slots[i].addr` on non-`generic` profiles (preserving learned `s->addr` on `generic`).
  3. `slots_differ_from_apc40_defaults`: checks `filter_*` fields across `0..17`.
  4. `mfd_engine_set_slot_label`: empty label restores active profile default (`"FX %d", slot_index - 9` for `10..17`).
  5. `resolve_slot_or_error` & `mfd_bridge` valid-slot error strings: `"Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8."`

### 4.2 Exact Data Structures, Discovery API, Bypass Lease Lifecycle & Persistence (`src/obs/mfd-engine.h`)
- **Fields added to `struct mfd_fader_slot` (and config fields mirrored on `struct mfd_setup_candidate` & `struct mfd_slot_config_edit`):**
  ```c
  /* Persisted filter_param configuration: */
  char filter_name[MFD_SOURCE_NAME_MAX];
  char filter_prop[128];
  double filter_from_val;
  double filter_to_val;
  bool filter_bypass_at_zero;
  bool filter_pre_bypass_enabled;    /* persisted pre-bypass enabled state (default true) */

  /* Cached property metadata (refreshed in mfd_engine_reevaluate_slots, never on video tick): */
  bool filter_prop_valid;
  bool filter_prop_is_int;
  double filter_prop_min;
  double filter_prop_max;
  double filter_prop_step;
  char filter_prop_suffix[32];
  char filter_prop_label[128];

  /* Runtime filter write & reset tracking: */
  bool has_last_written_mapped;
  int64_t last_written_int;
  double last_written_float;
  bool writing_filter_settings;
  bool auto_fade_is_reset_fx;
  bool pending_clean_apply_on_avail;
  ```
- **Shared Engine Discovery C API (`mfd_ui_catalog` + `mfd_source_filter_catalog`):**
  - Both `mfd_vendor_handle_get_catalog` (dynamic `obs_data_array_t`) and `mfd_engine_get_source_filter_catalog` (typed C struct for Qt UI) share one recursive walker `mfd_collect_filter_numeric_props(obs_properties_t *props, obs_data_t *settings, callback)` that traverses top-level properties and recurses into nested `OBS_PROPERTY_GROUP` (`obs_property_group_content(prop)`), emitting `OBS_PROPERTY_FLOAT` and `OBS_PROPERTY_INT` items while skipping `OBS_PROPERTY_LIST` and non-numeric types.
  - `struct mfd_ui_catalog` adds flat host summaries:
    ```c
    #define MFD_CATALOG_MAX_FILTER_HOSTS 128
    struct mfd_ui_filter_host_summary {
        char name[MFD_SOURCE_NAME_MAX];
        bool is_scene;
        uint32_t filter_count;
    };
    /* On struct mfd_ui_catalog: */
    bool filter_hosts_truncated;
    size_t filter_host_count;
    struct mfd_ui_filter_host_summary filter_hosts[MFD_CATALOG_MAX_FILTER_HOSTS];
    ```
  - Per-host filter/property inspection for `MfdSetupSheetDialog` Step 2 (`mfd-setup-sheet.cpp`):
    ```c
    #define MFD_HOST_MAX_FILTERS 32
    #define MFD_FILTER_MAX_PROPS 64

    struct mfd_filter_prop_info {
        char name[128];
        char label[128];
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

    /* Caller owns via create/destroy; populated under state_mutex by mfd_engine_get_source_filter_catalog: */
    struct mfd_source_filter_catalog *mfd_source_filter_catalog_create(void);
    void mfd_source_filter_catalog_destroy(struct mfd_source_filter_catalog *cat);
    bool mfd_engine_get_source_filter_catalog(struct mfd_engine *eng, const char *source_name,
                                              struct mfd_source_filter_catalog *out);
    ```
- **Complete `bypass_at_zero` Lease Lifecycle (`struct mfd_filter_bypass_lease leases[18]`):**
  - **Acquisition:** When an enabled, available `filter_param` slot with `filter_bypass_at_zero == true` binds `(source, filter)` and no active lease exists yet, allocates a lease capturing `pre_mfd_enabled = obs_source_enabled(flt)` (or `slot->filter_pre_bypass_enabled` when restoring a persisted slot after OBS restart).
  - **Aggregated enable evaluation (`sync_filter_bypass_lease_locked`):** Across all enabled, available `filter_param` slots $S$ targeting `(source, filter)`: if no slot in $S$ has `filter_bypass_at_zero == true`, restores `obs_source_set_enabled(flt, lease.pre_mfd_enabled)` and releases the lease; else `want_enabled = false` iff **every** slot in $S$ has `filter_bypass_at_zero == true && last_applied_val <= 0.001f`, writing `obs_source_set_enabled` (guarded by `lease.writing_enable = true`) only on change.
  - **Pre-mutation detachment (`detach_filter_binding_locked(eng, slot_index)`):** Called **before** overwriting `enabled`, `target_type`, `source_a_name`, `filter_name`, or `filter_bypass_at_zero` in `set_slot`, `commit_setup_session`, `update_slot_config`, `select_profile`, `load_profile_defaults`, and live `deserialize_slots` replacement — restoring `pre_mfd_enabled` immediately when the last lease owner leaves.
  - **Rename, Removal, Replacement & Shutdown Rules:**
    1. **Host source rename (`mfd_engine_handle_source_rename`):** Atomically renames `lease->source_name` (`prev_name -> new_name`) alongside `slot->source_a_name`.
    2. **Filter rename (`on_obs_filter_rename`):** Atomically renames `lease->filter_name` (`prev_name -> new_name`) alongside `slot->filter_name` for matching `parent_name`.
    3. **Filter or host removal (`on_obs_filter_remove` / `on_obs_source_remove_or_destroy`):** Invalidates and releases any matching `(source_name, filter_name)` lease **without** writing to the removed filter; if a replacement filter with the same name is later added (`on_obs_filter_add`), `mfd_engine_reevaluate_slots` acquires a fresh lease from the replacement filter's live `obs_source_enabled(flt)` before its first bypass write.
    4. **Shutdown (`OBS_FRONTEND_EVENT_EXIT` / `mfd_engine_shutdown`):** Calls `restore_all_bypass_leases_locked(eng)` **before** disconnecting signals while live OBS sources/filters are still valid so OBS never retains MFD-forced `enabled: false` states if the plugin is unloaded/removed, while persisted `filter_pre_bypass_enabled` protects against crashes or mid-session auto-saves.
- **ID-based JSON Deserialization & Conflict Resolution (`deserialize_slots`):**
  - Pre-seeds `0..17` from `mfd_engine_get_active_profile(eng)` defaults, matches JSON entries by `"id"` (`mfd_slot_index_for_id`), skips unknown or duplicate `"id"` entries in the same file, validates enum/field/range bounds via `mfd_engine_validate_filter_param_binding_locked` (**first valid owner in array/`0..17` order wins**; any later persisted slot targeting the exact same `(source_a, filter_name, filter_prop)` is logged with `obs_log(LOG_WARNING, ...)` and downgraded to `MFD_TARGET_UNASSIGNED`), and initializes runtime value from live OBS inverse-mapped value when reachable else `0.0f` (Clean), never `1.0f`.

### 4.3 Pure-C Filter Math (`src/core/filter-math.{h,c}`) & Exact Filter Write Transaction
- Pure-C17 `src/core/filter-math.{h,c}`: `mfd_filter_clamp_bound`, `mfd_filter_map_value`, `mfd_filter_should_write_mapped` (quantized `int64_t` / `double` delta gate + `0.0`/`1.0` endpoint snap), and `mfd_filter_format_display_value`.
- `apply_filter_param_output_locked`: checks `slot->cached_available && slot->filter_prop_valid` (zero `obs_source_properties()` calls), gates via `mfd_filter_should_write_mapped`, writes via `obs_source_get_settings` -> `obs_data_set_int`/`set_double` -> `obs_source_update` (wrapped in `slot->writing_filter_settings = true/false`), updates `last_written_*`, evaluates `sync_filter_bypass_lease_locked`, and releases `settings`, `flt`, and `host` on every path.

### 4.4 Weak-Ref Tracked Signal Ownership, Re-Entrancy & Dynamic Metadata Revalidation
- **Entry-Owned Weak-Ref Signal Registry (`struct mfd_tracked_signal_entry`):**
  - Each connected source/filter owns a heap `struct mfd_tracked_signal_entry { struct mfd_engine *eng; obs_source_t *key_ptr; obs_weak_source_t *weak_src; obs_weak_source_t *weak_parent; signal_handler_t *sh; bool is_filter; }` passed as `private_data` to `signal_handler_connect`.
  - Why weak refs (`obs_source_get_weak_source`):
    1. Filter callbacks resolve **both** `flt = obs_weak_source_get_source(entry->weak_src)` and `parent = obs_weak_source_get_source(entry->weak_parent)` safely on any signal (`"rename"`, `"update_properties"`, `"enable"`, `"update"`) without calling `obs_filter_get_parent()` outside `obs.h:1163-1167`'s guaranteed `video_render`/`filter_remove` contexts!
    2. Holds zero strong references at rest (`Number of memory leaks: 0`).
  - **Source-type filtering & disconnect-before-free ordering:**
    - `on_obs_source_create` inspects `obs_source_get_type(src)`: connects host `"filter_add"` / `"filter_remove"` on `OBS_SOURCE_TYPE_INPUT` and `OBS_SOURCE_TYPE_SCENE` (plus `"item_add"`/`"item_remove"`/`"reorder"` on scenes), and ignores `OBS_SOURCE_TYPE_FILTER` and `OBS_SOURCE_TYPE_TRANSITION`.
    - `on_obs_filter_add` (and initial `obs_source_enum_filters` on existing inputs/scenes) connects `"rename"`, `"update_properties"`, `"enable"`, `"update"` on user filters (`id != MFD_INTERNAL_OPACITY_FILTER_ID`), recording `weak_parent = obs_source_get_weak_source(host_src)`.
    - On `"filter_remove"`, `"source_remove"`, `"source_destroy"`, `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED` resync, and `disconnect_obs_signals`, disconnects all `signal_handler_t` callbacks **first**, releases `weak_src` and `weak_parent`, removes from `eng->tracked_signals`, and frees the entry.
- **Recursive Lock & Non-Cyclic Re-Entrancy:** All callbacks acquire recursive `eng->state_mutex`; self-writes short-circuit immediately on `slot->writing_filter_settings` / `lease->writing_enable`; external `"update"` edits update `t_ext`, `has_last_written_mapped = false`, smoother, Soft Takeover (`mfd_takeover_sync_external`), and emit `fader_changed` (`origin: "obs"`) without calling `obs_source_update`.
- **Dynamic Metadata & Range Drift Revalidation (`mfd_engine_reevaluate_slots`):** Revalidates on `"update_properties"` that `filter_prop` is still `OBS_PROPERTY_FLOAT` / `OBS_PROPERTY_INT` and configured `from_val` / `to_val` still fall within `[prop_min, prop_max]` (`fabs(to_val - from_val) >= 1e-6`), marking `cached_available = false` with an explicit fix sentence if violated.

### 4.5 OBS Studio Tools Dialog UI (`src/ui/`) & CMake Registration (`S4.5`–`S4.6`)
- **`Strips` Tab (`mfd-settings-dialog.cpp`):** Segmented pill **`[Faders]`** (`0..9` physical) | **`[FX & Knobs (8)]`** (`10..17` always visible) + **`[Clean / All FX Off]`** button (`mfd_engine_reset_fx(eng, 0)`).
- **3-Step `Set up` Sheet (`mfd-setup-sheet.cpp`):**
  - Step 1 adds **`Filter or effect setting`**.
  - Step 2 populates `Layer or Scene:` from `mfd_ui_catalog.filter_hosts`; on host selection (or engine `ui_notify` catalog revision bump), calls `mfd_engine_get_source_filter_catalog(eng, host_name, host_cat)` to populate `Filter:` and `Setting:` combos, configures `At 0% (Clean)` / `At 100% (Max)` `QDoubleSpinBox`es (`setRange`, `setDecimals`, `setSingleStep`, `setSuffix`, `[Use current OBS value]`, `Turn filter off at 0% (saves GPU)`, truncation notice if `*_truncated`), and validates via `mfd_engine_validate_setup_candidate` before enabling `[Next]`.
  - Step 3 (`Try it`) shows live simulated `display_value` preview until `[Done]`.
- **`Advanced` Tab (`<= 1380 px`):** 18 rows (`0..9` + section divider + `10..17`), reusing existing 10 columns (`Left` = Source/Scene, `Right` = `"<Filter>: <Setting> [<from>..<to>]"` + `[Edit...]`, `Feel` = `"Linear"`, `Also fade audio` = `"Off at 0%"`, `Output` = `display_value`).
- **CMake (`CMakeLists.txt`, `tests/CMakeLists.txt`):** Register `src/core/filter-math.{h,c}` in plugin target, `test-filter-mod` (`15/15` `ctest`), `test-ui-sentences`, `test-vendor-api` (+ `ui-sentences.c`), `test-ui-dialog`, and `_production_guard_files` (keeping `check-locale-keys` green).

---

## 5. Stream Deck Fadester (`0.2.0`) Technical Design (`C:\streamdeck-fadester`) (`S5`)

1. **`API-CONTRACT.md`, `src/types/contract.ts`, `FadesterClient` & `SurfaceStore`:**
   - Widen `SlotKind` (`"fx"`), `TargetMode` (`"filter_param"`), and `FaderChangeOrigin` (`"obs" | (string & {})`).
   - Add `filter_name?`, `filter_prop?`, `from_val?`, `to_val?`, `bypass_at_zero?`, `display_value?` to `FadesterSlot`, plus `resetFx({ ms })` and `getCatalog({ source })` on `FadesterClient`.
   - In `src/obs/fadester-client.ts:131-138`, extract `vendorEventData.display_value` onto `FaderChangedEvent`; in `packages/streamdeck-fadester-shared/src/surface-store.ts:84-93`, copy `ev.display_value` onto `existing.display_value`.
2. **`Fader` Dial (`src/types/settings.ts`, `src/actions/dial-math.ts`, `src/actions/fader-dial.ts`, `com.jbrick2070.fadester-for-obs.sdPlugin\ui\fader-dial.html`, `pi-common.js`, `svg-renderer.ts`):**
   - `settings.ts:18-29`: Add `"clean_0"` to `DialPressAction` and `"clean_all_fx"` to `DialLongPressAction`.
   - `dial-math.ts`: Add `{ kind: "reset_fx"; request: ResetFxRequest }` to `ResolvedDialCommand`; map `"clean_0"` -> `set_fader_value(0)` and `"clean_all_fx"` -> `reset_fx({ ms: 0 })`.
   - `fader-dial.ts:271-306`: Dispatch `cmd.kind === "reset_fx"` via `resetFx(cmd.request)` and validate `pressAction` / `longPressAction` against allowed sets in `mergeSettings`.
   - `com.jbrick2070.fadester-for-obs.sdPlugin\ui\fader-dial.html` & `pi-common.js`: Add `<option value="clean_0">Clean (0 %)</option>` to `#press-select` and `<option value="clean_all_fx">Clean All FX</option>` to `#long-press-select`; in `pi-common.js:populateSlotsDropdown`, group real slots into `<optgroup label="Faders">` (`s.kind !== "fx"`) and `<optgroup label="FX & Knobs">` (`s.kind === "fx"`, omitted when no `fx` slots exist).
   - `svg-renderer.ts`: Render `truncateLabel(slot.display_value || pctText, 10)` and cyan-to-amber FX bar (`#06B6D4` -> `#F59E0B`, stroke `>= 3 px`, max 2 text lines) when `slot.target_mode === "filter_param"` or `slot.kind === "fx"`.
3. **`Auto-fade` Key (`src/actions/auto-fade-key.ts`, `com.jbrick2070.fadester-for-obs.sdPlugin\ui\auto-fade-key.html`, `pi-common.js`, `svg-renderer.ts`):**
   - `com.jbrick2070.fadester-for-obs.sdPlugin\ui\auto-fade-key.html` & `pi-common.js`:
     - Mark `<select id="slot-select" data-allow-all-fx="true"></select>`; in `pi-common.js:populateSlotsDropdown`, prepend `<option value="__all_fx__">All FX — Clean / All FX Off</option>` whenever `selectEl.dataset.allowAllFx === "true"` and (`list.some((s) => s.kind === "fx")` || `selectedSlot === "__all_fx__"`).
     - Change `#duration-input` (`auto-fade-key.html:101`) to `min="0" max="20000" step="50"` (with `0 = Instant` hint) and fix line 138 from `settings.durationMs || 1000` to `settings.durationMs !== undefined ? settings.durationMs : 1000` so `durationMs: 0` is preserved across inspector reopens!
   - `auto-fade-key.ts`:
     - `onSlotValueChanged`: re-render when `slotId === this.settings.slot || this.settings.slot === "__all_fx__"`.
     - `mergeSettings`: preserve `0` (`rounded === 0 ? 0 : Math.max(50, Math.min(20000, rounded))`).
     - `handleKeyDown`: dispatch `resetFx({ ms: durationMs })` for `"__all_fx__"`, a target-aware `setFaderValue` for single-slot `durationMs === 0`, else `autoFade`. A zero request on a T-bar or unknown target must use `release: false`; `release: true` finishes a T-bar even when the requested value is zero.
     - `renderSvgString` & `KeyCellRenderInput<TSlot>` (`packages/streamdeck-fadester-shared/src/svg-renderer.ts:322`): add `slots?: readonly TSlot[]` to `KeyCellRenderInput<TSlot>`, pass `slots: this.store.getSlots()`, and handle `input.slotId === "__all_fx__"` before `if (!slot)` (rendering orange `CLEAN FX` / `N ACTIVE`, green `FX CLEAN`, or `"Update Midi Fadester to 0.4.0"` when `slots` has no `kind === "fx"` entries).
     - `N ACTIVE` counts only slots with `enabled !== false && target_mode === "filter_param"` and a nonzero value, matching provider eligibility in either bank. FX-bank color styling remains broad; it does not imply eligibility for Clean FX.

---

## 6. Verification, Mutation & Verify-at-Build Matrix (`S6`)

1. **`C:\midi-fadester` (`15/15` `ctest` + Live Portable OBS 33 Self-Test):**
   - `test-filter-mod`: `[from_val, to_val]` forward/inverted mapping, `int` rounding, `step`-precision `display_value` + `(Off)` suffix, quantized mapped-value write gate + endpoint snap, `±DBL_MAX` clamping.
   - `test-apc40-profile`: 18-slot registry, knob CCs across all 6 profiles (keeping Beta profiles labeled Beta until hardware-tested), truthful `mfd_profile_has_physical_slot` vs `mfd_slot_is_fx_bank`.
   - `test-ui-sentences`: Filter status sentences (`deleted`, `filter removed`, `setting missing`, `range changed`), `display_value` readouts, and `FX 1..8` strip cards.
   - `test-vendor-api`:
     - Nested `OBS_PROPERTY_GROUP` fixture containing `float`, `int`, and excluded `list`/boolean siblings: verifies `get_catalog` + `mfd_engine_get_source_filter_catalog` discovery, selection, mapping, and non-numeric rejection.
     - Universal single-writer conflict rejection across `set_slot`, `Set up` (`mfd_engine_commit_setup_session`), `Advanced` (`mfd_engine_update_slot_config` + disabled-to-enabled toggle), and JSON import/deserialization (first-valid-owner-wins in `0..17` order).
     - Complete `bypass_at_zero` lease lifecycle: multi-slot shared-filter aggregation, pre-mutation `detach_filter_binding_locked`, host source rename, filter rename, filter remove + replacement reacquisition, manual `"enable"` override, persisted `filter_pre_bypass_enabled` across simulated restart, repeated scene-collection switches, and shutdown restoration.
     - External `"update"` edit -> inverse `t_ext` + Soft Takeover pickup + `origin: "obs"` `display_value` emit; dynamic `"update_properties"` disappearance + type/range-drift invalidation.
     - `reset_fx` (`eligible_count`, `ms = 0`, `ms = 500`, unavailable-slot `queued_unavailable_count` -> `pending_clean_apply_on_avail` recovery, leaving `fader_1` `layer_opacity` untouched) + fake-backend multi-threaded `state_mutex` stress (`16g`).
   - `test-ui-dialog`: `[Faders]` <-> `[FX & Knobs (8)]` switch, 3-step `Set up` `Filter or effect setting` flow, `[Clean / All FX Off]`, 18-row `Advanced` table (`<= 1380 px`), and screenshot verification.
   - **Live Portable OBS 33 Self-Test (`src/obs/mfd-tbar.cpp:run_live_selftest_worker` via `scripts/run-live-selftest.ps1`):** Verifies live `obs-websocket` `get_catalog`, `set_slot(filter_param)` on `color_filter_v2`, non-deadlocking signal callbacks (`update`, `enable`, `update_properties`, source/filter rename, filter remove/recreate), `GetSourceScreenshot` pixel change, `reset_fx({ ms: 0 })` pixel restoration + `obs_source_enabled(flt) == false`, unbind/shutdown restore (`obs_source_enabled(flt) == true`), and `Number of memory leaks: 0`.
2. **`C:\streamdeck-fadester` (Vitest + PI Regression + Live OBS `0.4.0` Conformance):**
   - Unit + fake `obs-websocket` 5 server + Property Inspector DOM tests (`pi-common.js`, `fader-dial.html`, `auto-fade-key.html` with 10-slot and 18-slot payloads, persisted `__all_fx__`, and `durationMs: 0` reopen preservation) + SVG stroke/line/mutation tests.

---

## Appendix A — Verified Cross-Repo Grounding Excerpts

1. **`C:\streamdeck-fadester\com.jbrick2070.fadester-for-obs.sdPlugin\ui\pi-common.js:34-86` & `auto-fade-key.html:101,138`:**
   Real Property Inspector files referenced by `manifest.json`; updated for `<optgroup>` grouping, `data-allow-all-fx="true"`, `min="0"`, and `settings.durationMs !== undefined`.
2. **`C:\streamdeck-fadester\packages\streamdeck-fadester-shared\src\svg-renderer.ts:322-350` & `src\obs\fadester-client.ts:131-138`:**
   `KeyCellRenderInput<TSlot>` extended with `slots?: readonly TSlot[]` for `__all_fx__`; `FadesterClient` forwards `display_value` on `fader_changed`.
3. **`C:\Users\jeffr\Documents\obs-playlister\.deps\include\obs.h:1163-1167,1207`:**
   `obs_filter_get_parent` is only guaranteed inside `video_render`/`filter_audio`/`filter_video`/`filter_remove`; `mfd_tracked_signal_entry` stores `obs_weak_source_t *weak_src` and `weak_parent` for leak-free, callback-safe lookup.
