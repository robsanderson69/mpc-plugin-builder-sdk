// AmenityEngine.h - the Amenity jungle engine for the MPC: break slicer, sequencer, rolls, STRETCH
// and the effects chain. A float port of Amenity's integer engine (app/jungle/jungle.c), which was
// itself ported from the Jungle Choppa VST plugin.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Signal flow, per sample:
//   voices (two, crossfading) -> LO-FI -> feedback comb -> 4-band EQ -> saturation -> compression
//   -> DJ filter -> out (mono; the plugin copies it to both channels)
//
// Time: one engine "tick" is 1/48 of a 16th note (192 per beat). The pattern is 32 sixteenths
// (two bars of 4/4). With the MPC transport running the tick position is locked to the song
// position, so the phrase starts every 8 beats; otherwise the engine clock runs freely at the
// host tempo (rolls still work, the pattern only plays while "playing").
//
// Everything in here runs on the audio thread. Nothing allocates after init().

#ifndef AMENITY_ENGINE_H
#define AMENITY_ENGINE_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace amenity {

constexpr int kSteps = 32;                       // 16ths over the two bars
constexpr int kSlices = 16;                      // 8ths; each slice plays to the end of the loop
constexpr int kTicksPerStep = 48;
constexpr int kBarTicks = kSteps * kTicksPerStep;   // 1536 = two bars
constexpr int kBeatTicks = 4 * kTicksPerStep;       // 192
constexpr int kShotTicks = kTicksPerStep / 2;
constexpr int kRollKeys = 16;

// Instant effects, held.
enum : unsigned { kFxFeedback = 1, kFxLofi = 2, kFxOctave = 4 };

// A two-bar break, mono. `shots` are the slices the eight single-hit pads play
// (kick snare kick snare snare snare hat ride).
struct Loop
{
    std::vector<float> data;
    double fileRate = 44100.0;
    uint8_t shots[8] = {0, 2, 4, 6, 2, 6, 1, 3};
    char name[24] = {0};
    double nativeBpm() const { return data.empty() ? 0.0 : 480.0 * fileRate / (double)data.size(); }
};

class Engine
{
public:
    // ---- controls: the plugin writes these from process() before rendering ----
    int rhythm = 0;          // variation 0..127
    int warp = 0;            // 0..127
    int filterValue = 64;    // 0..127: <64 low-pass, 64 open, >64 high-pass
    int feedback = 0;        // 0..127
    int rootNote = 4;        // feedback root: 0..11 from C (E by default)
    int transpose = 0;       // -12..12
    int lofi = 0, saturation = 0, compression = 0;   // 0..127
    float eqDb[4] = {0, 0, 0, 0};                    // low, low-mid, high-mid, high: -12..12
    bool autoFill = false;   // SEQ
    bool freeze = false;     // REC
    int timeShift = 0;       // -1 half time, 0 normal, +1 double time
    int stretchSlow = 4;     // 2..4
    float grainMs = 38.0f, spliceMs = 10.0f;

    void init(double sampleRate)
    {
        sr_ = sampleRate;
        for (int i = 0; i < 49; ++i) pitchTable_[i] = std::pow(2.0f, (float)(i - 24) / 12.0f);
        for (Voice& v : voices_) v = Voice();
        std::memset(comb_, 0, sizeof comb_);
        std::memset(compDelay_, 0, sizeof compDelay_);
        std::memset(eqState_, 0, sizeof eqState_);
        eqCachedDb_[0] = eqCachedDb_[1] = eqCachedDb_[2] = eqCachedDb_[3] = 1000.0f;   // force a rebuild
        eqOn_ = 0;
        cachedSat_ = cachedComp_ = -1;
        straightPattern();
        patternReady_ = false;
        playing_ = false;
        lockedBefore_ = false;
        p_ = 0.0; lastTick_ = -1; barTick_ = 0;
        rollNow_ = 0; shotPending_ = -1;
        lastSlice_ = 0; lastPitch_ = 0;
        filterCoef_ = 0.0f; filterWasOpen_ = true;
        filterLow_ = filterBand_ = 0.0f;
        combFb_ = 0.0f; combGain_ = 1.0f; combWrite_ = 0;
        compEnv_ = 0.0f; compGain_ = 1.0f; compWrite_ = 0;
        lofiPhase_ = 0.0f; lofiHeld_ = 0.0f;
        instant_ = 0; instantHeld_ = 0;
        rollKey_ = 0;
        shotsQueued_ = 0;
        loop_ = nullptr;
    }

