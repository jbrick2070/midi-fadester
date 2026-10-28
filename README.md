# Midi Fadester

The coordinated development version is **0.6.0**, shared with Fadester for OBS and Visual Sidechain. Each remains a separate plugin; no 0.6.0 release tag or Marketplace submission is implied by the source version.

**Midi Fadester** is a continuous MIDI fader controller and video layer/filter mixer plugin for OBS Studio 33 on Windows x64, built for any class-compliant MIDI controller with plug-and-play support for the **Akai APC40 mkII** and full two-way `obs-websocket` sync with **Stream Deck + (`Fadester for OBS`)**. It provides an **18-slot control desk** (`crossfader`, `fader_1..fader_8`, `master`, plus 8 dedicated **`fx_1..fx_8`** filter/knob slots), channel-aware MIDI learn (`Ch 1–16` + `CC/Note`, 7-bit and 14-bit CC pairs), 30 ms slew smoothing, bidirectional Soft Takeover (including external OBS filter edits), Studio Mode T-bar crossfades (fixing OBS 33's Studio Mode off crash and `release_tbar` cancellation bug), single-layer and whole-scene opacity fades, two-source dip-free layer crossfades (`Source A ↔ Source B`), cubic-dB audio faders, **Filter & Effect Parameter Modulation (`filter_param`)** on any numeric float/int OBS filter setting (Blur, Color Correction, Chroma/Luma Key, Shaderfilter uniforms, Visual Sidechain reactivity) with shared-filter `bypass_at_zero` lease management, and one-click **`Clean / All FX Off` (`reset_fx`)** panic reset.

**Clean / All FX Off** resets enabled filter-parameter bindings in either bank. Audio volume, A/B crossfades, layer opacity, and T-bars stay unchanged even when assigned to an FX slot. This corrects the earlier bank-based reset scope; the eligible count can decrease for repurposed FX slots.

The vendor API's `set_slot.enabled` field accepts a boolean and preserves the binding when a slot is disabled. Explicit disable cancels active fades and pending cleans, so re-enabling cannot replay an old clean request. Re-enabled filter slots read their current value from OBS so the next adjustment continues from the live setting.

> **Disclaimer:** Midi Fadester is an independent third-party plugin for OBS Studio. It is not made, endorsed, or supported by the OBS Project or by Akai Professional / inMusic Brands.

## Installation (OBS Studio 33, Windows x64)

Download the latest release from [GitHub Releases](https://github.com/jbrick2070/midi-fadester/releases):

- **Standard Install (`midi-fadester-<version>-windows-x64-Installer.exe` or `.zip`):** Installs into `C:\ProgramData\obs-studio\plugins\midi-fadester\` using the OBS 33 non-legacy plugin layout.
- **Portable Install (`midi-fadester-<version>-windows-x64-Portable.zip`):** Extract directly into your portable OBS Studio 33 root folder (`<OBS root>\plugins\midi-fadester\`).

Open OBS Studio 33 and choose **Tools → Midi Fadester — Video Mixer & Fader Control**. When a recognized controller is connected, Midi Fadester auto-detects its USB MIDI port and loads its control layout (or lets you switch layouts under **Change controller → Layout** on **Strips** or **Profile** on **Advanced**).

## Supported MIDI Controllers (`0.4.0` Profile Registry — 10 Faders + 8 FX/Knob Slots)

Midi Fadester uses a declarative **Controller Profile Registry** (`src/core/apc40-profile.c`) that pairs each controller's factory MIDI CC map (faders + dedicated FX knobs) with its physical strip count, `[Show controller]` vector desk topology, and explicit verification tier. All `fx_1..fx_8` slots remain active across every profile for **Stream Deck +** dials/keys and `obs-websocket` even when no physical MIDI knobs are connected:

| Controller | Profile ID (`obs-websocket`) | Support Tier | Physical Faders & Pre-Mapped FX Knobs (`fx_1..fx_8`) |
|---|---|---|---|
| **Akai APC40 mkII** | `apc40mk2` | **Hardware-Verified** | **10 faders:** Crossfader (`Ch 1 CC 15`), Faders 1–8 (`Ch 1–8 CC 7`), Master (`Ch 1 CC 14`) + **8 Device Knobs:** `FX 1–8` (`Ch 1 CC 16–23`) |
| **Akai APC mini mk2** | `apcminimk2` | **Beta** *(Manufacturer spec — untested on hardware)* | **9 faders:** Faders 1–8 (`Ch 1 CC 48–55`), Master (`Ch 1 CC 56`); `FX 1–8` active via Stream Deck / MIDI Learn |
| **Akai MIDImix** | `midimix` | **Beta** *(Manufacturer spec — untested on hardware)* | **9 faders:** Faders 1–8 (`Ch 1 CC 19, 23, 27, 31, 49, 53, 57, 61`), Master (`Ch 1 CC 62`) + **8 Row-3 Knobs:** `FX 1–8` (`Ch 1 CC 18, 22, 26, 30, 48, 52, 56, 60`) |
| **Korg nanoKONTROL2** | `nanokontrol2` | **Beta** *(Manufacturer spec — untested on hardware)* | **8 sliders:** Sliders 1–8 (`Ch 1 CC 0–7`, Default CC Mode) + **8 Knobs:** `FX 1–8` (`Ch 1 CC 16–23`) |
| **Novation Launch Control XL** | `launchcontrolxl` | **Beta** *(Manufacturer spec — untested on hardware)* | **8 faders:** Faders 1–8 (`Ch 1 CC 77–84`, Factory User Template 1) + **8 Row-3 Pan/Device Knobs:** `FX 1–8` (`Ch 1 CC 49–56`) |
| **Generic MIDI (Custom)** | `generic` | **Custom / 1-Click MIDI Learn** | **Up to 18 controls:** 10 fader slots + 8 `FX 1–8` slots via **Set up** / **Learn** or Stream Deck + |

> **Beta Controller Note:** Profiles marked **Beta** are built from published manufacturer MIDI implementation specs and have not yet been verified on physical hardware. They display an amber `BETA` badge in the UI and `[Show controller]` map; if a fader uses a custom hardware editor template, re-learn it in one move via **Set up → Wrong fader? Move a different one** (or **Learn** in **Advanced**). Controllers without a horizontal crossfader can assign any vertical fader to **Switch scenes (T-bar)**. Adding a new controller requires only a single `struct mfd_controller_profile_def` entry in `src/core/apc40-profile.c`.

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
- [PLAN.md](PLAN.md) — Pre-planning OBS 33 & APC40 mkII audit findings, architecture, file layout, phased implementation plan (Phase 1 MVP through Phase 5), test matrix, risk register, and autonomous decisions (`D-8..D-21`).
- [PLAN-UI.md](PLAN-UI.md) — `0.2.0` three-zone `Strips` UI, 3-step `Set up` sheet, live controller SVG desk view, concurrency/locking architecture, and 4-round `kibitz` review disposition.
- [PLAN-FX.md](PLAN-FX.md) — `0.4.0` 18-slot `FX & Knobs` bank (`fx_1..fx_8`), `filter_param` engine (`R-FADE-4`), shared-filter `bypass_at_zero` lease system, `get_catalog` & `reset_fx` vendor API, and cross-repo Stream Deck `0.2.0` rollout specification.
- [TESTING.md](TESTING.md) — Automated `ctest` suite (`15` targets), rendered `0.4.0` dialog state screenshot gallery (`12` states including `FX & Knobs` bank and `Filter or effect setting` setup), and live OBS 33 + `loopMIDI` verification guide.
- [LICENSE](LICENSE) — GNU General Public License v2.0.
