# Acid Box

A monophonic acid bass synth plugin for Akai MPC standalone hardware, by RS.

It is inspired by the classic acid bass line machines: a single saw or square oscillator into a resonant 18 dB/oct ladder low-pass filter with a decaying filter envelope, accent and slide. It is an original implementation written for this SDK, not a copy of any hardware's circuit or firmware, and it is not affiliated with or endorsed by any hardware manufacturer.

Requires MPC OS 3.10.1 or later with External Plugin Support enabled (Preferences → Enhancements (Beta) → Enable Enhancements and External Plugin Support).

## Playing it

- **Monophonic, last note wins.** Releasing a note while another is still held returns to the held note.
- **Accent:** a note with velocity at or above *Acc Thresh* is accented. It is louder, sweeps the filter deeper with slightly more resonance, and decays faster. Consecutive accented notes build up extra emphasis for about 300 ms.
- **Slide:** with the *Slide* switch on, a note that starts while the previous note is still held glides to the new pitch over *Slide Time* and does not retrigger the envelopes. Play overlapping (legato) notes to slide; use separated notes for normal retriggered notes. With *Slide* off every note retriggers.
- Pitch bend is ±2 semitones.

## Parameters

Page ACID:

| Name | Range | What it does |
|---|---|---|
| Cutoff | 20 – 12000 Hz | Base filter cutoff |
| Reso | 0 – 100 % | Filter resonance (self-oscillation is not intended; high values squelch) |
| Env Mod | 0 – 100 % | How far the filter envelope opens the cutoff (up to 8 octaves) |
| Decay | 20 – 8000 ms | Filter envelope decay (accented notes cap at about 200 ms) |
| Accent | 0 – 100 % | Strength of accented notes |
| Drive | 0 – 100 % | Output saturation |
| Slide Time | 5 – 500 ms | Glide time for slid notes |
| Slide | on/off | Overlapping notes glide instead of retriggering |
| Waveform | Saw / Square | Oscillator waveform (switch) |
| Volume | -40 – +6 dB | Master volume (parameter 0) |

Page MORE: Tune (±100 cents), Octave (-2 – +2), Acc Thresh (velocity 1 – 127), Gate Rel (3 – 500 ms amp release).

There are 8 built-in programs: Init Acid, Classic Line, Squelch, Square Bass, Sub Pump, Wide Open, Zap, Slidey.

## Sound matching

The DSP constants were fitted against five reference recordings of a real hardware unit (sets A1-E1, all C2 notes, normal and accented). For each note the harmonic levels H1-H32 were measured at eight times after note-on and compared with the plugin's output. Measured and fitted: output high-pass (about 200 Hz), slow amp-envelope droop (about 2.3 s normal, 1.3 s accented), accent level boost (about 6.5 dB, decaying in 50 ms), resonance feedback gain and loop high-pass, a filter cutoff ceiling (about 2.3 kHz), a filter envelope range up to 8 octaves with a fast accented decay, and a partial 24 dB/oct output tap.

Result: mean error over 16 reference notes is about 1.9 dB rms across the first 24 harmonics (best note 0.4 dB, worst 3.3 dB). Only these measurable spectral features were matched. It is not proven identical to the hardware, pitches other than C2 were not measured, and it has not been tested on a real MPC. Presets were not re-voiced.

## Notes and limitations

- The filter is a 4-pole ladder with the output mostly taken after pole 3 and a tanh in the feedback path. It is fitted to reference recordings (see Sound matching), not modelled component by component, so it will sound close in character but not identical.
- The filter runs at the host sample rate with no oversampling. At the highest resonance and cutoff settings it can sound slightly harsher than a hardware filter.
- No built-in sequencer: play it from an MPC sequence or pad/keyboard. Plugins cannot send MIDI back to the MPC.
- CPU: one voice. Envelope and glide coefficients are computed once per block and the filter coefficients every 8 samples, so there is no per-sample `exp`/`tan` in the audio loop. Expected to be a very small part of one core; measured only on a desktop CPU with the host simulator, not on MPC hardware.

## Silver look

The touch skin uses a silver panel with silver knobs, fader caps and buttons (charcoal value arcs and pointers). The SDK's UI kit only recolours the accent, so `make_silver_skin.py` generates the layout with `tools/gen_ui.py` and then redraws the control graphics in silver at the same sizes. `skins/` is copied verbatim by the build; after changing `ui.json` or the parameters, run:

```
python tools/build.py "plugins/Acid Box" --target native
python "plugins/Acid Box/make_silver_skin.py"
```

## Build

```
python tools/build.py "plugins/Acid Box"
python tools/package.py "plugins/Acid Box"
```

## Project layout

- `plugin.json` - manifest (name, manufacturer, uid). The `uid` must match the C++ `uniqueId`.
- `src/AcidBox.cpp` - the plugin source (parameters, MIDI handling, DSP).
- `ui.json` - which parameters appear on the MPC screen, in which order.
- `build/` - build output (the deployable `RS - VST - Acid Box` folder lives here).
- `dist/` - zips produced by `package.py`.