    // The break to play. A voice already sounding keeps the one it started on.
    void setLoop(const Loop* loop)
    {
        loop_ = (loop != nullptr && loop->data.size() >= (size_t)kSlices * 320) ? loop : nullptr;
        sliceLen_ = loop_ ? (double)(loop_->data.size() / kSlices) : 0.0;
    }
    const Loop* loop() const { return loop_; }

    void killVoices() { for (Voice& v : voices_) v.active = false; rollNow_ = 0; }

    // ---- performance controls ----
    void rollOn(int key) { if (key >= 0 && key < kRollKeys) rollKey_ = (uint8_t)(1 + key); }
    void rollOff() { rollKey_ = 0; }
    void setInstant(unsigned mask) { instantHeld_ = (uint8_t)(mask & 7u); }
    void straight() { straightRequest_ = true; }
    // Play one slice on the next 32nd (at once while the pattern is not playing).
    void oneShot(int slice)
    {
        if (slice < 0 || slice >= kSlices || loop_ == nullptr) return;
        if (playing_) shotPending_ = (int8_t)slice;
        else { late_ = 0.0f; trigger((unsigned)slice, 0, 1.0f, 0, 1.0f, 0); lastSlice_ = (uint8_t)slice; lastPitch_ = 0; }
    }

    // Called once per process() block, before render(). `locked`: follow the host's song
    // position `ppq` (quarter notes); otherwise the clock runs on its own.
    void beginBlock(bool playing, bool locked, double ppq, double bpm)
    {
        bpm_ = bpm < 40.0 ? 40.0 : bpm > 320.0 ? 320.0 : bpm;
        const double scale = std::pow(2.0, (double)timeShift);
        const double effBpm = bpm_ * scale;
        tps_ = effBpm * (double)kBeatTicks / 60.0 / sr_;
        speed_ = loop_ ? effBpm * (double)loop_->data.size() / (480.0 * sr_) : 0.0;
        updateDerived();

        const bool wasPlaying = playing_;
        playing_ = playing && loop_ != nullptr;
        if (playing_ && !wasPlaying)
        {
            for (Voice& v : voices_) if (v.active) v.releasing = true;
            rollNow_ = 0;
            patternReady_ = false;
            ++startCount_;
            rng_ ^= startCount_ * 2654435761u;
            if (!rng_) rng_ = 1;
            if (!locked) { p_ = 0.0; lastTick_ = -1; }
            lockedBefore_ = false;     // locked: a hard resync to the song position
        }
        else if (!playing_ && wasPlaying)
        {
            for (Voice& v : voices_) if (v.active) v.releasing = true;
            rollNow_ = 0;
            shotPending_ = -1;
        }
        if (timeShift != lastShift_) { lastShift_ = timeShift; lockedBefore_ = false; }

        if (locked)
        {
            const double hostTicks = ppq * (double)kBeatTicks * scale;
            const double d = hostTicks - p_;
            if (!lockedBefore_ || d > 2.0 || d < -2.0)
            {
                p_ = hostTicks;
                lastTick_ = (int64_t)std::ceil(p_) - 1;
            }
            else p_ += d * 0.05;       // follow the host's position gently
        }
        lockedBefore_ = locked;
    }

    // Adds `frames` mono samples to out.
    void render(float* out, int frames)
    {
        updateDerived();
        for (int i = 0; i < frames; ++i)
        {
            p_ += tps_;
            const int64_t n = (int64_t)std::floor(p_);
            while (lastTick_ < n)
            {
                ++lastTick_;
                late_ = tps_ > 0.0 ? (float)((p_ - (double)lastTick_) / tps_) : 0.0f;
                if (late_ < 0.0f) late_ = 0.0f;
                if (late_ > 1.0f) late_ = 1.0f;
                int64_t bt = lastTick_ % kBarTicks; if (bt < 0) bt += kBarTicks;
                barTick_ = (int)bt;
                onTick();
            }
            float mix = 0.0f;
            for (Voice& v : voices_) if (v.active) mix += renderVoice(v);
            out[i] += chain(mix);
        }
    }

