// Anvil's page on Melodee's screen (melodee_screen.h, ui/page.h). STEPS is the panel's sequencer: the eight steps'
// pitch and velocity, the one playing lit, the one picked framed (the wheel picks, Push plays it as a pad would, K1
// and K2 turn its Pitch and Velocity, K3 the Cutoff). The other groups list the panel's knobs by section, the steps
// beside them.
#pragma once

#include "params.h"
#include "ui/page.h"

#include <atomic>

namespace anvil
{
// The page's own button: Push on STEPS plays the step picked. (Not a parameter: ids from here on are the screen's.)
constexpr uint32_t auditionId = 1000;

// What the audio thread says for the page: the step it is on and how many hits it has played.
struct StepState
{
    std::atomic<int> step { 0 };
    std::atomic<uint32_t> hits { 0 };
    std::atomic<bool> running { false };
};

class Screen final : public hdui::Page
{
public:
    Screen (const std::atomic<float>* values, const StepState& state) : values (values), state (state)
    {
        colours = { 0xff120e0f, 0xff1d1618, 0xff2a2023, 0xff4a3a36, 0xfff2e8e0, 0xffa89890, 0xff5c4c48, 0xffffa040, 0xff140e0c };
    }

    bool animating (const hdui::Host& h) const override { return flashAt != 0 && h.nowMs - flashAt < flashMs; }

    // The audio thread's news, taken before each paint.
    void update (uint64_t nowMs)
    {
        const uint32_t hits = state.hits.load (std::memory_order_relaxed);
        if (hits != seenHits)
            seenHits = hits, flashAt = nowMs;
    }

    int pickedStep() const { return picked; }

protected:
    int groupCount() const override { return (int) (sizeof (groups) / sizeof (groups[0])); }
    const hdui::Group& groupAt (int i) const override { return groups[i]; }
    const char* title() const override { return "ANVIL"; }

    float value (uint32_t id) const { return values[paramIndex (id)].load (std::memory_order_relaxed); }

    void read (uint32_t id, hdui::Value& out) const override
    {
        const int index = paramIndex (id);
        if (index < 0)
            return;
        const Param& p = paramTable[index];
        out.value = value (id);
        out.min = p.min, out.max = p.max, out.def = p.def;
        out.kind = p.kind == Kind::continuous ? hdui::Kind::continuous
                 : p.kind == Kind::integer   ? hdui::Kind::stepped
                 : p.kind == Kind::choice    ? hdui::Kind::choice
                                             : hdui::Kind::button;
        out.bipolar = p.min < 0.0f;
        TextContext context;
        context.quantize = value (ParamId::quantize);
        formatParam (p, out.value, out.text, sizeof (out.text), context);
    }

    const char* customLabel (uint32_t id) const override
    {
        static const char* const pitch[] = { "PITCH 1", "PITCH 2", "PITCH 3", "PITCH 4", "PITCH 5", "PITCH 6", "PITCH 7", "PITCH 8" };
        static const char* const velocity[] = { "VEL 1", "VEL 2", "VEL 3", "VEL 4", "VEL 5", "VEL 6", "VEL 7", "VEL 8" };
        if (id >= ParamId::pitch1 && id <= ParamId::pitch8)
            return pitch[id - ParamId::pitch1];
        if (id >= ParamId::velocity1 && id <= ParamId::velocity8)
            return velocity[id - ParamId::velocity1];
        return "";
    }

    // STEPS: the wheel picks a step, Push plays it.
    bool customInput (int, hdui::Key key, int amount, bool, hdui::Edits& out) override
    {
        if (key == hdui::Key::wheel)
        {
            const int next = std::clamp (picked + amount, 0, 7);
            const bool moved = next != picked;
            picked = next;
            return moved;
        }
        if (key == hdui::Key::push)
            return out.press (auditionId + (uint32_t) picked), true;
        return false;
    }

    void customKnobs (int, uint32_t ids[3]) const override
    {
        ids[0] = ParamId::pitch1 + (uint32_t) picked;
        ids[1] = ParamId::velocity1 + (uint32_t) picked;
        ids[2] = ParamId::cutoff;
    }

