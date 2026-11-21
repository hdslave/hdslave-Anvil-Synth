// Anvil's fingerprint scenarios (tests/fingerprint.cpp).
#pragma once

namespace
{
// The parameters of Anvil 0.2, in its table's order: the random scenarios draw for these.
const std::vector<std::string> order02 { "VCO Decay", "1-2 FM Amt", "VCO EG > FM 1-2 Amt", "VCO 1 Freq", "VCO 1 EG Amt", "VCO 1 Wave",
    "VCO 2 Freq", "VCO 2 EG Amt", "VCO 2 Wave", "Seq Pitch Mod", "Hard Sync", "Noise Color", "VCO 1 Level", "Noise Level", "VCO 2 Level",
    "Cutoff", "Resonance", "VCF Mode", "VCF Decay", "VCF EG Amt", "Noise/VCF Mod", "VCA EG", "VCA Decay", "Volume", "Clock Div", "Run",
    "Trigger", "Advance", "Pitch 1", "Pitch 2", "Pitch 3", "Pitch 4", "Pitch 5", "Pitch 6", "Pitch 7", "Pitch 8", "Velocity 1",
    "Velocity 2", "Velocity 3", "Velocity 4", "Velocity 5", "Velocity 6", "Velocity 7", "Velocity 8" };

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    for (const char* key : { "default", "kick", "fm-perc", "sync-bass", "noise-hat", "acid-steps", "ratchet-snare", "random-zap", "ping-pong-toms", "pad-kit" })
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
        s.values = randomValues (1000 + r, { "Trigger", "Advance" }, order02);
        s.seconds = 3.0, s.bpm = 150.0;
        s.events = { { 24000, Event::param, "Trigger", 1.0 }, { 48011, Event::param, "Advance", 1.0 }, { 62403, Event::noteOn, "", 60 },
                     { 72000, Event::noteOff, "", 60 }, { 96005, Event::param, "Run", 1.0 }, { 100007, Event::param, "Cutoff", 0.25 },
                     { 115201, Event::param, "Clock Div", 7.0 } };
        list.push_back (s);
    }
    // Other block sizes (the same sound as random-0) and other rates.
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        Scenario s = *std::find_if (list.begin(), list.end(), [] (const Scenario& x) { return x.name == "random-0"; });
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
    // 0.3: Length and Swing, from the top and after a jump, with Advance and a Length change while it plays.
    for (int n : { 5, 3, 1 })
        for (double swing : { 0.5, 0.62, 0.75 })
        {
            Scenario s;
            s.name = "length-" + std::to_string (n) + "-swing-" + std::to_string ((int) std::lround (swing * 100));
            s.preset = "fm-perc";
            s.values = { { "Length", (double) n }, { "Swing", swing } };
            s.seconds = 5.0;
            s.events = { { 60000, Event::param, "Advance", 1.0 }, { 150001, Event::param, "Length", 7.0 } };
            s.transport = [] (uint64_t at, double bpm, double rate) -> std::pair<bool, double> {
                const double f = (double) at;
                if (at < 120000) return { true, f * bpm / 60.0 / rate };
                return { true, 5.1 + (f - 120000) * bpm / 60.0 / rate };
            };
            list.push_back (s);
        }
    // 0.4: each direction with Chance and Ratchet; the patch routings; notes playing steps.
    const char* const directions[] = { "forward", "backward", "ping-pong", "random" };
    for (int d = 0; d < 4; ++d)
    {
        Scenario s;
        s.name = std::string ("direction-") + directions[d];
        s.preset = "sync-bass";
        s.values = { { "Direction", (double) d }, { "Length", 6.0 }, { "Chance", 0.7 }, { "Ratchet", (double) (1 + d % 3) } };
        s.seconds = 5.0;
        s.events = { { 50000, Event::param, "Advance", 1.0 }, { 130001, Event::param, "Ratchet", 4.0 } };
        list.push_back (s);
    }
    Scenario routed;
    routed.name = "patch-routings";
    routed.preset = "fm-perc";
    routed.values = { { "Vel > Cutoff", 0.6 }, { "Pitch > Cutoff", -0.4 }, { "VCF EG > VCO 2", 0.3 } };
    routed.events = { { 120000, Event::param, "VCF EG > VCO 2", -0.5 }, { 200000, Event::param, "Vel > Cutoff", -1.0 } };
    list.push_back (routed);
    for (bool playing : { true, false })
    {
        Scenario s;
        s.name = std::string ("notes-steps-") + (playing ? "playing" : "stopped");
        s.preset = "kick";
        s.values = { { "Notes", 1.0 } };
        s.seconds = 4.0;
        for (int i = 0; i < 12; ++i)
            s.events.push_back ({ (uint64_t) (4000 + i * 15000), Event::noteOn, "", (double) (36 + (i * 5) % 9) });
        if (! playing)
            s.transport = [] (uint64_t, double, double) -> std::pair<bool, double> { return { false, 0.0 }; };
        list.push_back (s);
    }
    return list;
}
} // namespace