    bool isPlaying() const { return playing_; }

private:
    // ------------------------------------------------------------------ types
    enum RollKind { REPEAT, DOWN, UP, SHRINK, BUILD, ECHO, STRETCH_ALL };
    struct RollDef { uint8_t ticks, kind; };
    static constexpr RollDef rolls_[kRollKeys] = {
        { kBeatTicks, REPEAT }, { kBeatTicks, BUILD }, { 0, STRETCH_ALL }, { kBeatTicks * 3 / 4, ECHO },
        { kBeatTicks / 2, REPEAT }, { kBeatTicks / 2, DOWN }, { kBeatTicks / 2, UP }, { kBeatTicks / 2, SHRINK },
        { kBeatTicks / 4, REPEAT }, { kBeatTicks / 4, DOWN }, { kBeatTicks / 4, UP }, { kBeatTicks / 4, SHRINK },
        { kBeatTicks / 8, REPEAT }, { kBeatTicks / 8, DOWN }, { kBeatTicks / 8, UP }, { kBeatTicks / 8, SHRINK },
    };
    enum { kStretchKey = 2, kFillInstant = 17 };

    struct Step { uint8_t active = 0, slice = 0, stretch = 0; int8_t pitch = 0; uint8_t flip = 0; };
    struct Reader { double pos = 0.0; };
    struct Voice
    {
        double pos = 0.0, rate = 0.0;        // timeline: source frame and step
        double grainRate = 0.0;              // stretch: grain step; 0 = read at the timeline
        float grainStep = 0.0f;              // crossfade rise per sample
        int grainHop = 0, grainFade = 0, grainAge = 0;
        float env = 0.0f, gain = 1.0f;
        float tone = 1.0f, low = 0.0f;       // one-pole low-pass coefficient (1 = open) and state
        int gate = 0;                        // ticks until release, 0 = play to the end
        bool active = false, releasing = false;
        int rising = 0;
        Reader read[2];
        const float* data = nullptr;
        size_t frames = 0;
    };

    static constexpr float kFade = 1.0f / 48.0f;      // ~1.1 ms in and out