    void legend (const char* lines[6]) const override
    {
        if (groupIndex() == 0)
            lines[0] = "WHEEL  STEP", lines[1] = "PUSH   PLAY IT", lines[2] = "-/+  GROUP", lines[3] = "K1 PITCH K2 VEL", lines[4] = "K3",
            lines[5] = "CUTOFF";
        else
            Page::legend (lines);
    }

    // The header's right: where the sequence is, and its clock.
    void drawStatus (hdui::Raster& r, const hdui::Host& h, int right) override
    {
        char text[64];
        const int length = (int) value (ParamId::length);
        hdui::Value clock;
        read (ParamId::clockDiv, clock);
        std::snprintf (text, sizeof (text), "%s  STEP %d/%d", clock.text, state.step.load (std::memory_order_relaxed) + 1, length);
        const int w = hdui::textWidth (text);
        r.text (right - w, headerY + 1, text, colours.text);
        const char* mode = value (ParamId::run) < 0.5f ? "STOPPED" : (h.playing ? "RUNNING" : "WAITING");
        r.text (right - hdui::textWidth (mode), headerY + 13, mode, value (ParamId::run) < 0.5f ? colours.dim : colours.muted);
        if (animating (h))
            r.box (right - w - 10, headerY + 4, 5, 5, accent (h));
    }

    // Beside a list: the eight steps, small.
    void drawPanel (hdui::Raster& r, const hdui::Host& h, int x, int y, int w, int height) override
    {
        drawSteps (r, h, x, y, w, height, false);
    }

    void drawCustom (hdui::Raster& r, const hdui::Host& h, int x, int y, int w, int height) override
    {
        drawSteps (r, h, x + 6, y, w - 12, height, true);
    }

private:
    const std::atomic<float>* values;
    const StepState& state;
    int picked = 0;
    uint32_t seenHits = 0;
    uint64_t flashAt = 0;
    static constexpr uint64_t flashMs = 160;

    // The steps: pitch bars from a middle line (up is higher), velocity bars from the floor, the step playing lit
    // (brightest as it is hit, fading), steps past Length dimmed; on STEPS, the step picked framed and each pitch read.
    void drawSteps (hdui::Raster& r, const hdui::Host& h, int x, int y, int w, int height, bool big)
    {
        const hdui::Palette& c = colours;
        const hdui::Ink hi = accent (h);
        const int length = (int) value (ParamId::length), playing = state.step.load (std::memory_order_relaxed);
        const int column = w / 8, bar = big ? 12 : 6;
        const int pitchMid = y + (big ? 30 : 30), pitchSpan = big ? 27 : 26;
        const int velFloor = y + (big ? 90 : 100), velSpan = big ? 26 : 32;
        const int fade = flashAt != 0 && h.nowMs - flashAt < flashMs ? (int) (256 - 256 * (h.nowMs - flashAt) / flashMs) : 0;
        r.box (x, pitchMid, column * 8 - 2, 1, c.rule);
        for (int s = 0; s < 8; ++s)
        {
            const int cx = x + s * column, mid = cx + (column - 2) / 2;
            const bool inLength = s < length, lit = s == playing && state.running.load (std::memory_order_relaxed);
            if (lit)
                r.box (cx, y, column - 2, velFloor - y + 2, hdui::mix (c.panel, hi, 40 + fade / 3));
            const hdui::Ink ink = ! inLength ? c.dim : (lit ? hi : (big && s == picked ? c.text : c.muted));
            const float pitch = value (ParamId::pitch1 + (uint32_t) s), velocity = value (ParamId::velocity1 + (uint32_t) s);
            // Full height is +-2.5 octaves (half the knob), where most lines live; further reads as a cap.
            const int ph = (int) std::lround (std::clamp (pitch * 2.0f, -1.0f, 1.0f) * (float) pitchSpan);
            r.box (mid - bar / 2, ph >= 0 ? pitchMid - ph : pitchMid + 1, bar, std::max (1, std::abs (ph)), ink);
            const int vh = (int) std::lround (velocity * (float) velSpan);
            r.box (mid - bar / 2, velFloor - vh, bar, vh + 1, ! inLength ? c.dim : hdui::mix (ink, c.bg, 70));
            r.box (mid - bar / 2, velFloor - vh, bar, 1, ink);
            if (big)
            {
                char n[2] = { (char) ('1' + s), 0 };
                r.text (mid - 3, velFloor + 4, n, s == picked ? hi : c.muted);
                hdui::Value v;
                read (ParamId::pitch1 + (uint32_t) s, v);
                char* unit = std::strstr (v.text, " st");
                if (unit != nullptr)
                    *unit = 0;
                r.text (mid - hdui::textWidth (v.text, 5) / 2, y - 1 + (ph >= 0 ? 34 : 3), v.text, ink, 5);
                if (s == picked)
                    r.frame (cx - 1, y - 2, column, height + 1, hi);
            }
        }
    }

