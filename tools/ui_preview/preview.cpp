// Exercise the same Screen used by the CLAP entry point, using factory patches and a simulated transport.
#include "screen.h"
#include "presets.h"
#include <array>
#include <chrono>
#include <cstdlib>

namespace
{
using namespace anvil;
using hdui::Key;
using Frame = std::array<uint32_t, hdui::width * hdui::height>;

void require (bool ok, const char* what)
{
    if (!ok) { std::fprintf (stderr, "Failed: %s\n", what); std::exit (1); }
}

struct Rig
{
    std::atomic<float> values[paramCount] {};
    StepState state;
    Screen screen { values, state };
    hdui::Host h;
    Frame frame {};

    explicit Rig (const char* key)
    {
        for (unsigned i = 0; i < paramCount; ++i) values[i] = paramTable[i].def;
        const auto* preset = findPreset (key);
        require (preset, "factory preset");
        for (unsigned i = 0; i < preset->count; ++i)
        {
            const auto& p = preset->values[i];
            const int index = paramIndex (p.id);
            require (index >= 0, "preset parameter");
            values[index] = p.value;
        }
        h.nowMs = 1000;
        h.playing = true;
        h.accent = 0xa586ff;
        state.step = 4;
        state.running = true;
        state.hits = 1;
        keyEvent (Key::wheel, 2);
    }

