# Midi Fadester

**Midi Fadester** is a hardware-style video mixer and continuous MIDI fader controller plugin for OBS Studio 33 on Windows x64, built for any class-compliant MIDI controller with plug-and-play support for the **Akai APC40 mkII**. Like an old-school hardware video mixer (Panasonic WJ-MX50, Roland/Edirol V-4, or Resolume A/B layer desk), it lets you mix **two or more live video sources simultaneously** using physical channel faders and an A/B crossfader bus (`Thru | Bus A | Bus B`) with dip-free dissolve curves, Non-Additive Mix (`NAM` / Lighten), Additive/Screen blending, GPU Luma Key and Wipe fader shaders, Studio Mode T-bar crossfades, cubic-dB audio levels, arbitrary numeric filter modulation, 30 ms slew smoothing, 14-bit CC support, soft takeover (pick-up), and RGB pad/LED feedback.

> **Disclaimer:** Midi Fadester is an independent third-party plugin for OBS Studio. It is not made, endorsed, or supported by the OBS Project or by Akai Professional / inMusic Brands.

## Development & AI Disclosure

This project is developed with the assistance of AI coding tools under human direction, review, and hardware verification on OBS Studio 33.

## Documentation

- [REQUIREMENTS.md](REQUIREMENTS.md) — Functional, multi-source video mixer (`R-MIX-*`), fader target (`R-FADE-*`), smoothing/takeover/LED feedback (`R-FEEL-*`), WinMM MIDI (`R-MIDI-*`), threading, and testing requirements, plus operator decisions (`D-1..D-6`).
- [PLAN.md](PLAN.md) — Pre-planning OBS 33 & APC40 mkII audit findings, 3-layer video mixer architecture, file layout, phased implementation steps, test matrix, and risk register.
- [LICENSE](LICENSE) — GNU General Public License v2.0.
