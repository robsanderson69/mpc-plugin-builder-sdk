Put your break here (WAV files, any sample rate, mono or stereo).

Amenity looks for WAV files in a folder called "Samples" beside plugin.json:

    VST2 Plugins/RS - VST - Amenity/Samples/

If that folder does not exist or has no WAV files, it uses this one
(RS - VST - Amenity/Content/Samples/). The Sample parameter picks between up to 12 files.

Tips
- A break of 1, 2, 4 or 8 bars. One bar is played twice; of 4 or 8 bars the first two are used.
- Put the tempo in the file name ("136 amen.wav") and it will guess the length correctly.
- Breaks are time-stretched to follow the MPC's tempo, like a loop at 2 bars per 8 beats.
- The files are read when the plugin loads: add or change them, then reload the plugin.
