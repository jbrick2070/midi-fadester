# Midi Fadester — Requirements

Status: 2026-10-27. Approved by the operator on 2026-10-27 with Phase 1 scope tightened to solve the core fader problem end-to-end first, and advanced mixer features ordered into Phases 2–5 (decisions recorded in Section 9).
Companion to [PLAN.md](PLAN.md); requirement IDs (`R-*`) and operator decisions (`D-*`) are referenced across the plan, test suite, and commit messages.

**Midi Fadester** (`midi-fadester.dll`) is a native C/C++ plugin for OBS Studio 33 on Windows x64 that gives class-compliant MIDI controllers—starting with the **Akai APC40 mkII**—direct, continuous fader control over OBS Studio Mode T-bar crossfades, two-source dip-free layer crossfades, single-layer opacity fades, and cubic-dB audio levels with 30 ms slew smoothing, 14-bit CC support, and soft takeover (Phase 1), followed by multi-strip `Thru | Bus A | Bus B` mixing, filter modulation, and hotkeys (Phase 2), hardware blend modes, Luma Key, and Wipe faders (Phase 3), a dockable mixer UI (Phase 4), and APC40 mkII RGB LED feedback (Phase 5).

---

## 1. Identity, Branding, and Disclosure (`R-ID-*`)

| ID | Requirement |
|---|---|
| `R-ID-1` | **Plugin Name & Binary:** The human-readable name is **Midi Fadester**. The module binary is `midi-fadester` (`midi-fadester.dll`). Local repository: `C:\midi-fadester` (`D-7`); remote origin: `https://github.com/jbrick2070/midi-fadester` (public, branch `main`). |
| `R-ID-2` | **No "OBS" in Branding:** Per OBS Project packaging and forum policy, the word `"OBS"` MUST NOT appear in the plugin's name, branding, repository name, dock title, or registered filter display names. Internal source/filter display names use the `"Midi Fadester — ..."` prefix. |
| `R-ID-3` | **Third-Party Disclaimer:** `README.md`, release notes, and documentation MUST state explicitly: *"Midi Fadester is an independent third-party plugin for OBS Studio. It is not made, endorsed, or supported by the OBS Project or by Akai Professional / inMusic Brands."* |
| `R-ID-4` | **AI Coding Tools Disclosure:** `README.md` and release notes MUST include an honest disclosure that the project is developed with the assistance of AI coding tools under human direction, review, and hardware verification. |
| `R-ID-5` | **100% Local & Open Source:** Zero cloud services, zero API keys, zero paid services, zero telemetry, and zero Python at runtime or in the build of the plugin itself. |

---

## 2. Licensing and Provenance (`R-LIC-*`)

| ID | Requirement |
|---|---|
| `R-LIC-1` | **Repository License:** The repository license is **GNU General Public License v2.0** (`LICENSE` at commit `df0ea1c`). `LICENSE` MUST NOT be removed or weakened. |
| `R-LIC-2` | **Source Header Stance:** Every original source file (`src/*.c`, `src/*.h`, `src/*.cpp`, `src/*.hpp`, `data/effects/*.effect`, `CMakeLists.txt`, `cmake/*.cmake`, `tests/*.c`) MUST carry an `SPDX-License-Identifier: GPL-2.0-or-later` header so our code is compatible with both `GPL-2.0-only` and `GPL-2.0-or-later` codebases. |
| `R-LIC-3` | **Shared Filter Modulation Architecture:** The numeric filter property modulator (`src/obs/mfd-filter-mod.c`, Phase 2) shares design and code patterns with the operator's sister plugin **Visual Sidechain** (`C:\visual-sidechain`, `GPL-2.0-or-later`). All shared code is first-party `GPL-2.0-or-later`. |
| `R-LIC-4` | **No Unlicensed or Incompatible Code:** Never copy code from unlicensed repositories or `GPL-3.0-only` / `CC BY-NC-*` sources. Never contact upstream plugin authors unless explicitly instructed by the operator. |

---

## 3. Target Platform, OBS 33 Layout, and Build (`R-PLAT-*`)