    // ----------------------------------------------------------- derived state
    void updateDerived()
    {
        // instant effects: held, plus the SEQ fill's
        instant_ = instantHeld_;
        if (autoFill && fillKey_ >= kFillInstant && barTick_ >= fillStart_ && playing_)
            instant_ |= (uint8_t)(1u << (fillKey_ - kFillInstant));
        const bool fb = (instant_ & kFxFeedback) != 0;

        // feedback comb
        const float fbAmount = (float)(feedback < 0 ? 0 : feedback > 127 ? 127 : feedback);
        const float fbKnob = fbAmount * 32440.0f / 127.0f / 32768.0f;       // 0..0.99
        combFbTo_ = fb ? 0.75f : fbKnob;
        combGainTo_ = std::sqrt(1.0f - combFbTo_ * combFbTo_);
        static const float notes[12] = {674, 636, 601, 567, 535, 505, 477, 450, 425, 401, 378, 357};   // C2..B2 at 44.1 kHz
        int d = (int)std::lround(notes[((rootNote % 12) + 12) % 12] * (float)(sr_ / 44100.0));
        combDelay_ = d < 1 ? 1 : d > kCombSize - 1 ? kCombSize - 1 : d;

        // lo-fi
        int lf = (instant_ & kFxLofi) ? 127 : (lofi < 0 ? 0 : lofi > 127 ? 127 : lofi);
        if (lf == 0) { lofiRate_ = 0.0f; lofiStep_ = 0.0f; }
        else
        {
            const float hz = 44100.0f * std::pow(10.0f, -((float)lf * 17.3f / 127.0f) / 20.0f);
            lofiRate_ = hz / (float)sr_;
            lofiStep_ = std::ldexp(1.0f, lf * 10 / 127) / 32768.0f;     // 16 - bits LSBs, in full-scale units
        }

        // EQ
        for (int b = 0; b < 4; ++b)
            if (eqDb[b] != eqCachedDb_[b]) { eqCachedDb_[b] = eqDb[b]; buildEq(b, eqDb[b]); }

        // saturation / compression tables
        const int sat = saturation < 0 ? 0 : saturation > 127 ? 127 : saturation;
        if (sat != cachedSat_)
        {
            cachedSat_ = sat;
            const int driveHalfDb = sat * 36 / 127;
            satDrive_ = sat ? std::pow(10.0f, (float)driveHalfDb / 2.0f / 20.0f) : 0.0f;
            satMakeup_ = std::pow(10.0f, (float)(-7 - driveHalfDb * 53 / 100) / 2.0f / 20.0f);
        }
        const int comp = compression < 0 ? 0 : compression > 127 ? 127 : compression;
        if (comp != cachedComp_)
        {
            cachedComp_ = comp;
            compOn_ = comp != 0;
            const float a = (float)comp;
            const float thr = -a * 24.0f / 127.0f;                  // dB
            const float ratio = 1.0f + a * 5.0f / 127.0f;
            const float makeup = (-thr) * (1.0f - 1.0f / ratio) * 0.5f + a * a * 0.00036f;
            for (int i = 0; i < 256; ++i)
            {
                const float lvl = 20.0f * std::log10(((float)i + 0.5f) / 256.0f);
                const float over = lvl - thr;
                const float reduce = over > 0.0f ? over * (1.0f - 1.0f / ratio) : 0.0f;
                compTable_[i] = std::pow(10.0f, (makeup - reduce) / 20.0f);
            }
        }

        // stretch: piece length, splice, speed
        const int slow = stretchSlow < 2 ? 2 : stretchSlow > 4 ? 4 : stretchSlow;
        stretchRatio_ = 1.0f / (float)slow;
        static const float gains[3] = {29889.0f / 32768.0f, 27853.0f / 32768.0f, 26029.0f / 32768.0f};
        stretchGain_ = gains[slow - 2];
        int hop = (int)(clampf(grainMs, 3.0f, 80.0f) * (float)sr_ / 1000.0f);
        int fade = (int)(clampf(spliceMs, 0.1f, 30.0f) * (float)sr_ / 1000.0f);
        if (fade < 1) fade = 1;
        if (fade > hop) fade = hop;
        grainHop_ = hop; grainFade_ = fade;

        // DJ filter, with the SEQ fill's sweep
        int fv = filterValue < 0 ? 0 : filterValue > 127 ? 127 : filterValue;
        filterDamp_ = 1.25f;
        const bool sweeping = autoFill && fillKey_ && fillSweep_ && playing_ && barTick_ >= fillStart_;
        if (sweeping)
        {
            const int from = fv > kOpen ? kOpen : fv;
            const int span = kBarTicks - fillStart_, done = barTick_ - fillStart_;
            fv = from - from * done / span;
            if (fv == kOpen) fv = kOpen - 1;
            filterDamp_ = 0.714f;
        }
        filterNow_ = fv;
        if (fv != kOpen)
        {
            float fc;
            if (fv < kOpen) fc = 150.0f * std::exp((float)fv * std::log(11206.0f / 150.0f) / 63.0f);
            else            fc = 30.0f * std::exp((float)(fv - 65) * std::log(4000.0f / 30.0f) / 62.0f);
            const float maxFc = 0.45f * (float)sr_ * 0.5f * 2.0f;
            if (fc > maxFc) fc = maxFc;
            filterTarget_ = 2.0f * std::sin(3.14159265f * fc / (2.0f * (float)sr_));
            if (filterWasOpen_) filterCoef_ = filterTarget_;     // leaving open jumps straight to the cutoff
        }
        filterWasOpen_ = fv == kOpen;
    }

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

    void buildEq(int band, float db)
    {
        if (db > -0.01f && db < 0.01f) { eqOn_ &= (uint8_t)~(1u << band); return; }
        static const float freq[4] = {100.0f, 500.0f, 2500.0f, 8000.0f};
        const float A = std::pow(10.0f, db / 40.0f);
        const float w0 = 2.0f * 3.14159265f * freq[band] / (float)sr_;
        const float cs = std::cos(w0), sn = std::sin(w0);
        float b0, b1, b2, a0, a1, a2;
        if (band == 1 || band == 2)                          // peaking, Q 1
        {
            const float alpha = sn / 2.0f;
            b0 = 1.0f + alpha * A; b1 = -2.0f * cs; b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A; a1 = -2.0f * cs; a2 = 1.0f - alpha / A;
        }
        else
        {
            const float alpha = sn / 2.0f * std::sqrt(2.0f);   // shelf slope 1
            const float sa = 2.0f * std::sqrt(A) * alpha;
            if (band == 0)
            {
                b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
                a0 = (A + 1) + (A - 1) * cs + sa;       a1 = -2 * ((A - 1) + (A + 1) * cs);    a2 = (A + 1) + (A - 1) * cs - sa;
            }
            else
            {
                b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
                a0 = (A + 1) - (A - 1) * cs + sa;       a1 = 2 * ((A - 1) - (A + 1) * cs);      a2 = (A + 1) - (A - 1) * cs - sa;
            }
        }
        eqC_[band][0] = b0 / a0; eqC_[band][1] = b1 / a0; eqC_[band][2] = b2 / a0;
        eqC_[band][3] = a1 / a0; eqC_[band][4] = a2 / a0;
        eqOn_ |= (uint8_t)(1u << band);
    }