    // The groups. STEPS draws itself; the rest list the panel's knobs by section.
    static constexpr hdui::Field vco[] = {
        { ParamId::vcoDecay, "VCO DECAY" }, { ParamId::fmAmt, "1-2 FM AMT" }, { ParamId::egFmAmt, "EG>FM 1-2" },
        { ParamId::vco1Freq, "VCO1 FREQ" }, { ParamId::vco1EgAmt, "VCO1 EG" }, { ParamId::vco1Wave, "VCO1 WAVE" },
        { ParamId::vco2Freq, "VCO2 FREQ" }, { ParamId::vco2EgAmt, "VCO2 EG" }, { ParamId::vco2Wave, "VCO2 WAVE" },
        { ParamId::seqPitchMod, "SEQ PITCH" }, { ParamId::hardSync, "HARD SYNC" },
    };
    static constexpr hdui::Field mixer[] = {
        { ParamId::vco1Level, "VCO1 LEVEL" }, { ParamId::noiseLevel, "NOISE LEVEL" }, { ParamId::vco2Level, "VCO2 LEVEL" },
        { ParamId::noiseColor, "NOISE COLOR" },
    };
    static constexpr hdui::Field vcf[] = {
        { ParamId::cutoff, "CUTOFF" }, { ParamId::resonance, "RESONANCE" }, { ParamId::vcfMode, "VCF MODE" },
        { ParamId::vcfDecay, "VCF DECAY" }, { ParamId::vcfEgAmt, "VCF EG AMT" }, { ParamId::noiseVcfMod, "NOISE>VCF" },
    };
    static constexpr hdui::Field vca[] = {
        { ParamId::vcaEg, "VCA EG" }, { ParamId::vcaDecay, "VCA DECAY" }, { ParamId::volume, "VOLUME" },
    };
    static constexpr hdui::Field patch[] = {
        { ParamId::velCutoff, "VEL>CUTOFF" }, { ParamId::pitchCutoff, "PITCH>CUT" }, { ParamId::vcfEgVco2, "VCF EG>VCO2" },
    };
    static constexpr hdui::Field seq[] = {
        { ParamId::clockDiv, "CLOCK DIV" }, { ParamId::length, "LENGTH" }, { ParamId::swing, "SWING" },
        { ParamId::direction, "DIRECTION" }, { ParamId::chance, "CHANCE" }, { ParamId::ratchet, "RATCHET" },
        { ParamId::quantize, "QUANTIZE" }, { ParamId::noteMode, "NOTES" }, { ParamId::run, "RUN" },
        { ParamId::trigger, "TRIGGER" }, { ParamId::advance, "ADVANCE" },
    };
    static constexpr hdui::Group groups[] = {
        { "STEPS", nullptr, 0 }, { "VCO", vco, 11 }, { "MIX", mixer, 4 }, { "VCF", vcf, 6 }, { "VCA", vca, 3 },
        { "PATCH", patch, 3 }, { "SEQ", seq, 11 },
    };
};
} // namespace anvil
