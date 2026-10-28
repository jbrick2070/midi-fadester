# Midi Fadester

**Midi Fadester** is a continuous MIDI fader controller and video layer mixer plugin for OBS Studio 33 on Windows x64, built for any class-compliant MIDI controller with plug-and-play support for the **Akai APC40 mkII**. It provides channel-aware MIDI learn (`Ch 1–16` + `CC/Note`, 7-bit and 14-bit CC pairs), 30 ms slew smoothing, soft takeover (pick-up), Studio Mode T-bar crossfades (fixing OBS 33's Studio Mode off crash and `release_tbar` cancellation bug), single-layer opacity fades, two-source dip-free layer crossfades (`Source A ↔ Source B`), and cubic-dB audio faders (Phase 1), followed by `Thru | Bus A | Bus B` multi-strip mixing, filter modulation, and hotkeys (Phase 2), hardware blend modes (`NAM`/Lighten, Additive, Screen), Luma Key, and Wipe faders (Phase 3), a dockable mixer panel (Phase 4), and APC40 mkII RGB LED feedback (Phase 5).

> **Disclaimer:** Midi Fadester is an independent third-party plugin for OBS Studio. It is not made, endorsed, or supported by the OBS Project or by Akai Professional / inMusic Brands.

## Installation (OBS Studio 33, Windows x64)

Download the latest release from [GitHub Releases](https://github.com/jbrick2070/midi-fadester/releases):

- **Standard Install (`midi-fadester-<version>-windows-x64-Installer.exe` or `.zip`):** Installs into `C:\ProgramData\obs-studio\plugins\midi-fadester\` using the OBS 33 non-legacy plugin layout.
- **Portable Install (`midi-fadester-<version>-windows-x64-Portable.zip`):** Extract directly into your portable OBS Studio 33 root folder (`<OBS root>\plugins\midi-fadester\`).

Open OBS Studio 33 and choose **Tools → Midi Fadester — Video Mixer & Fader Control**. When an **Akai APC40 mkII** is connected or selected in **MIDI In**, the **Akai APC40 mkII** profile auto-selects (`Crossfader` on `Ch 1 CC 15`, `Track Faders 1–8` on `Ch 1–8 CC 7`, and `Master Fader` on `Ch 1 CC 14`).

## Choosing Your Crossfader Mode: `Studio Mode T-Bar` vs. `Two-Source Crossfade (A ↔ B)`

Midi Fadester supports two distinct ways to crossfade video—understanding the difference makes setup intuitive:

1. **`Studio Mode T-Bar` (Default on Crossfader — Broadcast Switcher Style):**
   - Works out of the box with all of your existing OBS scenes when **Studio Mode** is enabled.
   - **Why the left `Preview` box swaps sides at the end of a throw:** In OBS Studio Mode, the **right box (`Program`)** is your actual live stream/recording output, while the **left box (`Preview`)** is your "next scene on deck" monitor. As you throw the crossfader, Midi Fadester drives the T-bar linearly across the full `0–100%` throw (`CC 127 = 1.0`, zero idle timers) so `Program` on the right fades smoothly into the new scene without snapping early at `90%`. Only when the fader enters the **`3%` target rail zone** of its calibrated range (`CC >= 124`, or `CC <= 3` on a reversed throw; calibrated `[cal_min, cal_max]` auto-widens and persists per profile and control) does Midi Fadester release the T-bar, causing OBS to swap the old scene into the left `Preview` box so it is queued up for your next throw and reset its white T-bar slider to `0%`. Returning the fader into the **`3%` origin rail zone** mid-throw cancels the transition cleanly without changing `Program`. Watch the **right (`Program`)** box to see your true live output—it never jumps. With **Bidirectional T-Bar (`0 → 1 then 1 → 0`)** checked, your next throw back in the opposite physical direction starts cleanly from `0%` without needing to reset the fader.
2. **`Two-Source Crossfade (A ↔ B)` (DJ / Hardware Video Mixer Style — Fixed Left = A, Right = B):**
   - If you want a traditional DJ or hardware video mixer crossfader where **Left (`0%`) is always Source A**, **Center (`50%`) is a 50/50 blend**, and **Right (`100%`) is always Source B** across the full `0–100%` throw with **no Studio Mode Preview/Program swap**:
   - Place both video sources (or two nested scenes!) inside a single scene with **Source A** stacked directly below **Source B**.
   - Set **Target Mode** to **`Two-Source Crossfade (A ↔ B)`** and choose **Source A** and **Source B**. The default **Dip-Free Dissolve** holds Source A at `100%` while Source B fades in (`0 → 100%`), giving a true optical crossfade with **0% black background bleed-through**.

## Development & AI Disclosure

This project is developed with the assistance of AI coding tools under human direction, review, and hardware verification on OBS Studio 33.

## Documentation

- [REQUIREMENTS.md](REQUIREMENTS.md) — Functional, fader target (`R-FADE-*`), two-source & bus mixer (`R-MIX-*`), smoothing/takeover/LED feedback (`R-FEEL-*`), WinMM MIDI (`R-MIDI-*`), threading, and testing requirements, plus approved operator decisions (`D-1..D-7`).
- [PLAN.md](PLAN.md) — Pre-planning OBS 33 & APC40 mkII audit findings, architecture, file layout, phased implementation plan (Phase 1 MVP through Phase 5), test matrix, risk register, and autonomous decisions (`D-8..D-20`).
- [PLAN-UI.md](PLAN-UI.md) — `0.2.0` three-zone `Strips` UI, 3-step `Set up` sheet, live APC40 mkII SVG desk view, concurrency/locking architecture, and 4-round `kibitz` review disposition.
- [TESTING.md](TESTING.md) — Automated `ctest` suite (`14` targets), rendered `0.2.0` dialog state screenshot gallery, and live OBS 33 + `loopMIDI` verification guide.
- [LICENSE](LICENSE) — GNU General Public License v2.0.
