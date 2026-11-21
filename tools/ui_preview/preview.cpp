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

int movingWirePixels (const Frame& a, const Frame& b)
{
    int moved = 0;
    // The upper-right arc of the mesh stays above the passing cloth.
    for (int y = 3; y < 30; ++y)
        for (int x = 218; x < 240; ++x)
        {
            const int at = y * hdui::width + x;
            // A wire crossing into or out of the darker contour field.
            const auto threshold = hdui::art::inks[2] & 255;
            moved += ((a[at] & 255) > threshold) != ((b[at] & 255) > threshold);
        }
    return moved;
}

int movingChecks (const Frame& a, const Frame& b, int left, int top, int right, int bottom)
{
    int moved = 0;
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x)
        {
            const int at = y*hdui::width + x;
            moved += (((a[at] >> 16) & 255) > 150) != (((b[at] >> 16) & 255) > 150);
        }
    return moved;
}

void testCheckerRegistration()
{
    // On a 30-pixel-wide swatch both internal row seams land exactly on a
    // pixel centre. The seams stay half silver as the checks travel past.
    hdui::art::RibbonSpan swatch {};
    swatch.top = 6;
    swatch.end = 37;
    swatch.edgeTop16 = 6*16;
    swatch.edgeBottom16 = 36*16;
    swatch.du256 = 26;
    swatch.slope128 = 40;
    std::array<hdui::Ink,60> column, start {};
    hdui::Raster raster {column.data(),1,60};
    for (int i = 0; i <= 32; ++i)
    {
        column.fill (hdui::ink::bg);
        hdui::masthead::ribbonColumn (raster, 0, 17, swatch, 0.f, i/16.f, 0.f, nullptr);
        if (!i) start = column;
        for (int y : {16,26})
            require (column[y+3] == start[y+3], "moving checks stay registered to both row seams");
        if (i == 16)
            for (int y : {11,21,31})
                require (std::abs (int (column[y+3] & 255) - int (start[y+3] & 255)) > 80,
                         "one check of travel exchanges ink inside every row");
    }

    // At the exit the cloth physically banks. Both hems draw inward around
    // the same centre and each of the three rows still carries its own ink.
    swatch.u256 = 256;
    swatch.slope128 = 0;
    for (int pose = 0; pose < hdui::torus::frames; ++pose)
    {
        const float phase = pose*hdui::torus::tau/hdui::torus::frames;
        column.fill (hdui::ink::bg);
        hdui::masthead::ribbonColumn (raster, 0, 249, swatch, phase, 0.f, 0.f, nullptr);
        require (column[7+3] == hdui::ink::bg && column[35+3] == hdui::ink::bg,
                 "both hems turn inward at the bank");
        for (int y : {14,28})
            require (int (column[y+3] & 255) - int (column[21+3] & 255) > 80,
                     "three checker rows survive the roll");
    }
}