    // -------------------------------------------------------------- randomness
    unsigned rnd()
    {
        uint32_t x = rng_;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rng_ = x;
        return x >> 16;                      // 0..65535
    }
    unsigned rndBelow(unsigned n) { return (rnd() * n) >> 16; }

    // -------------------------------------------------------------- the pattern
    void straightPattern()
    {
        for (int i = 0; i < kSteps; ++i)
        {
            steps_[i].active = !(i & 1);
            steps_[i].slice = (uint8_t)((i >> 1) & (kSlices - 1));
            steps_[i].pitch = 0; steps_[i].stretch = 0; steps_[i].flip = 0;
        }
        fillKey_ = 0;
    }

    // 8th-note base, 16th retriggers and slice swaps, STRETCH hits, the flip, and the SEQ fill.
    void generate()
    {
        const unsigned r = (unsigned)(rhythm < 0 ? 0 : rhythm > 127 ? 127 : rhythm);
        const unsigned w = (unsigned)(warp < 0 ? 0 : warp > 127 ? 127 : warp);
        straightPattern();
        for (int i = 0; i < kSteps; i += 2)
        {
            const bool early = i < 8;
            if (rnd() < r * 284u) { steps_[i + 1].active = 1; steps_[i + 1].slice = steps_[i].slice; }
            if (rnd() < r * 103u) steps_[i].slice = (uint8_t)rndBelow(kSlices);
            if (!early && rnd() < w * 258u)
            {
                if (rnd() < 43691u) steps_[i].stretch = 1;
                else steps_[i].pitch = -12;
            }
        }
        for (int bar = 0; bar < kSteps; bar += 16)
            if (rnd() < r * 258u)
                for (int i = 10; i < 16; ++i)
                {
                    Step& st = steps_[bar + i];
                    st.active = !(i & 1);
                    st.slice = (uint8_t)((i - 10) / 2);
                    st.pitch = 0; st.stretch = 0; st.flip = 1;
                }
        if (autoFill)
        {
            const unsigned length = 8u >> rndBelow(3);
            unsigned key;
            if (length == 2u)
            {
                key = rndBelow(12);
                key = key >= 9u ? kFillInstant - 1u + key - 9u : key == 8u ? (unsigned)kStretchKey : 8u + key;
            }
            else
            {
                key = rndBelow(15);
                key = key >= 12u ? kFillInstant - 1u + key - 12u : key;
            }
            fillKey_ = (uint8_t)(1u + key);
            fillStart_ = (kSteps - (int)length) * kTicksPerStep;
            fillSweep_ = rndBelow(4) == 0u;
        }
        patternReady_ = true;
    }

    // ------------------------------------------------------------ voices, rolls
    unsigned pitchGate(int pitch, unsigned stretch, unsigned ticks) const { return pitch > 0 && !stretch ? ticks : 0u; }

    void trigger(unsigned slice, int pitch, float gain, unsigned gate, float tone, unsigned stretch)
    {
        pitch += (instant_ & kFxOctave) ? -12 : transpose;
        if (pitch < -24) pitch = -24;
        if (pitch > 24) pitch = 24;
        stretch |= stretchAll_ ? 1u : 0u;
        if (loop_ == nullptr) return;
        Voice* v = !voices_[0].active ? &voices_[0] : !voices_[1].active ? &voices_[1] :
                   voices_[0].env <= voices_[1].env ? &voices_[0] : &voices_[1];
        for (Voice& o : voices_) if (o.active) o.releasing = true;      // the new one fades in, the old out
        const double pitched = speed_ * (double)pitchTable_[pitch + 24];
        v->rate = stretch ? pitched * stretchRatio_ : pitched;
        v->grainRate = stretch ? pitched : 0.0;
        v->grainHop = grainHop_;
        v->grainFade = grainFade_;
        v->grainStep = 1.0f / (float)grainFade_;
        v->grainAge = 0;
        v->rising = 1;
        const double ahead = (double)late_ * v->rate;        // the tick fell between samples
        v->data = loop_->data.data();
        v->frames = loop_->data.size();
        v->pos = (double)slice * sliceLen_ + ahead;
        v->read[0].pos = v->read[1].pos = v->pos;
        v->env = 0.0f;
        v->gain = stretch ? gain * stretchGain_ : gain;
        v->tone = tone;
        v->low = 0.0f;
        v->gate = (int)gate;
        v->active = true;
        v->releasing = false;
    }

