# Midi Fadester

**Midi Fadester** is a continuous MIDI fader controller and video layer mixer plugin for OBS Studio 33 on Windows x64, built for any class-compliant MIDI controller with plug-and-play support for the **Akai APC40 mkII**. It provides channel-aware MIDI learn (`Ch 1–16` + `CC/Note`, 7-bit and 14-bit CC pairs), 30 ms slew smoothing, soft takeover (pick-up), Studio Mode T-bar crossfades (fixing OBS 33's Studio Mode off crash and `release_tbar` cancellation bug), single-layer opacity fades, two-source dip-free layer crossfades (`Source A ↔ Source B`), and cubic-dB audio faders (Phase 1), followed by `Thru | Bus A | Bus B` multi-strip mixing, filter modulation, and hotkeys (Phase 2), hardware blend modes (`NAM`/Lighten, Additive, Screen), Luma Key, and Wipe faders (Phase 3), a dockable mixer panel (Phase 4), and APC40 mkII RGB LED feedback (Phase 5).

> **Disclaimer:** Midi Fadester is an independent third-party plugin for OBS Studio. It is not made, endorsed, or supported by the OBS Project or by Akai Professional / inMusic Brands.

## Development & AI Disclosure

This project is developed with the assistance of AI coding tools under human direction, review, and hardware verification on OBS Studio 33.

## Documentation

- [REQUIREMENTS.md](REQUIREMENTS.md) — Functional, fader target (`R-FADE-*`), two-source & bus mixer (`R-MIX-*`), smoothing/takeover/LED feedback (`R-FEEL-*`), WinMM MIDI (`R-MIDI-*`), threading, and testing requirements, plus approved operator decisions (`D-1..D-7`).
- [PLAN.md](PLAN.md) — Pre-planning OBS 33 & APC40 mkII audit findings, architecture, file layout, phased implementation plan (Phase 1 MVP through Phase 5), test matrix, and risk register.
- [LICENSE](LICENSE) — GNU General Public License v2.0.
