// Amenity - a live jungle break slicer for the Akai MPC, by RS.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// A port of Amenity, the M-VAVE FM-1 firmware (github.com/robsanderson69/amenity, GPL-3.0), whose
// engine was itself ported from the Jungle Choppa VST. The sound engine is in AmenityEngine.h.
//
// Signal flow:
//   a two-bar break from <plugin folder>/Samples/*.wav, sliced into sixteen 8ths
//   -> pattern sequencer (variation, warp, flip, SEQ fill) + pad rolls / hits
//   -> LO-FI -> feedback comb -> 4-band EQ -> saturation -> compression -> DJ filter -> volume -> out
//
// Pads (MPC default note map, notes 36..51 = Bank A, 52..67 = Bank B). Hold a pad for an effect:
//   Bank A  pad  1..4    1/4 repeat | build | STRETCH | echo
//           pad  5..8    1/8  repeat | down | up | shrink
//           pad  9..12   1/16 repeat | down | up | shrink
//           pad 13..16   1/32 repeat | down | up | shrink
//   Bank B  pad  1..8    single hits: kick snare kick snare snare snare hat ride (the break's slices)
//           pad  9..11   held effects: feedback | lo-fi | octave down
//
// Samples: copy WAV files into the "Samples" folder inside the plugin folder (beside plugin.json).
// (If that folder is missing or empty, Content/Samples is used.) The Sample parameter picks one. A one-bar file is played twice; of four or eight bars the first
// two are used. A tempo in the file name ("90 MYBREAK.wav") helps it guess the length.
//
// Realtime: process() allocates nothing and does no I/O. Files are read in prepare().

#include "mpc_plugin.h"
#include "dsp/wav.h"
#include "AmenityEngine.h"

#include <algorithm>
#include <dirent.h>

