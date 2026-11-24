# Visual Sidechain (part of Midi Fadester)

> **Setting up the Fadester Suite?** Start with **[START-HERE.md](https://github.com/jbrick2070/streamdeck-fadester/blob/main/docs/START-HERE.md)** for what to install for your controller, which video player to pick, first-run OBS settings, and the compatibility matrix.

**Visual Sidechain** makes your video move with your music in OBS Studio 33 on Windows and macOS. It is part of **[Midi Fadester](../README.md)** since 0.9.0: install Midi Fadester and you have it. Its effects react to the sound as it plays, its Audio Modulator moves the settings of your other filters to the beat, and its **[Tape Deck](#tape-deck)** plays a video file at any speed from 25% to 800% (0.25x to 8x).

## Quick start

<img src="visual-sidechain-steps.svg" alt="Visual Sidechain in three steps: 1, install Midi Fadester (Visual Sidechain comes with it); 2, in a source's Filters press + and add Visual Sidechain: Shader Filter; 3, play music and the picture moves with the sound." width="900">

1. **Install Midi Fadester** (see [Install](#install)) and start OBS Studio 33.
2. **Add the filter.** Right-click a source or a scene, choose **Filters**, press **`+`** under **Effect Filters** and add **`Visual Sidechain: Shader Filter`**.
3. **Play music.** The picture moves with the sound on **Track 1 (Main Mix)**.

That's it. Choose another look under **`Effect`** in the filter's window.

> **Before you go live, set a Panic key:** **Settings → Hotkeys → `Visual Sidechain: Panic (all effects off)`**. One press turns off every Visual Sidechain effect in every scene. See [Photosensitivity safety](#photosensitivity-safety-panic-restore-visuals-and-the-flash-limit).

![Visual Sidechain in OBS Studio 33: the Program scene "Visual Sidechain" shows the Hex Pulse Lattice shader reacting to the soundtrack, with Studio Mode previewing a layer mix. A frame of a real OBS window capture.](screenshots/visual-sidechain-hero.png)

> **Disclaimer:** Visual Sidechain is an independent third-party plugin for OBS Studio. It is not made, endorsed, or supported by the OBS Project or NVIDIA Corporation.

## Install

Visual Sidechain comes with **[Midi Fadester](../README.md#install) 0.9.0 or newer** (OBS Studio 33.0.0 or newer, Windows x64 or macOS 13.0 or newer). There is nothing else to download.

- **Coming from the separate Visual Sidechain plugin (0.8.0 or older)?** Close OBS and run the Midi Fadester installer. On Windows it uninstalls the old plugin first; the Mac package removes the old `visual-sidechain.plugin` from your OBS plugins folder. Your filters, Tape Decks, hotkeys and saved settings stay as they are: the filter names, hotkey names and the obs-websocket vendor name `visual-sidechain` have not changed.
- **If the old plugin is still there** (for example after unpacking a `.zip` by hand), Midi Fadester leaves Visual Sidechain switched off so the two do not register the same effects twice. The Fadester Desk dock then shows a yellow line naming the old file: remove it and restart OBS. OBS's log says the same.

## Good to know

- **Which sound it hears.** The line at the top of the filter's window names the track it listens to (**Track 1 (Main Mix)** by default). If nothing moves, check that your music goes to that track: **Audio Mixer → Advanced Audio Properties → Tracks**.
- **Short window, more behind a switch.** The window opens on the controls most people need: the effect, Effect Mix, Audio Reactivity, the effect's own parameters and Audio Input. Tick **More settings** for the Custom Band controls and the **Faders** group. The Audio Modulator has the same switch for its Depth Fader, Invert and update rate; Flash Guard always stays in view.

## Photosensitivity safety: Panic, Restore visuals and the flash limit

Both hotkeys are in **Settings → Hotkeys** and have no key until you set one. A line in the Shader Filter's window says where to set them.

- **`Visual Sidechain: Panic (all effects off)`** at once puts every `Visual Sidechain: Shader Filter` (custom shaders too) into pass-through and sets every strobe, flash and modulation amount to zero, in every scene. While Panic is on, the top of each Shader Filter and Audio Modulator window says so.
- **`Visual Sidechain: Restore visuals (effects on again)`** ends the pass-through and puts the amounts back as they were before Panic.
- **One 3 Hz flash clock for everything.** At most **3 flashes per second across all Visual Sidechain filters and guarded Audio Modulator slots combined**, admitted in arrival order.
  - In **`Strobe & Flash`**, **`Maximum Flash Rate (Hz)`** can lower that filter's own limit down to 0.5 Hz (default 3 Hz, never above the 3 Hz ceiling).
  - In all 11 built-in shaders, whole-frame brightness, highlight bloom, aura, trail persistence and edge vignette transients run through `vs_flash_envelope` or `audio_kick_safe` (UV geometry motion stays raw).
  - Audio Modulator slots turn **`Flash Guard`** **on** by default whenever a transient drives brightness, contrast, gamma, opacity or color settings.
  - Custom shaders cannot be policed automatically; the Properties window says so next to the shader file picker and code box. A custom shader can opt into the shared cap with `vs_flash_envelope`, `audio_kick_safe`, `audio_snare_safe` and `audio_flux_safe`.
- **Check the final output** before you stream, record or project it: your footage and third-party filters outside Visual Sidechain can still add flashes. Keep a key on Panic for an instant stop.

## Shader Filter and Audio Modulator

**`Visual Sidechain: Shader Filter`** applies one of the 11 built-in effects or your own `.shader` / `.effect` file.

- The **`Effect Mix (Dry / Wet)`** and **`Audio Reactivity`** sliders sit under the preset and need no MIDI hardware.
- The audio group is at the bottom of the window, with its advanced tuning behind **`Show Advanced Analysis Settings`**.
- Under **`Faders`** (behind **More settings**) you can bind any Fadester slot (`Crossfader`, `Fader 1..8`, `Master`, `FX 1..8`) to **`Effect Mix Fader (Dry / Wet)`**, **`Audio Reactivity Fader`**, or up to two **`Parameter Fader`** targets (set to the parameter's own range, and held inside its safety limits).

**`Visual Sidechain: Audio Modulator`** moves numeric settings of another filter on the same source (such as **Color Correction**, **NVIDIA Background Removal** or **NVIDIA Blur**).

- Add that filter first: the modulator lists only filters on its own source.
- Each slot has a **`Filter to move`**, a **`Setting to move`** and what it **`Follows`**: `Kick`, `Bass`, `Beat`, any `Spectrum Band 1..32`, a `Custom band`, or a Midi Fadester fader slot. The minimum and maximum are filled in from the setting's own range when you pick it.
- A **`Depth Fader`** per slot can scale `Amount` live from `0–100%` from a MIDI fader or a Stream Deck + dial.
- When you turn the modulator off or remove it, and when OBS saves the scene collection, each setting goes back to its own value.

## Geometric presets and fader assignments

- **Hex Pulse Lattice** draws honeycomb outlines over your source. Bass expands the cells, kick drives a spatial ripple, and highs shift the edge accents. Try a Parameter Fader on the cell density, with the second fader on overlay opacity.
- **Polygon Tunnel** draws nested polygons with continuous depth motion. Choose three to eight sides, then adjust tunnel depth, travel speed and rotation. Try a Parameter Fader on tunnel depth.
- Both keep source transparency and stay visible during silence; set audio response to zero for a purely geometric look.

Fader assignments:

- Assignment summaries show the selected fader, destination and range.
- **`Clear Parameter Fader Assignment`** clears that parameter fader's target and custom range. Every other fader binding is cleared by choosing its **(None ...)** entry, which leaves the rest as it is: an Audio Modulator slot keeps its filter, its setting, what it follows and its amount.
- Within a Shader Filter, the first parameter assignment owns its target even when it uses the slider value. Within an Audio Modulator, the first enabled slot owns each filter/setting pair. Conflicting choices are disabled, and duplicate assignments loaded from settings are cleared by the same ownership rules.

## Tape Deck

**`Visual Sidechain: Tape Deck`** turns a Browser Source into a tape deck for a video file on this computer or a network share. Speed runs from 25% to 800% (0.25x to 8x) and changes in place, picture and sound, with no restart. Nothing is downloaded. (Tubester is its code name in the source tree.)

### Tape Deck quick start

1. **Add a new Browser Source** to a scene, just for the deck. Its address does not matter: the deck sets the source up for itself.
2. **Add the filter.** Right-click the Browser Source → **Filters** → **Effect Filters (`+`)** → **`Visual Sidechain: Tape Deck`**.
3. **Choose a video file** under **Tape**. It plays at once, looping. Move **Speed**.

That's it. With **Tape pitch** ticked the sound drops and rises with the speed, like a tape; unticked, the pitch stays put.

- **Use a Browser Source of its own.** The deck sets the deck page as its address, turns **Control audio via OBS** on, and turns **Shutdown source when not visible** and **Refresh browser when scene becomes active** off. It does not put the old settings back when you remove the filter, so do not use a Browser Source that shows something else, such as an alert page or a chat. The filter's window says this too.
- **The status line** near the top of the filter's window says what the deck's page last reported, for example "Playing. Speed 150%, at 0:42 of 2:02." It is a snapshot: press **Refresh Status** to read it again. Right after you choose a tape, the window redraws once and reads "Loading the tape. Press Refresh Status to see the result." The window never refreshes on a timer, only right after something you do in it, so it never moves the focus while you are editing.
- **Sound** reaches OBS's mixer through the Browser Source.

### Which video player when

| Player | What it plays | Speed range (both units) | Live speed behavior | Surface controls |
| :--- | :--- | :--- | :--- | :--- |
| **VJster (Chrome / Edge tab)** | A YouTube tab (with quality choice) or an HTML5 `<video>` tab captured into OBS | **25% to 200% (0.25x to 2x)** default (up to **400% (4x)** if YouTube lists it; **10% to 1600% (0.1x to 16x)** with **Extended** enabled) | Changes live in place without restarting | **Fadester for OBS** (VJster dials/keys) and **Fadester Companion** |
| **VJster OBS page** | One YouTube video (`v=`) or playlist (`list=`) in an OBS **Browser Source** | **25% to 200% (0.25x to 2x)** (OBS CEF 150 embed ceiling) | Changes live in place without restarting; speed and play/pause only (no embed quality API) | **Fadester for OBS** (VJster dials/keys) and **Fadester Companion** |
| **Visual Sidechain: Tape Deck** | One local or network-share video file (`MP4`, `MKV`, `MOV`, `WebM`) on a dedicated OBS **Browser Source** (Windows and macOS; the Mac deck is not hand-tested yet) | **25% to 800% (0.25x to 8x)** (`100% (1x)` normal) | Changes picture and sound live in place without restarting; optional **Tape pitch** and ratio **Speed Fader** | **Fadester for OBS** (**Tape Deck** key/dial or **OBS Filter**), Midi Fadester **Speed Fader**, OBS hotkeys, `obs-websocket` (`deck_command`). *(Fadester Companion preset support is a follow-up.)* |
| **OBS Playlister** | Ordered playlists of local files, folders, `.m3u` / `.m3u8` / `.pls` playlists, and stream URLs in an OBS **Playlister** source (Windows, Linux and macOS; on a Mac it loads and creates its source, the hands-on test is still to come) | **1% to 200% (0.01x to 2x)** (`100% (1x)` normal) | Source setting only: **changing Speed restarts the playing clip from its beginning** | **Fadester for OBS** (**Playlister** transport key), OBS Media Controls, OBS hotkeys, `obs-websocket` media inputs |

### Controls

| Control | What it does |
|---|---|
| **Playing** | Untick to pause where the tape is, tick to play on. A paused deck stays paused through a page reload, and loads a new tape paused at its start position. After a tape without **Loop** has ended, it reads Ended until you play it again: press the **Play / Pause** hotkey, or untick and tick **Playing** (it is still ticked after the end). Either starts it from **Start position**. |
| **Cue to start** | Goes to **Start position**. A playing deck plays on from there; a paused deck stays paused. |
| **Start position** (seconds) | Where a tape starts when it is loaded, when it is cued, and when it plays again after the end. A start position at or past the end of the tape counts as 0. |
| **Speed Fader (Midi Fadester & Fadester for OBS)** | Bind any of the 18 Fadester slots to drive the speed between **Speed at Fader Bottom** and **Speed at Fader Top**. The scale is a ratio: equal fader travel gives an equal speed ratio, so with 50 % at the bottom and 200 % at the top the middle is 100 %. The Speed slider is left as it is, and applies again when you choose **(None - Use Speed Slider)** in the **Speed Fader** list. Without Midi Fadester the slider applies, and the status line under the binding says so. |
| **Hotkeys** (**Settings → Hotkeys**, under the Browser Source's own name) | **Tape Deck: Play / Pause** (on a tape that has ended it plays again from the start position), **Cue to start**, **Speed Up** and **Speed Down** (along 25, 50, 75, 100, 125, 150, 200, 300, 400, 600 and 800 %), and **Normal Speed**. The speed hotkeys move the Speed setting; while a Speed Fader is live, the fader keeps the speed. Bindings are saved with the source, and a deck you remove and bring back with Undo keeps them. |

**Two decks, hidden decks.** Each Browser Source holds one deck. A second Tape Deck on the same source waits and says so, and steps in if the first is removed (with whatever hotkey bindings the source last saved). A deck whose source is hidden keeps its tape loaded and playing, without sound. Visual Sidechain's Panic and Restore visuals leave decks alone.

### What plays

| File | Result |
|---|---|
| H.264 in MP4, MKV and MOV (also 10-bit and 4:2:2), VP9 in WebM, AV1 in MP4, HEVC in MP4 where this PC's hardware decodes it | Picture and seeking |
| AAC, MP3 and Opus sound | Plays |
| AC-3 sound | Picture without sound |
| MPEG-2, MJPEG, DXV, or a missing file | "Cannot play this file" |
| MPEG-4 Part 2, Theora, ProRes | "This file plays sound only" |

Convert the others to MP4 (H.264 video, AAC sound) with FFmpeg, for example:

```bash
ffmpeg -i input.mov -c:v libx264 -crf 18 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart output.mp4
```

### From a Stream Deck or OpenDeck key or dial (Fadester for OBS)

- Add the **Tape Deck** action and choose the deck's Browser Source.
- **On a dial**, turning steps along the 11-rung ladder from **25% (0.25x)** to **800% (8x)**, and pressing runs your chosen action (**Play / Pause**, **Cue to start**, **Normal speed**, **Speed up** or **Speed down**).
- **On a key**, pressing runs that action while the face shows `"150% (1.5x)"` and the playing or paused state live from `decks_changed`.
- **Continuous 1% steps instead of the ladder:** add an **OBS Filter** action on the Browser Source's **Tape Deck** filter, pick **`speed_percent`**, and set **From 25 To 800** with **Whole numbers** on (pressing drops to **25% (0.25x)** while held).
- **Ratio scale:** bind a Midi Fadester slot as a **Speed Fader** instead (see [Controls](#controls)).
- Do not combine a Speed Fader with a direct speed dial on one deck: while the Speed Fader is live, `speed_percent` changes wait until the fader is cleared.
- *(Direct Fadester Companion / TourBox Tape Deck preset bindings are a planned follow-up.)*

## When something is off

| You see | Do |
|---|---|
| The picture does not move with the music | Send your music to the track named at the top of the filter's window (see [Good to know](#good-to-know)). |
| Every effect is off and the window says Panic is on | Press your **`Visual Sidechain: Restore visuals (effects on again)`** hotkey. |
| Visual Sidechain is missing and the Fadester Desk dock shows a yellow line | The old separate plugin is still installed. Remove the file it names and restart OBS. |
| A custom shader shows the source unchanged | It failed to compile; the source passes through until a valid shader is loaded. The compile message names a parameter to rename if it collides with an internal control. |
| Tape Deck: "No answer from the deck page yet" | Wait a few seconds and press **Refresh Status**. |
| Tape Deck: "Cannot play this file" or "This file plays sound only" | Convert it to MP4 (see [What plays](#what-plays)). |
| A second Tape Deck on a source does nothing | Each Browser Source holds one deck; the second waits until the first is removed. |

## Languages

Visual Sidechain follows the language OBS runs in (**Settings → General → Language**; OBS needs a restart to change it). Besides English it speaks Simplified Chinese, Spanish, Brazilian Portuguese, Russian, Japanese, German, French, Korean, Traditional Chinese, Turkish, Dutch and Italian.

- A few strings start from OBS's own translation. The rest was drafted by an AI model (Claude), and every file but the Italian one was reviewed by a second one (Gemini), whose corrections were applied. No native speaker has checked them yet.
- If something reads wrong in your language, please open an issue or a pull request: the files are in `data/locale/`, one per language, and `en-US.ini` is the English source.
- The built-in effects' parameter names and tooltips are translated too (the `ShaderParam.*` lines); a custom shader's own names stay as written in its file. Log lines stay English.

## For developers

How it works inside, custom shaders, the Tape Deck over obs-websocket and the `visual-sidechain` vendor API (`get_signals`, `get_decks`, `deck_command` and their events) are on their own page: **[Visual Sidechain for developers](VISUAL-SIDECHAIN-DEVELOPERS.md)**.

## Development & AI Disclosure

This project is written with AI coding tools and reviewed and tested by the author on OBS Studio 33. All bundled shaders are original `GPL-2.0-or-later` implementations with no Shadertoy copies.

## Documentation & Beta Feedback

- **[BETA-TESTING.md](https://github.com/jbrick2070/visual-sidechain/blob/main/BETA-TESTING.md)**: 10-minute beta tester guide and hardware checklist for OBS Studio 33 ([Submit a Beta Report](https://github.com/jbrick2070/midi-fadester/issues)).
- Design documents from when Visual Sidechain was a separate project: see [Visual Sidechain for developers](VISUAL-SIDECHAIN-DEVELOPERS.md#older-design-documents).

## License

Visual Sidechain is part of Midi Fadester and shares its license, the **GNU General Public License v2.0 or later (GPL-2.0-or-later)**. See [LICENSE](../LICENSE).