void testClothFlow()
{
    // Hold the fabric pose fixed and follow the leading edge of a light check
    // at the entry, just before the aperture, and near the screen edge.
    std::array<hdui::Ink,hdui::width*60> header;
    hdui::Raster raster {header.data(),hdui::width,60};
    struct Edge { int y, start, first = 0, last = 0; };
    Edge edges[] = {{13,11}, {31,195}, {46,306}};
    auto light = [&](int x, int y) {
        const auto c = header[y*hdui::width+x];
        return (2*((c>>16)&255) + 3*((c>>8)&255) + (c&255)) > 6*127;
    };
    for (int i = 0; i <= 8; ++i)
    {
        header.fill (hdui::ink::bg);
        hdui::masthead::draw (raster, 0, 0.f, i / 8.f, 0.f);
        for (auto& edge : edges)
        {
            const int begin = i ? edge.last : edge.start - 1;
            const int end = begin + (i ? 3 : 6);
            int x = begin;
            while (x < end && (!light (x, edge.y) || light (x-1, edge.y))) ++x;
            require (x < end, "light check edge advances continuously");
            if (!i) edge.first = x;
            edge.last = x;
        }
    }
    for (const auto& edge : edges)
        require (edge.last - edge.first >= 5, "print travels along each fold");

    // Inspect the bare ribbon so neither text nor mesh can bridge a gap.
    for (int pose = 0; pose < hdui::torus::frames; ++pose)
    {
        header.fill (hdui::ink::bg);
        const float cycle = float (pose) / hdui::torus::frames;
        hdui::masthead::draw (raster, 0, cycle*hdui::torus::tau, 2.f*cycle, 0.f);
        for (int x = 205; x < hdui::width; ++x)
        {
            int overlap = 0;
            for (int y = 0; y < 59; ++y)
            {
                const int at = y*hdui::width+x;
                if (header[at] == hdui::ink::bg) continue;
                require (x < 240 || y > 30, "cloth clears status readouts");
                if (header[at-1] != hdui::ink::bg) ++overlap;
            }
            require (overlap >= 5, "ribbon is continuous to pixel 319");
        }
    }

    std::array<hdui::Ink,80*60> ring;
    ring.fill (hdui::ink::bg);
    hdui::Raster right {ring.data(),80,60};
    hdui::torus::Outlet outlet;
    constexpr float phase = hdui::torus::tau * .25f;
    hdui::torus::draw (right, phase, outlet, 0.f);
    const auto bare = ring;
    hdui::masthead::draw (right, 160, phase, .5f, 0.f);
    const auto noFrontWires = ring;
    ring = bare;
    hdui::masthead::draw (right, 160, phase, .5f, 0.f, &outlet);
    int crossings = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 80; ++x)
            if (ring[y*80+x] != noFrontWires[y*80+x])
            {
                require (x >= hdui::torus::Outlet::first && x < hdui::torus::Outlet::end &&
                         outlet.wire[y][x-hdui::torus::Outlet::first] != 0, "only nearer strands cross the tail");
                ++crossings;
            }
    require (crossings > 12, "visible threading at the near rim");
}

void testTipGlint()
{
    // With just the lettering on the page, the old flare and its long left
    // ray must be absent even when the torus would have run behind them.
    Frame ink;
    ink.fill (hdui::ink::bg);
    hdui::Raster bare {ink.data(),320,60};
    hdui::masthead::draw (bare, 0, 0.f, 0.f, 0.f);
    require (ink[15*320+156] == hdui::ink::bg && ink[15*320+181] == hdui::ink::bg,
             "no printed flare at rest");
    require (ink[17*320+174] != hdui::ink::bg, "the chrome serif survives at rest");

    for (bool playing : {false, true})
    {
        Rig r ("fm-perc");
        r.h.playing = r.state.running = playing;
        r.paint();
        r.h.nowMs += 12360;
        r.paint();
        const Frame quiet = r.frame;
        // Same pose of the cloth, mesh and contour print, with a glint due
        // only on the middle pass. A skipped clock interval must still repaint.
        r.h.nowMs += 12800;
        require (r.paint(), "rare glint appears on a repeated ribbon pose");
        const Frame peak = r.frame;
        const int lit = difference (quiet, peak);
        require (lit >= 18 && lit < 75, "compact glint on the tip");
        for (int y = 0; y < hdui::height; ++y)
            for (int x = 0; x < hdui::width; ++x)
                if (peak[y*320+x] != quiet[y*320+x])
                    require (std::abs (x-175) <= 8 && std::abs (y-17) <= 7, "glint stays on the serif");
        r.paint (true);
        require (r.frame == peak, "full redraw preserves the glint age");
        r.h.nowMs += 12800;
        require (r.paint() && r.frame == quiet, "glint clears completely on a repeated pose");
    }
}