    hdui::Edits keyEvent (Key key, int amount = 0, bool shift = false)
    {
        hdui::Edits edits;
        screen.input (key, amount, shift, edits);
        for (int i = 0; i < edits.count; ++i)
        {
            const auto& e = edits.list[i];
            if (!e.press) values[paramIndex (e.id)] = e.value;
        }
        return edits;
    }
    bool paint (bool whole = false)
    {
        screen.update (h.nowMs);
        return screen.paint (h, frame.data(), 320, whole);
    }
};

struct State { const char* name; const char* patch; int group, row; };
constexpr State states[] = {
    { "playing", "fm-perc", 0, 0 }, { "six-steps", "ping-pong-toms", 0, 0 },
    { "vco", "fm-perc", 1, 1 }, { "mix", "noise-hat", 2, 1 }, { "vcf", "noise-hat", 3, 0 },
    { "vca", "sync-bass", 4, 1 }, { "patch", "random-zap", 5, 0 }, { "seq", "ping-pong-toms", 6, 2 },
    { "shift", "fm-perc", 0, 0 }, { "pickup", "acid-steps", 0, 0 }, { "stopped", "pad-kit", 0, 0 },
    { "buttons", "ratchet-snare", 6, 9 }
};

void configure (Rig& r, const State& s)
{
    for (int i = 0; i < s.group; ++i) r.keyEvent (Key::nudge, 1);
    if (s.row) r.keyEvent (Key::wheel, s.row);
    if (s.group == 1) r.keyEvent (Key::push);
    if (!std::strcmp (s.name, "shift")) r.h.shift = true;
    if (!std::strcmp (s.name, "pickup")) { r.h.pot[1] = 14; r.h.engaged[1] = false; }
    if (!std::strcmp (s.name, "stopped"))
    {
        r.state.running = false;
        r.state.hits = 0;
        r.h.playing = false;
        r.values[paramIndex (ParamId::run)] = 0;
    }
}
void save (const char* name, const Frame& pixels)
{
    char path[128];
    std::snprintf (path, sizeof (path), "build/ui-preview/frames/%s.ppm", name);
    FILE* out = std::fopen (path, "wb");
    require (out, "open frame");
    std::fputs ("P6\n320 218\n255\n", out);
    for (auto p : pixels)
    {
        const uint8_t rgb[] = {uint8_t (p >> 16), uint8_t (p >> 8), uint8_t (p)};
        std::fwrite (rgb, 1, 3, out);
    }
    require (!std::fclose (out), "write frame");
}
int difference (const Frame& a, const Frame& b)
{
    int count = 0;
    for (size_t i = 0; i < a.size(); ++i) count += a[i] != b[i];
    return count;
}

void test()
{
    for (const auto& spec : states)
    {
        Rig r (spec.patch);
        configure (r, spec);
        r.paint();
        std::array<uint32_t,323*218> pad;
        pad.fill (0x12345678);
        require (!r.screen.paint (r.h, pad.data(), 323, false), "idle paint");
        require (std::all_of (pad.begin(), pad.end(), [](auto c) {return c == 0x12345678;}), "idle touched canvas");
        require (r.screen.paint (r.h, pad.data(), 323, true), "whole paint");
        for (int y = 0; y < 218; ++y)
        {
            require (std::equal (r.frame.begin() + y*320, r.frame.begin() + (y+1)*320, pad.begin()+y*323), "full repaint");
            for (int x = 320; x < 323; ++x) require (pad[y*323+x] == 0x12345678, "stride");
        }
    }
    Rig r ("fm-perc");
    r.paint();
    uint32_t ids[3];
    r.screen.knobs (ids);
    require (ids[0]==ParamId::pitch3 && ids[1]==ParamId::velocity3 && ids[2]==ParamId::cutoff, "picked step bindings");
    const auto edits = r.keyEvent (Key::push);
    require (edits.count == 1 && edits.list[0].press && edits.list[0].id == auditionId+2, "picked audition");
    auto prev = r.frame;
    r.h.nowMs += 60;
    require (r.paint() && difference (prev,r.frame) <= 16, "hit lamp transition");
    r.h.nowMs += 200;
    r.paint();
    require (!r.screen.animating (r.h) && !r.paint(), "hit lamp settles");
    for (int step = 0; step < 8; ++step)
    {
        prev = r.frame;
        r.state.step = step;
        ++r.state.hits;
        r.paint();
        require (difference (prev,r.frame) < 3000, "step damage");
        auto partial = r.frame;
        r.paint (true);
        require (r.frame == partial, "step matches whole");
    }
    std::puts ("All seven groups, 12 fixture states, idle, full, stride, bindings, audition, hit decay and step damage passed.");
}

void bench()
{
    using Clock = std::chrono::steady_clock;
    Rig r ("fm-perc");
    r.paint();
    for (const char* mode : {"full", "advance", "idle", "cursor"})
    {
        const bool whole = !std::strcmp (mode, "full"), steps = !std::strcmp (mode, "advance");
        const bool cursor = !std::strcmp (mode, "cursor");
        if (cursor) { r.keyEvent (Key::nudge, 1); r.paint(); }
        int peak = 0, draws = 0;
        Clock::duration elapsed {};
        for (int i = 0; i < 1200; ++i)
        {
            if (steps) { r.state.step = (r.state.step + 1) % 8; ++r.state.hits; r.h.nowMs += 100; }
            if (cursor) r.keyEvent (Key::wheel, i%2 ? -1 : 1);
            const auto prev = r.frame;
            const auto start = Clock::now();
            draws += r.paint (whole);
            elapsed += Clock::now() - start;
            peak = std::max (peak, difference (prev, r.frame));
        }
        std::printf ("%-8s %.6f ms    %d paints    max %d changed pixels\n", mode,
            std::chrono::duration<double,std::milli> (elapsed).count()/1200, draws, peak);
    }
}
} // namespace

int main (int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "--render";
    if (!std::strcmp (mode, "--test")) { test(); return 0; }
    if (!std::strcmp (mode, "--bench")) { bench(); return 0; }
    require (!std::strcmp (mode,"--render"), "mode");
    for (const auto& s : states)
    {
        Rig r (s.patch);
        configure (r,s);
        r.paint (true);
        save (s.name,r.frame);
    }
    Rig r ("fm-perc");
    for (int i = 0; i < 8; ++i)
    {
        r.state.step = i;
        ++r.state.hits;
        r.h.nowMs += 125;
        r.paint();
        char name[16];
        std::snprintf (name,sizeof (name),"step-%d",i+1);
        save (name,r.frame);
    }
}