namespace {

enum ParamIndex
{
    kVolume = 0,     // dB. Parameter 0: the host nudges it on every load, so it must be harmless.
    kSample,         // which WAV in Samples/
    kPattern,        // Transport: plays while the MPC sequence plays | Always | Off (pads only)
    kTime,           // half / normal / double time
    kVariation,      // 0..100 %: fills, swapped slices, the flip
    kWarp,           // 0..100 %: stretched and half-speed hits
    kFilter,         // DJ filter: left low-pass, centre off, right high-pass
    kFeedback,       // 0..100 %: short metallic feedback delay
    kRoot,           // feedback root note
    kTranspose,      // semitones
    kFill,           // SEQ: every phrase ends with a random effect
    kFreeze,         // REC: keep the current pattern
    kLofi,
    kSaturate,
    kCompress,
    kStretch,        // STRETCH slowdown 2x / 3x / 4x
    kEqLow, kEqLowMid, kEqHighMid, kEqHigh,
    kNumParams
};

constexpr int kFirstRollNote = 36;      // Bank A pad 1
constexpr int kFirstHitNote = 52;       // Bank B pad 1
constexpr int kFirstFxNote = 60;        // Bank B pad 9
constexpr int kMaxSamples = 12;
constexpr double kMaxSeconds = 40.0;

bool hasWavExt(const std::string& n)
{
    if (n.size() < 5 || n[0] == '.') return false;
    std::string e = n.substr(n.size() - 4);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav";
}

// Wav files in <plugin folder>/Samples (then Content/Samples), sorted by name.
std::vector<std::string> findSamples(std::string& dirOut)
{
    std::vector<std::string> names;
    const std::string dirs[2] = { mpc::pluginFolder() + "/Samples", mpc::contentPath("Samples") };
    for (const std::string& d : dirs)
    {
        if (DIR* dir = opendir(d.c_str()))
        {
            while (const dirent* e = readdir(dir))
            {
                const std::string n = e->d_name;
                if (hasWavExt(n)) names.push_back(n);
            }
            closedir(dir);
        }
        if (!names.empty()) { dirOut = d; break; }
    }
    std::sort(names.begin(), names.end());
    if (names.size() > (size_t)kMaxSamples) names.resize(kMaxSamples);
    return names;
}

// A tempo written in the name ("90 SMALL18.wav", "amen_136bpm.wav"), or 0.
double tempoFromName(const std::string& n)
{
    for (size_t i = 0; i < n.size(); ++i)
    {
        if (!std::isdigit((unsigned char)n[i]) || (i > 0 && std::isdigit((unsigned char)n[i - 1]))) continue;
        size_t j = i; double v = 0;
        while (j < n.size() && std::isdigit((unsigned char)n[j])) v = v * 10 + (n[j++] - '0');
        const bool boundary = j >= n.size() || !std::isalnum((unsigned char)n[j]) || n[j] == 'b' || n[j] == 'B';
        if (boundary && v >= 60 && v <= 220) return v;
    }
    return 0;
}

// WAV -> a mono two-bar loop at the host rate, peak-normalised.
bool makeLoop(const dsp::WavData& w, const std::string& file, double hostRate, amenity::Loop& out)
{
    const size_t n = w.numFrames();
    if (n < 4096 || w.numChannels() < 1) return false;
    std::vector<float> mono(n, 0.0f);
    for (int c = 0; c < w.numChannels(); ++c)
        for (size_t i = 0; i < n; ++i) mono[i] += w.channels[(size_t)c][i];
    const float inv = 1.0f / (float)w.numChannels();
    for (float& s : mono) s *= inv;

    const double seconds = (double)n / w.sampleRate;
    if (seconds > kMaxSeconds) return false;

    // How many bars is the file? From a tempo in its name, else the length that gives a jungle-ish tempo.
    int bars = 2;
    const double nameBpm = tempoFromName(file);
    double best = 1e9;
    for (int b : {1, 2, 4, 8})
    {
        const double bpm = nameBpm > 0 ? nameBpm : 160.0;
        const double implied = 240.0 * (double)b / seconds;
        const double err = std::fabs(std::log(implied / bpm));
        if (err < best) { best = err; bars = b; }
    }
    const size_t twoBars = (size_t)((double)n * (double)std::min(bars, 2) / (double)bars);
    std::vector<float> loop;
    loop.reserve(twoBars * 2);
    loop.assign(mono.begin(), mono.begin() + (long)twoBars);
    if (bars == 1) loop.insert(loop.end(), mono.begin(), mono.begin() + (long)twoBars);   // one bar: play it twice

    // Resample to the host rate (linear: the break is a loop, not a reference tone).
    const double ratio = hostRate / w.sampleRate;
    std::vector<float> res((size_t)((double)loop.size() * ratio));
    for (size_t i = 0; i < res.size(); ++i)
    {
        const double pos = (double)i / ratio;
        const size_t k = (size_t)pos;
        const float f = (float)(pos - (double)k);
        const float a = loop[std::min(k, loop.size() - 1)], b = loop[std::min(k + 1, loop.size() - 1)];
        res[i] = a + (b - a) * f;
    }
    float peak = 0.0f;
    for (float s : res) peak = std::max(peak, std::fabs(s));
    if (peak < 1e-5f) return false;
    const float g = 0.7f / peak;
    for (float& s : res) s *= g;

    out.data.swap(res);
    out.fileRate = hostRate;
    std::snprintf(out.name, sizeof(out.name), "%.23s", file.c_str());
    // The eight single-hit pads: kick snare kick snare snare snare hat ride, as slice numbers of the 16.
    const uint8_t shots[8] = {0, 2, 4, 6, 2, 6, 1, 3};
    std::memcpy(out.shots, shots, 8);
    return true;
}

} // namespace

