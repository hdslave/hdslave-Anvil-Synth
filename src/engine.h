// Anvil's sound and sequencer. One voice, run at twice the host's rate and filtered back down:
//
//   sequencer step --pitch--> VCO 1 and/or VCO 2 (Seq Pitch Mod)       VCO EG --> VCO 1 / VCO 2 pitch (EG Amts)
//                  --velocity--> the peak of all three EGs             VCO EG --> 1-2 FM depth (the new knob)
//   VCO 1's triangle --linear FM--> VCO 2, VCO 1 --hard sync--> VCO 2
//   VCO 1 + VCO 2 + noise (white or pink) --> mixer (clips when hot)
//     --> 4-pole ladder (LP or HP; VCF EG and noise move the cutoff) --> VCA (its EG) --> Volume --> out
//
// Every step of the 8-step sequencer (Length plays fewer; Direction, Chance and Ratchet reshape it) triggers all three
// envelopes. The clock is the host's: a step starts on each
// multiple of Clock Div from the song's top, while the host plays.
//
// Cost: no allocation, no locks, no branches on the sound's path beyond the oscillators' edges; the knobs are
// worked out every 8 frames, the oscillators and filter only while the VCA is open.
#pragma once

#include "params.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace anvil
{
// --- Fast math ------------------------------------------------------------------------------------------------

// Two branch-free instructions (fmaxnm, fminnm); a NaN comes out as `lo`.
inline float clampf (float x, float lo, float hi) { return std::fmin (std::fmax (x, lo), hi); }

// 2^x to about 3e-6 relative (0.005 cent): the integer part by rounding, so the polynomial only spans [-0.5, 0.5]
// and two octaves meet without a step. For pitch and cutoff, whose octaves the parameter ranges keep within
// [-30, 30] (no clamp here); envelope rates use exp (they sit so near 1 that this error would change a decay).
inline float exp2Fast (float x)
{
    const float xi = std::rint (x), f = x - xi;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    const int32_t bits = ((int32_t) xi + 127) << 23;
    float scale;
    std::memcpy (&scale, &bits, sizeof (scale));
    return p * scale;
}

// tanh, rational, exact at 0 and +-3 where it reaches +-1 and holds: bounded, smooth, cheap.
inline float tanhFast (float x)
{
    x = clampf (x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// The trapezoidal one-pole's gain G = g / (1 + g), g = tan(x), x in [0, 1.2] (the cutoff's angle a sample): tan as
// a Pade (3,2), under 0.5 % off at the top, folded in so it costs one division.
inline float onePoleGain (float x)
{
    const float x2 = x * x, top = x * (15.0f - x2);
    return top / (15.0f - 6.0f * x2 + top);
}

// --- Envelope ---------------------------------------------------------------------------------------------------
// The panel's EGs: a trigger charges the envelope from wherever it is toward the step's velocity (its peak), as a
// capacitor through a resistor would (aimed past the peak and stopped at it), then it decays exponentially. A step
// softer than where a tail still is does not cut it: the tail goes on falling, as on the hardware. Nor does a softer
// step cut a louder one's attack: the charge goes on to the louder peak.
struct Envelope
{
    float value = 0.0f;
    float peak = 0.0f;
    float attack = 1.0f; // charge a sample
    float decay = 0.0f;  // multiplier a sample
    bool attacking = false;

    void trigger (float level)
    {
        peak = attacking ? std::max (peak, level) : level;
        attacking = peak > value;
    }
    bool idle() const { return ! attacking && value == 0.0f; }
    float next()
    {
        if (attacking)
        {
            value += (1.25f * peak - value) * attack;
            if (value >= peak)
                value = peak, attacking = false;
        }
        else
        {
            value *= decay;
            if (value < 1.6e-5f) // -96 dB, a 16-bit step: done, and exactly zero so the VCA can close
                value = 0.0f;
        }
        return value;
    }
};

// --- Oscillator -------------------------------------------------------------------------------------------------
// Square or triangle from a phase in [0, 1), band-limited with polynomial BLEP (steps) and BLAMP (corners) residuals
// over two samples. The output runs one sample behind so an edge can correct the sample before it too.
struct Oscillator
{
    float phase = 0.0f;
    float held = 0.0f; // the sample still being built (emitted at the next step)

    static float square (float p) { return p < 0.5f ? 1.0f : -1.0f; }
    static float triangle (float p) { return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; }

    // A step of `h` (or a slope change of `s` a sample) `d` samples before now: shares of it for the held sample and
    // for the current one.
    static void blep (float h, float d, float& before, float& now)
    {
        before += 0.5f * h * d * d;
        now -= 0.5f * h * (1.0f - d) * (1.0f - d);
    }
    static void blamp (float s, float d, float& before, float& now)
    {
        const float e = 1.0f - d;
        before += s * d * d * d * (1.0f / 6.0f);
        now += s * e * e * e * (1.0f / 6.0f);
    }

    // The edges crossed moving from phase `from` by `move` (at most one each of 0.5 and 1: move < 0.5), at `dt` a
    // sample, the move ending `ago` samples before now; corrected.
    static void edges (bool tri, float from, float move, float dt, float ago, float& before, float& now)
    {
        const float to = from + move;
        if (from < 0.5f && to >= 0.5f)
        {
            const float d = (to - 0.5f) / dt + ago;
            if (tri) blamp (-8.0f * dt, d, before, now);
            else blep (-2.0f, d, before, now);
        }
        if (to >= 1.0f)
        {
            const float d = (to - 1.0f) / dt + ago;
            if (tri) blamp (8.0f * dt, d, before, now);
            else blep (2.0f, d, before, now);
        }
    }

    // One sample at increment dt (cycles a sample). Returns the finished previous sample; `wrapped` gets how long ago
    // (in samples, in [0, 1)) the phase passed 1, or -1.
    float step (bool tri, float dt, float& wrapped)
    {
        float before = 0.0f, now = 0.0f;
        edges (tri, phase, dt, dt, 0.0f, before, now);
        float p = phase + dt;
        wrapped = -1.0f;
        if (p >= 1.0f)
            p -= 1.0f, wrapped = p / dt;
        phase = p;
        const float out = held + before;
        held = (tri ? triangle (p) : square (p)) + now;
        return out;
    }

    // As step, for a slave reset by its master `ago` samples before now (ago < 0: no reset this sample).
    [[gnu::always_inline]] float stepSynced (bool tri, float dt, float ago)
    {
        if (ago < 0.0f)
        {
            float unused;
            return step (tri, dt, unused);
        }
        float before = 0.0f, now = 0.0f;
        // Up to the reset: its own edges, then the jump to phase 0 and the corner of the new slope.
        const float run = dt * (1.0f - ago);
        edges (tri, phase, run, dt, ago, before, now);
        float at = phase + run;
        if (at >= 1.0f)
            at -= 1.0f;
        if (tri)
        {
            const float slope = at < 0.5f ? 4.0f * dt : -4.0f * dt;
            blep (-1.0f - triangle (at), ago, before, now);
            blamp (4.0f * dt - slope, ago, before, now);
        }
        else
            blep (1.0f - square (at), ago, before, now);
        phase = dt * ago;
        const float out = held + before;
        held = (tri ? triangle (phase) : square (phase)) + now;
        return out;
    }
};

// --- Ladder filter -----------------------------------------------------------------------------------------------
// Four one-pole stages in trapezoidal form with the feedback solved without delay, and one tanh on the input, which
// keeps self-oscillation bounded and gives the ladder's overdrive. LP: the fourth stage; HP: the binomial mix of the
// stages, which a ladder's high-pass mode makes.
struct Ladder
{
    float s[4] {};

    float process (float x, float G, float k, bool highPass)
    {
        const float B = 1.0f - G;
        // y4 = G^4 u + S: S from the stages' states, u the stage input after the feedback.
        const float S = B * (s[3] + G * (s[2] + G * (s[1] + G * s[0])));
        const float G2 = G * G;
        const float u = tanhFast ((x - k * S) / (1.0f + k * G2 * G2));
        float y = u, taps[4];
        for (int i = 0; i < 4; ++i)
        {
            const float v = (y - s[i]) * G;
            y = v + s[i];
            s[i] = y + v;
            taps[i] = y;
        }
        return highPass ? u - 4.0f * taps[0] + 6.0f * taps[1] - 4.0f * taps[2] + taps[3] : taps[3];
    }
};

// --- Half-band decimator ------------------------------------------------------------------------------------------
// From twice the host's rate to its rate: a 47-tap Kaiser-windowed half-band FIR. Its even taps are zero but the
// centre, so a frame costs 12 multiplies: flat to about 19 kHz at 48 kHz, about 80 dB down from 29 kHz.
//   y[m] = 0.5 a[m-11] + sum_i h(2i+1) (b[m-11+i] + b[m-12-i]),  a = x[2m], b = x[2m+1]
struct Decimator
{
    static constexpr int half = 12;
    float coeff[half] {};
    float odd[4 * half] {};  // the last 2*half b samples, written twice so a window reads straight
    float even[half] {};     // the last half a samples
    int at = 0, evenAt = 0, quiet = 0;

    void design()
    {
        const double beta = 8.0;
        auto bessel = [] (double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k)
                term *= (x / (2.0 * k)) * (x / (2.0 * k)), sum += term;
            return sum;
        };
        const int edge = 4 * half - 1; // the outermost tap's distance is 2*half-1; the window spans +-(2*half)
        double total = 0.0, h[half];
        for (int i = 0; i < half; ++i)
        {
            const int k = 2 * i + 1;
            const double t = (double) k / (double) (edge + 1) * 2.0;
            const double w = bessel (beta * std::sqrt (std::max (0.0, 1.0 - t * t))) / bessel (beta);
            h[i] = std::sin (M_PI * k / 2.0) / (M_PI * k) * w;
            total += 2.0 * h[i];
        }
        for (int i = 0; i < half; ++i) // the odd taps sum to exactly 0.5: unity at DC
            coeff[i] = (float) (h[i] * 0.5 / total);
        clear();
    }
    void clear()
    {
        std::memset (odd, 0, sizeof (odd));
        std::memset (even, 0, sizeof (even));
        at = evenAt = 0;
        quiet = 0;
    }
    bool silent() const { return quiet >= 2 * half + 1; }
    float push (float a, float b)
    {
        constexpr int len = 2 * half;
        odd[at] = odd[at + len] = b;
        even[evenAt] = a;
        quiet = (a == 0.0f && b == 0.0f) ? std::min (quiet + 1, 2 * half + 1) : 0; // held at silent(): never overflows
        const float* w = odd + at + 1; // w[0] .. w[len-1]: b[m-len+1] .. b[m]
        float sum = 0.0f;
        for (int i = 0; i < half; ++i)
            sum += coeff[i] * (w[half + i] + w[half - 1 - i]);
        evenAt = evenAt + 1 == half ? 0 : evenAt + 1;
        const float centre = even[evenAt]; // the oldest: a[m-11]
        at = at + 1 == len ? 0 : at + 1;
        return sum + 0.5f * centre;
    }
};

// --- Engine -------------------------------------------------------------------------------------------------------
// A clock less than this many frames away is due now. CLAP gives the song's place in steps of 2^-31 beats, which
// at 48 kHz is about 1e-5 frames: a clock that falls on a block's first frame could otherwise come a frame late, by
// how the host's rounding fell for that block, so the sound would depend on the block size.
constexpr double clockSlack = 1.0e-3;
// A place moved by less than a 64th note (in quarters: hdslaveOS's own jump tolerance) is not a jump. hdslaveOS works
// the place out from its ticks, so it is not quite a straight line from block to block (a swung tick, a tick on a
// block's edge, MTC's trim: up to 0.0021 of a quarter), and a plug-in two tracks play is run twice a block (its second
// call told a block on, the next block's first a block back).
constexpr double jumpBeats = 1.0 / 16.0;
// CLAP rounds the place to 2^-31 beats: a move told as exactly a 64th note can read a hair over it.
constexpr double jumpSlack = 1.0e-9;

class Engine
{
public:
    void prepare (double hostRate, uint32_t)
    {
        rate = std::isfinite (hostRate) && hostRate > 0.0 ? hostRate : 48000.0; // a rate that is no rate reads as 48 kHz
        inner = (float) (2.0 * rate);
        invInner = 1.0f / inner;
        chokeStep = (float) (1.0 / std::max (1.0, std::round ((double) chokeSeconds * rate))); // the fade's share a sample
        decimator.design();
        // Pink noise: three one-pole filters on white (Paul Kellet's), their poles moved from 44.1 kHz to this rate.
        const float poles[3] = { 0.99765f, 0.96300f, 0.57000f };
        for (int i = 0; i < 3; ++i)
            pinkPole[i] = std::pow (poles[i], 44100.0f / inner);
        reset();
    }

    void reset()
    {
        vcoEg = vcfEg = vcaEg = Envelope {};
        osc1 = osc2 = Oscillator {};
        osc2.phase = 0.37f; // the two VCOs never start in step
        filter = Ladder {};
        decimator.clear();
        noise = 0x9e3779b9u;
        pink[0] = pink[1] = pink[2] = 0.0f;
        tick = 0;
        playing = aligned = false;
        lastStep = -1;
        stepIndex = 0;
        travel = 0;
        now = 0;
        clockedAt = -(int64_t (1) << 40); // no clock played yet
        clockedLoud = false;
        ratchetsLeft = 0;
        hitKey = -1;
        fadeGain = 1.0f, fadeStep = 0.0f, waiting = Waiting {};
        // As a fresh instance's: no hit has fired (one on frame 0 next is no second hit of a frame), no step is
        // held, and the last block's size is forgotten.
        firedAt = ~(uint64_t) 0, firedLevel = 0.0f;
        heldPitch = 0.0f, heldKey = false, heldQuantize = knob[ParamId::quantize];
        stepLevel = 0.0f, velocityLonger = 1.0f;
        lastBlock = 0;
        // A Run "press" made before this (the defaults switching Run on as the plug-in is made, a flush while
        // inactive) is no press: a first block that already plays starts where the song is, as any Play does.
        runPressed = false;
        dirty = true;
        retarget();
        for (int i = 0; i < smoothed; ++i)
            smooth[i] = target[i];
        moving = false;
    }

    // A state or preset loaded: the knobs' sound is taken at once, with no glide across the load (the host
    // activates, then loads; a load over a ringing hit settles the same way).
    void settle()
    {
        retarget();
        for (int i = 0; i < smoothed; ++i)
            smooth[i] = target[i];
        moving = false;
    }

    // A parameter's value, from the frame the next render starts.
    void set (uint32_t id, float v)
    {
        if (id >= ParamId::count || knob[id] == v)
            return;
        const float was = knob[id];
        knob[id] = v;
        dirty = true;
        if (id == ParamId::clockDiv)
            regridWanted = true;
        if (id == ParamId::quantize && ! heldKey)
            heldQuantize = v;
        if (id == ParamId::run && v >= 0.5f && was < 0.5f)
            runPressed = true; // Run plays the step it is on at once, then goes on from it
    }

    // Trigger plays the step the sequence is on without moving it; Advance moves it on a step without playing it.
    void press (uint32_t id)
    {
        if (id == ParamId::advance)
        {
            stepIndex = following (stepIndex);
            heldPitch = knob[ParamId::pitch1 + stepIndex]; // the pitch CV follows the step, sounding or not
            heldKey = false, heldQuantize = knob[ParamId::quantize];
            dirty = true;
        }
        else
            fire();
    }

    // A MIDI note (a pad). Run on (the sequencer's own switch, whether or not the host plays), or Notes: Steps: the
    // step the key names plays, 36 to 43 steps 1 to 8, and so on up and down the keyboard (a pad previews its step).
    // Run off with Notes: Pitch: the voice plays the key, as a step of its own (chromatic).
    void note (int key, float velocity)
    {
        if (knob[ParamId::run] >= 0.5f || knob[ParamId::noteMode] >= 0.5f)
            playStep (((key - firstStepNote) % 8 + 8) % 8, velocity, key);
        else
            playKey (key, velocity);
    }

    // A note plays step `s` at once, at its pitch and velocity (times the note's), without moving the sequence. `key`:
    // the note's (-1: the page's Push, no note), for a choke.
    void playStep (int s, float velocity, int key = -1)
    {
        fireStep (s & 7, knob[ParamId::velocity1 + (s & 7)] * velocity, key);
    }

    // Notes: Pitch with Run off: the voice plays at once as a step whose Pitch is the key's distance from keyAtKnobs
    // (note 36, pad 1, plays the Freq knobs as they are; each key a semitone) and whose velocity is the note's,
    // through the routes a step's pitch and velocity take (Seq Pitch Mod, Pitch > Cutoff, Vel > Cutoff ...), not
    // snapped by Quantize; the sequence does not move. The pitch stays within what a step's Pitch knob reaches (+-5
    // octaves: keys 0 to 96; higher keys play as 96), so no route sees more than a step could give it.
    void playKey (int key, float velocity)
    {
        strike (clampf ((float) (key - keyAtKnobs) / (seqPitchOctaves * 12.0f), -1.0f, 1.0f), true, velocity, key);
    }

    // A choke (a CLAP note choke; hdslaveOS makes one of its silence for each key played): the hit key `key` played
    // stops, if it is the one sounding (the sequencer's hits, Trigger's and the page's have no key); key -1, whatever
    // sounds. MIDI's All Sound Off (CC 120 at 0) is choke (-1).
    void choke (int key)
    {
        if (key < 0 || key == hitKey)
            stop();
    }

    // Where the sequence's own count is: for a host that gives no song position (0 when it last stopped).
    double ownPosition() const { return playing ? position : 0.0; }

    // For the screen: the step the sequence is on, how many hits it has played, whether the host's clock moves it.
    int currentStep() const { return stepIndex; }
    uint32_t hitCount() const { return played; }
    bool running() const { return playing && knob[ParamId::run] >= 0.5f; }

    // The host's transport at the start of a process call of `frames`.
    void transport (bool isPlaying, double beats, double bpm, uint32_t frames)
    {
        perFrame = bpm / 60.0 / rate;
        const double window = (double) std::max (frames, lastBlock); // how late a wrap inside a block can be seen
        lastBlock = frames;
        if (! isPlaying)
        {
            playing = false;
            return;
        }
        // A Play, or a place other than where the last block ended by more than a block and a 64th note (a jump, a
        // loop going round): line up again. Less than that is drift (a tempo change inside the last block, the host's
        // place off its line), followed as it is: the steps already played are not played again (a step back holds
        // them until the song is past them again), one passed is played at once.
        const double moved = std::fabs (beats - position);
        if (! playing)
            // A Play's "just past a step" is one host block (hdslaveOS's 128 frames), whatever block size this host
            // calls with: the same Play heard in 1-, 37- or 4096-frame blocks plays the same steps. A jump keeps
            // the block window, which is how late a loop's wrap inside a block can be seen.
            relocate (beats, 128.0, false);
        else if (moved > 0.5 * perFrame && moved >= window * perFrame && moved > jumpBeats + jumpSlack)
            relocate (beats, window, true);
        playing = true;
        position = beats;
    }

    void render (float* left, float* right, uint32_t frames)
    {
        while (frames > 0)
        {
            uint32_t run = frames;
            if (ratchetsLeft > 0)
            {
                // Ratchet: the step's further hits, spread evenly over it.
                if (now >= ratchetNext)
                {
                    fire();
                    if (--ratchetsLeft > 0)
                        ratchetNext = ratchetFrom + (uint64_t) std::llround (ratchetGap * (double) (ratchetCount - ratchetsLeft));
                    continue;
                }
                run = (uint32_t) std::min<uint64_t> (run, ratchetNext - now);
            }
            if (playing)
            {
                if (regridWanted)
                    regrid();
                const double next = clockBeat (lastStep + 1);
                const double ahead = (next - position) / perFrame;
                if (ahead <= clockSlack)
                {
                    // Run switched on on a clock's own frame (with Play, or later): the clock plays its step, once.
                    runPressed = false;
                    ++lastStep;
                    clock (lastStep);
                    continue;
                }
                const double untilStep = std::ceil (ahead - clockSlack);
                if (untilStep < (double) run)
                    run = (uint32_t) untilStep;
            }
            if (runPressed)
            {
                // Run switched on between clocks: the step it holds plays at once, and the clock goes on from it.
                runPressed = false;
                if (playing)
                    fire(), aligned = true;
            }
            sound (left, right, run);
            now += run;
            if (playing)
                position += run * perFrame;
            left += run;
            if (right)
                right += run;
            frames -= run;
        }
    }

private:
    // --- Sequencer ---
    double stepBeats() const { return clockDivBeats[(int) knob[ParamId::clockDiv]]; }
    int length() const { return std::max (1, (int) knob[ParamId::length]); }
    // The step after `s`: Length steps, then step 1 again.
    int nextStep (int s) const { return s + 1 >= length() ? 0 : s + 1; }
    int direction() const { return (int) knob[ParamId::direction]; }

    // A number in [0, 1) from a clock's number: the same for the same place in the song (Chance, Random).
    static float unit (int64_t n, uint32_t salt)
    {
        uint64_t x = (uint64_t) n * 0x9e3779b97f4a7c15ull + salt;
        x ^= x >> 30, x *= 0xbf58476d1ce4e5b9ull, x ^= x >> 27, x *= 0x94d049bb133111ebull, x ^= x >> 31;
        return (float) (x >> 40) * (1.0f / 16777216.0f);
    }
    // The step for the `n`th clock of the sequence (n = 0: step 1) when it does not go forward: Backward is 1, L, L-1
    // ... 2; Ping-Pong 1 ... L ... 2, its ends once; Random any of the L.
    int stepFor (int64_t n) const
    {
        const int64_t len = length();
        switch (direction())
        {
            case 1: return (int) ((len - ((n % len) + len) % len) % len);
            case 2:
            {
                if (len == 1)
                    return 0;
                const int64_t period = 2 * len - 2, at = ((n % period) + period) % period;
                return (int) (at < len ? at : period - at);
            }
            case 3: return (int) (unit (n, 0x2545f491u) * (float) len) % (int) len;
            default: return (int) (((n % len) + len) % len);
        }
    }
    // The step after this one: forward as always; the other directions by their count.
    int following (int s)
    {
        ++travel;
        return direction() == 0 ? nextStep (s) : stepFor (travel);
    }

    // Swing: the second clock of each pair comes later, at Swing's share of the pair (50 %: straight, 66 %: a
    // triplet shuffle, 75 %: a dotted one), as an MPC's swing does. At 50 % the clocks are k grids from the top, worked
    // out exactly as before Swing was added.
    double clockBeat (int64_t k) const
    {
        const double grid = stepBeats(), swing = knob[ParamId::swing];
        if (swing == 0.5 || (k & 1) == 0)
            return (double) k * grid;
        return (double) (k - 1) * grid + 2.0 * grid * swing;
    }
    // The last clock at or before `beats`.
    int64_t clockAtOrBefore (double beats) const
    {
        const double grid = stepBeats();
        const int64_t b = (int64_t) std::floor (beats / grid + 1.0e-9);
        if ((b & 1) == 0 || knob[ParamId::swing] == 0.5f)
            return b;
        return beats / grid + 1.0e-9 < clockBeat (b) / grid ? b - 1 : b; // between the grid line and the late clock
    }

    // A step starts on this frame (clock k): the sequence moves on and plays it (with Run off it holds where it is and
    // stays silent, as the panel's sequencer does when stopped).
    void clock (int64_t k)
    {
        ratchetsLeft = 0;
        clockedAt = (int64_t) now, clockedLoud = false;
        if (knob[ParamId::run] < 0.5f)
            return;
        advanceTo (k);
        // Chance: whether this clock's step sounds, the same for the same place in the song.
        if (knob[ParamId::chance] >= 1.0f || unit (k, 0x5bd1e995u) < knob[ParamId::chance])
        {
            fire();
            clockedLoud = true;
            const int hits = (int) knob[ParamId::ratchet];
            if (hits > 1)
            {
                ratchetCount = hits;
                ratchetsLeft = hits - 1;
                ratchetFrom = now;
                ratchetGap = (clockBeat (k + 1) - clockBeat (k)) / perFrame / hits;
                ratchetNext = ratchetFrom + (uint64_t) std::llround (ratchetGap);
            }
        }
    }

    void relocate (double beats, double window, bool jumped)
    {
        // Play and jumps line the sequence up with the song by themselves: a Run switched on in the same block does not
        // also play the held step (which would leave the clock following it instead of the song's place).
        runPressed = false;
        const int64_t b = clockAtOrBefore (beats);
        const double late = (beats - clockBeat (b)) / perFrame; // frames since the host's step b began
        position = beats;
        regridWanted = false;
        // A jump whose step b began on the very frame a step played here: a loop gone round inside a block (the host
        // tells only at the next one, so the steps ran on past the loop's end; its end is its start). That step was
        // b: the sequence lines up with b and goes on from it, and b is not played again, unless the step played
        // then stayed silent (Chance) and b sounds.
        if (jumped && std::fabs ((double) ((int64_t) now - clockedAt) - late) <= 2.0)
        {
            lastStep = b;
            if (knob[ParamId::run] < 0.5f)
                return;
            aligned = false;
            if (clockedLoud)
                advanceTo (b); // the hit sounding goes on as it is
            else
                clock (b);
            return;
        }
        // Just past a step's start (less than a block: a Play landing a hair late, a wrap seen early enough): that
        // step plays at once; further in, the next step is the first.
        lastStep = b - (late < window ? 1 : 0);
        if (knob[ParamId::run] >= 0.5f) // stopped by Run, the sequence holds its step through the host's moves
            aligned = false;
    }

    // Clock Div changed: count steps on the new grid from here, without a burst of them. A step of the new grid due on
    // this frame (in the last frame's time) still plays.
    void regrid()
    {
        lastStep = clockAtOrBefore (position - perFrame * (1.0 - clockSlack));
        regridWanted = false;
    }

    // The step a clock lands on: after a Play or a jump, the song's place (so its top is step 1); else the next one.
    void advanceTo (int64_t clock)
    {
        const int64_t n = length();
        if (! aligned)
        {
            travel = clock;
            stepIndex = direction() == 0 ? (int) (((clock % n) + n) % n) : stepFor (clock);
            aligned = true;
        }
        else
            stepIndex = following (stepIndex);
    }

    // All three envelopes, toward the step's velocity, with its pitch held from now (no glide into a step: it lands on
    // it). A louder step also decays a little longer.
    void fire() { fireStep (stepIndex, knob[ParamId::velocity1 + stepIndex]); }

    void fireStep (int s, float v, int from = -1) { strike (knob[ParamId::pitch1 + s], false, v, from); }

    // A hit at `pitch` (a step's Pitch knob, -1..1 for +-5 octaves; `key`: a played key's, which Quantize leaves as it
    // is) and level `v`; `from`: the MIDI key that struck it (-1: none), for a choke.
    void strike (float pitch, bool key, float v, int from)
    {
        // Two steps on one frame (two pads at once, in whatever order the host sends them): the louder plays, whole (its
        // pitch, cutoff and decays as well as its level); on a tie the later.
        if (firedAt == now && v < firedLevel)
            return;
        firedAt = now, firedLevel = v;
        hitKey = from;
        if (fadeStep != 0.0f)
        {
            // A hit inside a choke's fade waits for its end, 1.5 ms at most: the choke finishes what it stops, and the
            // hit plays whole from silence. Struck at once, it either stepped when the fade ended (to 0.6.5) or, softer
            // than the tail it landed on, left the choked hit ringing at full level (0.6.6). The latest hit waits.
            waiting = { true, key, pitch, v };
            return;
        }
        hit (pitch, key, v);
    }

    // The hit itself (strike(): whether and when).
    void hit (float pitch, bool key, float v)
    {
        ++played;
        heldPitch = pitch;
        heldKey = key, heldQuantize = key ? 0.0f : knob[ParamId::quantize];
        stepLevel = v;
        velocityLonger = 0.9f + 0.2f * v; // before retarget(): it sets the three decays with it
        retarget();
        smooth[pitch1] = target[pitch1];
        smooth[pitch2] = target[pitch2];
        if (knob[ParamId::velCutoff] != 0.0f || knob[ParamId::pitchCutoff] != 0.0f)
            smooth[cut] = target[cut]; // the step's own cutoff from its first sample
        vcoEg.trigger (v);
        vcfEg.trigger (v);
        vcaEg.trigger (v);
    }

    // A choke or All Sound Off: the hit sounding fades out (the output, straight down to zero over chokeSeconds: a
    // sample's share of it, at the host's rate), its envelopes and what the decimator holds of it are cleared once it
    // is out, and the Ratchet hits still to come in this step do not play. The sequence's place and Run are untouched:
    // the next step plays as ever. A hit after it on the same frame plays, as soon as the fade is out: it is no softer
    // second hit of that frame. Nothing sounding (the VCA closed and the decimator empty), it is done at once. Inside a
    // fade, the fade goes on from where it is, and a hit waiting for it is stopped before it sounds.
    void stop()
    {
        ratchetsLeft = 0;
        firedAt = ~(uint64_t) 0;
        waiting.due = false;
        if (vcaEg.idle() && decimator.silent())
            endFade();
        else
            fadeStep = chokeStep;
    }

    // A choke's fade is out (or there was nothing to fade): the hit it stopped is gone, as a hit that never was, and
    // the decimator keeps nothing of it to play after the fade at full gain. A hit that waited for it plays now.
    void endFade()
    {
        fadeGain = 1.0f, fadeStep = 0.0f;
        vcoEg = vcfEg = vcaEg = Envelope {};
        if (! decimator.silent()) // one already silent keeps its count, so the knobs are still taken at once
            decimator.clear();
        if (waiting.due)
            waiting.due = false, hit (waiting.pitch, waiting.key, waiting.level);
    }

    // A decay's multiplier a sample: -60 dB over its time. An attack's charge a sample: at the peak in `seconds`
    // (charging toward 1.25 times it, the peak is reached after ln 5 time constants).
    float decayOf (float k, float longer) const { return (float) std::exp (-6.907755 / ((double) decaySeconds (k) * longer * inner)); }
    float attackOf (float seconds) const { return (float) (1.0 - std::exp (-1.609438 / ((double) seconds * inner))); }

    // --- Knobs ---
    // What the knobs and the held step ask for; the sound eases toward it every 8 frames.
    enum Smoothed { pitch1, pitch2, eg1, eg2, fm, egFm, level1, level2, levelNoise, cut, res, vcfAmt, noiseMod, gain, vcfVco2, smoothed };

    void retarget()
    {
        const int mod = (int) knob[ParamId::seqPitchMod];
        // The held step's pitch in octaves, as Quantize plays it (Off: the knob as it is). A played key's pitch is held
        // with Quantize Off (heldQuantize: the knob's, or Off for a key).
        const int quantize = (int) heldQuantize;
        const float seq = quantize == 0 ? heldPitch * seqPitchOctaves : quantizedSemitones (heldPitch, quantize) * (1.0f / 12.0f);
        target[pitch1] = std::log2 (vcoLowHz) + knob[ParamId::vco1Freq] * vcoOctaves + (mod == 0 ? seq : 0.0f);
        target[pitch2] = std::log2 (vcoLowHz) + knob[ParamId::vco2Freq] * vcoOctaves + (mod != 1 ? seq : 0.0f);
        target[eg1] = knob[ParamId::vco1EgAmt] * vcoEgOctaves;
        target[eg2] = knob[ParamId::vco2EgAmt] * vcoEgOctaves;
        target[fm] = knob[ParamId::fmAmt] * knob[ParamId::fmAmt]; // the knob's taper; the VCO EG adds to it linearly, as a CV
        target[egFm] = knob[ParamId::egFmAmt];
        target[level1] = knob[ParamId::vco1Level] * knob[ParamId::vco1Level];
        target[level2] = knob[ParamId::vco2Level] * knob[ParamId::vco2Level];
        target[levelNoise] = knob[ParamId::noiseLevel] * knob[ParamId::noiseLevel];
        // The patch routings: the step's velocity and pitch move the cutoff (0 when their knobs are at 0).
        target[cut] = std::log2 (cutoffLowHz) + knob[ParamId::cutoff] * cutoffOctaves
                    + knob[ParamId::velCutoff] * stepLevel * velCutoffOctaves + knob[ParamId::pitchCutoff] * seq;
        target[vcfVco2] = knob[ParamId::vcfEgVco2] * vcfEgVco2Octaves;
        target[res] = knob[ParamId::resonance] * (knob[ParamId::vcfMode] >= 0.5f ? maxFeedbackHighPass : maxFeedback);
        target[vcfAmt] = knob[ParamId::vcfEgAmt] * vcfEgOctaves;
        target[noiseMod] = knob[ParamId::noiseVcfMod] * noiseVcfOctaves;
        target[gain] = knob[ParamId::volume] * knob[ParamId::volume];

        tri1 = knob[ParamId::vco1Wave] >= 0.5f;
        tri2 = knob[ParamId::vco2Wave] >= 0.5f;
        sync = knob[ParamId::hardSync] >= 0.5f;
        highPass = knob[ParamId::vcfMode] >= 0.5f;
        pinkNoise = knob[ParamId::noiseColor] >= 0.5f;
        vcoEg.decay = decayOf (knob[ParamId::vcoDecay], velocityLonger);
        vcfEg.decay = decayOf (knob[ParamId::vcfDecay], velocityLonger);
        vcaEg.decay = decayOf (knob[ParamId::vcaDecay], velocityLonger);
        vcoEg.attack = vcfEg.attack = attackOf (attackFast);
        vcaEg.attack = attackOf (knob[ParamId::vcaEg] >= 0.5f ? attackSlow : attackFast);
        dirty = false;
        moving = true;
    }

    void sound (float* left, float* right, uint32_t frames)
    {
        uint32_t i = 0;
        while (i < frames)
        {
            if (vcaEg.idle() && vcoEg.idle() && vcfEg.idle() && decimator.silent())
            {
                // Nothing sounds and nothing is running down: the rest of this run is silence. The knobs need no
                // easing while nothing plays (a step lands on them), so they are taken as they are.
                if (dirty)
                    retarget();
                for (int k = 0; k < smoothed; ++k)
                    smooth[k] = target[k];
                moving = false;
                const uint32_t n = frames - i;
                std::memset (left + i, 0, sizeof (float) * n);
                if (right)
                    std::memset (right + i, 0, sizeof (float) * n);
                tick += n;
                return;
            }
            if ((tick++ & 7) == 0)
            {
                if (dirty)
                    retarget();
                if (moving)
                {
                    // About 1 ms to arrive at 48 kHz: no zipper noise, no lag a hand would notice.
                    float far = 0.0f;
                    for (int k = 0; k < smoothed; ++k)
                    {
                        smooth[k] += (target[k] - smooth[k]) * 0.25f;
                        far = std::max (far, std::fabs (target[k] - smooth[k]));
                    }
                    if (far < 1.0e-5f)
                    {
                        for (int k = 0; k < smoothed; ++k)
                            smooth[k] = target[k];
                        moving = false;
                    }
                }
            }
            float y;
            if (vcaEg.idle() && decimator.silent())
            {
                // The VCA is closed: the other envelopes run down; the oscillators and filter wait.
                vcoEg.next(), vcoEg.next(), vcfEg.next(), vcfEg.next();
                y = 0.0f;
            }
            else
            {
                const float a = voice(), b = voice();
                // A soft ceiling, so a hot patch rounds off instead of clipping.
                y = tanhFast (decimator.push (a, b)) * 0.98f;
            }
            if (fadeStep != 0.0f)
            {
                y *= fadeGain;
                fadeGain -= fadeStep;
                if (fadeGain <= 0.0f || (vcaEg.idle() && decimator.silent())) // out, or died away by itself inside it
                    endFade();
            }
            left[i] = y;
            if (right)
                right[i] = y;
            ++i;
        }
    }

    [[gnu::always_inline]] float voice()
    {
        const float eVco = vcoEg.next(), eVcf = vcfEg.next();
        if (vcaEg.idle())
            return 0.0f;
        const float eVca = vcaEg.next();
        // VCO 1, then VCO 2: its pitch from the knob, the step and the VCO EG, then VCO 1's triangle moving its
        // frequency linearly (never below 0 Hz) by the depth the 1-2 FM knob and the VCO EG set, and VCO 1's sync.
        const float dt1 = clampf (exp2Fast (smooth[pitch1] + eVco * smooth[eg1]) * invInner, 1.0e-7f, 0.45f);
        float wrapped;
        const float v1 = osc1.step (tri1, dt1, wrapped);
        const float depth = clampf (smooth[fm] + smooth[egFm] * eVco, 0.0f, 1.0f);
        const float swing = 1.0f + depth * fmIndex * Oscillator::triangle (osc1.phase);
        const float dt2 = clampf (exp2Fast (smooth[pitch2] + eVco * smooth[eg2] + eVcf * smooth[vcfVco2]) * invInner * swing, 1.0e-9f, 0.45f);
        const float v2 = osc2.stepSynced (tri2, dt2, sync ? wrapped : -1.0f);
        // Noise: a 32-bit xorshift, white, or pink through three poles.
        noise ^= noise << 13, noise ^= noise >> 17, noise ^= noise << 5;
        float n = (float) (int32_t) noise * (1.0f / 2147483648.0f);
        if (pinkNoise)
        {
            pink[0] = pinkPole[0] * pink[0] + n * 0.0990460f;
            pink[1] = pinkPole[1] * pink[1] + n * 0.2965164f;
            pink[2] = pinkPole[2] * pink[2] + n * 1.0526913f;
            n = (pink[0] + pink[1] + pink[2] + n * 0.1848f) * 0.25f;
        }
        // The mixer: each level a VCA; together they clip, as the panel's mixer does when pushed.
        const float mix = tanhFast (0.9f * (smooth[level1] * v1 + smooth[level2] * v2 + smooth[levelNoise] * n));
        // The ladder, its cutoff moved by the VCF EG and by the noise.
        const float hz = exp2Fast (smooth[cut] + eVcf * smooth[vcfAmt] + n * smooth[noiseMod]);
        const float G = onePoleGain (clampf ((float) M_PI * hz * invInner, 0.0005f, 1.2f));
        const float y = filter.process (mix, G, smooth[res], highPass);
        // The VCA (its EG at the step's velocity) and Volume; the soft ceiling follows the decimator.
        return y * eVca * smooth[gain] * 1.6f;
    }

    double rate = 48000.0, perFrame = 0.0, position = 0.0;
    float inner = 96000.0f, invInner = 1.0f / 96000.0f;
    float chokeStep = 1.0f / 72.0f;   // a choke's fade: the share of the output it takes off a sample (prepare())
    float fadeGain = 1.0f, fadeStep = 0.0f; // a choke's fade, while it runs (0: none)
    struct Waiting
    {
        bool due = false; // a hit struck inside a choke's fade, to play at its end (strike())
        bool key = false;
        float pitch = 0.0f, level = 0.0f;
    } waiting;
    float knob[ParamId::count] {};
    float target[smoothed] {}, smooth[smoothed] {};
    float heldPitch = 0.0f, velocityLonger = 1.0f, stepLevel = 0.0f;
    float pink[3] {}, pinkPole[3] {};
    bool tri1 = false, tri2 = false, sync = false, highPass = false, pinkNoise = false;
    bool playing = false, aligned = false, regridWanted = false, dirty = true, moving = false, runPressed = false;
    bool heldKey = false; // the held pitch is a played key's (Notes: Pitch), not a step's: Quantize leaves it
    float heldQuantize = 0.0f; // the Quantize the held pitch plays by: the knob's, or Off for a played key
    int64_t lastStep = -1, travel = 0;
    int stepIndex = 0;
    uint32_t played = 0; // hits played (for the screen)
    // Ratchet: the hits left in this step, and when the next comes (frames since prepare()).
    uint64_t now = 0, ratchetFrom = 0, ratchetNext = 0;
    uint64_t firedAt = ~(uint64_t) 0; // the frame the last step fired on, and its level
    float firedLevel = 0.0f;
    int hitKey = -1;           // the MIDI key that struck the hit sounding (-1: the sequencer, a button, the page)
    int64_t clockedAt = 0;     // the frame the last step's clock came on
    bool clockedLoud = false;  // and whether that step played
    double ratchetGap = 0.0;
    int ratchetsLeft = 0, ratchetCount = 0;
    uint32_t tick = 0, lastBlock = 0, noise = 0x9e3779b9u;
    Envelope vcoEg, vcfEg, vcaEg;
    Oscillator osc1, osc2;
    Ladder filter;
    Decimator decimator;
};
} // namespace anvil
