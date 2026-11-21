// Anvil's factory presets: the patches the demo renders play (tests/host.cpp checks they are the same values and
// the same sound). A preset sets every parameter: the ones listed here, the rest to their defaults.
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
// Choice values by their place in the list: VCA EG Fast 0, Slow 1; Clock Div 1/8 is 3, 1/16 is 5; VCO 1 Wave 1 is
// Triangle; VCF Mode 1 is High Pass; Hard Sync 1 is On.
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
} // namespace presetValues

#define ANVIL_PRESET(name, key, list) Preset { name, key, list, (uint32_t) (sizeof (list) / sizeof (list[0])) }
inline const Preset presetTable[] = {
    Preset { "Default", "default", nullptr, 0 },
    ANVIL_PRESET ("Kick", "kick", presetValues::kick),
    ANVIL_PRESET ("FM Perc", "fm-perc", presetValues::fmPerc),
    ANVIL_PRESET ("Sync Bass", "sync-bass", presetValues::syncBass),
    ANVIL_PRESET ("Noise Hat", "noise-hat", presetValues::noiseHat),
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
