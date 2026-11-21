// Anvil's fingerprint scenarios (tests/fingerprint.cpp).
#pragma once

namespace
{
std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    for (const char* key : { "default", "kick", "fm-perc", "sync-bass", "noise-hat" })
    {
        Scenario s;
        s.name = std::string ("preset-") + key;
        s.preset = key;
        list.push_back (s);
    }
    // The bench's patches.
    Scenario worst;
    worst.name = "bench-worst";
    worst.values = { { "1-2 FM Amt", 0.5 }, { "VCO EG > FM 1-2 Amt", 0.6 }, { "Hard Sync", 1 }, { "Noise Level", 0.4 }, { "Noise Color", 1 },
                     { "Noise/VCF Mod", 0.5 }, { "Resonance", 0.75 }, { "VCA Decay", 1.0 }, { "VCA EG", 1 }, { "VCO 1 Wave", 1 }, { "Clock Div", 5 } };
    list.push_back (worst);
    Scenario typical;
    typical.name = "bench-typical";
    typical.values = { { "1-2 FM Amt", 0.2 }, { "VCA Decay", 0.3 }, { "Clock Div", 5 }, { "Resonance", 0.3 } };
    list.push_back (typical);
    // Random settings with the buttons, MIDI and knobs moving while it plays.
    for (uint32_t r = 0; r < 24; ++r)
    {
        Scenario s;
        s.name = "random-" + std::to_string (r);
        s.values = randomValues (1000 + r, { "Trigger", "Advance" });
        s.seconds = 3.0, s.bpm = 150.0;
        s.events = { { 24000, Event::param, "Trigger", 1.0 }, { 48011, Event::param, "Advance", 1.0 }, { 62403, Event::noteOn, "", 60 },
                     { 72000, Event::noteOff, "", 60 }, { 96005, Event::param, "Run", 1.0 }, { 100007, Event::param, "Cutoff", 0.25 },
                     { 115201, Event::param, "Clock Div", 7.0 } };
        list.push_back (s);
    }
    // Other block sizes (the same sound as random-0) and other rates.
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        Scenario s = list[7];
        s.name = "random-0-block-" + std::to_string (block);
        s.block = block;
        list.push_back (s);
    }
    for (double rate : { 44100.0, 96000.0 })
    {
        Scenario s;
        s.name = "rate-" + std::to_string ((int) rate);
        s.preset = "fm-perc";
        s.rate = rate;
        s.seconds = 4.0;
        list.push_back (s);
    }
    // Stopped, started again, jumped, a loop's wrap.
    Scenario moves;
    moves.name = "stop-start-jump";
    moves.preset = "sync-bass";
    moves.seconds = 6.0;
    moves.transport = [] (uint64_t at, double bpm, double rate) -> std::pair<bool, double> {
        const double f = (double) at;
        if (at < 48000) return { true, f * bpm / 60.0 / rate };
        if (at < 72000) return { false, 2.0 };
        if (at < 144000) return { true, (f - 72000) * bpm / 60.0 / rate };
        if (at < 200000) return { true, 3.3 + (f - 144000) * bpm / 60.0 / rate };
        return { true, 100.0 / 24000.0 + (f - 200000) * bpm / 60.0 / rate };
    };
    list.push_back (moves);
    return list;
}
} // namespace
