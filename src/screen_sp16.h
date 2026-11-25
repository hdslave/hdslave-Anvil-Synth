#pragma once

// Body screen for a host that asks for org.sixteen.screen/1 (sixteen_screen.h): 780 x 285, six parameter
// ids for the host's own encoders. The MPC page (screen.h, three knobs) is a separate object and is not used here.
// The title advances on the host UI clock across all tabs, including while transport is stopped.

#include "screen.h"
#include "sixteen_screen.h"
#include "ui/sp16_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace anvil
{
class ScreenSp16
{
    using Ink = hdui::Ink;

public:
    static constexpr int width = SIXTEEN_SCREEN_WIDTH;
    static constexpr int height = SIXTEEN_SCREEN_HEIGHT;
    // Tab row. The tests press these centres: STEPS (378, 40), VCO (450, 40), MIX (522, 40).
    static constexpr int tabX = 344, tabY = 30, tabW = 68, tabH = 38, tabPitch = 72;
    static constexpr int laneLeft = 68, laneW = 87;
    static constexpr int stepGridBottom = height - 13;
    static constexpr int pitchZero = 159, pitchTravel = 24, velocityBottom = 254, velocityTravel = 33;

    ScreenSp16 (const std::atomic<float>* values, const StepState& state) : values (values), state (state) {}

    bool knobs (uint32_t ids[6], char tabName[SIXTEEN_TAB_NAME_SIZE]) const
    {
        if (tab == Steps)
        {
            ids[0] = ParamId::pitch1 + picked;
            ids[1] = ParamId::velocity1 + picked;
            ids[2] = ParamId::cutoff;
            ids[3] = ParamId::resonance;
            ids[4] = ParamId::vcfDecay;
            ids[5] = ParamId::vcaDecay;
            std::snprintf (tabName, SIXTEEN_TAB_NAME_SIZE, "ANVIL - STEP 0%c", '1' + picked);
        }
        else
        {
            const int which = currentBank ();
            const auto& bank = banks[tab][which];
            std::copy_n (bank.ids, 6, ids);
            if (twoBanks ())
                std::snprintf (tabName, SIXTEEN_TAB_NAME_SIZE, "ANVIL - %s %d/2", tabNames[tab], which + 1);
            else
                std::snprintf (tabName, SIXTEEN_TAB_NAME_SIZE, "ANVIL - %s", tabNames[tab]);
        }
        return true;
    }

    bool turn (int delta)
    {
        if (delta == 0)
            return false;
        if (tab == Steps)
        {
            const int next = std::clamp (picked + delta, 0, 7);
            if (next == picked)
                return false;
            picked = next;
            return true;
        }
        if (tab == Vco || tab == Seq)
        {
            int& bank = tab == Vco ? bankVco : bankSeq;
            const int next = delta > 0 ? 1 : 0;
            if (next == bank)
                return false;
            bank = next;
            return true;
        }
        return false;
    }

    bool push (hdui::Edits& out)
    {
        if (tab == Steps)
        {
            out.press (auditionId + (uint32_t) picked);
            flashBtn = 1;
            flashFrames = 8;
            return true;
        }
        if (tab == Vco || tab == Seq)
        {
            int& bank = tab == Vco ? bankVco : bankSeq;
            bank ^= 1;
            return true;
        }
        return false;
    }

    bool touch (int phase, int x, int y, hdui::Edits& out)
    {
        if (phase == SIXTEEN_TOUCH_UP)
        {
            if (drag == Drag::None)
                return false;
            if (drag == Drag::Parameter)
                moveParameter (x, y, out);
            drag = Drag::None;
            return true;
        }
        if (phase == SIXTEEN_TOUCH_MOVE && drag != Drag::None)
        {
            if (drag == Drag::Parameter)
                moveParameter (x, y, out);
            else
                writeStep (y, out);
            return true;
        }
        if (phase != SIXTEEN_TOUCH_DOWN)
            return false;
        drag = Drag::None;

        if (y >= tabY && y < tabY + tabH && x >= tabX)
        {
            const int slot = (x - tabX) / tabPitch;
            const int within = (x - tabX) % tabPitch;
            if (slot >= 0 && slot < TabCount && within < tabW)
            {
                if (tab == slot && (slot == Vco || slot == Seq))
                    (slot == Vco ? bankVco : bankSeq) ^= 1;
                else
                    tab = slot;
                return true;
            }
        }

        if (tab == Steps && hitButton (x, y, out))
            return true;

        if (tab == Steps && x >= laneLeft && x < laneLeft + 8 * laneW && y >= 108 && y < stepGridBottom)
        {
            const int i = (x - laneLeft) / laneW;
            if (i >= 0 && i < 8)
            {
                picked = i;
                if (y < 214)
                    drag = Drag::Pitch;
                else if (y >= 220)
                    drag = Drag::Vel;
                else
                    return true;
                dragId = (drag == Drag::Pitch ? ParamId::pitch1 : ParamId::velocity1) + i;
                writeStep (y, out);
                return true;
            }
        }

        if (tab != Steps && touchBank (x, y))
            return true;

        if (tab != Steps && x >= 572 && x < 764 && y >= 104 && y < 170)
        {
            const int i = (x - 572) / 24;
            if (i >= 0 && i < 8)
            {
                picked = i;
                return true;
            }
        }
        return false;
    }

    bool paint (uint32_t* pixels, int stride, uint32_t now, bool force)
    {
        if (pixels == nullptr || stride < width)
            return false;
        step = std::clamp (state.step.load (std::memory_order_relaxed), 0, 7);
        running = state.running.load (std::memory_order_relaxed);
        if (! painted)
            motionStart = now;
        const uint64_t tick = (uint64_t) (now - motionStart) / hdui::torus::frameMs;
        const int pose = (int) (tick % hdui::masthead::frames);
        const int glint = hdui::masthead::glintFrame (tick);

        bool moved = false;
        for (unsigned i = 0; i < paramCount; ++i)
        {
            const float v = values[i].load (std::memory_order_relaxed);
            if (! painted || v != sampled[i])
                moved = true;
            sampled[i] = v;
        }
        const int flashWas = flashFrames;
        if (flashFrames > 0)
            --flashFrames;

        const bool drawMast = force || ! painted || pose != lastPose || glint != lastGlint;
        const uint32_t held = drag == Drag::Parameter ? dragId : hdui::noParam;
        const bool drawPage = held != shownHeld || force || ! painted || moved || step != shownStep || running != shownRunning || tab != shownTab
                              || bankVco != shownVco || bankSeq != shownSeq || picked != shownPicked || flashWas > 0;
        if (! drawMast && ! drawPage)
            return false;

        if (drawMast)
            composeMast (pose, glint);
        Canvas canvas {pixels, width, height, stride};
        paintMast (canvas);
        chrome (canvas);
        if (drawPage)
        {
            canvas.box (0, mastHeight + 1, width, height - mastHeight - 1, sp16::base);
            body (canvas);
            shownHeld = held;
            shownStep = step;
            shownRunning = running;
            shownTab = tab;
            shownVco = bankVco;
            shownSeq = bankSeq;
            shownPicked = picked;
        }
        lastPose = pose;
        lastGlint = glint;
        painted = true;
        return true;
    }

private:
    enum Tab { Steps = 0, Vco, Mix, Vcf, Vca, Seq, TabCount };
    enum class Drag { None, Pitch, Vel, Parameter };
    enum class Axis { Pending, Horizontal, Vertical };
    inline static constexpr const char* tabNames[] = { "STEPS", "VCO", "MIX", "VCF", "VCA", "SEQ" };

    using Canvas = sp16::Canvas;

    const std::atomic<float>* values;
    const StepState& state;
    float sampled[paramCount] {};
    static constexpr int mastHeight = 76;
    static constexpr int artWidth = hdui::art::sp16ArtWidth, artHeight = 59; // Row 59 of the MPC tile is its own separator.
    Ink mastTile[160 * (artHeight + 1)];
    static constexpr int titleInset = hdui::art::sp16TitleInset;
    Ink mastArt[artHeight][artWidth + titleInset];
    int tab = Steps, bankVco = 0, bankSeq = 0, picked = 0;
    int step = 0;
    bool running = false, painted = false;
    Drag drag = Drag::None;
    uint32_t dragId = hdui::noParam, shownHeld = hdui::noParam;
    Axis dragAxis = Axis::Pending;
    int dragX = 0, dragY = 0;
    int flashBtn = 0, flashFrames = 0;
    uint32_t motionStart = 0;
    int lastPose = -1, lastGlint = -2;
    int shownStep = -1, shownTab = -1, shownVco = -1, shownSeq = -1, shownPicked = -1;
    bool shownRunning = false;

    float value (uint32_t id) const
    {
        const int index = paramIndex (id);
        return index < 0 ? 0.f : values[index].load (std::memory_order_relaxed);
    }
    TextContext context () const
    {
        TextContext c;
        c.quantize = value (ParamId::quantize);
        return c;
    }
    void textOf (uint32_t id, char* buf, int size) const
    {
        const int index = paramIndex (id);
        if (index < 0)
        {
            std::snprintf (buf, (size_t) size, "-");
            return;
        }
        formatParam (paramTable[index], value (id), buf, (uint32_t) size, context ());
    }
    struct Bank
    {
        const char* heading;
        uint32_t ids[6];
    };
    // In encoder order. Drawing, hit testing and the hardware strip use the same bindings.
    inline static constexpr Bank banks[TabCount][2] = {
        {}, // Steps has a pitch/velocity pair for the selected lane.
        {{"Oscillators", {ParamId::vco1Freq, ParamId::vco1EgAmt, ParamId::vco1Wave, ParamId::vco2Freq, ParamId::vco2EgAmt, ParamId::vco2Wave}},
         {"Modulation & sync", {ParamId::vcoDecay, ParamId::fmAmt, ParamId::egFmAmt, ParamId::seqPitchMod, ParamId::hardSync, hdui::noParam}}},
        {{"Source mixer", {ParamId::vco1Level, ParamId::noiseLevel, ParamId::vco2Level, ParamId::noiseColor, hdui::noParam, hdui::noParam}}},
        {{"Ladder filter", {ParamId::cutoff, ParamId::resonance, ParamId::vcfMode, ParamId::vcfDecay, ParamId::vcfEgAmt, ParamId::noiseVcfMod}}},
        {{"Amplifier & patch", {ParamId::vcaEg, ParamId::vcaDecay, ParamId::volume, ParamId::velCutoff, ParamId::pitchCutoff, ParamId::vcfEgVco2}}},
        {{"Clock & groove", {ParamId::clockDiv, ParamId::length, ParamId::swing, ParamId::direction, ParamId::chance, ParamId::ratchet}},
         {"Scale & note control", {ParamId::quantize, ParamId::noteMode, ParamId::run, hdui::noParam, hdui::noParam, hdui::noParam}}},
    };
    bool twoBanks () const { return tab == Vco || tab == Seq; }
    int currentBank () const { return tab == Vco ? bankVco : (tab == Seq ? bankSeq : 0); }

    struct Rect
    {
        int x, y, w, h;
        bool contains (int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };
    struct BankLayout
    {
        Rect area;
        int count;
        bool compact;

        Rect card (int i) const
        {
            constexpr int gap = 3;
            const int columns = compact ? count : (count + 1) / 2;
            const int col = i % columns, rows = compact ? 1 : 2;
            const int inner = area.w - 12 + gap;
            const int left = col * inner / columns, right = (col + 1) * inner / columns - gap;
            const int h = (area.h - 32 - (rows - 1) * gap) / rows;
            return {area.x + 6 + left, area.y + 27 + (i / columns) * (h + gap), right - left, h};
        }
    };
    BankLayout bankLayout (int which) const
    {
        const auto& bank = banks[tab][which];
        int count = 0;
        while (count < 6 && bank.ids[count] != hdui::noParam) ++count;
        return {{8, 82 + which * 101, 548, twoBanks () ? 96 : 197}, count, twoBanks ()};
    }
    bool touchBank (int x, int y)
    {
        for (int which = 0; which < (twoBanks () ? 2 : 1); ++which)
        {
            const auto layout = bankLayout (which);
            if (! layout.area.contains (x, y)) continue;
            const bool switched = which != currentBank ();
            if (tab == Vco) bankVco = which;
            if (tab == Seq) bankSeq = which;
            for (int i = 0; i < layout.count; ++i)
            {
                if (! layout.card (i).contains (x, y)) continue;
                drag = Drag::Parameter;
                dragId = banks[tab][which].ids[i];
                dragAxis = Axis::Pending;
                dragX = x;
                dragY = y;
                return true;
            }
            return switched;
        }
        return false;
    }

    void moveParameter (int x, int y, hdui::Edits& out)
    {
        x = std::clamp (x, 0, width - 1);
        y = std::clamp (y, 0, height - 1);
        const int dx = x - dragX, up = dragY - y;
        if (dragAxis == Axis::Pending)
        {
            if (std::max (std::abs (dx), std::abs (up)) < 4) return;
            dragAxis = std::abs (dx) > std::abs (up) ? Axis::Horizontal : Axis::Vertical;
        }
        const int travel = dragAxis == Axis::Horizontal ? dx : up;
        const auto& p = paramTable[paramIndex (dragId)];
        const float current = value (dragId);
        // Retain sub-detent travel for stepped controls. Read the current value on each move
        // so a hardware turn during this gesture is included in the next adjustment.
        constexpr int pixelsPerDetent = 12;
        const bool stepped = p.kind != Kind::continuous;
        const float delta = stepped ? (float) (travel / pixelsPerDetent) : travel * (p.max - p.min) / 128.f;
        const float next = clampParam (p, current + delta);
        const bool atLimit = (travel < 0 && next == p.min) || (travel > 0 && next == p.max);
        const int used = stepped && ! atLimit ? (travel / pixelsPerDetent) * pixelsPerDetent : travel;
        if (dragAxis == Axis::Horizontal) dragX += used;
        else dragY -= used;
        if (next != current) out.set (dragId, next);
    }
    static float pitchFromY (int y) { return std::clamp ((pitchZero - (float) y) / pitchTravel, -1.f, 1.f); }
    static float velFromY (int y) { return std::clamp ((velocityBottom - (float) y) / velocityTravel, 0.f, 1.f); }
    void writeStep (int y, hdui::Edits& out) const
    {
        out.set (dragId, drag == Drag::Pitch ? pitchFromY (y) : velFromY (y));
    }
    bool hitButton (int x, int y, hdui::Edits& out)
    {
        if (y < 79 || y >= 104)
            return false;
        if (x >= 536 && x < 648)
        {
            out.press (auditionId + (uint32_t) picked);
            flashBtn = 1;
            flashFrames = 8;
            return true;
        }
        if (x >= 654 && x < 710)
        {
            out.press (ParamId::trigger);
            flashBtn = 2;
            flashFrames = 8;
            return true;
        }
        if (x >= 716 && x < 772)
        {
            out.press (ParamId::advance);
            flashBtn = 3;
            flashFrames = 8;
            return true;
        }
        return false;
    }

    void composeMast (int pose, int glint)
    {
        const float cycle = float (pose % hdui::torus::frames) / hdui::torus::frames;
        const float phase = cycle * hdui::torus::tau;
        const float travel = 2.f * cycle;
        const hdui::print::Drift drift (pose * hdui::torus::tau / hdui::masthead::frames);
        // Compose at the artwork's authored coordinates so the cloth, wire and glint stay registered.
        for (int origin = 0; origin < artWidth;)
        {
            const int tileW = origin == 160 ? 80 : std::min (160, artWidth - origin);
            hdui::Raster r {mastTile, tileW, artHeight + 1};
            std::fill_n (mastTile + artHeight * tileW, tileW, hdui::ink::bg);
            if (origin < hdui::art::sp16TailOrigin)
                hdui::print::underprint (r, origin, drift);
            else
                hdui::print::underprint (r, origin - hdui::art::sp16TailOrigin, drift, hdui::art::sp16ContourTail);
            if (origin == 160)
            {
                hdui::torus::Outlet outlet;
                hdui::torus::draw (r, phase, outlet, drift.inkOffset);
                hdui::masthead::draw (r, origin, phase, travel, drift.inkOffset, &outlet);
                hdui::masthead::drawGlint (r, glint);
            }
            else
                hdui::masthead::draw (r, origin, phase, travel, drift.inkOffset);
            for (int y = 0; y < artHeight; ++y)
                std::copy_n (mastTile + y * tileW, tileW, mastArt[y] + titleInset + origin);
            origin += tileW;
        }
        // The original crop begins one column before its first printed check.
        // Fill that column and the newly exposed left edge with the incoming fold.
        constexpr int leadWidth = int (std::size (hdui::art::sp16RibbonLead));
        static_assert (leadWidth == titleInset + 1);
        hdui::Raster lead {mastTile, leadWidth, artHeight};
        hdui::print::underprint (lead, 0, drift, hdui::art::sp16ContourLead);
        for (int x = 0; x < leadWidth; ++x)
            hdui::masthead::ribbonColumn (lead, x, x - leadWidth, hdui::art::sp16RibbonLead[x],
                                          phase, travel, drift.inkOffset, nullptr);
        for (int y = 0; y < artHeight; ++y)
            std::copy_n (mastTile + y * leadWidth, leadWidth, mastArt[y]);
    }

    void paintMast (Canvas& c) const
    {
        struct Sample { int sx; unsigned fade; };
        static constexpr auto xMap = [] {
            auto ease = [](float t) {
                t = std::clamp (t, 0.f, 1.f);
                return t * t * (3.f - 2.f * t);
            };
            std::array<Sample, width> samples {};
            for (int x = 0; x < width; ++x)
            {
                const int artX = x - titleInset;
                const int sx = titleInset * 256 + artX * (artWidth - 1) * 256 / (width - 1);
                const float fade = .72f * ease ((artX - 239.f) / 178.f) + .12f * ease ((artX - 417.f) / 362.f);
                samples[x] = {sx, (unsigned) (fade * 256.f + .5f)};
            }
            return samples;
        }();
        for (int y = 0; y < mastHeight; ++y)
        {
            const int sy = y * (artHeight - 1) * 256 / (mastHeight - 1);
            const Ink* upper = mastArt[sy >> 8];
            const Ink* lower = mastArt[std::min ((sy >> 8) + 1, artHeight - 1)];
            Ink* row = c.px + y * c.stride;
            for (int x = 0; x < width; ++x)
            {
                const auto [sx, fade] = xMap[x];
                const int left = sx >> 8, right = std::min (left + 1, artWidth + titleInset - 1);
                const Ink top = hdui::print::mix (upper[left], upper[right], sx & 255);
                const Ink bottom = hdui::print::mix (lower[left], lower[right], sx & 255);
                const Ink colour = hdui::print::mix (top, bottom, sy & 255);
                row[x] = hdui::print::mix (colour, sp16::base, fade);
            }
        }
    }

    int length () const { return std::clamp ((int) std::lround (value (ParamId::length)), 1, 8); }
    bool playing () const { return running && value (ParamId::run) >= 0.5f; }

    void chrome (Canvas& c) const
    {
        using namespace sp16;
        char head[32];
        if (tab == Steps)
            std::snprintf (head, sizeof head, "Step %02d", picked + 1);
        else if (tab == Vco || tab == Seq)
            std::snprintf (head, sizeof head, "%s   %d / 2", tabNames[tab], 1 + (tab == Vco ? bankVco : bankSeq));
        else
            std::snprintf (head, sizeof head, "%s", tabNames[tab]);
        c.text (tabX + 3, 7, head, white, Control);

        const bool live = playing ();
        const int len = length ();
        for (int i = 0; i < 8; ++i)
            c.rounded (565 + i * 8, 11, 6, 5, 2, i == step && i < len ? (live ? lime : lilac) : (i < len ? rail : panel));
        char pos[32];
        std::snprintf (pos, sizeof pos, "%02d / %02d", step + 1, len);
        c.text (638, 8, pos, secondary);
        const char* stateText = value (ParamId::run) < 0.5f ? "STOP" : (live ? "RUN" : "ARMED");
        c.rounded (705, 3, 67, 20, 5, live ? lime : tile);
        c.center (705, 8, 67, stateText, live ? base : secondary);

        for (int i = 0; i < TabCount; ++i)
        {
            const int x = tabX + i * tabPitch;
            const bool on = i == tab;
            c.rounded (x, tabY, tabW, tabH, 6, on ? lime : tile);
            c.center (x, tabY + 12, tabW, tabNames[i], on ? base : secondary, Control);
            if (i == Vco || i == Seq)
            {
                const int bank = i == Vco ? bankVco : bankSeq;
                for (int j = 0; j < 2; ++j)
                    c.rounded (x + 25 + j * 10, tabY + 30, 8, 2, 1,
                               j == bank ? (on ? base : violet) : (on ? 0xff92b324 : rail));
            }
        }
        c.box (0, mastHeight, width, 1, rule);
    }

    static const char* panelName (const Param& p)
    {
        switch (p.id)
        {
            case ParamId::vco1Freq: return "Osc 1 freq";
            case ParamId::vco1EgAmt: return "Osc 1 EG";
            case ParamId::vco1Wave: return "Osc 1 wave";
            case ParamId::vco2Freq: return "Osc 2 freq";
            case ParamId::vco2EgAmt: return "Osc 2 EG";
            case ParamId::vco2Wave: return "Osc 2 wave";
            case ParamId::fmAmt: return "FM amount";
            case ParamId::egFmAmt: return "EG to FM";
            case ParamId::seqPitchMod: return "Pitch mod";
            case ParamId::noiseVcfMod: return "Noise to filter";
            default: return p.name;
        }
    }

    static void dial (Canvas& c, int cx, int cy, float u, bool bipolar)
    {
        using namespace sp16;
        struct Point { float x, y; };
        constexpr int segments = 32;
        static const auto arc = [] {
            std::array<Point, segments + 1> points {};
            for (int i = 0; i <= segments; ++i)
            {
                const float a = (0.375f + 0.75f * i / segments) * hdui::torus::tau;
                points[i] = {std::cos (a), std::sin (a)};
            }
            return points;
        }();
        const int stop = (int) std::lround (u * segments), start = bipolar ? segments / 2 : 0;
        for (int i = 0; i < segments; ++i)
        {
            const Ink colour = i >= std::min (start, stop) && i < std::max (start, stop) ? lilac : rail;
            c.line (cx + 14 * arc[i].x, cy + 14 * arc[i].y, cx + 14 * arc[i + 1].x, cy + 14 * arc[i + 1].y, 2.5f, colour);
        }
        const auto at = arc[stop];
        c.line (cx + 4 * at.x, cy + 4 * at.y, cx + 10 * at.x, cy + 10 * at.y, 2.5f, white);
    }

    static void choices (Canvas& c, int x, int y, int w, int count, int selected, Ink accent)
    {
        for (int i = 0; i < count; ++i)
        {
            const int left = i * w / count;
            c.rounded (x + left, y, (i + 1) * w / count - left - 2, 5, 2, i == selected ? accent : sp16::rail);
        }
    }

    void card (Canvas& c, int x, int y, int w, int h, int knob, uint32_t id, bool active, bool compact) const
    {
        using namespace sp16;
        const auto& p = paramTable[paramIndex (id)];
        const float v = value (id);
        const float u = std::clamp ((v - p.min) / (p.max - p.min), 0.f, 1.f);
        const bool bipolar = p.min < 0.f;
        char readout[32], key[3] = { 'K', (char) ('1' + knob), 0 };
        formatParam (p, v, readout, sizeof readout, context ());
        const bool held = drag == Drag::Parameter && dragId == id;
        c.rounded (x, y, w, h, 5, held ? selection : (active ? tile : panel));
        c.text (x + 8, y + 6, panelName (p), active ? secondary : muted, compact ? Label : Control);
        if (compact)
        {
            const auto size = Canvas::measure (readout, Control) <= w - 14 ? Control : Label;
            c.text (x + 8, y + 24, readout, active ? white : secondary, size);
            c.text (x + 8, y + h - 15, key, active ? lilac : muted);
            const int left = x + 30, span = w - 38, mid = bipolar ? span / 2 : 0;
            if (p.kind == Kind::choice)
            {
                choices (c, left, y + h - 11, span, (int) p.max + 1, (int) std::lround (v), active ? lilac : secondary);
                return;
            }
            const int at = (int) std::lround (u * (span - 1));
            c.rounded (left, y + h - 10, span, 3, 1, rail);
            c.box (left + std::min (at, mid), y + h - 10, std::abs (at - mid), 3, active ? violet : muted);
            c.rounded (left + at - 1, y + h - 12, 3, 7, 1, active ? lilac : secondary);
        }
        else
        {
            c.right (x + w - 9, y + 7, key, lilac);
            c.text (x + 9, y + h - 33, readout, white, Value);
            if (p.kind == Kind::choice)
                choices (c, x + w - 41, y + h - 25, 32, (int) p.max + 1, (int) std::lround (v), lilac);
            else
                dial (c, x + w - 24, y + h - 23, u, bipolar);
        }
    }

    void bankBox (Canvas& c, int which) const
    {
        using namespace sp16;
        const auto& bank = banks[tab][which];
        const auto layout = bankLayout (which);
        const auto [x, y, w, h] = layout.area;
        const bool active = which == currentBank ();
        c.rounded (x, y, w, h, 7, active ? panel : shade);
        c.rounded (x + 8, y + 8, 3, 12, 1, active ? lime : violet);
        c.text (x + 18, y + 7, bank.heading, active ? white : secondary, Control);
        if (layout.compact)
            c.right (x + w - 9, y + 9, active ? "EDITING" : "TAP TO EDIT", active ? lime : muted);
        for (int i = 0; i < layout.count; ++i)
        {
            const auto r = layout.card (i);
            card (c, r.x, r.y, r.w, r.h, i, bank.ids[i], active, layout.compact);
        }
    }

    void mini (Canvas& c) const
    {
        using namespace sp16;
        constexpr int x = 564, y = 82, w = 208;
        const bool detailed = tab != Vco && tab != Mix;
        c.rounded (x, y, w, detailed ? 197 : 96, 7, panel);
        c.text (x + 9, y + 8, "Sequence", white, Control);
        char clock[24];
        textOf (ParamId::clockDiv, clock, sizeof clock);
        c.right (x + w - 10, y + 10, clock, lilac);
        const int len = length (), q = (int) std::lround (value (ParamId::quantize));
        for (int i = 0; i < 8; ++i)
        {
            const int lx = x + 8 + i * 24;
            const bool live = playing () && i < len && i == step;
            const Ink col = live ? lime : (i < len ? lilac : muted);
            c.rounded (lx + 1, y + 28, 21, 59, 3, i == picked ? sp16::selection : shade);
            const float pitch = quantizedSemitones (value (ParamId::pitch1 + i), q);
            const int py = y + 49 - (int) std::lround (pitch * 15.f / 60.f);
            c.box (lx + 4, y + 49, 15, 1, rail);
            c.rounded (lx + 4, py - 1, 15, 3, 1, col);
            const int vel = (int) std::lround (value (ParamId::velocity1 + i) * 15);
            c.box (lx + 8, y + 83 - vel, 7, vel, i < len ? (live ? lime : violet) : rail);
            if (i == picked || live)
                c.box (lx + 4, y + 28, 15, 2, live ? lime : lilac);
        }
        if (detailed)
        {
            c.box (x + 9, y + 99, w - 18, 1, rule);
            char selected[24], pitch[24], vel[16];
            std::snprintf (selected, sizeof selected, "Step %02d", picked + 1);
            textOf (ParamId::pitch1 + picked, pitch, sizeof pitch);
            textOf (ParamId::velocity1 + picked, vel, sizeof vel);
            c.text (x + 10, y + 112, selected, white, Control);
            c.text (x + 10, y + 142, "Pitch", muted);
            c.text (x + 112, y + 142, "Velocity", muted);
            c.text (x + 10, y + 161, pitch, lilac, Value);
            c.text (x + 112, y + 161, vel, white, Value);
        }
    }

    void steps (Canvas& c) const
    {
        using namespace sp16;
        char clock[24], dir[24], swing[24], quant[24], info[160];
        textOf (ParamId::clockDiv, clock, sizeof clock);
        textOf (ParamId::direction, dir, sizeof dir);
        textOf (ParamId::swing, swing, sizeof swing);
        textOf (ParamId::quantize, quant, sizeof quant);
        const int len = length ();
        std::snprintf (info, sizeof info, "%s    %s     /     Length %d     /     Swing %s     /     Scale %s", clock, dir, len, swing, quant);
        c.text (8, 86, info, secondary);
        char aud[24];
        std::snprintf (aud, sizeof aud, "Audition %02d", picked + 1);
        button (c, 536, 112, aud, flashFrames > 0 && flashBtn == 1);
        button (c, 654, 56, "Trig", flashFrames > 0 && flashBtn == 2);
        button (c, 716, 56, "Adv", flashFrames > 0 && flashBtn == 3);

        c.rounded (7, 108, 765, stepGridBottom - 108, 7, panel);
        c.text (15, 155, "Pitch", secondary);
        c.text (15, 232, "Velocity", secondary);
        const int q = (int) std::lround (value (ParamId::quantize));
        for (int i = 0; i < 8; ++i)
        {
            const int x = laneLeft + i * laneW, cx = x + laneW / 2;
            const bool selected = picked == i, enabled = i < len;
            const bool live = playing () && enabled && step == i;
            const Ink surface = selected ? sp16::selection : (enabled ? tile : shade);
            const Ink signal = live ? lime : (enabled ? lilac : muted);
            const Ink valueInk = enabled || selected ? white : muted;
            c.rounded (x + 2, 111, laneW - 4, stepGridBottom - 3 - 111, 5, surface);
            char number[] = { '0', (char) ('1' + i), 0 };
            if (selected || live)
                c.rounded (cx - 18, 113, 36, 17, 4, live ? lime : lilac);
            c.center (x, 117, laneW, number, selected || live ? base : secondary);
            for (int gy = pitchZero - pitchTravel; gy <= pitchZero + pitchTravel; gy += pitchTravel)
                c.box (x + 10, gy, laneW - 20, 1, gy == pitchZero ? rail : rule);
            const float semis = quantizedSemitones (value (ParamId::pitch1 + i), q);
            const int py = pitchZero - (int) std::lround (semis * pitchTravel / 60.f);
            c.box (cx - 10, std::min (pitchZero, py), 20, std::abs (py - pitchZero), selected ? violet : rail);
            c.rounded (cx - 15, py - 2, 30, 4, 2, signal);
            char pitch[24];
            textOf (ParamId::pitch1 + i, pitch, sizeof pitch);
            c.center (x, 192, laneW, pitch, valueInk, Control);
            c.box (x + 7, 212, laneW - 14, 1, rule);
            const float vel = std::clamp (value (ParamId::velocity1 + i), 0.f, 1.f);
            const int vh = (int) std::lround (vel * velocityTravel);
            c.rounded (cx - 15, velocityBottom - velocityTravel, 30, velocityTravel, 3, shade);
            if (vh > 0)
            {
                c.rounded (cx - 15, velocityBottom - vh, 30, vh, 3, live ? lime : (enabled ? violet : rail));
                c.box (cx - 11, velocityBottom - vh, 22, std::min (vh, 2), signal);
            }
            char pct[8];
            std::snprintf (pct, sizeof pct, "%d%%", (int) std::lround (vel * 100));
            c.center (x, velocityBottom + 3, laneW, pct, selected ? white : secondary);
        }

        // The footer spans the host's six encoder columns, independently of the eight step lanes.
        auto group = [&](int first, int end, const char* label, Ink accent) {
            constexpr int column = width / SIXTEEN_KNOB_COUNT;
            constexpr int y = stepGridBottom + 2;
            const int left = first * column + 7, right = end * column - 8;
            c.box (left, y + 2, 3, 5, accent);
            c.text (left + 8, y, label, secondary);
            const int line = left + 8 + Canvas::measure (label, Label) + 6;
            c.box (line, y + 4, right - line, 1, rule);
        };
        group (0, 2, "SEQ", lilac);
        group (2, SIXTEEN_KNOB_COUNT, "PATCH", lime);
    }

    static void button (Canvas& c, int x, int w, const char* label, bool lit)
    {
        using namespace sp16;
        c.rounded (x, 79, w, 25, 5, lit ? lime : tile);
        c.center (x, 86, w, label, lit ? base : white);
    }

    void waves (Canvas& c) const
    {
        using namespace sp16;
        constexpr int x = 564, y = 183;
        c.rounded (x, y, 208, 96, 7, panel);
        c.box (x + 103, y + 10, 1, 76, rule);
        for (int i = 0; i < 2; ++i)
        {
            const int lx = x + 11 + i * 100;
            const bool tri = value (i == 0 ? ParamId::vco1Wave : ParamId::vco2Wave) >= 0.5f;
            const Ink col = i == 1 && value (ParamId::fmAmt) > 0.05f ? lime : lilac;
            c.text (lx, y + 9, i == 0 ? "VCO 1" : "VCO 2", secondary);
            if (i == 1 && value (ParamId::hardSync) >= 0.5f)
                c.right (x + 199, y + 9, "SYNC", lime);
            c.box (lx, y + 45, 84, 1, rule);
            const int high = y + 32, low = y + 58;
            for (int k = 0; k < 3; ++k)
            {
                const int start = lx + 28 * k;
                if (tri)
                {
                    c.line (start, low, start + 14, high, 1.8f, col);
                    c.line (start + 14, high, start + 28, low, 1.8f, col);
                }
                else
                {
                    c.box (start, high, 14, 2, col);
                    c.box (start + 13, high, 2, low - high + 2, col);
                    c.box (start + 14, low, 14, 2, col);
                    if (k < 2) c.box (start + 27, high, 2, low - high + 2, col);
                }
            }
            char hz[24];
            textOf (i == 0 ? ParamId::vco1Freq : ParamId::vco2Freq, hz, sizeof hz);
            c.text (lx, y + 72, hz, white, Control);
        }
    }

    void meters (Canvas& c) const
    {
        using namespace sp16;
        constexpr const char* names[] = { "VCO 1", "Noise", "VCO 2" };
        constexpr uint32_t ids[] = { ParamId::vco1Level, ParamId::noiseLevel, ParamId::vco2Level };
        constexpr Ink colours[] = { lilac, lime, secondary };
        c.rounded (564, 183, 208, 96, 7, panel);
        c.text (574, 192, "Levels", white, Control);
        for (int i = 0; i < 3; ++i)
        {
            const int y = 216 + i * 20;
            c.text (574, y, names[i], secondary);
            const int lit = (int) std::lround (std::clamp (value (ids[i]), 0.f, 1.f) * 19.f);
            for (int n = 0; n < 19; ++n)
                c.rounded (630 + n * 7, y + 2, 5, 7, 1, n < lit ? colours[i] : rail);
        }
    }

    void body (Canvas& c) const
    {
        if (tab == Steps)
        {
            steps (c);
            return;
        }
        mini (c);
        bankBox (c, 0);
        if (twoBanks ()) bankBox (c, 1);
        if (tab == Vco) waves (c);
        if (tab == Mix)
            meters (c);
    }
};
} // namespace anvil