    unsigned autoRequest() const
    {
        return autoFill && playing_ && fillKey_ && fillKey_ < kFillInstant && barTick_ >= fillStart_ ? fillKey_ : 0u;
    }

    void fireRoll()
    {
        const unsigned key = (unsigned)(rollNow_ - 1u), variant = rolls_[key].kind;
        unsigned every = rolls_[key].ticks, gate = 0;
        const unsigned n = rollCount_ < 12 ? rollCount_ : 12;
        if (variant == BUILD)       // a beat each of 1/4, 1/8 and 1/16, then 1/32 until release
            every = rollElapsed_ < (unsigned)kBeatTicks ? kBeatTicks : rollElapsed_ < 2u * kBeatTicks ? kBeatTicks / 2 :
                    rollElapsed_ < 3u * kBeatTicks ? kBeatTicks / 4 : kBeatTicks / 8;
        int pitch = rollPitch_;
        float gain = 1.0f, tone = 1.0f;
        if (variant == DOWN) pitch -= 2 * (int)n;
        else if (variant == UP) pitch += 2 * (int)n;
        else if (variant == SHRINK)
        {
            gain = rollGain_;
            rollGain_ *= 0.85f;                                   // -1.4 dB per repeat
            gate = (every * (8u - (n < 7u ? n : 7u))) >> 3;
            if (!gate) gate = 1;
        }
        else if (variant == ECHO)
        {
            gain = rollGain_; tone = rollTone_;
            rollGain_ *= 0.75f;                                   // -2.5 dB and a lower cutoff per repeat
            rollTone_ *= 0.7f;
        }
        if (!gate) gate = pitchGate(pitch, 0, every);
        trigger(rollSlice_, pitch, gain, gate, tone, 0);
        ++rollCount_;
        rollWait_ = (int)every;
    }

    void onTick()
    {
        const int step = barTick_ / kTicksPerStep, offset = barTick_ % kTicksPerStep;
        for (Voice& v : voices_)
            if (v.active && v.gate && --v.gate == 0) v.releasing = true;

        if (playing_ && (!patternReady_ || (barTick_ == 0 && !freeze))) generate();
        if (straightRequest_) { straightRequest_ = false; straightPattern(); }

        const bool seqFire = playing_ && offset == 0;
        const Step& s = steps_[step];

        // Manual rolls win over the fill. A new roll waits for the next 16th (32nd for 1/32 rolls);
        // until then the previous roll or the pattern keeps playing.
        unsigned request = rollKey_;
        if (!request) request = autoRequest();
        if (!request)
        {
            if (rollNow_ && ((seqFire && s.active) || (offset == 0 && !(step & 1)))) rollNow_ = 0;
        }
        else if (request != rollNow_)
        {
            const bool onGrid = rolls_[request - 1u].ticks <= kTicksPerStep / 2 ? barTick_ % (kTicksPerStep / 2) == 0 : offset == 0;
            if (onGrid)
            {
                if (!rollNow_)
                {
                    const bool here = seqFire && s.active;
                    rollSlice_ = here ? s.slice : lastSlice_;
                    rollPitch_ = here ? s.pitch : lastPitch_;
                }
                rollNow_ = (uint8_t)request;
                rollWait_ = 0; rollCount_ = 0; rollElapsed_ = 0;
                rollGain_ = rollTone_ = 1.0f;
            }
        }
        stretchAll_ = rollNow_ && rolls_[rollNow_ - 1u].kind == STRETCH_ALL;
        if (rollNow_ && !stretchAll_)
        {
            if (rollWait_ == 0) fireRoll();
            --rollWait_;
            if (rollElapsed_ < 0xffffu) ++rollElapsed_;
        }
        else if (seqFire && s.active)
        {
            // with STRETCH held the pattern itself plays, every hit stretched
            trigger(s.slice, s.pitch, 1.0f, pitchGate(s.pitch, s.stretch | (stretchAll_ ? 1u : 0u), kTicksPerStep), 1.0f, s.stretch);
            lastSlice_ = s.slice;
            lastPitch_ = s.pitch;
        }

        if (shotPending_ >= 0 && barTick_ % kShotTicks == 0)
        {
            trigger((unsigned)shotPending_, 0, 1.0f, 0, 1.0f, 0);
            lastSlice_ = (uint8_t)shotPending_;
            lastPitch_ = 0;
            shotPending_ = -1;
        }
    }

