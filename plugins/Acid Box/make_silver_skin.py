#!/usr/bin/env python3
"""Regenerate Acid Box's silver touch skin into skins/.

The SDK's UI kit draws dark knobs, faders and buttons and only changes the accent colour per
theme. Acid Box wants silver controls, so this script:

  1. runs tools/gen_ui.py on ui.json (needs build/params.json from a native build) to produce
     TUI.json and Q-Links.json with the colours from ui.json's "style";
  2. re-renders the control graphics with the kit's own renderer using a silver palette. The
     pixel sizes and file names are identical, so TUI.json stays valid.

skins/ is then copied verbatim by tools/build.py (it no longer runs the generator), so run this
script again whenever ui.json or the parameter list changes:

    python tools/build.py "plugins/Acid Box" --target native      # refreshes build/params.json
    python "plugins/Acid Box/make_silver_skin.py"                 # rewrites skins/
    python tools/build.py "plugins/Acid Box"                      # full build

Standard library only.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(SDK, "tools"))

import make_ui_kit as kit   # noqa: E402  (the SDK's own kit renderer)

SKINS = os.path.join(HERE, "skins")

# --- silver palettes (ARGB) ------------------------------------------------------------------
# Dials: silver knob bodies and fader caps; the value arc is charcoal on a mid-grey track.
DIAL = {
    "body": "ffc4c7cb", "bodyLight": "fff1f2f4", "bodyDark": "ff7b7e84",
    "track": "ff8a8d92", "outline": "ff2e3034",
    "pointer": "ff1a1b1e", "thumb": "ffd3d6da", "thumbLight": "fff6f7f8", "thumbDark": "ff8c8f94",
    "accent": "ff2a2c30",
}
# Buttons and switches: lit = bright polished silver, unlit = darker brushed silver; the LED and
# the lit outline stay charcoal so the state reads clearly with the dark captions.
BUTTON = {
    "body": "ffa3a6ab", "bodyLight": "ffbcbfc3", "bodyDark": "ff6f7277",
    "track": "ff8f9297", "outline": "ff2e3034",
    "pointer": "ff1a1b1e", "thumb": "ffd3d6da", "thumbLight": "fff6f7f8", "thumbDark": "ff8c8f94",
    "accent": "fff3f4f6",
}
# Which graphics use which palette (only the ones this plugin's layout references).
DIAL_ASSETS = ("knob_uni", "knob_bi", "fader_h_uni", "fader_h_bi")
BUTTON_ASSETS = ("toggle_on", "toggle_off", "seg2_on", "seg2_off", "seg3_on", "seg3_off",
                 "seg4_on", "seg4_off", "button_on", "button_off")


def themed(overrides):
    th = kit.make_theme("mono", overrides["accent"])
    for key, value in overrides.items():
        th[key] = kit.parse_argb(value)
    # keep the derived colours consistent with the overridden accent / body
    th["accentDark"] = kit.mix(th["accent"], th["outline"], 0.30)
    th["accentGlow"] = kit.with_alpha(th["accent"], 70)
    th["ledOff"] = kit.mix(th["bodyDark"], th["body"], 0.5)
    return th


def main():
    params = os.path.join(HERE, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json not found. Run: python tools/build.py \"plugins/Acid Box\" --target native")
    # 1. layout + colours from ui.json
    os.makedirs(SKINS, exist_ok=True)
    cache = os.path.join(HERE, "build", "kit-cache")
    subprocess.run([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                    "--ui", os.path.join(HERE, "ui.json"), "--out", SKINS, "--kit-cache", cache],
                   check=True)
    # 2. silver graphics over the generated (dark) ones, same names and sizes
    assets = kit.asset_table()
    for names, palette in ((DIAL_ASSETS, DIAL), (BUTTON_ASSETS, BUTTON)):
        th = themed(palette)
        for name in names:
            render, size, _frames = assets[name]
            target = os.path.join(SKINS, name + ".png")
            if os.path.isfile(target):   # only replace graphics the layout actually uses
                kit.write_png(target, size[0], render(th))
                print("silver: %s" % target)
    shutil.rmtree(cache, ignore_errors=True)


if __name__ == "__main__":
    main()
