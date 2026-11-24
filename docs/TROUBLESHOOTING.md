# If something is wrong

Every message Midi Fadester can show, what it means and what to do. [Back to the README](../README.md).

The dialog says what is wrong in a sentence, next to the strip it concerns, and usually offers the button that fixes it.

| You see | It means | Do |
|---|---|---|
| *Another program has the APC40 mkII open. Close it, then press Reconnect.* | Windows lets one program use a MIDI port at a time. | Close the DAW, MIDI-OX, a browser tab using Web MIDI, or whatever else holds the desk, then **Reconnect**. |
| *APC40 mkII not found. Plug it in, then press Reconnect.* / *No MIDI controller found. Plug one in.* | The port is gone or the desk is off. | Plug it in or power it on. The plugin notices when Windows reports the device again; **Reconnect** forces a rescan, and **Change controller** picks a different port. |
| *Could not open APC40 mkII. Unplug and replug it, then press Reconnect.* | Windows refused the port for another reason. | Do that. |
| *Studio Mode is off.* | Switch scenes needs Studio Mode. | **Turn on Studio Mode** on the strip, or the **Studio Mode** button in OBS's Controls dock. |
| *No fader assigned. Click Set up and move the fader.* | The strip has a job but no MIDI control. | Do that and move the fader, or press **Learn** on the Advanced tab. |
| *No ready layout for …* in a blue bar at the top | Midi Fadester has no layout for this desk, and the layout in use is for another desk, so the strips may not follow this desk's faders. | Learn each fader as the bar says. **OK** hides the bar until the window is opened again. |
| *Scene Red was deleted.* / *Cam is no longer in scene Blue.* / *Color and Color 2 are not in the same scene.* | The scene or layer the job named is gone or moved. | **Set up** on the strip and pick again. Renames follow by themselves; only removals break a job. |
| *Mic/Aux has no audio.* | A Volume job points at a source without sound. | Pick a source with audio. |
| *Filter 'Blur' was removed from Cam. Re-add it in OBS Filters or click Set up.* / *Setting 'radius' is no longer available on 'Blur'.* | The filter or its setting is gone. | Re-add the filter in OBS, or **Set up** and pick another. |
| *Move the fader up to 40% to start.* | Not an error: the value moved without the fader. | Move the fader past the marker. |
| *At end.* | Not an error: the control is parked on a rail. | Move it 8 % back to pick up again. |
| The crossfader moves but nothing happens | A throw starts only past the first 6 % of the travel. | Throw further. If the strip is amber, Studio Mode is off. |
| The transition is a Fade although Cut or Stinger is selected | OBS uses Fade for any T-bar move with those two. | Pick another transition for throws, or use OBS's Transition button for cuts. |
| A layer fades in a scene you did not expect | The opacity filter sits on the source, which is in both scenes. | Put a copy of the source in the other scene. |
| *Midi Fadester cannot find the T-bar in this OBS version. Switch scenes does not work. Everything else still works. Look for a newer version of Midi Fadester.* in red on the crossfader strip | OBS changed its Studio Mode window and the T-bar cannot be driven. The log says *CRITICAL: Studio Mode is active, but Qt QSlider 'slider-tbar' was not found in OBS main window!* | Everything else still works. Please open an issue with your OBS version. |

The OBS log (**Help → Log Files → Show Log Files**) has every Midi Fadester line prefixed with `[midi-fadester]`.