    // ------------------------------------------------------------------ audio
    static float interp(const float* d, size_t frames, double pos)
    {
        const size_t i = (size_t)pos;
        if (i + 1 >= frames) return 0.0f;
        const float f = (float)(pos - (double)i);
        return d[i] + (d[i + 1] - d[i]) * f;
    }

    float renderVoice(Voice& v)
    {
        if (v.releasing)
        {
            v.env -= kFade;
            if (v.env <= 0.0f) { v.active = false; return 0.0f; }
        }
        else if (v.env < 1.0f)
        {
            v.env += kFade;
            if (v.env > 1.0f) v.env = 1.0f;
        }
        if (v.pos + 1.0 >= (double)v.frames) { v.active = false; return 0.0f; }
        const double left = (double)v.frames - 1.0 - v.pos;
        if (!v.releasing && left <= 48.0 * v.rate + 1.0) v.releasing = true;
        float s;
        if (v.grainRate == 0.0) s = interp(v.data, v.frames, v.pos);     // turntable: read at the timeline
        else
        {
            // stretch: the newest grain alone, after the splice crossfade
            Reader& a = v.read[v.rising];
            s = interp(v.data, v.frames, a.pos);
            a.pos += v.grainRate;
            if (v.grainAge < v.grainFade)
            {
                const float w = (float)v.grainAge * v.grainStep;
                Reader& b = v.read[v.rising ^ 1];
                const float bs = interp(v.data, v.frames, b.pos);
                b.pos += v.grainRate;
                s = s * w + bs * (1.0f - w);
            }
        }
        if (v.tone < 1.0f) { v.low += (s - v.low) * v.tone; s = v.low; }
        s = s * v.env * v.gain;
        v.pos += v.rate;
        if (v.grainRate != 0.0 && ++v.grainAge >= v.grainHop)
        {
            // the older grain is done: it restarts, rising, at the timeline; the current one fades out
            Reader& done = v.read[v.rising ^ 1];
            v.grainAge = 0;
            v.rising ^= 1;
            done.pos = v.pos;
        }
        return s;
    }

    float chain(float mix)
    {
        // LO-FI: take a new sample only when the slow clock ticks, rounded to the coarse step
        if (lofiRate_ > 0.0f)
        {
            lofiPhase_ += lofiRate_;
            if (lofiPhase_ >= 1.0f)
            {
                lofiPhase_ -= 1.0f;
                lofiHeld_ = std::floor(mix / lofiStep_ + 0.5f) * lofiStep_;
            }
            mix = lofiHeld_;
        }
        // Comb: the line always runs, so raising the feedback starts clean. Feedback glides.
        combFb_ += (combFbTo_ - combFb_) * 0.005f;
        combGain_ += (combGainTo_ - combGain_) * 0.005f;
        float y = mix + comb_[(combWrite_ - combDelay_) & (kCombSize - 1)] * combFb_;
        y = y > 32.0f ? 32.0f : y < -32.0f ? -32.0f : y;
        if (y > -1e-20f && y < 1e-20f) y = 0.0f;
        comb_[combWrite_] = y;
        combWrite_ = (combWrite_ + 1) & (kCombSize - 1);
        mix = y * combGain_;
        // EQ
        for (int b = 0; eqOn_ && b < 4; ++b)
        {
            if (!(eqOn_ & (1u << b))) continue;
            const float* c = eqC_[b];
            float* z = eqState_[b];       // x1 x2 y1 y2
            const float out = c[0] * mix + c[1] * z[0] + c[2] * z[1] - c[3] * z[2] - c[4] * z[3];
            z[1] = z[0]; z[0] = mix; z[3] = z[2]; z[2] = out;
            mix = out;
        }
        // Saturation: a cubic soft clipper, full scale at +-1
        if (satDrive_ > 0.0f)
        {
            float u = mix * satDrive_;
            u = u > 1.0f ? 1.0f : u < -1.0f ? -1.0f : u;
            mix = (u - u * u * u / 3.0f) * 1.5f * satMakeup_;
        }
        // Compression: peak envelope (0.5 ms attack, 120 ms release), gain from the table, 1.5 ms look-ahead
        if (compOn_)
        {
            const float level = mix < 0.0f ? -mix : mix;
            compEnv_ += (level - compEnv_) * (level > compEnv_ ? 0.0448f : 0.000183f);
            int index = (int)(compEnv_ * 256.0f);
            index = index < 0 ? 0 : index > 255 ? 255 : index;
            const float target = compTable_[index];
            compGain_ = target < compGain_ ? target : compGain_ + (target - compGain_) * (1.0f / 64.0f);
            const float ahead = compDelay_[compWrite_];
            compDelay_[compWrite_] = mix;
            compWrite_ = (compWrite_ + 1) & (kCompAhead - 1);
            mix = ahead * compGain_;
        }
        // DJ filter: Chamberlin state-variable, run twice per sample for stability up to 12 kHz
        if (filterNow_ != kOpen) filterCoef_ += (filterTarget_ - filterCoef_) * 0.004f;
        float x = mix * 0.5f;
        for (int k = 0; k < 2; ++k)
        {
            filterLow_ += filterCoef_ * filterBand_;
            const float high = x - filterLow_ - filterDamp_ * filterBand_;
            filterBand_ += filterCoef_ * high;
            if (k) x = filterNow_ < kOpen ? filterLow_ : filterNow_ > kOpen ? high : x;
        }
        x *= 2.0f;
        return (x > -1e-20f && x < 1e-20f) ? 0.0f : x;     // no denormals out of the feedback paths
    }

