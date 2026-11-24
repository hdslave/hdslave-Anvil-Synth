#pragma once

#include "hdslave_tiles.h"
#include "screen.h"

namespace anvil
{

struct TilesAccess
{
    // 9 tiles (3x3 grid): STEPS, VCO 1-6, VCO 6-11, MIX, VCF, VCA, PATCH, SEQ 1-6, SEQ 6-11
    static constexpr int kTileCount = 9;
    static constexpr int kTileGroup[kTileCount] = { 0, 1, 1, 2, 3, 4, 5, 6, 6 };
    static constexpr int kTileTop[kTileCount]   = { 0, 0, 5, 0, 0, 0, 0, 0, 5 };

    static uint32_t CLAP_ABI count (const clap_plugin_t*, int* out_cols, int* out_rows)
    {
        if (out_cols) *out_cols = 3;
        if (out_rows) *out_rows = 3;
        return kTileCount;
    }

    static const char* CLAP_ABI title (const clap_plugin_t*, uint32_t tile)
    {
        static constexpr const char* kTitles[kTileCount] = {
            "01 // 8-STEP PITCH & VEL",
            "02 // VCO & FM (1-6)",
            "03 // VCO & SYNC (6-11)",
            "04 // MIXER & NOISE",
            "05 // LADDER VCF",
            "06 // VCA & VOLUME",
            "07 // MOD PATCH",
            "08 // SEQUENCER (1-6)",
            "09 // SEQUENCER (6-11)"
        };
        return tile < kTileCount ? kTitles[tile] : "";
    }

    static bool CLAP_ABI paint (const clap_plugin_t* plugin, uint32_t tile, const melodee_screen_frame_t* frame, clap_id knobs_out[3])
    {
        if (tile >= kTileCount || !frame || !frame->pixels) return false;
        auto& p = of (plugin);
        const int savedGroup = p.screen.group;
        const int savedRow = p.screen.row;
        const int savedTop = p.screen.pinnedTop;

        p.screen.group = kTileGroup[tile];
        p.screen.pinnedTop = kTileTop[tile];
        p.screen.row = std::clamp (p.screen.tileRows[tile], kTileTop[tile],
                                   std::max (0, p.screen.fields().count - 1));
        p.screen.lastGroup = -1;

        melodee_screen_frame_t c = *frame;
        c.redraw_all = 1;
        const hdui::Host h = hostOf (&c);
        p.screen.update (h.nowMs);
        const bool drew = p.screen.paint (h, c.pixels, (int) c.stride, true);
        if (knobs_out)
            screenControls (plugin, knobs_out);

        p.screen.group = savedGroup;
        p.screen.row = savedRow;
        p.screen.pinnedTop = savedTop;
        return drew;
    }