class Amenity : public mpc::Plugin
{
public:
    Amenity() : mpc::Plugin(makeInfo())
    {
        sampleNames_ = findSamples(samplesDir_);
        std::vector<std::string> choices;
        for (const std::string& n : sampleNames_)
        {
            std::string c = n.substr(0, n.size() - 4);
            if (c.size() > 14) c.resize(14);
            choices.push_back(c);
        }
        if (choices.empty()) choices.push_back("NO SAMPLE");
        loops_.resize(std::max<size_t>(1, sampleNames_.size()));

        addParam(mpc::floatParam("volume",    "Volume",    -40.0f, 6.0f, -3.0f, "dB", 1));
        addParam(mpc::choiceParam("sample",   "Sample",    choices, 0));
        addParam(mpc::choiceParam("pattern",  "Pattern",   {"Transport", "Always", "Off"}, 0));
        addParam(mpc::choiceParam("time",     "Time",      {"1/2X", "Normal", "2X"}, 1));
        addParam(mpc::floatParam("variation", "Variation", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("warp",      "Warp",      0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("filter",    "Filter",   -100.0f, 100.0f, 0.0f, "", 0));
        addParam(mpc::floatParam("feedback",  "Feedback",  0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::choiceParam("root",     "Fb Root",   {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 4));
        addParam(mpc::intParam   ("transpose", "Transpose", -12, 12, 0, "st"));
        addParam(mpc::boolParam  ("fill",     "SEQ Fill",  false));
        addParam(mpc::boolParam  ("freeze",   "Freeze",    false));
        addParam(mpc::floatParam("lofi",      "Lo-Fi",     0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("saturate",  "Saturate",  0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("compress",  "Compress",  0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::choiceParam("stretch",  "Stretch",   {"2x", "3x", "4x"}, 2));
        addParam(mpc::floatParam("eqlow",     "EQ Low",   -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("eqlowmid",  "EQ Low Mid", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("eqhighmid", "EQ High Mid", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(mpc::floatParam("eqhigh",    "EQ High",  -12.0f, 12.0f, 0.0f, "dB", 1));

        //            vol  smp pat tm  var  warp filt fb  root tr fill frz lofi sat comp str lo lm hm hi
        addProgram("Straight", {-3, 0, 0, 1,   0,   0,   0,  0, 4, 0, 0, 0,   0,  0,  0,  2, 0, 0, 0, 0});
        addProgram("Tear Up",  {-3, 0, 0, 1,  60,  30,   0, 10, 4, 0, 1, 0,   0, 20, 25,  2, 2, 0, 1, 2});
        addProgram("Half Time",{-3, 0, 0, 0,  35,  15, -20,  0, 4, 0, 1, 0,  10, 30, 30,  2, 4, 0, 0, 0});
        addProgram("Old Sampler",{-3, 0, 0, 1, 40, 60,   0, 25, 7, 0, 1, 0,  45, 40, 20,  2, 0, 2, 0,-2});
        addProgram("Mangled",  {-6, 0, 0, 2,  100, 80,  30, 60, 2, 0, 1, 0,  70, 70, 50,  2, 0, 0, 0, 0});
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sr_ = sampleRate;
        engine_.init(sampleRate);
        if (loadedRate_ != sampleRate)       // read the files once per sample rate (never in process())
        {
            loadedRate_ = sampleRate;
            for (size_t i = 0; i < sampleNames_.size(); ++i)
            {
                dsp::WavData w;
                amenity::Loop fresh;
                const std::string path = samplesDir_ + "/" + sampleNames_[i];
                if (dsp::loadWav(path, w) && makeLoop(w, sampleNames_[i], sampleRate, fresh))
                    loops_[i] = std::move(fresh);
                else
                    loops_[i] = amenity::Loop();
            }
        }
        currentSample_ = -1;
        smoothGain_ = dbToGain(get(kVolume));
    }

    void reset() override
    {
        engine_.init(sr_);
        currentSample_ = -1;
        rollNote_ = -1;
        fxMask_ = 0;
        smoothGain_ = dbToGain(get(kVolume));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        applyParams();

        const mpc::Transport& t = ctx.transport;
        const int mode = getInt(kPattern);
        const bool hostPlaying = t.valid && t.playing;
        bool playing = false, locked = false;
        if (mode == 0)      { playing = hostPlaying; locked = hostPlaying && t.ppqValid; }
        else if (mode == 1) { playing = true; }
        engine_.beginBlock(playing, locked, t.ppqPos, t.tempoValid ? t.bpm : 120.0);

        // Render between MIDI events so pads land on their sample.
        int frame = 0, ev = 0;
        while (frame < n)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= frame) handleMidi(ctx.midi[ev++]);
            int end = n;
            if (ev < ctx.numMidiEvents) end = mpc::clamp(ctx.midi[ev].sampleOffset, frame + 1, n);
            engine_.render(outL + frame, end - frame);
            frame = end;
        }

        const float target = dbToGain(get(kVolume));
        for (int i = 0; i < n; ++i)
        {
            smoothGain_ += 0.002f * (target - smoothGain_);
            float y = outL[i] * smoothGain_;
            y = y > 1.5f ? 1.5f : y < -1.5f ? -1.5f : y;     // safety clip
            outL[i] = y;
            if (outR) outR[i] = y;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Amenity";
        info.vendor = "RS";                    // must match plugin.json "manufacturer"
        info.uniqueId = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version = 1000;
        info.isInstrument = true;
        info.numInputs = 0;
        info.numOutputs = 2;
        info.tailSamples = 0;
        return info;
    }
    static constexpr char kUid[5] = "Ro1q";

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

    void applyParams()
    {
        const int idx = std::min(getInt(kSample), (int)loops_.size() - 1);
        if (idx != currentSample_)
        {
            currentSample_ = idx;
            engine_.setLoop(loops_[(size_t)idx].data.empty() ? nullptr : &loops_[(size_t)idx]);
        }
        amenity::Engine& e = engine_;
        e.rhythm = (int)std::lround(get(kVariation) * 1.27f);
        e.warp = (int)std::lround(get(kWarp) * 1.27f);
        e.filterValue = mpc::clamp((int)std::lround(64.0f + get(kFilter) * 0.64f), 0, 127);
        e.feedback = (int)std::lround(get(kFeedback) * 1.27f);
        e.rootNote = getInt(kRoot);
        e.transpose = getInt(kTranspose);
        e.autoFill = getBool(kFill);
        e.freeze = getBool(kFreeze);
        e.timeShift = getInt(kTime) - 1;
        e.lofi = (int)std::lround(get(kLofi) * 1.27f);
        e.saturation = (int)std::lround(get(kSaturate) * 1.27f);
        e.compression = (int)std::lround(get(kCompress) * 1.27f);
        e.stretchSlow = 2 + getInt(kStretch);
        e.eqDb[0] = get(kEqLow); e.eqDb[1] = get(kEqLowMid); e.eqDb[2] = get(kEqHighMid); e.eqDb[3] = get(kEqHigh);
    }

    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isAllNotesOff())
        {
            engine_.rollOff(); engine_.setInstant(0); engine_.killVoices();
            rollNote_ = -1; fxMask_ = 0;
            return;
        }
        if (!e.isNoteOn() && !e.isNoteOff()) return;
        const int note = e.note();
        const bool on = e.isNoteOn();

        if (note >= kFirstRollNote && note < kFirstRollNote + amenity::kRollKeys)       // Bank A: effects, held
        {
            if (on) { engine_.rollOn(note - kFirstRollNote); rollNote_ = note; }
            else if (note == rollNote_) { engine_.rollOff(); rollNote_ = -1; }
        }
        else if (note >= kFirstHitNote && note < kFirstHitNote + 8)                      // Bank B pads 1-8: single hits
        {
            if (on && currentSample_ >= 0)
                engine_.oneShot(loops_[(size_t)currentSample_].shots[note - kFirstHitNote]);
        }
        else if (note >= kFirstFxNote && note < kFirstFxNote + 3)                        // Bank B pads 9-11: held effects
        {
            const unsigned bit = 1u << (note - kFirstFxNote);
            fxMask_ = on ? (fxMask_ | bit) : (fxMask_ & ~bit);
            engine_.setInstant(fxMask_);
        }
    }

    amenity::Engine engine_;
    std::vector<amenity::Loop> loops_;
    std::vector<std::string> sampleNames_;
    std::string samplesDir_;
    double sr_ = 44100.0, loadedRate_ = 0.0;
    int currentSample_ = -1;
    int rollNote_ = -1;
    unsigned fxMask_ = 0;
    float smoothGain_ = 0.7f;
};

MPC_PLUGIN_ENTRY(Amenity)