void test()
{
    testCheckerRegistration();
    testClothFlow();
    testTipGlint();
    for (const auto& spec : states)
    {
        Rig r (spec.patch);
        configure (r, spec);
        r.paint();
        std::array<uint32_t,323*218> pad;
        pad.fill (0x12345678);
        require (!r.screen.paint (r.h, pad.data(), 323, false), "same-pose paint");
        require (std::all_of (pad.begin(), pad.end(), [](auto c) {return c == 0x12345678;}), "same pose touched canvas");
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
    const Frame opening = r.frame;
    auto prev = opening;
    for (int i = 1; i <= hdui::masthead::frames; ++i)
    {
        r.h.nowMs += hdui::torus::frameMs;
        if (i % 2 == 0) ++r.state.hits;
        require (r.paint(), "wire moves on each frame");
        const int moved = movingWirePixels (prev, r.frame);
        require (moved > 0 && moved < 500, "continuous wire displacement including loop seam");
        require (std::equal (opening.begin() + 60*320, opening.end(), r.frame.begin() + 60*320), "header-only motion");
        for (int y = 0; y < 55; ++y)
            for (int x = 0; x < 238; ++x)
            {
                const auto pair = hdui::art::masthead[y][x/2];
                const int pigment = x % 2 ? pair & 15 : pair >> 4;
                if (pigment >= 11)
                    require (r.frame[(y+3)*320+x+1] == hdui::art::inks[pigment], "stationary letter faces");
            }
        // Exposed paper above the lettering isolates the slow plate from the
        // moving cloth and mesh. Check the wrap as well as the mid-cycle pose.
        int contoursChanged = 0;
        for (int y = 0; y < 7; ++y)
            for (int x = 78; x < 142; ++x)
            {
                const int at = y*320+x;
                const int blue = r.frame[at] & 255;
                require (std::abs (blue - int (prev[at] & 255)) <= 8, "continuous contour drift");
                contoursChanged += std::abs (blue - int (opening[at] & 255)) > 8;
            }
        if (i == hdui::torus::frames)
            require (contoursChanged > 30, "contours keep drifting on the second torus turn");
        if (i == hdui::torus::frames / 4)
        {
            require (movingWirePixels (opening, r.frame) > 100, "torus changes shape");
            require (movingChecks (opening, r.frame, 1,3,34,26) > 80, "left ribbon billows");
            require (movingChecks (opening, r.frame, 90,37,102,52) > 10, "fold between letters moves");
            require (movingChecks (opening, r.frame, 107,47,148,58) > 30, "lower ribbon moves");
            require (movingChecks (opening, r.frame, 183,23,216,43) > 50, "ribbon waves through the ring");
        }
        prev = r.frame;
        r.paint (true);
        require (r.frame == prev, "masthead partial matches whole");
    }
    require (opening == r.frame, "same pose after the entire print cycle");
    r.h.playing = false;
    r.state.running = false;
    r.paint();
    prev = r.frame;
    r.h.nowMs += 160;
    require (r.paint() && movingWirePixels (prev, r.frame) > 0, "torus turns while stopped");
    require (movingChecks (prev, r.frame, 183,23,216,43) > 0, "ribbon follows while stopped");
    r.h.playing = true;
    r.state.running = true;
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
    std::puts ("All seven groups, 12 fixture states, same pose, full, stride, bindings, audition, checker drift, rim crossings, rare tip glint, ribbon, torus and contour motion, loop seam, stopped motion and step damage passed.");
}

void bench()
{
    using Clock = std::chrono::steady_clock;
    Rig r ("fm-perc");
    r.paint();
    for (const char* mode : {"full", "advance", "motion", "same", "cursor"})
    {
        const bool whole = !std::strcmp (mode, "full"), steps = !std::strcmp (mode, "advance");
        const bool cursor = !std::strcmp (mode, "cursor");
        const bool motion = !std::strcmp (mode, "motion");
        if (cursor) { r.keyEvent (Key::nudge, 1); r.paint(); }
        int peak = 0, draws = 0;
        Clock::duration elapsed {};
        for (int i = 0; i < 1200; ++i)
        {
            if (steps) { r.state.step = (r.state.step + 1) % 8; ++r.state.hits; r.h.nowMs += 100; }
            if (motion) { if (i % 5 == 0) ++r.state.hits; r.h.nowMs += 40; }
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
    // Two turns of the torus cover one full orbit of the contour plate.
    for (int i = 0; i < hdui::masthead::frames; ++i)
    {
        r.state.step = i / 5 % 8;
        if (i % 5 == 0) ++r.state.hits;
        r.paint();
        char name[16];
        std::snprintf (name, sizeof (name), "motion-%02d", i);
        save (name,r.frame);
        r.h.nowMs += 40;
    }
}