    static bool CLAP_ABI hit_test (const clap_plugin_t* plugin, uint32_t tile, int x, int y, hdslave_tile_hotspot_t* out_spot)
    {
        if (!out_spot || tile >= kTileCount) return false;
        *out_spot = {};
        out_spot->param_id = CLAP_INVALID_ID;
        out_spot->param_id_y = CLAP_INVALID_ID;

        auto& p = of (plugin);
        const int g = kTileGroup[tile];
        const int top = kTileTop[tile];

        // Top-right status box (x: 240..320, y: 0..59) -> Run / Stop toggle
        if (x >= 240 && x < 320 && y >= 18 && y < 48)
        {
            out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
            out_spot->param_id = ParamId::run;
            out_spot->x = 244;
            out_spot->y = 18;
            out_spot->w = 72;
            out_spot->h = 26;
            return true;
        }

        if (g == 0)
        {
            // STEPS screen (y: 60..169)
            if (y >= 60 && y < 76)
            {
                if (x < 58)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::clockDiv;
                    out_spot->x = 4; out_spot->y = 60; out_spot->w = 54; out_spot->h = 16;
                }
                else if (x < 190)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::direction;
                    out_spot->x = 58; out_spot->y = 60; out_spot->w = 132; out_spot->h = 16;
                }
                else
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::quantize;
                    out_spot->x = 190; out_spot->y = 60; out_spot->w = 126; out_spot->h = 16;
                }
                return true;
            }
            if (x >= Screen::left && x < Screen::left + 8 * Screen::lane && y >= 76 && y < 170)
            {
                const int step = std::clamp ((x - Screen::left) / Screen::lane, 0, 7);
                const int sx = Screen::left + step * Screen::lane;
                if (y < 91)
                {
                    // Step number badge 01..08: click selects & auditions step
                    out_spot->kind = HDSLAVE_HOTSPOT_ACTION;
                    out_spot->custom_id = step;
                    out_spot->x = (int16_t) sx;
                    out_spot->y = 76;
                    out_spot->w = (int16_t) Screen::lane;
                    out_spot->h = 15;
                    return true;
                }
                if (y < 139)
                {
                    // Pitch terrace lane
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::pitch1 + (uint32_t) step;
                    out_spot->x = (int16_t) sx;
                    out_spot->y = 91;
                    out_spot->w = (int16_t) Screen::lane;
                    out_spot->h = 48;
                    return true;
                }
                // Velocity pyramid lane
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = ParamId::velocity1 + (uint32_t) step;
                out_spot->x = (int16_t) sx;
                out_spot->y = 140;
                out_spot->w = (int16_t) Screen::lane;
                out_spot->h = 28;
                out_spot->is_absolute = true;
                out_spot->invert_y = true;
                return true;
            }
        }
        else
        {
            // List screens (group 1..6): left list (x: 0..178, y: 60..169)
            if (x >= 0 && x < 178 && y >= 64 && y < 64 + Screen::visible * Screen::rowH)
            {
                const int visIdx = (y - 64) / Screen::rowH;
                const int rowIdx = top + visIdx;
                const auto& grp = layout::groups[g];
                if (rowIdx >= 0 && rowIdx < grp.count)
                {
                    const uint32_t id = grp.fields[rowIdx].id;
                    const auto& param = paramTable[paramIndex (id)];
                    out_spot->kind = param.kind == Kind::button ? HDSLAVE_HOTSPOT_BUTTON : HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = id;
                    out_spot->x = 4;
                    out_spot->y = (int16_t) (64 + visIdx * Screen::rowH);
                    out_spot->w = 172;
                    out_spot->h = (int16_t) (Screen::rowH - 1);
                    return true;
                }
            }
            // Right mini step graph (x: 184..315, y: 64..165)
            if (x >= 184 && x < 314 && y >= 64 && y < 165)
            {
                const int step = std::clamp ((x - 186) / 16, 0, 7);
                const int sx = 186 + step * 16;
                if (y < 120)
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::pitch1 + (uint32_t) step;
                    out_spot->x = (int16_t) sx;
                    out_spot->y = 75;
                    out_spot->w = 15;
                    out_spot->h = 44;
                    return true;
                }
                else
                {
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = ParamId::velocity1 + (uint32_t) step;
                    out_spot->x = (int16_t) sx;
                    out_spot->y = 122;
                    out_spot->w = 15;
                    out_spot->h = 40;
                    out_spot->is_absolute = true;
                    out_spot->invert_y = true;
                    return true;
                }
            }
        }
        return false;
    }

    static void CLAP_ABI focus (const clap_plugin_t* plugin, uint32_t tile, int x, int y, bool)
    {
        if (tile >= kTileCount) return;
        auto& p = of (plugin);
        const int g = kTileGroup[tile];
        const int top = kTileTop[tile];
        p.screen.group = g;
        if (g == 0)
        {
            if (x >= Screen::left && x < Screen::left + 8 * Screen::lane && y >= 76 && y < 170)
                p.screen.picked = std::clamp ((x - Screen::left) / Screen::lane, 0, 7);
        }
        else if (x >= 0 && x < 178 && y >= 64 && y < 64 + Screen::visible * Screen::rowH)
        {
            const int rowIdx = top + (y - 64) / Screen::rowH;
            if (rowIdx >= 0 && rowIdx < layout::groups[g].count)
            {
                p.screen.tileRows[tile] = rowIdx;
                p.screen.row = rowIdx;
            }
        }
    }

    static void CLAP_ABI action (const clap_plugin_t* plugin, uint32_t, int32_t custom_id, int32_t, bool)
    {
        auto& p = of (plugin);
        if (custom_id >= 0 && custom_id < 8)
        {
            p.screen.picked = custom_id;
            p.pressedAt.store (steadyMs(), std::memory_order_release);
            p.audition.store (custom_id, std::memory_order_release);
            if (p.host && p.host->request_process)
                p.host->request_process (p.host);
        }
    }
};

const clap_plugin_hdslave_tiles_t tilesExtension {
    TilesAccess::count,
    TilesAccess::title,
    TilesAccess::paint,
    TilesAccess::hit_test,
    TilesAccess::focus,
    TilesAccess::action,
    170,
};

} // namespace anvil
