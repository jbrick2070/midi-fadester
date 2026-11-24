# Visual Sidechain for developers

How Visual Sidechain works inside, custom shaders, the Tape Deck over obs-websocket, the `visual-sidechain` vendor API and the Midi Fadester and Stream Deck integration. For using it, see the [Visual Sidechain guide](VISUAL-SIDECHAIN.md).

## How it works

Visual Sidechain has been part of Midi Fadester since **0.9.0** and shares its version; before that it was a separate plugin (last release 0.8.0, [history](https://github.com/jbrick2070/visual-sidechain)). 0.9.0 changed nothing in what it does. With 1.5.0 every product of the Fadester suite moved to 1.5 together; Fadester for OBS is still released on its own.

- **Platforms.** OBS Studio 33 on Windows x64 (NVIDIA RTX first) and macOS 13.0+ (universal). On a Mac, OBS turns the shaders into GLSL (OpenGL renderer) or Metal Shading Language (OBS 33's experimental Metal renderer); CI builds every built-in shader with both translators, and on 2026-11-17 all 11 compiled in OBS 33.0.1 on an Apple Silicon Mac and looked and moved as they do on Windows. The NVIDIA filters do not exist on a Mac; the Audio Modulator drives any other filter there the same way.
- **Audio analysis.** By default it taps **Track 1 (the main mix)** (or any individual OBS audio source / mixer track) and runs a real-time dual-resolution Hann-windowed FFT: **4096-point** for high-resolution sub-bass and kick detection $\le 250\text{ Hz}$, paired with **2048-point** for low-latency mids and highs.
- **GPU textures.** It uploads a `512x2` GPU FFT + waveform texture (`audio_fft`) and a `32x1` logarithmic spectrum texture (`audio_spectrum`).
- **Shaders.** 11 original `GPL-2.0-or-later` FFT-reactive shaders: **Spectral Edge / Border Aura**, **Wave Warp & Glitch**, **Chromatic Prism & Bloom**, **Visualizer Overlays**, **Zoom Pulse & Barrel Lens**, **Kaleidoscope**, **Feedback Trails**, **Strobe & Flash**, **Mirror & Split**, **Hex Pulse Lattice** and **Polygon Tunnel**.
- **Audio Modulator** sidechains sibling filters, including OBS 33's built-in **NVIDIA Blur**, **NVIDIA Background Blur** and **NVIDIA Background Removal**.
- **Signals out and faders in.** All audio and beat signals are available over the `obs-websocket` 5 vendor API (`vendor_name = "visual-sidechain"`). All 18 Fadester slots (`crossfader`, `fader_1`..`fader_8`, `master`, `fx_1`..`fx_8`) can act as modulation sources in the Audio Modulator and as float uniforms (`fader_crossfader`, `fader_1`..`fader_8`, `fader_master`, `fader_fx_1`..`fader_fx_8`) in shaders.

## Custom shaders

- Custom shader parameters can use `effect_mix` and `audio_reactivity` independently of the host's percent sliders.
- Names that collide with internal controls, such as `preset_shader`, `custom_low_hz` or `vs_effect_mix_pct`, produce a compile message naming the parameter to rename. A failed shader passes the source through until a valid shader is loaded.
- In full-effect mode, `vs_effect_mix`, `vs_reactivity_scale` and `vs_flash_envelope` are host-supplied uniforms, so they do not appear as editable shader parameters.
- `vs_flash_envelope`, `audio_kick_safe`, `audio_snare_safe` and `audio_flux_safe` (0 to 1) are the shared 3 Hz flash clock's limited transient outputs; use them for brightness, tint or inversion. A shader's own `onset_threshold` and `max_flash_rate_hz` parameters, if it declares them, set its trigger threshold and personal rate limit (up to 3 Hz).
- For a custom shader that also runs on a Mac, write casts as `float(x)` rather than `(float)x` and leave out HLSL attributes such as `[unroll]`: OBS on macOS translates shaders to GLSL or Metal, and those compilers refuse both forms.

## Tape Deck over `obs-websocket`

- `SetSourceFilterSettings` on the deck filter accepts `tape` (a full path), `speed_percent` (25 to 800), `tape_pitch`, `loop`, `start_seconds`, `playing`, `speed_fader_slot` (-1 for none, 0 to 17 in the order crossfader, fader 1 to 8, master, FX 1 to 8), `speed_fader_min_percent` and `speed_fader_max_percent`.
- `TriggerHotkeyByName` reaches the hotkeys: `hotkeyName` is `tubester_deck.play_pause`, `tubester_deck.cue_to_start`, `tubester_deck.speed_up`, `tubester_deck.speed_down` or `tubester_deck.normal_speed`, and `contextName` is the Browser Source's name.
- To play an ended tape again, trigger `tubester_deck.play_pause`. Writing `playing` false and then true works only if the deck has applied the first write before the second arrives: OBS applies a filter's settings once per frame, so two quick writes merge into one with no change. Wait until `get_decks` shows `playing: false` before writing true.
- `get_decks` (in the vendor API below) reads every deck's state. The deck writes one more setting itself, `hotkey_bindings`, a copy of its hotkey bindings; leave it alone.

## `obs-websocket` 5 Vendor API

- **Vendor Name:** `"visual-sidechain"`
- **Request:** `CallVendorRequest` with `vendorName: "visual-sidechain"`, `requestType: "get_signals"` returns `{ success, version, api_version: 1, level, sub_bass, bass, mid, high, kick, snare, beat, beat_phase, spectral_centroid, spectral_flux, estimated_bpm, bpm, spectrum: [{ band: 0..31, value }] }`.
- **Event:** `VendorEvent` (`vendorName: "visual-sidechain"`, `eventType: "signals_changed"`) streams the same payload coalesced to at most 30 events/sec.
- **Request:** `requestType: "get_decks"` returns every Tape Deck as its page last reported it: `{ success, version, api_version: 1, decks: [{ source, filter, deck_id, role, state, tape, speed_percent, effective_speed_percent, speed_fader, tape_pitch, playing, loop, start_seconds, linked, reported? }] }`. `playing` is the Playing setting; `state` and `reported` say what the page does. `speed_percent` is the Speed setting; `effective_speed_percent` is the speed the deck asks for, which is the Speed Fader's while that fader is live. `speed_fader` is `{ slot, slot_id, min_percent, max_percent, live }`: `slot` is -1 and `slot_id` empty when no fader is bound, and `live` is true while the bound fader drives the speed. `role` is `owner`, `waiting` (another deck drives that Browser Source), `pending`, `wrong_parent` or `detached`. `reported` (`current_tape, rate, position, duration, paused, ended, ready_state, error, no_picture`) is present only while `linked` is true; `current_tape` is false while it still describes an earlier tape.
- **Event:** `VendorEvent` (`vendorName: "visual-sidechain"`, `eventType: "decks_changed"`) emits the same `{ success: true, version, api_version: 1, decks }` payload whenever a deck's presence, settings, speed, or playing/paused/ended state changes (emitted immediately on explicit commands, hotkeys, settings, or attachment changes; tick-driven Speed Fader updates coalesce to at most 30 events/sec, and ordinary playback position ticks do not emit).
- **Request:** `requestType: "deck_command"` accepts `{ deck, op, percent?, steps? }` (`deck` is the Browser Source name or `deck_id`; `op` is `"play_pause"`, `"cue_to_start"`, `"speed_set"`, `"speed_step"`, or `"normal_speed"`), reuses the five `tubester_deck.*` hotkey paths over the Browser Source's `javascript_event` proc, snaps `speed_set` (`25` to `800`) to whole percentages (coalescing rapid calls to one applied change per video frame), returns `{ success: true, version, api_version: 1, deck, deck_id, op, playing, speed_percent, effective_speed_percent, speed_fader_live, state }`, and returns `{ success: false, version, api_version: 1, error, comment }` with a plain-sentence explanation on invalid or unknown targets. Tape Decks remain outside Panic and Restore visuals.

## Midi Fadester and Stream Deck integration

- **18 slots.** Midi Fadester 0.4.0 and Fadester for OBS expose an 18-slot desk. All 18 slots are available as Shader Filter mix/reactivity/parameter controls and Audio Modulator signal/depth sources. The in-process bridge uses the same slot IDs as Stream Deck.
- **Saved ids.** Existing saved fader indices `0..9` and modulation signal IDs `200..209` stay unchanged; the FX bank appends indices `10..17` and signal IDs `210..217`. Older Midi Fadester providers continue to supply their ten legacy slots; unsupported FX slots read zero.
- **From the Stream Deck Property Inspector:** assign a Midi slot to **Filter setting (FX)**, select the Visual Sidechain filter, then choose a numeric setting from its catalog.
- **Depth trims.** Audio Modulator depth trims are `slot_1_amount_pct` through `slot_8_amount_pct`: choose **At 0 % = 0** and **At 100 % = 100** to remove audio-driven movement at the zero rail. Amount zero holds the modulator target at its configured Minimum; choose that Minimum as the desired neutral setting. The property itself supports `-100..100`; using that full range makes the zero rail maximum ducking. For ducking, keep a negative Modulation Amount and bind a Depth Fader instead of sweeping the amount across zero, so zero depth keeps the Maximum endpoint of the ducking branch.
- **Shader percent controls.** Shader Filter's host percent controls are `vs_effect_mix_pct` and `vs_audio_reactivity_pct`; custom numeric shader parameters also appear in the catalog.
- **Clean / All FX Off** (Midi Fadester) resets enabled `filter_param` bindings in either bank to their configured zero endpoint. It does not reset arbitrary audio/layer assignments or invoke Visual Sidechain's separate global **Panic / Restore visuals** state. A numeric trim reset changes that trim; enabling **Bypass at 0 %** also bypasses the entire target filter, with shared ownership managed by Midi Fadester.
- **One control path per setting.** Use either the filter's internal fader assignment or a Midi `filter_param` binding for a setting, not both.

## Older design documents

These were written while Visual Sidechain was a separate project and are kept in its old repository.

- [REQUIREMENTS.md](https://github.com/jbrick2070/visual-sidechain/blob/main/REQUIREMENTS.md): functional, dual-FFT/DSP, GPU shader, NVIDIA RTX modulator, baseline save-protection, threading, licensing and testing requirements (`R-*`) and decisions (`D-1..D-4`).
- [PLAN.md](https://github.com/jbrick2070/visual-sidechain/blob/main/PLAN.md): phased implementation plan, OBS 33 and upstream audit findings, 3-layer architecture, per-step named tests and mutation checks, risk register and verify-at-build checklist.
- [TESTING.md](https://github.com/jbrick2070/visual-sidechain/blob/main/TESTING.md): headless CTest, mutation-check and live OBS Studio 33.0.0 hardware verification results.
- [LICENSE](../LICENSE): GNU General Public License v2.0.
