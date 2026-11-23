#pragma once

// Body screen for a host that asks for org.sixteen.screen/1 (sixteen_screen.h): 780 x 285, six parameter
// ids for the host's own encoders. The MPC page (screen.h, three knobs) is a separate object and is not used here.
// The sound is not touched. Animate while the sequence runs, and for 4 s after a hit or a touch; then hold still.

#include "screen.h"
#include "sixteen_screen.h"

#include <algorithm>
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
    static constexpr int tabX = 344, tabY = 27, tabW = 68, tabH = 27, tabPitch = 72;
    static constexpr int laneLeft = 68, laneW = 87;

    ScreenSp16 (const std::atomic<float>* values, const StepState& state) : values (values), state (state) {}

    bool knobs (uint32_t ids[6], char tabName[SIXTEEN_TAB_NAME_SIZE]) const
    {
        uint32_t active[6];
        char title[SIXTEEN_TAB_NAME_SIZE];
        activeBank (active, title);
        for (int i = 0; i < 6; ++i)
            ids[i] = active[i];
        std::snprintf (tabName, SIXTEEN_TAB_NAME_SIZE, "%s", title);
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
            touched = true;
            return true;
        }
        if (tab == Vco || tab == Seq)
        {
            int& bank = tab == Vco ? bankVco : bankSeq;
            const int next = delta > 0 ? 1 : 0;
            if (next == bank)
                return false;
            bank = next;
            touched = true;
            return true;
        }
        return false;
    }

    bool push (hdui::Edits& out)
    {
        touched = true;
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
            drag = Drag::None;
            return true;
        }
        if (phase == SIXTEEN_TOUCH_MOVE && drag != Drag::None)
        {
            touched = true;
            writeDrag (y, out);
            return true;
        }
        if (phase != SIXTEEN_TOUCH_DOWN)
            return false;

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
                touched = true;
                return true;
            }
        }

        if (tab == Steps && hitButton (x, y, out))
        {
            touched = true;
            return true;
        }

        if (tab == Steps && x >= laneLeft && x < laneLeft + 8 * laneW && y >= 108 && y < 278)
        {
            const int i = (x - laneLeft) / laneW;
            if (i >= 0 && i < 8)
            {
                picked = i;
                touched = true;
                if (y < 214)
                    drag = Drag::Pitch;
                else if (y >= 220)
                    drag = Drag::Vel;
                else
                    return true;
                dragStep = i;
                writeDrag (y, out);
                return true;
            }
        }

        if ((tab == Vco || tab == Seq) && x >= 8 && x < 556)
        {
            if (y >= 82 && y < 178)
                return setBank (0);
            if (y >= 183 && y < 279)
                return setBank (1);
        }

        if (tab != Steps && x >= 572 && x < 764 && y >= 104 && y < 168)
        {
            const int i = (x - 572) / 24;
            if (i >= 0 && i < 8)
            {
                picked = i;
                touched = true;
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
        const auto hits = state.hits.load (std::memory_order_acquire);
        if (hits != seenHits)
        {
            seenHits = hits;
            touched = true;
        }
        if (! painted || touched || running)
            busyMs = now;
        const bool animate = running || (uint32_t) (now - busyMs) < 4000;
        touched = false;
        if (! painted)
            motionStart = now;
        const uint64_t tick = (uint64_t) (now - motionStart) / hdui::torus::frameMs;
        const int pose = (int) (tick % hdui::masthead::frames);
        const int glint = animate ? hdui::masthead::glintFrame (tick) : lastGlint;

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

        const bool drawMast = force || ! painted || (animate && (pose != lastPose || glint != lastGlint));
        const bool drawPage = force || ! painted || moved || step != shownStep || running != shownRunning || tab != shownTab
                              || bankVco != shownVco || bankSeq != shownSeq || picked != shownPicked || flashWas > 0;
        if (! drawMast && ! drawPage)
            return false;

        if (force || ! painted)
            for (int y = 0; y < height; ++y)
                std::fill (pixels + y * stride, pixels + y * stride + width, hdui::ink::bg);

        bool drew = false;
        if (drawMast)
            drew |= mast (pixels, stride, pose, glint, force || ! painted);
        if (drawPage)
        {
            Canvas canvas { pixels, width, height, stride };
            chrome (canvas);
            canvas.box (0, 77, width, height - 77, hdui::ink::bg);
            body (canvas);
            drew = true;
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
        return drew;
    }

private:
    enum Tab { Steps = 0, Vco, Mix, Vcf, Vca, Seq, TabCount };
    enum class Drag { None, Pitch, Vel };

    struct Canvas
    {
        Ink* px;
        int w, h, stride;

        void dot (int x, int y, Ink c) const
        {
            if ((unsigned) x < (unsigned) w && (unsigned) y < (unsigned) h)
                px[y * stride + x] = c;
        }
        void box (int x, int y, int wide, int high, Ink c) const
        {
            if (wide <= 0 || high <= 0)
                return;
            const int x0 = std::max (0, x), x1 = std::min (w, x + wide);
            const int y0 = std::max (0, y), y1 = std::min (h, y + high);
            if (x1 <= x0)
                return;
            for (int j = y0; j < y1; ++j)
                std::fill (px + j * stride + x0, px + j * stride + x1, c);
        }
        void frame (int x, int y, int wide, int high, Ink c) const
        {
            box (x, y, wide, 1, c);
            box (x, y + high - 1, wide, 1, c);
            box (x, y, 1, high, c);
            box (x + wide - 1, y, 1, high, c);
        }
        void text (int x, int y, const char* s, Ink color) const
        {
            for (; *s != 0; ++s, x += hdui::font::glyphWidth)
            {
                const unsigned ch = (unsigned char) *s;
                const auto& glyph = hdui::font::glyphs[(ch >= 32 && ch <= 127 ? ch : '?') - 32];
                for (int j = 0; j < hdui::font::glyphHeight; ++j)
                    for (int bits = glyph[j], k = 0; bits != 0; bits >>= 1, ++k)
                        if ((bits & 1) != 0)
                            dot (x + k, y + j, color);
            }
        }
        void right (int end, int y, const char* s, Ink c) const
        {
            text (end - hdui::font::glyphWidth * (int) std::strlen (s), y, s, c);
        }
        void center (int x, int y, int wide, const char* s, Ink c) const
        {
            text (x + (wide - hdui::font::glyphWidth * (int) std::strlen (s)) / 2, y, s, c);
        }
    };

    const std::atomic<float>* values;
    const StepState& state;
    float sampled[paramCount] {};
    hdui::Picture masthead;
    int tab = Steps, bankVco = 0, bankSeq = 0, picked = 0;
    int step = 0;
    bool running = false, touched = true, painted = false;
    Drag drag = Drag::None;
    int dragStep = -1;
    int flashBtn = 0, flashFrames = 0;
    uint32_t seenHits = 0, busyMs = 0, motionStart = 0;
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
    static const char* shortName (uint32_t id)
    {
        if (id >= ParamId::pitch1 && id <= ParamId::pitch8)
            return "PITCH";
        if (id >= ParamId::velocity1 && id <= ParamId::velocity8)
            return "VEL";
        switch (id)
        {
            case ParamId::cutoff: return "CUTOFF";
            case ParamId::resonance: return "RESON";
            case ParamId::vcfDecay: return "VCF DEC";
            case ParamId::vcaDecay: return "VCA DEC";
            case ParamId::vco1Freq: return "VCO1 FRQ";
            case ParamId::vco1EgAmt: return "VCO1 EG";
            case ParamId::vco1Wave: return "VCO1 WAV";
            case ParamId::vco2Freq: return "VCO2 FRQ";
            case ParamId::vco2EgAmt: return "VCO2 EG";
            case ParamId::vco2Wave: return "VCO2 WAV";
            case ParamId::vcoDecay: return "VCO DEC";
            case ParamId::fmAmt: return "FM AMT";
            case ParamId::egFmAmt: return "EG>FM";
            case ParamId::seqPitchMod: return "SEQ PIT";
            case ParamId::hardSync: return "SYNC";
            case ParamId::vco1Level: return "VCO1 LVL";
            case ParamId::noiseLevel: return "NOISE";
            case ParamId::vco2Level: return "VCO2 LVL";
            case ParamId::noiseColor: return "COLOR";
            case ParamId::vcfMode: return "MODE";
            case ParamId::vcfEgAmt: return "VCF EG";
            case ParamId::noiseVcfMod: return "NSE>VCF";
            case ParamId::vcaEg: return "VCA EG";
            case ParamId::volume: return "VOLUME";
            case ParamId::velCutoff: return "VEL>CUT";
            case ParamId::pitchCutoff: return "PIT>CUT";
            case ParamId::vcfEgVco2: return "EG>VCO2";
            case ParamId::clockDiv: return "CLOCK";
            case ParamId::length: return "LENGTH";
            case ParamId::swing: return "SWING";
            case ParamId::direction: return "DIRECT";
            case ParamId::chance: return "CHANCE";
            case ParamId::ratchet: return "RATCHET";
            case ParamId::quantize: return "QUANT";
            case ParamId::noteMode: return "NOTES";
            case ParamId::run: return "RUN";
            default: return "";
        }
    }
    static void bank6 (uint32_t out[6], uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
    {
        const uint32_t ids[6] = { a, b, c, d, e, f };
        for (int i = 0; i < 6; ++i)
            out[i] = ids[i];
    }
    void activeBank (uint32_t ids[6], char title[SIXTEEN_TAB_NAME_SIZE]) const
    {
        bank6 (ids, hdui::noParam, hdui::noParam, hdui::noParam, hdui::noParam, hdui::noParam, hdui::noParam);
        switch (tab)
        {
            case Steps:
            {
                static const char stepTitles[8][16] = {
                    "ANVIL - STEP 01", "ANVIL - STEP 02", "ANVIL - STEP 03", "ANVIL - STEP 04",
                    "ANVIL - STEP 05", "ANVIL - STEP 06", "ANVIL - STEP 07", "ANVIL - STEP 08",
                };
                bank6 (ids, ParamId::pitch1 + (uint32_t) picked, ParamId::velocity1 + (uint32_t) picked, ParamId::cutoff,
                       ParamId::resonance, ParamId::vcfDecay, ParamId::vcaDecay);
                std::memcpy (title, stepTitles[picked & 7], 16);
                return;
            }
            case Vco:
                if (bankVco == 0)
                {
                    bank6 (ids, ParamId::vco1Freq, ParamId::vco1EgAmt, ParamId::vco1Wave, ParamId::vco2Freq, ParamId::vco2EgAmt,
                           ParamId::vco2Wave);
                    std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - VCO 1/2");
                }
                else
                {
                    bank6 (ids, ParamId::vcoDecay, ParamId::fmAmt, ParamId::egFmAmt, ParamId::seqPitchMod, ParamId::hardSync,
                           hdui::noParam);
                    std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - VCO 2/2");
                }
                return;
            case Mix:
                bank6 (ids, ParamId::vco1Level, ParamId::noiseLevel, ParamId::vco2Level, ParamId::noiseColor, hdui::noParam,
                       hdui::noParam);
                std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - MIX");
                return;
            case Vcf:
                bank6 (ids, ParamId::cutoff, ParamId::resonance, ParamId::vcfMode, ParamId::vcfDecay, ParamId::vcfEgAmt,
                       ParamId::noiseVcfMod);
                std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - VCF");
                return;
            case Vca:
                bank6 (ids, ParamId::vcaEg, ParamId::vcaDecay, ParamId::volume, ParamId::velCutoff, ParamId::pitchCutoff,
                       ParamId::vcfEgVco2);
                std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - VCA/PATCH");
                return;
            case Seq:
                if (bankSeq == 0)
                {
                    bank6 (ids, ParamId::clockDiv, ParamId::length, ParamId::swing, ParamId::direction, ParamId::chance,
                           ParamId::ratchet);
                    std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - SEQ 1/2");
                }
                else
                {
                    bank6 (ids, ParamId::quantize, ParamId::noteMode, ParamId::run, hdui::noParam, hdui::noParam, hdui::noParam);
                    std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL - SEQ 2/2");
                }
                return;
            case TabCount: break;
        }
        std::snprintf (title, SIXTEEN_TAB_NAME_SIZE, "ANVIL");
    }

    bool setBank (int which)
    {
        int& bank = tab == Vco ? bankVco : bankSeq;
        if (bank == which)
            return false;
        bank = which;
        touched = true;
        return true;
    }
    static float pitchFromY (int y) { return std::clamp ((210.f - (float) y) / 100.f, 0.f, 1.f) * 2.f - 1.f; }
    static float velFromY (int y) { return std::clamp ((275.f - (float) y) / 55.f, 0.f, 1.f); }
    void writeDrag (int y, hdui::Edits& out) const
    {
        if (dragStep < 0 || dragStep > 7)
            return;
        if (drag == Drag::Pitch)
            out.set (ParamId::pitch1 + (uint32_t) dragStep, pitchFromY (y));
        else if (drag == Drag::Vel)
            out.set (ParamId::velocity1 + (uint32_t) dragStep, velFromY (y));
    }
    bool hitButton (int x, int y, hdui::Edits& out)
    {
        if (y < 80 || y >= 99)
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

    bool mast (uint32_t* pixels, int stride, int pose, int glint, bool whole)
    {
        const float cycle = float (pose % hdui::torus::frames) / hdui::torus::frames;
        const float phase = cycle * hdui::torus::tau;
        const float travel = 2.f * cycle;
        const hdui::print::Drift drift (pose * hdui::torus::tau / hdui::masthead::frames);
        bool drew = false;
        drew |= masthead.region<160, 60> (pixels, stride, whole, 0, 0, [&] (hdui::Raster& r) {
            hdui::print::underprint (r, 0, drift);
            hdui::masthead::draw (r, 0, phase, travel, drift.inkOffset);
        });
        drew |= masthead.region<80, 60> (pixels, stride, whole, 160, 0, [&] (hdui::Raster& r) {
            hdui::print::underprint (r, 160, drift);
            hdui::torus::Outlet outlet;
            hdui::torus::draw (r, phase, outlet, drift.inkOffset);
            hdui::masthead::draw (r, 160, phase, travel, drift.inkOffset, &outlet);
            hdui::masthead::drawGlint (r, glint);
        });
        drew |= masthead.region<80, 60> (pixels, stride, whole, 240, 0, [&] (hdui::Raster& r) {
            hdui::print::underprint (r, 240, drift);
            hdui::masthead::draw (r, 240, phase, travel, drift.inkOffset);
        });
        return drew;
    }

    void chrome (Canvas& c) const
    {
        c.box (328, 0, width - 328, 76, hdui::ink::bg);
        c.box (340, 2, 436, 56, hdui::ink::field);
        c.box (335, 5, 1, 51, hdui::ink::rule);
        char head[24];
        switch (tab)
        {
            case Steps:
            {
                static const char heads[8][8] = { "STEP 01", "STEP 02", "STEP 03", "STEP 04",
                                                  "STEP 05", "STEP 06", "STEP 07", "STEP 08" };
                std::memcpy (head, heads[picked & 7], 8);
                break;
            }
            case Vco: std::snprintf (head, sizeof head, "VCO %d/2", bankVco + 1); break;
            case Mix: std::snprintf (head, sizeof head, "MIX"); break;
            case Vcf: std::snprintf (head, sizeof head, "VCF"); break;
            case Vca: std::snprintf (head, sizeof head, "VCA / PATCH"); break;
            case Seq: std::snprintf (head, sizeof head, "SEQ %d/2", bankSeq + 1); break;
            case TabCount: head[0] = 0; break;
        }
        c.text (346, 8, head, hdui::ink::paper);
        const int len = std::clamp ((int) std::lround (value (ParamId::length)), 1, 8);
        const bool live = running && value (ParamId::run) >= 0.5f;
        char pos[8];
        static const char digits[] = "0123456789";
        pos[0] = '0';
        pos[1] = digits[(step & 7) + 1];
        pos[2] = '/';
        pos[3] = '0';
        pos[4] = digits[len];
        pos[5] = 0;
        c.text (600, 8, pos, live ? hdui::ink::live : hdui::ink::paper);
        const char* runText = value (ParamId::run) < 0.5f ? "STOP" : (running ? "RUN" : "ARMED");
        c.text (668, 8, runText, live ? hdui::ink::live : hdui::ink::muted);
        static const char* const names[] = { "STEPS", "VCO", "MIX", "VCF", "VCA/P", "SEQ" };
        for (int i = 0; i < TabCount; ++i)
        {
            const int x = tabX + i * tabPitch;
            const bool on = i == tab;
            c.box (x, tabY, tabW, tabH, on ? hdui::ink::selected : hdui::ink::field);
            c.frame (x, tabY, tabW, tabH, on ? hdui::ink::soft : hdui::ink::rule);
            if (on)
                c.box (x, tabY, tabW, 2, hdui::ink::live);
            c.center (x, 34, tabW, names[i], on ? hdui::ink::paper : hdui::ink::muted);
            if (on && (i == Vco || i == Seq))
            {
                const int bank = i == Vco ? bankVco : bankSeq;
                c.box (x + 27, 48, 5, 2, bank == 0 ? hdui::ink::live : hdui::ink::soft);
                c.box (x + 36, 48, 5, 2, bank == 1 ? hdui::ink::live : hdui::ink::soft);
            }
            else if (on)
                c.box (x + 30, 48, 8, 2, hdui::ink::soft);
        }
        c.box (0, 76, width, 1, hdui::ink::rule);
    }

    void card (Canvas& c, int x, int y, int w, int h, int knob, uint32_t id, bool live) const
    {
        if (id == hdui::noParam)
            return;
        const int index = paramIndex (id);
        if (index < 0)
            return;
        const Param& p = paramTable[index];
        c.box (x, y, w, h, live ? hdui::ink::card : hdui::ink::field);
        c.frame (x, y, w, h, live ? hdui::ink::rule : hdui::ink::grid);
        char mark[4] = { 'K', (char) ('0' + (knob < 1 ? 1 : (knob > 6 ? 6 : knob))), 0, 0 };
        c.text (x + 4, y + 4, mark, live ? hdui::ink::live : hdui::ink::muted);
        c.text (x + 24, y + 4, shortName (id), live ? hdui::ink::paper : hdui::ink::muted);
        char text[32];
        formatParam (p, value (id), text, sizeof text, context ());
        c.right (x + w - 6, y + 4, text, live ? hdui::ink::paper : hdui::ink::soft);
        const float span = p.max - p.min;
        const float u = span > 0.f ? std::clamp ((value (id) - p.min) / span, 0.f, 1.f) : 0.f;
        const int bx = x + 6, bw = w - 12, by = y + h - 6;
        c.box (bx, by, bw, 2, hdui::ink::grid);
        const int at = bx + (int) std::lround (u * (float) bw);
        const int origin = p.min < 0.f ? bx + bw / 2 : bx;
        const int left = std::min (at, origin);
        c.box (left, by, std::max (2, at - left), 2, live ? hdui::ink::live : hdui::ink::muted);
    }

    void cards (Canvas& c, int x, int y, int w, int h, const uint32_t ids[6], bool live) const
    {
        const int cw = (w - 16) / 3;
        const int ch = (h - 4) / 2;
        for (int i = 0; i < 6; ++i)
        {
            if (ids[i] == hdui::noParam)
                continue;
            const int cx = x + (i % 3) * (cw + 4);
            const int cy = y + (i / 3) * (ch + 4);
            card (c, cx, cy, cw - 4, ch - 2, i + 1, ids[i], live);
        }
    }

    void bankBox (Canvas& c, int x, int y, int w, int h, const char* title, const uint32_t ids[6], bool live) const
    {
        c.box (x, y, w, h, hdui::ink::field);
        c.frame (x, y, w, h, live ? hdui::ink::live : hdui::ink::grid);
        c.box (x + 1, y + 1, w - 2, 16, live ? hdui::ink::selected : hdui::ink::card);
        c.text (x + 6, y + 3, title, live ? hdui::ink::live : hdui::ink::muted);
        int last = 0;
        for (int i = 0; i < 6; ++i)
            if (ids[i] != hdui::noParam)
                last = i + 1;
        static const char* const bound[] = { "", "K1", "K1-K2", "K1-K3", "K1-K4", "K1-K5", "K1-K6" };
        c.right (x + w - 6, y + 3, live ? bound[last] : "TAP", live ? hdui::ink::paper : hdui::ink::muted);
        cards (c, x + 6, y + 22, w - 12, h - 26, ids, live);
    }

    void mini (Canvas& c) const
    {
        constexpr int x = 564, y = 82, w = 208, h = 94;
        c.box (x, y, w, h, hdui::ink::field);
        c.frame (x, y, w, h, hdui::ink::grid);
        c.text (x + 6, y + 4, "STEPS", hdui::ink::muted);
        char clock[24];
        textOf (ParamId::clockDiv, clock, (int) sizeof clock);
        c.right (x + w - 6, y + 4, clock, hdui::ink::paper);
        const int len = std::clamp ((int) std::lround (value (ParamId::length)), 1, 8);
        const int q = (int) std::lround (value (ParamId::quantize));
        constexpr int zero = y + 48;
        c.box (x + 8, zero, w - 16, 1, hdui::ink::grid);
        const int cell = (w - 16) / 8;
        for (int i = 0; i < 8; ++i)
        {
            const int lx = x + 8 + i * cell;
            const bool inLen = i < len;
            const bool live = running && inLen && i == step;
            const Ink col = live ? hdui::ink::live : (inLen ? hdui::ink::soft : hdui::ink::parked);
            if (live)
                c.box (lx + 2, y + 20, cell - 5, 2, hdui::ink::live);
            if (i == picked)
                c.frame (lx + 1, y + 18, cell - 3, 62, hdui::ink::rule);
            const float semis = quantizedSemitones (value (ParamId::pitch1 + (uint32_t) i), q);
            const int py = zero - (int) std::lround (std::clamp (semis / 60.f, -1.f, 1.f) * 16.f);
            c.box (lx + 2, py - 1, cell - 5, 3, col);
            const int vel = (int) std::lround (value (ParamId::velocity1 + (uint32_t) i) * 16.f);
            c.box (lx + cell / 2 - 3, y + h - 8 - vel, 6, std::max (2, vel), col);
        }
    }

    void steps (Canvas& c) const
    {
        char clock[24], dir[24], swing[24], quant[24];
        textOf (ParamId::clockDiv, clock, (int) sizeof clock);
        textOf (ParamId::direction, dir, (int) sizeof dir);
        textOf (ParamId::swing, swing, (int) sizeof swing);
        textOf (ParamId::quantize, quant, (int) sizeof quant);
        const int len = std::clamp ((int) std::lround (value (ParamId::length)), 1, 8);
        char row[96];
        std::snprintf (row, sizeof row, "%s  %s  LEN %d", clock, dir, len);
        c.text (10, 84, row, hdui::ink::paper);
        c.text (250, 84, swing, hdui::ink::muted);
        c.text (330, 84, quant, hdui::ink::soft);
        static const char auditions[8][13] = {
            "AUDITION 01", "AUDITION 02", "AUDITION 03", "AUDITION 04",
            "AUDITION 05", "AUDITION 06", "AUDITION 07", "AUDITION 08",
        };
        const char* aud = auditions[picked & 7];
        button (c, 536, 80, 112, 19, aud, flashFrames > 0 && flashBtn == 1);
        button (c, 654, 80, 56, 19, "TRIG", flashFrames > 0 && flashBtn == 2);
        button (c, 716, 80, 56, 19, "ADV", flashFrames > 0 && flashBtn == 3);

        c.box (8, 103, width - 16, height - 111, hdui::ink::field);
        c.frame (8, 103, width - 16, height - 111, hdui::ink::grid);
        c.text (14, 110, "PITCH", hdui::ink::muted);
        c.text (14, 228, "VEL", hdui::ink::muted);
        constexpr int zeroY = 163;
        const int q = (int) std::lround (value (ParamId::quantize));
        for (int i = 0; i < 8; ++i)
        {
            const int x = laneLeft + i * laneW;
            const bool inLen = i < len;
            const bool live = running && inLen && i == step;
            const bool pick = i == picked;
            const Ink col = live ? hdui::ink::live : (inLen ? hdui::ink::soft : hdui::ink::parked);
            if (live)
            {
                c.box (x + 4, 106, laneW - 8, 18, hdui::ink::cursor);
                c.box (x + 4, 104, laneW - 8, 2, hdui::ink::live);
            }
            if (pick)
                c.frame (x + 3, 106, laneW - 6, 20, hdui::ink::paper);
            char num[4] = { '0', (char) ('1' + i), 0, 0 };
            c.center (x, 108, laneW, num, pick ? hdui::ink::paper : col);
            c.box (x + 8, zeroY, laneW - 16, 1, hdui::ink::rule);
            const float semis = quantizedSemitones (value (ParamId::pitch1 + (uint32_t) i), q);
            const int dy = (int) std::lround (std::clamp (semis / 60.f, -1.f, 1.f) * 40.f);
            const int top = std::min (zeroY, zeroY - dy);
            c.box (x + 22, top, laneW - 44, std::abs (dy) + 1, live ? hdui::ink::cursor : hdui::ink::grid);
            c.box (x + 22, zeroY - dy - 1, laneW - 44, 3, col);
            char pitch[16];
            textOf (ParamId::pitch1 + (uint32_t) i, pitch, (int) sizeof pitch);
            c.center (x, 196, laneW, pitch, pick ? hdui::ink::paper : col);
            const float vel = value (ParamId::velocity1 + (uint32_t) i);
            const int vh = (int) std::lround (vel * 32.f);
            c.box (x + 16, 258, laneW - 32, 1, hdui::ink::grid);
            c.box (x + laneW / 2 - 8, 258 - vh, 16, std::max (2, vh), col);
            char pct[8];
            const int pctN = (int) std::lround (std::clamp (vel, 0.f, 1.f) * 100.f);
            pct[0] = (char) ('0' + pctN / 100);
            pct[1] = (char) ('0' + (pctN / 10) % 10);
            pct[2] = (char) ('0' + pctN % 10);
            pct[3] = '%';
            pct[4] = 0;
            const char* pctText = pctN >= 100 ? pct : (pctN >= 10 ? pct + 1 : pct + 2);
            c.center (x, 262, laneW, pctText, pick ? hdui::ink::paper : hdui::ink::muted);
        }
    }

    static void button (Canvas& c, int x, int y, int w, int h, const char* label, bool lit)
    {
        c.box (x, y, w, h, lit ? hdui::ink::cursor : hdui::ink::field);
        c.frame (x, y, w, h, lit ? hdui::ink::live : hdui::ink::rule);
        c.center (x, y + 4, w, label, lit ? hdui::ink::live : hdui::ink::paper);
    }

    void waves (Canvas& c) const
    {
        constexpr int x = 564, y = 182, w = 208, h = 97;
        c.box (x, y, w, h, hdui::ink::field);
        c.frame (x, y, w, h, hdui::ink::grid);
        c.text (x + 6, y + 4, "VCO 1", hdui::ink::muted);
        c.text (x + 108, y + 4, "VCO 2", hdui::ink::muted);
        if (value (ParamId::hardSync) >= 0.5f)
        {
            c.box (x + 156, y + 3, 46, 14, hdui::ink::cursor);
            c.center (x + 156, y + 4, 46, "SYNC", hdui::ink::live);
        }
        for (int n = 0; n < 2; ++n)
        {
            const int wx = x + 8 + n * 100;
            const bool tri = value (n == 0 ? ParamId::vco1Wave : ParamId::vco2Wave) >= 0.5f;
            const Ink col = (n == 1 && value (ParamId::fmAmt) > 0.05f) ? hdui::ink::live : hdui::ink::soft;
            int prev = -1;
            for (int i = 0; i < 86; ++i)
            {
                const int k = i % 28;
                const int wy = tri ? (y + 28 + std::abs (14 - k) * 2) : (k < 14 ? y + 28 : y + 56);
                if (prev < 0)
                    c.box (wx + i, wy, 2, 2, col);
                else
                    c.box (wx + i, std::min (prev, wy), 2, std::abs (wy - prev) + 2, col);
                prev = wy;
            }
            char hz[24];
            textOf (n == 0 ? ParamId::vco1Freq : ParamId::vco2Freq, hz, (int) sizeof hz);
            c.text (wx, y + 68, hz, hdui::ink::paper);
            c.text (wx, y + 82, tri ? "TRI" : "SQR", hdui::ink::muted);
        }
    }

    void meters (Canvas& c) const
    {
        struct Source
        {
            const char* name;
            uint32_t id;
            Ink col;
        };
        const Source sources[3] = {
            { "VCO 1", ParamId::vco1Level, hdui::ink::soft },
            { "NOISE", ParamId::noiseLevel, hdui::ink::live },
            { "VCO 2", ParamId::vco2Level, hdui::ink::paper },
        };
        c.box (564, 182, 208, 97, hdui::ink::field);
        c.frame (564, 182, 208, 97, hdui::ink::grid);
        for (int i = 0; i < 3; ++i)
        {
            const int x = 576 + i * 66;
            c.text (x, 186, sources[i].name, hdui::ink::muted);
            const float level = std::clamp (value (sources[i].id), 0.f, 1.f);
            const int bh = (int) std::lround (level * 48.f);
            c.box (x + 8, 202, 16, 48, hdui::ink::grid);
            c.box (x + 8, 250 - bh, 16, bh, sources[i].col);
        }
        char color[24];
        textOf (ParamId::noiseColor, color, (int) sizeof color);
        c.text (576, 258, color, hdui::ink::paper);
    }

    void body (Canvas& c) const
    {
        if (tab == Steps)
        {
            steps (c);
            return;
        }
        mini (c);
        uint32_t a[6], b[6];
        if (tab == Vco)
        {
            bank6 (a, ParamId::vco1Freq, ParamId::vco1EgAmt, ParamId::vco1Wave, ParamId::vco2Freq, ParamId::vco2EgAmt, ParamId::vco2Wave);
            bank6 (b, ParamId::vcoDecay, ParamId::fmAmt, ParamId::egFmAmt, ParamId::seqPitchMod, ParamId::hardSync, hdui::noParam);
            bankBox (c, 8, 82, 548, 96, "VCO 1 AND VCO 2", a, bankVco == 0);
            bankBox (c, 8, 183, 548, 96, "EG, FM, SYNC", b, bankVco == 1);
            waves (c);
            return;
        }
        if (tab == Seq)
        {
            bank6 (a, ParamId::clockDiv, ParamId::length, ParamId::swing, ParamId::direction, ParamId::chance, ParamId::ratchet);
            bank6 (b, ParamId::quantize, ParamId::noteMode, ParamId::run, hdui::noParam, hdui::noParam, hdui::noParam);
            bankBox (c, 8, 82, 548, 96, "CLOCK AND GROOVE", a, bankSeq == 0);
            bankBox (c, 8, 183, 548, 96, "QUANTIZE, NOTES, RUN", b, bankSeq == 1);
            return;
        }
        uint32_t ids[6];
        char title[SIXTEEN_TAB_NAME_SIZE];
        activeBank (ids, title);
        const char* heading = tab == Mix ? "SOURCE MIXER" : (tab == Vcf ? "LADDER FILTER" : "VCA AND PATCH");
        bankBox (c, 8, 82, 548, 197, heading, ids, true);
        if (tab == Mix)
            meters (c);
    }
};
} // namespace anvil
