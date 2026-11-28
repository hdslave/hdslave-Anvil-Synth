// Anvil's factory presets: the patches the demo renders play (tests/host.cpp checks they are the same values and
// the same sound). A preset sets every parameter: the ones listed here, the rest to their defaults.
//
// The list only grows: 0.8.0 added fifteen presets built around the 1-2 FM after the ten of 0.2 to 0.7. A host may
// keep a preset's key, so a released key always loads the same preset and is never reused.
#pragma once

#include "params.h"

#include <cstring>

namespace anvil
{
struct PresetValue
{
    uint32_t id;
    float value;
};

struct Preset
{
    const char* name; // as a host lists it
    const char* key;  // its load key (CLAP preset-load), never changed once released
    const PresetValue* values;
    uint32_t count;
};

namespace presetValues
{
using namespace ParamId;
// Choice values by their place in the list: VCA EG Fast 0, Slow 1; Clock Div 1/2 is 0, 1/4 is 1, 1/8 is 3, 1/16 is
// 5; VCO 1 / VCO 2 Wave 1 is Triangle; VCF Mode 1 is High Pass; Hard Sync 1 is On; Seq Pitch Mod Off 1, VCO 2 2;
// Direction Ping-Pong 2, Random 3; Notes Steps 1; Quantize Minor 3, Minor Pent 4, Fifths 6.

// --- 0.2 to 0.7 ---
constexpr PresetValue kick[] = {
    { vco1Freq, 0.18f }, { vco1EgAmt, 0.55f }, { vcoDecay, 0.25f }, { vco1Wave, 1.0f }, { vco2Level, 0.0f }, { noiseLevel, 0.05f },
    { cutoff, 0.45f }, { vcfEgAmt, 0.3f }, { vcfDecay, 0.3f }, { vcaEg, 0.0f }, { vcaDecay, 0.45f }, { clockDiv, 3.0f },
};
constexpr PresetValue fmPerc[] = {
    { fmAmt, 0.25f }, { egFmAmt, 0.7f }, { vcoDecay, 0.3f }, { vco2Level, 1.0f }, { vco1Level, 0.0f }, { cutoff, 0.7f },
    { resonance, 0.4f }, { vcaDecay, 0.35f }, { clockDiv, 5.0f }, { pitch2, 0.4f }, { pitch4, -0.3f }, { pitch6, 0.8f },
    { velocity3, 0.3f }, { velocity7, 0.5f },
};
constexpr PresetValue syncBass[] = {
    { hardSync, 1.0f }, { vco2EgAmt, 0.6f }, { vco1Freq, 0.3f }, { vco2Freq, 0.45f }, { vco1Level, 0.3f }, { vco2Level, 0.9f },
    { cutoff, 0.4f }, { resonance, 0.55f }, { vcfEgAmt, 0.5f }, { vcaEg, 1.0f }, { vcaDecay, 0.5f }, { clockDiv, 5.0f },
    { pitch1, 0.0f }, { pitch3, 0.5f }, { pitch5, -0.4f }, { pitch8, 0.25f },
};
constexpr PresetValue noiseHat[] = {
    { vco1Level, 0.0f }, { vco2Level, 0.0f }, { noiseLevel, 1.0f }, { vcfMode, 1.0f }, { cutoff, 0.75f }, { resonance, 0.3f },
    { vcaDecay, 0.15f }, { clockDiv, 5.0f }, { velocity2, 0.4f }, { velocity4, 0.4f }, { velocity6, 0.4f }, { velocity8, 0.4f },
};
// 0.5: patches for what 0.3 to 0.5 added.
// A bassline in a key: Minor Pent from the VCOs' pitch, 7 steps swung, the cutoff following each step's pitch and
// velocity.
constexpr PresetValue acidSteps[] = {
    { vco1Freq, 0.34f }, { vco2Freq, 0.342f }, { vco1Level, 0.7f }, { vco2Level, 0.6f }, { vco1EgAmt, 0.0f }, { vco2EgAmt, 0.0f },
    { vcoDecay, 0.3f }, { cutoff, 0.42f }, { resonance, 0.6f }, { vcfEgAmt, 0.45f }, { vcfDecay, 0.33f }, { vcaDecay, 0.42f },
    { clockDiv, 5.0f }, { length, 7.0f }, { swing, 0.58f }, { quantize, 4.0f }, { pitchCutoff, 0.5f }, { velCutoff, 0.4f },
    { pitch3, 0.2f }, { pitch4, 0.05f }, { pitch6, 0.117f }, { pitch7, 0.083f },
    { velocity2, 0.5f }, { velocity3, 0.8f }, { velocity4, 0.6f }, { velocity5, 1.0f }, { velocity6, 0.4f }, { velocity7, 0.7f },
};
// Every eighth hit twice (Ratchet 2), the backbeat loud, Chance leaving out a few ghost notes.
constexpr PresetValue ratchetSnare[] = {
    { vco1Freq, 0.565f }, { vco1Wave, 1.0f }, { vco1EgAmt, 0.15f }, { vcoDecay, 0.2f }, { vco1Level, 0.5f }, { vco2Level, 0.0f },
    { noiseLevel, 0.85f }, { cutoff, 0.72f }, { resonance, 0.25f }, { vcfEgAmt, 0.2f }, { vcfDecay, 0.25f }, { vcaDecay, 0.3f },
    { clockDiv, 3.0f }, { ratchet, 2.0f }, { chance, 0.85f },
    { velocity1, 0.35f }, { velocity2, 0.25f }, { velocity3, 1.0f }, { velocity4, 0.3f }, { velocity5, 0.35f }, { velocity6, 0.25f },
    { velocity7, 1.0f }, { velocity8, 0.45f },
};
// Laser zaps in a random order: VCO 2 swept down by its EG, FM riding the VCO EG, the VCF EG bending VCO 2 too, steps
// a fifth or an octave apart, some left out.
constexpr PresetValue randomZap[] = {
    { vco2Freq, 0.75f }, { vco2EgAmt, 0.7f }, { vcoDecay, 0.28f }, { fmAmt, 0.3f }, { egFmAmt, 0.5f }, { vco1Freq, 0.45f },
    { vco1Level, 0.0f }, { vco2Level, 1.0f }, { cutoff, 0.8f }, { resonance, 0.35f }, { vcfEgAmt, 0.0f }, { vcfEgVco2, 0.25f },
    { vcfDecay, 0.35f }, { vcaDecay, 0.32f }, { clockDiv, 5.0f }, { direction, 3.0f }, { chance, 0.7f }, { quantize, 6.0f },
    { pitch2, 0.2f }, { pitch3, -0.2f }, { pitch4, 0.117f }, { pitch5, 0.3f }, { pitch6, -0.1f }, { pitch7, 0.4f }, { pitch8, 0.083f },
    { velocity2, 0.7f }, { velocity3, 0.9f }, { velocity4, 0.6f }, { velocity6, 0.8f }, { velocity7, 0.65f }, { velocity8, 0.75f },
};
// Six toms down a minor scale and back up (Ping-Pong over Length 6), swung.
constexpr PresetValue pingPongToms[] = {
    { vco1Freq, 0.42f }, { vco1Wave, 1.0f }, { vco1EgAmt, 0.25f }, { vcoDecay, 0.3f }, { vco2Freq, 0.425f }, { vco2Wave, 1.0f },
    { vco2Level, 0.4f }, { vco2EgAmt, 0.25f }, { noiseLevel, 0.05f }, { cutoff, 0.6f }, { resonance, 0.2f }, { vcfEgAmt, 0.25f },
    { vcfDecay, 0.35f }, { vcaDecay, 0.45f }, { clockDiv, 5.0f }, { length, 6.0f }, { direction, 2.0f }, { swing, 0.62f },
    { quantize, 3.0f },
    { pitch1, 0.15f }, { pitch2, 0.1f }, { pitch3, 0.05f }, { pitch5, -0.05f }, { pitch6, -0.1f },
    { velocity2, 0.7f }, { velocity3, 0.85f }, { velocity4, 0.6f }, { velocity5, 0.9f }, { velocity6, 0.65f },
};
// Eight drums on eight pads (Notes: Steps, MIDI notes 36 to 43), low to high, brighter the higher (Pitch > Cutoff);
// with the transport running the sequence plays them as a pattern too (Run Stop: the pads only).
constexpr PresetValue padKit[] = {
    { noteMode, 1.0f }, { vco1Wave, 1.0f }, { vco1Freq, 0.3f }, { vco1EgAmt, 0.5f }, { vcoDecay, 0.22f }, { vco1Level, 1.0f },
    { vco2Level, 0.0f }, { noiseLevel, 0.35f }, { cutoff, 0.5f }, { resonance, 0.15f }, { vcfEgAmt, 0.35f }, { vcfDecay, 0.25f },
    { vcaDecay, 0.35f }, { pitchCutoff, 0.8f }, { velCutoff, 0.3f }, { clockDiv, 5.0f },
    { pitch2, 0.25f }, { pitch3, 0.4f }, { pitch4, 0.55f }, { pitch5, 0.7f }, { pitch6, 0.85f }, { pitch7, 1.0f }, { pitch8, 0.6f },
    { velocity2, 0.5f }, { velocity3, 0.8f }, { velocity4, 0.5f }, { velocity5, 0.9f }, { velocity6, 0.45f }, { velocity7, 0.7f },
};

// --- 0.8.0: the FM set ---
// VCO 1 modulates VCO 2 (1-2 FM), so VCO 2's pitch against VCO 1's is the FM ratio. The VCO knobs sit on notes:
// knob = log2 (Hz / 4.5) / 10, so 0.3195 is E1, 0.3278 F1, 0.3445 G1, 0.3611 A1, 0.3861 C2, 0.4611 A2, 0.5861 C4;
// +0.1 is an octave up. A step's Pitch is semitones / 60 (0.0833 is 5, 0.1167 is 7, 0.2 is 12).
// With Seq Pitch Mod on VCO 2 (2) the steps move VCO 2 alone: the note stays on VCO 1 and every step is a new ratio.
//
// Growl Morph: F1 held on VCO 1, VCO 2 an octave up and moved by the steps; inside each hit the FM grows (VCO EG > FM
// negative), VCO 2 rises (VCF EG > VCO 2 negative) and the filter opens (VCF EG negative), noise roughing the cutoff.
constexpr PresetValue growlMorph[] = {
    { seqPitchMod, 2.0f }, { vco1Freq, 0.3278f }, { vco1EgAmt, 0.0f }, { vco1Level, 0.5f }, { vco2Freq, 0.4278f },
    { vco2EgAmt, 0.0f }, { vco2Level, 1.0f }, { fmAmt, 0.72f }, { egFmAmt, -0.5f }, { vcoDecay, 0.48f },
    { vcfEgVco2, -0.12f }, { resonance, 0.62f }, { vcfEgAmt, -0.25f }, { vcfDecay, 0.45f }, { noiseVcfMod, 0.15f },
    { vcaDecay, 0.62f }, { clockDiv, 5.0f }, { swing, 0.56f }, { pitch2, 0.1167f }, { pitch3, 0.2f },
    { pitch4, 0.05f }, { pitch5, 0.1667f }, { pitch6, 0.0833f }, { pitch7, 0.25f }, { pitch8, -0.0833f },
    { velocity2, 0.55f }, { velocity6, 0.45f }, { velocity7, 0.85f }, { velocity8, 0.7f }
};
// Sync Snarl: VCO 2 hard-synced to VCO 1 (G1), the steps on VCO 2 alone: the pitch holds and each step moves the
// formant, which the VCO and VCF EGs sweep down inside the hit; FM from VCO 1 on top. Steps unquantized.
constexpr PresetValue syncSnarl[] = {
    { seqPitchMod, 2.0f }, { hardSync, 1.0f }, { vco1Freq, 0.3445f }, { vco1EgAmt, 0.0f }, { vco1Level, 0.45f },
    { vco2Freq, 0.4028f }, { vco2EgAmt, 0.15f }, { vco2Level, 1.0f }, { vcfEgVco2, 0.12f }, { fmAmt, 0.45f },
    { egFmAmt, 0.25f }, { vcoDecay, 0.36f }, { cutoff, 0.58f }, { resonance, 0.58f }, { vcfDecay, 0.38f },
    { vcaDecay, 0.5f }, { volume, 0.76f }, { clockDiv, 5.0f }, { swing, 0.55f }, { pitch2, 0.25f }, { pitch3, 0.1f },
    { pitch4, 0.4f }, { pitch5, 0.05f }, { pitch6, 0.33f }, { pitch7, 0.18f }, { pitch8, 0.5f }, { velocity3, 0.85f },
    { velocity4, 0.5f }, { velocity5, 0.95f }, { velocity6, 0.55f }, { velocity8, 0.65f }
};
// Gut Wobble: eighths on E1, VCO 2 alone a fifth or octaves away each step (Quantize Fifths), the FM blooming as the
// VCO EG falls, VCO 2 rising, the slow VCA swelling under a closing filter.
constexpr PresetValue gutWobble[] = {
    { seqPitchMod, 2.0f }, { quantize, 6.0f }, { vco1Freq, 0.3195f }, { vco1Wave, 1.0f }, { vco1Level, 0.62f },
    { vco1EgAmt, 0.0f }, { vco2Freq, 0.3195f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.66f },
    { egFmAmt, -0.44f }, { vcoDecay, 0.56f }, { vcfEgVco2, -0.1f }, { cutoff, 0.46f }, { resonance, 0.6f },
    { vcfEgAmt, 0.42f }, { vcfDecay, 0.5f }, { vcaEg, 1.0f }, { vcaDecay, 0.66f }, { pitch2, 0.2f },
    { pitch3, 0.1167f }, { pitch4, 0.3167f }, { pitch6, 0.4f }, { pitch7, 0.1167f }, { pitch8, 0.2f },
    { velocity3, 0.9f }, { velocity4, 0.5f }, { velocity6, 0.7f }, { velocity7, 0.85f }, { velocity8, 0.55f }
};
// Stutter Growl: every eighth three times (Ratchet 3) on A1, a new ratio each step (VCO 2 alone), each stutter
// sweeping VCO 2 down, the FM near its harshest, Chance dropping a step now and then.
constexpr PresetValue stutterGrowl[] = {
    { seqPitchMod, 2.0f }, { vco1Freq, 0.3611f }, { vco1Level, 0.4f }, { vco1EgAmt, 0.0f }, { vco2Freq, 0.4611f },
    { vco2Level, 1.0f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.78f }, { egFmAmt, -0.3f }, { vcoDecay, 0.32f },
    { vcfEgVco2, 0.18f }, { cutoff, 0.55f }, { resonance, 0.66f }, { vcfEgAmt, 0.35f }, { vcfDecay, 0.3f },
    { noiseVcfMod, 0.2f }, { volume, 0.82f }, { ratchet, 3.0f }, { chance, 0.9f }, { pitch2, 0.0833f },
    { pitch3, -0.1167f }, { pitch4, 0.2f }, { pitch5, 0.05f }, { pitch6, 0.1333f }, { pitch7, -0.0333f },
    { pitch8, 0.25f }, { velocity2, 0.7f }, { velocity3, 0.9f }, { velocity6, 0.65f }, { velocity7, 0.85f },
    { velocity8, 0.75f }
};
// Churn Bass: quarter notes on A1, VCO 2 a hair off VCO 1 and each step putting it a little further off (VCO 2 alone,
// unquantized), so the FM's sidebands churn at another speed every beat.
constexpr PresetValue churnBass[] = {
    { seqPitchMod, 2.0f }, { vco1Freq, 0.3611f }, { vco1Wave, 1.0f }, { vco1Level, 0.62f }, { vco1EgAmt, 0.0f },
    { vco2Freq, 0.3619f }, { vco2Level, 0.9f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.62f }, { egFmAmt, 0.15f },
    { vcoDecay, 0.55f }, { cutoff, 0.52f }, { resonance, 0.5f }, { vcfEgAmt, 0.25f }, { vcfDecay, 0.62f },
    { vcaEg, 1.0f }, { vcaDecay, 0.74f }, { clockDiv, 1.0f }, { pitch2, 0.012f }, { pitch3, 0.025f },
    { pitch4, 0.006f }, { pitch5, 0.035f }, { pitch6, 0.018f }, { pitch7, 0.03f }, { pitch8, 0.009f },
    { velocity2, 0.8f }, { velocity3, 0.9f }, { velocity4, 0.75f }, { velocity6, 0.85f }, { velocity7, 0.95f },
    { velocity8, 0.8f }
};
// Rust Scream: a lead, VCO 2 three times VCO 1 (A2) and deeply modulated, rising into each note (VCF EG > VCO 2
// negative) through a resonant filter; the steps on VCO 2 alone, a minor scale from its own E4 (several land on A's
// harmonics), in a random order, a few left out.
constexpr PresetValue rustScream[] = {
    { seqPitchMod, 2.0f }, { quantize, 3.0f }, { direction, 3.0f }, { chance, 0.85f }, { vco1Freq, 0.4611f },
    { vco1Level, 0.3f }, { vco1EgAmt, 0.0f }, { vco2Freq, 0.6196f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.0f },
    { fmAmt, 0.7f }, { egFmAmt, 0.25f }, { vcoDecay, 0.5f }, { vcfEgVco2, -0.2f }, { cutoff, 0.66f },
    { resonance, 0.68f }, { vcfEgAmt, 0.2f }, { vcfDecay, 0.5f }, { noiseVcfMod, 0.2f }, { vcaEg, 1.0f },
    { vcaDecay, 0.6f }, { volume, 0.66f }, { pitch2, 0.0833f }, { pitch3, -0.1167f }, { pitch4, 0.2f },
    { pitch5, 0.05f }, { pitch6, -0.2f }, { pitch7, 0.1333f }, { pitch8, 0.2833f }, { velocity2, 0.75f },
    { velocity3, 0.9f }, { velocity5, 0.95f }, { velocity6, 0.7f }, { velocity7, 0.85f }, { velocity8, 0.8f }
};
// Ratio Steps: FM laid bare. Two triangles on C2, only VCO 2 heard; the steps walk VCO 2 alone through the ratios 1,
// 1.5, 2, 3 and 4 and back (Quantize Fifths): the note holds while the tone steps. Brightest on each attack.
constexpr PresetValue ratioSteps[] = {
    { seqPitchMod, 2.0f }, { quantize, 6.0f }, { vco1Freq, 0.3861f }, { vco1Wave, 1.0f }, { vco1Level, 0.0f },
    { vco1EgAmt, 0.0f }, { vco2Freq, 0.3861f }, { vco2Wave, 1.0f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.0f },
    { fmAmt, 0.5f }, { egFmAmt, 0.45f }, { vcoDecay, 0.4f }, { cutoff, 0.78f }, { resonance, 0.2f },
    { vcfEgAmt, 0.2f }, { vcaDecay, 0.45f }, { volume, 0.8f }, { clockDiv, 5.0f }, { pitch2, 0.1167f },
    { pitch3, 0.2f }, { pitch4, 0.3167f }, { pitch5, 0.4f }, { pitch6, 0.3167f }, { pitch7, 0.2f },
    { pitch8, 0.1167f }, { velocity2, 0.7f }, { velocity3, 0.85f }, { velocity4, 0.7f }, { velocity6, 0.7f },
    { velocity7, 0.85f }, { velocity8, 0.7f }
};
// FM Pluck Bass: an A minor bassline, the VCOs in unison (the steps move both), VCO 2 modulated hard on the attack
// and settling, VCO 1 under it as a sub, swung.
constexpr PresetValue fmPluckBass[] = {
    { quantize, 3.0f }, { vco1Freq, 0.3611f }, { vco1Wave, 1.0f }, { vco1Level, 0.5f }, { vco1EgAmt, 0.0f },
    { vco2Freq, 0.3611f }, { vco2Wave, 1.0f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.3f },
    { egFmAmt, 0.7f }, { resonance, 0.3f }, { vcfEgAmt, 0.35f }, { vcfDecay, 0.33f }, { vcaDecay, 0.45f },
    { volume, 1.0f }, { clockDiv, 5.0f }, { swing, 0.57f }, { pitch3, 0.2f }, { pitch5, 0.05f }, { pitch6, 0.1167f },
    { pitch7, 0.1667f }, { pitch8, -0.0333f }, { velocity2, 0.5f }, { velocity5, 0.9f }, { velocity6, 0.5f },
    { velocity7, 0.75f }
};
// Tidal Morph: a drone in half notes on C2; each step sets VCO 2 alone to a new ratio (unquantized), and over each
// long note the FM grows while the slow VCA and the filter swell and fall.
constexpr PresetValue tidalMorph[] = {
    { seqPitchMod, 2.0f }, { vco1Freq, 0.3861f }, { vco1Wave, 1.0f }, { vco1Level, 0.6f }, { vco1EgAmt, 0.0f },
    { vco2Freq, 0.3861f }, { vco2Level, 0.85f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.55f }, { egFmAmt, -0.3f },
    { vcoDecay, 0.75f }, { vcfEgVco2, -0.06f }, { cutoff, 0.5f }, { resonance, 0.5f }, { vcfDecay, 0.8f },
    { vcaEg, 1.0f }, { vcaDecay, 0.86f }, { volume, 0.75f }, { clockDiv, 0.0f }, { pitch2, 0.1f }, { pitch3, 0.0583f },
    { pitch4, 0.2f }, { pitch5, 0.1583f }, { pitch6, 0.0167f }, { pitch7, 0.3f }, { pitch8, 0.1167f },
    { velocity2, 0.7f }, { velocity3, 0.85f }, { velocity5, 0.95f }, { velocity6, 0.75f }, { velocity8, 0.65f }
};
// Glass Bells: C minor pentatonic; VCO 2 the note (C4), VCO 1 below it at 1 : 1.41 so the partials ring
// inharmonic, the FM deep on the strike and fading; the steps move both VCOs, keeping the ratio.
constexpr PresetValue glassBells[] = {
    { quantize, 4.0f }, { vco1Freq, 0.5361f }, { vco1Wave, 1.0f }, { vco1Level, 0.0f }, { vco1EgAmt, 0.0f },
    { vco2Freq, 0.5861f }, { vco2Wave, 1.0f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.2f },
    { egFmAmt, 0.6f }, { vcoDecay, 0.45f }, { cutoff, 0.85f }, { resonance, 0.1f }, { vcfEgAmt, 0.0f },
    { vcaDecay, 0.62f }, { volume, 0.55f }, { clockDiv, 5.0f }, { swing, 0.55f }, { pitch2, 0.05f },
    { pitch3, 0.1167f }, { pitch4, 0.1667f }, { pitch5, 0.2f }, { pitch6, 0.1667f }, { pitch7, 0.1167f },
    { pitch8, 0.25f }, { velocity4, 0.55f }, { velocity5, 0.95f }, { velocity7, 0.75f }, { velocity8, 0.7f }
};
// FM Kick: four on the floor and a pickup. VCO 2 a triangle swept down from about 600 Hz to G1, VCO 1 below it (2 : 3)
// modulating it on the attack only, a knock gone with the sweep; the steps leave the pitch alone (Seq Pitch Mod Off).
constexpr PresetValue fmKick[] = {
    { seqPitchMod, 1.0f }, { vco1Freq, 0.286f }, { vco1Wave, 1.0f }, { vco1Level, 0.0f }, { vco1EgAmt, 0.45f },
    { vco2Freq, 0.3445f }, { vco2Wave, 1.0f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.45f }, { egFmAmt, 0.4f },
    { vcoDecay, 0.24f }, { resonance, 0.1f }, { vcfDecay, 0.28f }, { vcaDecay, 0.5f }, { volume, 0.8f },
    { velocity2, 0.0f }, { velocity3, 1.0f }, { velocity4, 0.0f }, { velocity6, 0.0f }, { velocity7, 1.0f },
    { velocity8, 0.4f }
};
// FM Clang: a snare from metal, VCO 2 modulated at an inharmonic ratio with a short drop in pitch, noise for the snap
// (and roughing the cutoff), on the backbeat with ghost notes.
constexpr PresetValue fmClang[] = {
    { seqPitchMod, 1.0f }, { vco1Freq, 0.5361f }, { vco1Level, 0.3f }, { vco1EgAmt, 0.1f }, { vco2Freq, 0.5861f },
    { vco2Wave, 1.0f }, { vco2Level, 0.9f }, { vco2EgAmt, 0.1f }, { fmAmt, 0.45f }, { egFmAmt, 0.35f },
    { vcoDecay, 0.28f }, { noiseLevel, 0.8f }, { noiseVcfMod, 0.25f }, { cutoff, 0.7f }, { resonance, 0.3f },
    { vcfEgAmt, 0.25f }, { vcfDecay, 0.3f }, { vcaDecay, 0.45f }, { volume, 1.0f }, { velocity1, 0.2f },
    { velocity2, 0.0f }, { velocity3, 1.0f }, { velocity4, 0.25f }, { velocity5, 0.0f }, { velocity6, 0.2f },
    { velocity7, 1.0f }, { velocity8, 0.35f }
};
// FM Toms: six toms down a minor scale and back up (Ping-Pong, Length 6), VCO 2 a fifth over VCO 1 and modulated by
// it on the strike, both swept down a little, swung.
constexpr PresetValue fmToms[] = {
    { quantize, 3.0f }, { direction, 2.0f }, { length, 6.0f }, { vco1Freq, 0.4445f }, { vco1Wave, 1.0f },
    { vco1Level, 0.4f }, { vco1EgAmt, 0.22f }, { vco2Freq, 0.503f }, { vco2Wave, 1.0f }, { vco2Level, 0.9f },
    { vco2EgAmt, 0.22f }, { fmAmt, 0.2f }, { egFmAmt, 0.4f }, { vcoDecay, 0.3f }, { noiseLevel, 0.05f },
    { cutoff, 0.62f }, { resonance, 0.2f }, { vcfEgAmt, 0.25f }, { vcaDecay, 0.48f }, { volume, 0.75f },
    { clockDiv, 5.0f }, { swing, 0.6f }, { pitch1, 0.25f }, { pitch2, 0.2f }, { pitch3, 0.1667f }, { pitch4, 0.1167f },
    { pitch5, 0.05f }, { velocity1, 0.9f }, { velocity2, 0.7f }, { velocity3, 0.85f }, { velocity4, 0.65f },
    { velocity5, 0.95f }, { velocity6, 0.75f }
};
// Metal Hats: hats from FM alone, two square VCOs near 1 and 1.47 kHz at full FM depth, a little noise, through the
// high pass; each step tints VCO 2 a little (VCO 2 alone), accents on the beat.
constexpr PresetValue metalHats[] = {
    { seqPitchMod, 2.0f }, { vco1Freq, 0.7796f }, { vco1Level, 0.3f }, { vco1EgAmt, 0.0f }, { vco2Freq, 0.8352f },
    { vco2Level, 1.0f }, { vco2EgAmt, 0.0f }, { fmAmt, 0.85f }, { vcoDecay, 0.2f }, { noiseLevel, 0.35f },
    { vcfMode, 1.0f }, { cutoff, 0.7f }, { resonance, 0.3f }, { vcfEgAmt, 0.1f }, { vcfDecay, 0.2f },
    { vcaDecay, 0.25f }, { volume, 0.8f }, { clockDiv, 5.0f }, { pitch2, 0.01f }, { pitch3, -0.008f },
    { pitch4, 0.015f }, { pitch6, 0.006f }, { pitch7, -0.012f }, { pitch8, 0.02f }, { velocity2, 0.35f },
    { velocity3, 0.65f }, { velocity4, 0.35f }, { velocity5, 0.9f }, { velocity6, 0.35f }, { velocity7, 0.65f },
    { velocity8, 0.5f }
};
// FM Pad Kit: eight FM drums on pads 1-8 (Notes: Steps, MIDI notes 36 to 43; again on pads 9-16), from a kick up to
// metal, the higher the brighter (Pitch > Cutoff), each struck with an FM knock; with the transport running the
// sequence plays them as a pattern too (Run Stop: the pads only).
constexpr PresetValue fmPadKit[] = {
    { noteMode, 1.0f }, { vco1Freq, 0.3195f }, { vco1Wave, 1.0f }, { vco1Level, 0.3f }, { vco1EgAmt, 0.35f },
    { vco2Freq, 0.3695f }, { vco2Wave, 1.0f }, { vco2Level, 1.0f }, { vco2EgAmt, 0.35f }, { fmAmt, 0.15f },
    { egFmAmt, 0.5f }, { vcoDecay, 0.24f }, { noiseLevel, 0.3f }, { cutoff, 0.48f }, { vcfDecay, 0.25f },
    { pitchCutoff, 0.7f }, { velCutoff, 0.3f }, { volume, 0.75f }, { clockDiv, 5.0f }, { pitch2, 0.2f },
    { pitch3, 0.3f }, { pitch4, 0.4f }, { pitch5, 0.55f }, { pitch6, 0.7f }, { pitch7, 0.85f }, { pitch8, 1.0f },
    { velocity2, 0.5f }, { velocity4, 0.5f }, { velocity5, 0.9f }, { velocity6, 0.45f }, { velocity7, 0.7f },
    { velocity8, 0.55f }
};
} // namespace presetValues

#define ANVIL_PRESET(name, key, list) Preset { name, key, list, (uint32_t) (sizeof (list) / sizeof (list[0])) }
// The presets a host lists, in this order.
inline const Preset presetTable[] = {
    Preset { "Default", "default", nullptr, 0 },
    ANVIL_PRESET ("Kick", "kick", presetValues::kick),
    ANVIL_PRESET ("FM Perc", "fm-perc", presetValues::fmPerc),
    ANVIL_PRESET ("Sync Bass", "sync-bass", presetValues::syncBass),
    ANVIL_PRESET ("Noise Hat", "noise-hat", presetValues::noiseHat),
    ANVIL_PRESET ("Acid Steps", "acid-steps", presetValues::acidSteps),
    ANVIL_PRESET ("Ratchet Snare", "ratchet-snare", presetValues::ratchetSnare),
    ANVIL_PRESET ("Random Zap", "random-zap", presetValues::randomZap),
    ANVIL_PRESET ("Ping-Pong Toms", "ping-pong-toms", presetValues::pingPongToms),
    ANVIL_PRESET ("Pad Kit", "pad-kit", presetValues::padKit),
    // 0.8.0
    ANVIL_PRESET ("Growl Morph", "growl-morph", presetValues::growlMorph),
    ANVIL_PRESET ("Sync Snarl", "sync-snarl", presetValues::syncSnarl),
    ANVIL_PRESET ("Gut Wobble", "gut-wobble", presetValues::gutWobble),
    ANVIL_PRESET ("Stutter Growl", "stutter-growl", presetValues::stutterGrowl),
    ANVIL_PRESET ("Churn Bass", "churn-bass", presetValues::churnBass),
    ANVIL_PRESET ("Rust Scream", "rust-scream", presetValues::rustScream),
    ANVIL_PRESET ("Ratio Steps", "ratio-steps", presetValues::ratioSteps),
    ANVIL_PRESET ("FM Pluck Bass", "fm-pluck-bass", presetValues::fmPluckBass),
    ANVIL_PRESET ("Tidal Morph", "tidal-morph", presetValues::tidalMorph),
    ANVIL_PRESET ("Glass Bells", "glass-bells", presetValues::glassBells),
    ANVIL_PRESET ("FM Kick", "fm-kick", presetValues::fmKick),
    ANVIL_PRESET ("FM Clang", "fm-clang", presetValues::fmClang),
    ANVIL_PRESET ("FM Toms", "fm-toms", presetValues::fmToms),
    ANVIL_PRESET ("Metal Hats", "metal-hats", presetValues::metalHats),
    ANVIL_PRESET ("FM Pad Kit", "fm-pad-kit", presetValues::fmPadKit),
};
#undef ANVIL_PRESET
constexpr uint32_t presetCount = sizeof (presetTable) / sizeof (presetTable[0]);

inline const Preset* findPreset (const char* key)
{
    if (key == nullptr)
        return nullptr;
    for (const auto& p : presetTable)
        if (std::strcmp (p.key, key) == 0)
            return &p;
    return nullptr;
}

// Every parameter's value under `preset`, in the table's order (buttons at 0).
inline void presetValuesOf (const Preset& preset, float* out)
{
    for (uint32_t i = 0; i < paramCount; ++i)
        out[i] = paramTable[i].def;
    for (uint32_t n = 0; n < preset.count; ++n)
    {
        const int index = paramIndex (preset.values[n].id);
        if (index >= 0 && paramTable[index].kind != Kind::button)
            out[index] = clampParam (paramTable[index], preset.values[n].value);
    }
}
} // namespace anvil
