# Control Midi Fadester from scripts and other programs

The obs-websocket 5 vendor API, its events and the in-process procs. For the shared mix setup, see [Set up the shared mix for MIDI](../README.md#set-up-the-shared-mix-for-midi). [Back to the README](../README.md).

Midi Fadester registers an obs-websocket 5 vendor API. Call it with `CallVendorRequest`, `vendorName` `"midi-fadester"`. Values are floats from 0.0 to 1.0. Slot ids are stable strings: `crossfader`, `fader_1` … `fader_8`, `master`, `fx_1` … `fx_8`.

| Request | Does |
|---|---|
| `get_state` | Returns everything: version, Studio Mode, the active layout, the MIDI device and all 18 slots with their job, value, readout and availability. |
| `set_fader_value` | Moves a slot to a value. On a T-bar, `release: true` finishes the throw; `release: false` with `value: 0` cancels it. |
| `nudge_fader` | Adds a delta, for endless encoders and dials. |
| `auto_fade` | Fades a slot to a value over 50 to 20 000 ms. |
| `set_slot` | Gives a slot a job, with the same fields as the dialog. Unknown names come back as an error sentence, never as a silent no-op. |
| `load_profile` | Loads a controller layout: `apc40mk2`, `apcminimk2`, `midimix`, `nanokontrol2`, `launchcontrolxl` or `generic`. Replaces all 18 slots. |
| `get_catalog` | Lists scenes, layers (including `has_internal_opacity` and `opacity_slot` ownership), audio sources, filters and their numeric settings with ranges and current values, so a client can build pickers and explain internal layer opacity. |
| `reset_fx` | Clean / All FX Off, instant or over a timed ramp. |
| `mix_reply` | Accepts a shared mix provider's reply only while its matching request UUID is pending. The response includes `accepted`. |
| `fill_from_scene` | Fills empty channel faders 1–8 from a scene's video sources (`scene`, Program when left out), bottom first, as strips that follow the scene on air; `all: true` also refills the *Show or hide a layer* strips. Returns `scene`, `filled` and `status` (`ok`, `no_empty_strips`, `no_video_layers`), or `scene_not_found`. |

| Event | When |
|---|---|
| `vjster_action` | The APC40's video speed controls (see [APC40 mkII pads](APC40.md)). |
| `fader_changed` | A value changes, at most 30 times a second per slot. Fields: `slot`, `value`, `display_value` and `origin` (`midi`, `ui`, `api`, `auto_fade` or `obs`). |
| `state_changed` | A job, device, layout or Studio Mode changes. |

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "fade-cam-in",
    "requestData": {
      "vendorName": "midi-fadester",
      "requestType": "auto_fade",
      "requestData": { "slot": "fader_2", "to": 1.0, "ms": 1500 }
    }
  }
}
```

- Validation errors come back inside `responseData` as `{ "error": "...", "comment": "..." }`, with a sentence that names the fix, for example *Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.*
- If obs-websocket is not loaded, Midi Fadester logs one line and keeps working from MIDI.
- Other plugins inside the same OBS do not need the websocket: Midi Fadester registers the procs `midi_fadester_get_slot_value` and `midi_fadester_set_fader_value` and the signal `midi_fadester_fader_changed` on OBS's global handlers.
- The full contract, with every field and the rules both sides follow, is [API-CONTRACT.md](https://github.com/jbrick2070/streamdeck-fadester/blob/main/API-CONTRACT.md) in the Stream Deck repository.
