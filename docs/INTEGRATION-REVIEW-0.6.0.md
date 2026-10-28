# Three-plugin integration review — 28 October 2026

Midi Fadester, Fadester for OBS (Stream Deck), and Visual Sidechain now share **0.6.0**; the Stream Deck manifest uses **0.6.0.0**. These are development builds ready for operator acceptance. They remain separate plugins, with Midi required by the Stream Deck plugin and Visual Sidechain optional. No Marketplace submission or release tag is part of this work.

## Changes and evidence

| Component | Integration changes | Validation |
|---|---|---|
| Midi Fadester | Filter-only Clean across both banks; explicit Enabled lifecycle; abort active T-bar before configuration replacement; full errors for paused or unavailable movement. | RelWithDebInfo and Release build; CTest **15/15 each**; dedicated live OBS wrapper passes screenshot, loopMIDI, T-bar, bridge and filter checks; zero reported leaks. |
| Fadester for OBS | Target-aware zero/Finish; protected inspector drafts; partial ranges and explicit full-range reset; Enabled and calibration; current readouts; filter-only Clean counts and custom All FX labels. | **69/69** Vitest; TypeScript; build; Elgato validation and packaging; **17/17** mutations detected. Browser checks at 320 px cover range preservation, disabled target changes and saved instant All FX settings. |
| Visual Sidechain | All 18 Midi slots in bridge, pickers and shader uniforms; newer signal values win over stale reads; distinct slot-qualified numeric labels. | Debug, RelWithDebInfo and Release build; CTest **13/13 each**, including interleaving and numeric catalog checks. |

Three requested GPT-6.1 Terra reviews and scoped rechecks informed the fixes. Earlier Claude Opus reviews are preserved as historical evidence. Formatting and file hygiene are checked before committing. The delivery receipt records exact final hashes and remote CI results; Stream Deck has no `.github/workflows` directory, so local validation is its available evidence.

The live run used only the dedicated MidiFadester OBS root through `scripts/run-live-selftest.ps1`. OBS and loopMIDI were closed afterward. This does not certify the final Visual Sidechain GPU output or the new Stream Deck UI on hardware. Fake-provider timed fades settle immediately; they validate dispatch and state, not real-time rendering.

## Decisions

- **D21.** Integrate all three current main branches because the user lifted the Stream Deck hold and authorized Visual Sidechain fixes and final pushes.
- **D22.** Use 0.6.0 across the three components because the user requested matching versions and it exceeds every current version.
- **D23.** Keep separate install packages because OBS and Stream Deck have different runtimes and the inspector will continue evolving.
- **D24.** Preserve existing published tags and history; create no new tags, forks or worktrees because this is integration and acceptance work.
- **D25.** Apply only changed inspector fields and preserve disabled state because multiple surfaces edit the same slots.
- **D26.** Preserve T-bar release semantics and make callers target-aware because zero must cancel while Finish completes a throw.
- **D27.** Make Visual Sidechain consume all 18 slots and reject stale readbacks because current Midi signals must remain authoritative.
- **D28.** Keep explicit 0→100 ranges when mapping positive Visual Sidechain amount controls because the native amount range also supports negative ducking.
- **D29.** Gate Marketplace submission on operator hardware acceptance because browser and unit tests cannot establish dial feel or real-device usability.
- **D30.** Preserve the earlier blocked temporary review copy because automatic approval review rejected its cleanup; it is not a Git fork or worktree.

## Three-minute operator script

Use a disposable scene setup with the 0.6.0 builds already installed; this script is manual, not an additional automation harness.

1. **0:00–1:00:** In Stream Deck, select a Fader dial. Bind an FX slot through Source → Filter → Setting. Set a partial range, apply, reopen, and verify it persists. Disable/re-enable without losing the binding.
2. **1:00–2:00:** Turn the dial and compare the OBS value. Test short press and hold. In Studio Mode, start a partial throw, return to zero to cancel, then try explicit Finish. Create an All FX key with duration 0; confirm filters clean while audio and layers stay unchanged.
3. **2:00–3:00:** Choose an FX slot as a Visual Sidechain input or map a positive amount control explicitly from 0 to 100. Verify the intended effect and Clean behavior. Record confusing labels or dial feel for the next Stream Deck iteration, then close the test apps.

Remaining: operator Stream Deck acceptance, joint Visual Sidechain GPU verification, listing assets and Elgato review. Packaging and current official submission links are in the Stream Deck repository's `PUBLISHING.md`. MIDI Learn and device selection remain in OBS until their vendor API is extended.
