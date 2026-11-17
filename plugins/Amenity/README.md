# Amenity

A live jungle break slicer for Akai MPC standalone hardware, by RS. A port of the Amenity firmware
for the M-VAVE FM-1 (https://github.com/robsanderson69/amenity, GPL-3.0). It plays a break sliced
into sixteen 8ths and lets you tear it up from the pads.

## Install

1. Copy the `RS - VST - Amenity` folder into `VST2 Plugins` on a USB drive or SD card.
2. Copy your break(s) (WAV) into a folder called `Samples` inside it, beside `plugin.json`.
3. MPC OS 3.10.1 or later, with Preferences → Enhancements (Beta) → *Enable Enhancements* and
   *External Plugin Support* on.

## Pads

Hold a pad for an effect. The pattern keeps running underneath.

| Pad | Bank A: effects (notes 36–51) |
|---|---|
| 1–4 | 1/4 repeat, build (1/4→1/8→1/16→1/32), STRETCH, echo |
| 5–8 | 1/8 repeat, down, up, shrink |
| 9–12 | 1/16 repeat, down, up, shrink |
| 13–16 | 1/32 repeat, down, up, shrink |

| Pad | Bank B: hits and held effects (notes 52–62) |
|---|---|
| 1–8 | single hits: kick, snare, kick, snare, snare, snare, hat, ride |
| 9–11 | held: feedback, lo-fi, octave down |

The pad map assumes the MPC's default note layout (Bank A = 36–51, Bank B = 52–67).

## Sound

Pattern: plays while the MPC sequence plays (locked to its song position, restarting every 8 beats),
or always, or off (pads only). Variation adds 16th fills, swapped slices and the "flip"; Warp stretches
or half-speeds some hits; SEQ Fill ends every phrase with a random effect; Freeze keeps the pattern.
Time runs the break at half or double speed. Filter is a DJ filter. FX page: Lo-Fi, Saturate, Compress,
Stretch amount. EQ page: four bands, ±12 dB. Feedback is a short metallic comb delay tuned to a root note.

## Not in this version

The FM-1's panel, MIDI clock, Bluetooth and USB-host features, the web break uploader, and the
built-in breaks (third-party recordings; bring your own). Settings are saved with the MPC project.

## Licence

GPL-3.0-only, as the FM-1 firmware it derives from. Your samples are your own.
