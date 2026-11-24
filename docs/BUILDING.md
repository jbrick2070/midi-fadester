# Building Midi Fadester from source

[Back to the README](../README.md).

You need Visual Studio 2022 Build Tools (MSVC 14.44 and the Windows 10.0.26100 SDK) and CMake 3.28 or newer. The first configure downloads the OBS 33.0.0 sources, the prebuilt OBS dependencies and Qt6 named in `buildspec.json`.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
ctest --preset windows-x64 -C RelWithDebInfo --output-on-failure
```

The built plugin lands in `build_x64\rundir\RelWithDebInfo\midi-fadester\`; copy that folder into your OBS `plugins` folder.

- **Tests.** Seventy-eight `ctest` tests (36 for Midi Fadester, 42 for Visual Sidechain) run in about 25 seconds without OBS or hardware: the MIDI parser and queue, the smoother, pick-up, the crossfade curves, the T-bar state machine, the filter math, every UI sentence, the vendor API with a fake websocket, a real Qt6 dialog and the Fadester Desk dock that render the thirteen screenshots in `docs/screenshots/`, Visual Sidechain's audio analysis, modulator, shader parser and Tape Deck, and repository hygiene (UTF-8 without BOM, no timers, every locale key present, every translation in step with the English file).
- **Live self-test.** `scripts\run-live-selftest.ps1` boots a dedicated portable OBS 33 with a loopMIDI port, throws the T-bar eight ways, checks GPU pixels through `GetSourceScreenshot`, and exits with zero leaks. [TESTING.md](../TESTING.md) explains the setup.
- **Formatting.** CI checks C and C++ with clang-format 17 and CMake with gersemi, using the repository's `.clang-format` and `.gersemirc`.
- **Design.** The MIDI input thread is lock-free and never calls OBS; the engine has no timers and runs on MIDI and OBS events. [PLAN.md](../PLAN.md) has the thread contract.