    // -------------------------------------------------------------------- data
    static constexpr int kOpen = 64;
    static constexpr int kCombSize = 1024;
    static constexpr int kCompAhead = 64;

    double sr_ = 44100.0;
    const Loop* loop_ = nullptr;
    double sliceLen_ = 0.0;
    double bpm_ = 120.0, tps_ = 0.0, speed_ = 0.0;
    float pitchTable_[49] = {};

    // clock
    double p_ = 0.0;
    int64_t lastTick_ = -1;
    int barTick_ = 0;
    float late_ = 0.0f;
    bool playing_ = false, lockedBefore_ = false;
    int lastShift_ = 0;
    uint32_t startCount_ = 0;
    uint32_t rng_ = 0x9e3779b9u;

    // pattern
    Step steps_[kSteps];
    bool patternReady_ = false;
    uint8_t fillKey_ = 0;
    bool fillSweep_ = false;
    int fillStart_ = 0;
    bool straightRequest_ = false;

    // voices and rolls
    Voice voices_[2];
    uint8_t rollKey_ = 0, rollNow_ = 0;
    uint8_t rollSlice_ = 0;
    int rollPitch_ = 0;
    int rollWait_ = 0;
    unsigned rollCount_ = 0, rollElapsed_ = 0;
    float rollGain_ = 1.0f, rollTone_ = 1.0f;
    bool stretchAll_ = false;
    int8_t shotPending_ = -1;
    unsigned shotsQueued_ = 0;
    uint8_t lastSlice_ = 0;
    int lastPitch_ = 0;
    uint8_t instant_ = 0, instantHeld_ = 0;
    float stretchRatio_ = 0.25f, stretchGain_ = 0.8f;
    int grainHop_ = 1675, grainFade_ = 441;

    // effects
    float lofiRate_ = 0.0f, lofiStep_ = 0.0f, lofiPhase_ = 0.0f, lofiHeld_ = 0.0f;
    float comb_[kCombSize];
    int combWrite_ = 0, combDelay_ = 535;
    float combFb_ = 0.0f, combGain_ = 1.0f, combFbTo_ = 0.0f, combGainTo_ = 1.0f;
    float eqC_[4][5] = {};
    float eqState_[4][4] = {};
    float eqCachedDb_[4] = {1000, 1000, 1000, 1000};
    uint8_t eqOn_ = 0;
    float satDrive_ = 0.0f, satMakeup_ = 1.0f;
    int cachedSat_ = -1, cachedComp_ = -1;
    bool compOn_ = false;
    float compTable_[256] = {};
    float compEnv_ = 0.0f, compGain_ = 1.0f;
    float compDelay_[kCompAhead];
    int compWrite_ = 0;
    int filterNow_ = kOpen;
    float filterCoef_ = 0.0f, filterTarget_ = 0.0f, filterDamp_ = 1.25f;
    float filterLow_ = 0.0f, filterBand_ = 0.0f;
    bool filterWasOpen_ = true;
};

constexpr Engine::RollDef Engine::rolls_[kRollKeys];

} // namespace amenity

#endif // AMENITY_ENGINE_H