| ID | Requirement |
|---|---|
| `R-PLAT-1` | **Platform & Toolchain:** Target **Windows 11 x64** and **OBS Studio 33.0.0** (`libobs` API 33). Compiles with Visual Studio 2022 Build Tools (`MSVC 14.44`, Windows SDK `10.0.26100.0`) and CMake `>= 3.28` (`C17` / `C++17`, `/W4 /WX`). |
| `R-PLAT-2` | **OBS 33 Non-Legacy Plugin Layout:** Installs and packages exclusively in the OBS 33 layout (`C:\ProgramData\obs-studio\plugins\midi-fadester\midi-fadester.dll` with `data\manifest.json`, `data\locale\en-US.ini`, and `data\effects\mfd_layer_mixer.effect`; portable layout `<OBS root>\plugins\midi-fadester\`). Legacy `bin\64bit` paths are never built or installed. The plugin MUST NOT show a `Legacy` badge in the OBS 33 Plugin Manager. |
| `R-PLAT-3` | **Shared Read-Only Dependency Cache:** Reuses the local prebuilt dependency cache in `C:\Users\jeffr\Documents\obs-playlister\.deps` (`obs-studio-33.0.0`, `obs-deps-2026-10-23-x64`, `obs-deps-qt6-2026-10-23-x64`) in strictly read-only mode without modifying `obs-playlister`. |
| `R-PLAT-4` | **Zero External MIDI Dependencies:** Uses Windows Multimedia MIDI (`winmm.dll` / `winmm.lib`: `midiInOpen`, `midiOutOpen`, `midiInStart`, `midiOutShortMsg`, `midiOutLongMsg`) directly. Links `libobs`, `obs-frontend-api`, `Qt6::Widgets`, and `winmm`. |

---

## 4. WinMM MIDI I/O, Channel-Aware Learn, and Device Profiles (`R-MIDI-*`)

| ID | Phase | Requirement |
|---|---|---|
| `R-MIDI-1` | **Phase 1** | **WinMM Exclusive-Access Error Reporting (No Silent Fallback):** On Windows legacy `winmm` (`midiInOpen`), only one process can hold a MIDI input port open at a time (`MMSYSERR_ALLOCATED`). When `midiInOpen` (or `midiOutOpen` in Phase 5) fails, the plugin MUST log the exact WinMM error code and device name AND display a clear error banner in the UI: *"Cannot open MIDI device '<name>': device is in use by another application (WinMM allows only one app at a time; close DAWs/browsers using this controller and click Reconnect)."* Silent fallback is forbidden. |
| `R-MIDI-2` | **Phase 1** | **Disconnect & Reconnect Resilience:** If an open MIDI controller is unplugged (`MIM_CLOSE`), the plugin closes the stale WinMM handle cleanly on the UI thread, updates the UI status to *"Disconnected — waiting for <name>"*, and provides a one-click **Reconnect / Rescan** action (plus automatic 2-second rescan while disconnected). |
| `R-MIDI-3` | **Phase 1** | **Channel-Aware MIDI Addressing:** Every control binding is addressed by `(msg_type, channel, control_number)` where `msg_type` is `CC`, `NOTE`, or `PITCH_BEND`, `channel` is `0..15` (MIDI Channels 1–16), and `control_number` is `0..127`. Binding by CC number alone is forbidden because controllers like the Akai APC40 mkII send `CC 7` on Channels 1–8 for Track Faders 1–8. |
| `R-MIDI-4` | **Phase 1** | **One-Click MIDI Learn:** Clicking **Learn** on any mapping row enters a channel-aware capture state. Moving any hardware fader/knob or pressing any button/pad captures its `(msg_type, channel, control_number)` (and automatically detects whether a 14-bit CC MSB/LSB pair `CC 0..31` + `CC 32..63` is being transmitted on that channel), assigns it to the row, and exits Learn mode. |
| `R-MIDI-5` | **Phase 1** | **7-Bit and 14-Bit Continuous Resolution:** Standard 7-bit CC (`0..127`) normalizes to `[0.0, 1.0]` (`v / 127.0f`). When a controller sends 14-bit CC pairs (`CC 0..31` MSB + `CC 32..63` LSB on the same channel, `0..16383`) or 14-bit Pitch Bend (`0..16383`), the parser combines MSB and LSB into `[0.0, 1.0]` (`v / 16383.0f`). |
| `R-MIDI-6` | **Phase 1 (Input) / Phase 2–5 (Buses & LEDs)** | **Built-In Plug-and-Play Profile for Akai APC40 mkII:** Auto-selected when a MIDI device matching `"APC40 mkII"` is chosen, or selectable manually:<br>• **Phase 1 Default Fader Map (`D-1`):**<br>  – **Crossfader (`Ch 1, CC 15`):** Defaults to **Program Crossfade (Studio Mode T-bar)** when Studio Mode is on (works out of the box with existing scenes), and can also be switched to **Two-Source Layer Crossfade (`Source A ↔ Source B`)**.<br>  – **Track Faders 1–8 (`Ch 1–8, CC 7`):** Ready-mapped fader slots for **Single-Layer Opacity Fade**, **Two-Source Dip-Free Crossfade**, or **Audio Level**.<br>  – **Master Fader (`Ch 1, CC 14`):** Master Audio Level or Master Layer Opacity.<br>• **Phase 2 Additions:** `A|B` Crossfader Assign buttons (`Ch 1–8, Note 66`), Track/Device Knobs (`CC 48–55`, `CC 16–23`) for filter modulation, and Clip/Scene pads (`Notes 0–39`, `82–86`) for scene select, transitions, and hotkeys.<br>• **Phase 5 Additions (`D-4`):** SysEx Mode 2 (`0x42`) Introduction (`F0 47 7F 29 60 00 04 42 01 00 00 F7`), RGB clip-grid LEDs, `A|B` bus LEDs, Mute/Activator LEDs, and Knob LED rings. |

---

## 5. Fader Targets and Video Mixer Capabilities (`R-FADE-*`, `R-MIX-*`)

### 5.1 Phase 1 Core Targets (Solving the Fader Problem First)

| ID | Phase | Requirement & OBS 33 Mechanism |
|---|---|---|
| `R-FADE-1` | **Phase 1** | **Program Crossfade (Studio Mode T-Bar) with Both OBS 33 Fixes & Loud Internal-Widget Error:**<br>Drives the Studio Mode transition T-bar (`0..1023`).<br>• **Fix 1 — Studio Mode Gate (Crash Prevention):** Verified in OBS 33.0.0 (`OBSBasic.hpp:1519`, `OBSBasic_StudioMode.cpp:259`, `OBSStudioAPI.cpp:120`, `OBSBasic_Transitions.cpp:752`): `OBSBasic::tBar` is an uninitialized or deleted raw `QSlider *` whenever Studio Mode is off, so calling `obs_frontend_get_tbar_position()` or `obs_frontend_release_tbar()` while Studio Mode is off crashes OBS. The plugin MUST check `obs_frontend_preview_program_mode_active()` before every T-bar call and display *"Studio Mode is off — enable Studio Mode to use the T-bar"* when inactive.<br>• **Fix 2 — Release-Cancellation Fix + Loud Error on Internal Widget Change:** In OBS 33.0.0 (`OBSStudioAPI.cpp:115-118`), `obs_frontend_set_tbar_position(pos)` calls `OBSBasic::TBarChanged(pos)` without updating `tBar->setValue(pos)`. Because `tBar->value()` stays `0`, calling `obs_frontend_release_tbar()` reads `val = 0 <= T_BAR_CLAMP` and cancels the transition back to the previous scene. When Studio Mode is active, the plugin locates the `QSlider` with `property("class") == "slider-tbar"` under `obs_frontend_get_main_window()`, calls `setValue(pos)`, and invokes `obs_frontend_release_tbar()` once per throw at the 90 % completion threshold (`pos >= 921`, matching OBS's `(tBar->maximum() - val) <= T_BAR_CLAMP` where `T_BAR_CLAMP = 1024 / 10 = 102`). **Because this reaches into OBS's internal Qt widget tree and could break in a future OBS version, if `obs_frontend_preview_program_mode_active()` is true but the `"slider-tbar"` `QSlider` cannot be found, the plugin MUST log a `LOG_ERROR` and show a loud UI error banner (`"Cannot find OBS Studio Mode T-bar widget ('slider-tbar') — incompatible OBS version"`), never failing silently.**<br>• **Rail Behavior — Parked State, Completion Peak & 8 % Hysteresis Rescale:** When a throw reaches the 90 % completion threshold (`t >= 0.90f` / `pos >= 921`), `obs_frontend_release_tbar()` is called exactly once, the smoother resets, and the slot enters `MFD_TAKEOVER_PARKED`. While parked, no slider writes happen and the state machine keeps tracking the furthest position reached (`parked_peak`, the completion peak) as the fader continues toward the physical end. The slot leaves `MFD_TAKEOVER_PARKED` only when the fader moves more than 8 % (`> 0.08f`) back from `parked_peak` (absorbing rail jitter and remaining travel to the physical stop); at that moment it flips polarity and rescales `[rescale_origin, opposite_physical_rail]` to `[0.0f, 1.0f]` so the fader's current position is 0 % of the new transition and the opposite physical rail is 100 %, eliminating any visible jump at unparking. |
| `R-FADE-2` | **Phase 1** | **Single-Layer Opacity Fade (`mfd_opacity_filter`):**<br>Fades any individual video source or nested scene in and out (`0.0% .. 100.0%`) independent of Studio Mode.<br>• Registers `"Midi Fadester — Layer Opacity"` (`id = "midi_fadester_opacity_filter"`, `OBS_SOURCE_TYPE_FILTER`, `OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB`). When a source is bound to a Layer Fade or Two-Source Crossfade slot, the plugin looks up or auto-attaches an instance named `"__midi_fadester_opacity__"` via `obs_source_get_filter_by_name` / `obs_source_filter_add`, leaving all of the user's own filters untouched.<br>• Uses `data/effects/mfd_layer_mixer.effect` (`DrawAlpha` technique) to multiply premultiplied RGBA by `opacity` (`0.0..1.0`). Because `PSAlpha` outputs premultiplied alpha (`col * opacity`), `mfd_layer_filter_render` MUST push `gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA)` around `obs_source_process_filter_tech_end`; leaving OBS's default `GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA` blend state active would multiply `RGB` by `Src.a` (`opacity`) a second time ($t^2$ double-multiply). When `opacity >= 0.999f`, `video_render` calls `obs_source_skip_video_filter` for zero GPU cost (`D-6`). |
| `R-MIX-1` | **Phase 1** | **Two-Source Layer Crossfade (`Source A ↔ Source B`) with Dip-Free Dissolve Curve (`D-2`):**<br>Any single fader or crossfader can bind directly to two sources or nested scenes (**Source A** and **Source B**) to crossfade between them inside a scene without Studio Mode.<br>• **Dip-Free Video Mixer Dissolve (Default):** In standard Porter-Duff `Source Over` compositing over a black canvas, setting two stacked layers to `50%` alpha each yields $0.5B + (1-0.5)(0.5A) = 0.5B + 0.25A$ (letting `25%` black bleed through at midpoint). Our default **Dip-Free Dissolve** holds the lower layer at `100%` ($\alpha_A = 1.0$ for all $t \in [0, 1]$) while the upper layer fades in ($\alpha_B = t$), producing a true $(1-t)A + tB$ optical crossfade with **0% black bleed-through** across the entire throw.<br>• Also provides selectable **Full-Center (`min(1, 2*(1-t))` / `min(1, 2*t)`)**, **Equal-Power Cosine**, and **Linear (`1-t` / `t`)** curves.<br>• **Rail Behavior:** Uses the same completion-peak parked state (`MFD_TAKEOVER_PARKED` at $\ge 90\%$ throw progress), 8 % hysteresis (`> 0.08f` retreat from `parked_peak`), and unpark rescaling to the opposite physical rail as `R-FADE-1`, mapped to `1.0f` at the upper rail (Source B) and `0.0f` at the lower rail (Source A) so unparking has zero visible jump. |
| `R-FADE-3` | **Phase 1** | **Audio Level (OBS Cubic dB Curve):**<br>Controls any audio source's volume (`obs_source_set_volume`) using OBS's exact `OBS_FADER_CUBIC` law (`libobs/obs-audio-controls.c:81-99`): deflection $d \in [0.0, 1.0]$ maps to $\text{mul} = d^3$ ($\text{dB} = 60 \log_{10}(d)$), and inverse mapping for soft takeover computes $d = \sqrt[3]{\text{mul}}$ (`cbrtf`). Includes optional **Audio-Follows-Video (AFV)** checkbox on layer fades (`D-6`). |

### 5.2 Later-Phase Video Mixer Extensions (Phases 2–3)

| ID | Phase | Requirement |
|---|---|---|
| `R-MIX-2` | **Phase 2** | **8-Strip `Thru \| Bus A \| Bus B` Crossfader Matrix & N-Source Chain:**<br>Assign any of the 8 channel strips to **Thru**, **Bus A**, or **Bus B** (via UI or APC40 mkII `A|B` buttons `Note 66` on Ch 1–8). Each strip's opacity is $\text{StripFader}_k \times \text{BusGain}(\text{Bus}_k, x_{\text{xfade}})$, allowing 2 to 8 sources to be balanced on vertical faders and cross-mixed between Bus A and Bus B. Also supports sweeping a chain of $3..8$ sources on a single fader. |
| `R-FADE-4` | **Phase 2** | **Any Numeric Filter Setting:**<br>Drives any `OBS_PROPERTY_FLOAT` or `OBS_PROPERTY_INT` setting on any filter attached to any source or scene (Blur, Color Correction, Transform, NVIDIA RTX filters, Visual Sidechain) with `obs_weak_source_t` safety, `[min_val, max_val]` mapping, per-tick batching, and an epsilon gate (`0.001`). |
| `R-FADE-5` | **Phase 2** | **Buttons, Scene Grid, Hotkeys & Timed Fade-to-Black:**<br>Maps MIDI Note On/Off and button CCs to Preview/Program scene select, Trigger Transition, Timed Fade-to-Black over $N$ ms (`obs_transition_start(..., NULL)`), Audio Mute toggle/momentary, Scene Item visibility, Stream/Record toggles, and any registered OBS hotkey (`obs_enum_hotkeys` + `obs_hotkey_trigger_routed_callback`). |
| `R-MIX-3` | **Phase 3** | **Hardware Video Mixer Blend Modes, Luma Key Fader & Wipe Faders:**<br>• **Blend Modes (`obs_sceneitem_set_blending_mode`):** Per-layer selection of `Normal`, **NAM / Non-Additive Mix (`OBS_BLEND_LIGHTEN`, `GS_BLEND_OP_MAX`)**, `Additive`, `Screen`, `Multiply`, and `Darken` (auto-selecting the **Full-Center** crossfade curve for `NAM`/`Additive`/`Screen`/`Luma Key` per `D-2`).<br>• **Luma Key Fader (`DrawLumaKey` in `mfd_layer_mixer.effect`):** Sweeps luminance key threshold + softness from `0.0 → 1.0` so highlights or dark silhouettes key in before completing to full frame.<br>• **Geometric Wipe Fader (`DrawWipe` in `mfd_layer_mixer.effect`):** Continuous fader-driven Horizontal, Vertical, Radial Circle, and Box wipes with adjustable feathering. |

---

## 6. Feel: Smoothing, Soft Takeover, and Controller Feedback (`R-FEEL-*`)

| ID | Phase | Requirement |
|---|---|---|
| `R-FEEL-1` | **Phase 1** | **30 ms Slew / Exponential Smoothing:** Every continuous fader slot runs a deterministic smoother (`src/core/smoother.c`) ticked on the OBS video thread (`obs_add_tick_callback`, `dt` in seconds) using exponential slew $\alpha = 1 - e^{-\Delta t / \tau}$ with configurable time constant $\tau \in [0\text{ ms}, 250\text{ ms}]$ (default **`30 ms`**; `0 ms` = instant bypass). Snaps cleanly to exact `0.0`, `1.0`, or target within `0.0005`. |
| `R-FEEL-2` | **Phase 1** | **Soft Takeover (Pick-Up Mode):** If a target's live value in OBS changed externally, moving the physical fader MUST NOT jump the target.<br>• While `|fader_norm - target_norm| > pickup_threshold` (default `0.015`, ~2 MIDI steps), the slot enters `MFD_TAKEOVER_WAITING` and ignores fader changes while displaying a directional hint (`Move ▲` or `Move ▼`).<br>• Once a fader movement segment `[prev_norm, curr_norm]` crosses or lands within `pickup_threshold` of `target_norm`, it transitions to `MFD_TAKEOVER_ENGAGED` and tracks seamlessly. |
| `R-FEEL-3` | **Phase 5** | **Controller Lights & Bidirectional MIDI Out Feedback:** Sends dirty-checked MIDI Out updates to the controller: APC40 mkII SysEx Mode 2 (`0x42`) init, RGB clip-grid pads (Red = Program, Green = Preview, Cyan = Active Layer, Amber = Assigned), `A|B` bus LEDs (Off/Yellow/Orange), Track Mute/Activator LEDs (solid/off/blink on takeover wait), and Knob LED rings (`CC 48–55`, `CC 16–23`). |

---

## 7. Threading, Testing, and Working Rules (`R-THR-*`, `R-TST-*`, `R-STD-*`)

| ID | Requirement |
|---|---|
| `R-THR-1` | **Three-Thread Safety Contract:**<br>1. **WinMM Callback Thread (`midiInProc`):** Strictly real-time and lock-free. Parses `MIM_DATA` bytes and pushes fixed-size `mfd_midi_event_t` structs into a lock-free SPSC ring buffer (`src/core/midi-queue.c`). Zero heap allocations, zero mutexes, zero OBS API calls, and zero WinMM calls inside `midiInProc`.<br>2. **OBS Video Tick Thread (`obs_add_tick_callback`):** Drains the SPSC MIDI queue, updates 14-bit CC state, evaluates soft takeover, dip-free crossfader math, and the 30 ms smoother (`dt`), applies Layer Opacity values and Audio Levels (`obs_source_set_volume`), and queues Qt-thread T-bar actions.<br>3. **Qt UI Thread:** Executes `obs_frontend_*` calls and `"slider-tbar"` `QSlider` sync, manages the Tools Dialog (Phase 1) and Dock (Phase 4), and opens/closes WinMM devices. |
| `R-TST-1` | **Headless Pure-C Unit Test Suite (`ctest`):** All core logic in `src/core/` MUST be pure C17 with zero OBS or WinMM dependencies and tested via `ctest` in Phase 1 (`test-midi-queue`, `test-midi-parser`, `test-smoother`, `test-takeover`, `test-mixer-math`, `test-apc40-profile`, `test-shader-syntax`). |
| `R-TST-2` | **Portable OBS 33 Live Harness Verification:** Automated and interactive verification in `C:\Users\jeffr\OBS-Studio-33-Portable` (working directory `bin\64bit`, closed via `CloseMainWindow`): verifies clean plugin load without `Legacy` badge, T-bar crossfade completion and Studio Mode off guard, two-source dip-free layer crossfade, and zero memory leaks on shutdown (`obs_get_num_leaks() == 0`). |
| `R-STD-1` | **Code & Repo Hygiene:**<br>• Plan first (`REQUIREMENTS.md` and `PLAN.md`) and wait for operator approval before writing plugin code.<br>• Run the full test suite (`ctest`) after every code change without being asked.<br>• UTF-8 without BOM in every file; zero 0-byte files; meaningful identifiers; never use the banned placeholder word (`d-u-m-m-y`; use `placeholder` or `stub`); no profanity anywhere.<br>• Format C/C++ with `clang-format` and CMake with `gersemi` rules (2-space indent, 120 columns).<br>• Maximum one `git push` attempt per cycle; verify `HEAD == origin/main`, zero 0-byte files, and zero BOMs after every push. |

---

## 8. Phase Roadmap Summary (Approved 2026-10-27)

| Phase | Scope |
|---|---|
| **Phase 1 (Core MVP — Usable on APC40 mkII)** | WinMM MIDI In (`MMSYSERR_ALLOCATED` UI banner), channel-aware MIDI Learn (`Ch 1–16` + `CC/Note`, 7-bit + 14-bit CC), APC40 mkII default fader map, 30 ms slew smoothing, Soft Takeover, Studio Mode T-bar binding (with Studio Mode guard, `"slider-tbar"` `QSlider` release fix, and loud error if internal widget is missing), single-layer opacity fade + two-source (`Source A ↔ Source B`) dip-free crossfade, cubic audio fader + AFV, Tools menu configuration dialog, headless `ctest` suite, and portable OBS 33 verification. |
| **Phase 2 (Buses, Filter Mod & Buttons)** | 8-strip `Thru \| Bus A \| Bus B` crossfader bus matrix (`Note 66` `A|B` assign), N-source chain crossfader, sibling/target numeric filter setting modulator (`mfd-filter-mod.c`), and button/pad triggers (scene select, transition, mute, timed Fade-to-Black over $N$ ms, any OBS hotkey). |
| **Phase 3 (Blend Modes, Luma Key & Wipes)** | Scene-item blend modes (`NAM / Lighten`, `Additive`, `Screen`, `Multiply`, `Darken`), Full-Center crossfade curve auto-selection, and GPU `DrawLumaKey` (Bright/Dark Luma Key fader) + `DrawWipe` (Horizontal, Vertical, Radial, Box wipe faders) in `mfd_layer_mixer.effect`. |
| **Phase 4 (Dockable Video Mixer Panel)** | Dockable 8-strip Video Mixer Panel (`obs_frontend_add_dock_by_id`) inside the OBS main window. |
| **Phase 5 (APC40 mkII Lights & MIDI Out)** | APC40 mkII SysEx Mode 2 (`0x42`) introduction, dirty-checked RGB clip-grid LEDs, `A|B` bus LEDs, Mute/Activator LEDs (with soft-takeover blink), and Knob LED rings. |

---

## 9. Resolved Operator Decisions (`D-1 .. D-7`, Approved 2026-10-27)

| ID | Topic | Approved Resolution |
|---|---|---|
| `D-1` | **APC40 mkII Crossfader Default Mode** | **Default the Crossfader (`CC 15`) to the Studio Mode T-Bar** when Studio Mode is on (works immediately with existing scenes); Two-Source Layer Crossfade (`Source A ↔ Source B`) and Phase 2 `A/B` Buses are selectable when scenes are set up as layers. |
| `D-2` | **Default Crossfade Curve Between Two Sources** | **Dip-Free Video Mixer Dissolve** ($\alpha_A = 1.0, \alpha_B = t$) for Normal alpha layers so 0% black bleeds through across the entire throw, and **Full-Center (`100% / 100%` at midpoint)** for Phase 3 `NAM (Lighten)`, `Additive`, `Screen`, and `Luma Key`, with a manual curve dropdown. |
| `D-3` | **UI Surface in OBS** | **Tools → Midi Fadester dialog in Phase 1** (keeps Phase 1 tight and immediately usable), followed by the **Dockable Video Mixer Panel in Phase 4**. |
| `D-4` | **APC40 mkII Hardware Mode (Phase 5)** | **Mode 2 (`0x42`, Alternate Ableton Live Mode)** via SysEx Intro (`F0 47 7F 29 60 00 04 42 01 00 00 F7`), with a Mode 0 Generic (`0x40`) option. |
| `D-5` | **Where Mappings Are Saved** | **Two-tier persistence:** MIDI device selection + controller profile saved in global OBS user config; scene/source fader bindings saved inside each OBS Scene Collection (`obs_frontend_add_save_callback`) with JSON export/import. |
| `D-6` | **Layer Fade Behavior at 0% Opacity** | **Keep source active at `opacity = 0.0`** (so media/Playlister sources stay warm for instant fade-in, while `opacity >= 0.999` in Normal mode bypasses the shader via `obs_source_skip_video_filter`), plus a per-mapping **Audio-Follows-Video (AFV)** checkbox. |
| `D-7` | **Local Workspace Folder** | Confirmed `C:\midi-fadester` (same root layout as `C:\visual-sidechain`). |
