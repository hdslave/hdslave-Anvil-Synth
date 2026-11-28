// Drawing primitives for the 780 x 285 SP-16 body. Coordinates and type are in physical pixels.
#pragma once

#include "print.h"
#include "sp16_font.h"
#include <algorithm>
#include <cmath>

namespace anvil::sp16
{
using hdui::Ink;
enum Type { Label, Control, Value };
constexpr Ink base = 0xff100e15, panel = 0xff201c28, tile = 0xff2c2735, shade = 0xff19161e;
constexpr Ink rule = 0xff393341, rail = 0xff48404f, selection = 0xff3c304d;
constexpr Ink white = 0xfff6f3f8, secondary = 0xffc9c1d1, muted = 0xffa99fb6;
constexpr Ink lilac = 0xffd5b1fa, violet = 0xff9776b9, lime = hdui::ink::live;

struct Canvas
{
    Ink* px;
    int w, h, stride;

    void blend (int x, int y, Ink colour, unsigned weight) const
    {
        if ((unsigned) x < (unsigned) w && (unsigned) y < (unsigned) h)
        {
            auto& dest = px[y * stride + x];
            dest = hdui::print::mix (dest, colour, weight);
        }
    }

    void box (int x, int y, int wide, int high, Ink colour) const
    {
        const int left = std::max (0, x), right = std::min (w, x + wide);
        if (right <= left) return;
        for (int row = std::max (0, y); row < std::min (h, y + high); ++row)
            std::fill (px + row * stride + left, px + row * stride + right, colour);
    }

    void rounded (int x, int y, int wide, int high, int radius, Ink colour) const
    {
        const int r = std::min ({radius, wide / 2, high / 2});
        box (x, y + r, wide, high - r * 2, colour);
        for (int j = 0; j < r; ++j)
        {
            const float dy = r - j - 0.5f;
            const float edge = r - std::sqrt (r * r - dy * dy);
            const int inset = (int) std::ceil (edge);
            const auto coverage = (unsigned) ((inset - edge) * 256);
            for (const int row : {y + j, y + high - 1 - j})
            {
                box (x + inset, row, wide - 2 * inset, 1, colour);
                blend (x + inset - 1, row, colour, coverage);
                blend (x + wide - inset, row, colour, coverage);
            }
        }
    }

    // Antialiased round-ended stroke for the encoder arcs and oscillator traces.
    void line (float ax, float ay, float bx, float by, float thick, Ink colour) const
    {
        const float dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy;
        const float outer = (thick + 1.f) * 0.5f, inner = std::max (0.f, outer - 1.f);
        const int left = (int) std::floor (std::min (ax, bx) - outer);
        const int right = (int) std::ceil (std::max (ax, bx) + outer);
        const int bottom = (int) std::ceil (std::max (ay, by) + outer);
        for (int y = (int) std::floor (std::min (ay, by) - outer); y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
            {
                const float t = length2 > 0.f ? std::clamp (((x - ax) * dx + (y - ay) * dy) / length2, 0.f, 1.f) : 0.f;
                const float ex = x - (ax + t * dx), ey = y - (ay + t * dy), d2 = ex * ex + ey * ey;
                if (d2 < outer * outer)
                    blend (x, y, colour, d2 <= inner * inner ? 256u : (unsigned) ((outer - std::sqrt (d2)) * 256));
            }
    }

    static const font::Glyph& glyph (char ch, Type size)
    {
        const unsigned code = (unsigned char) ch;
        return font::glyphs[size][(code >= 32 && code < 127 ? code : '?') - 32];
    }

    static int measure (const char* s, Type size)
    {
        int width = 0;
        while (*s) width += glyph (*s++, size).advance;
        return width;
    }

    void text (int x, int y, const char* s, Ink colour, Type size = Label) const
    {
        for (; *s; ++s)
        {
            const auto& g = glyph (*s, size);
            const auto* mask = font::masks + g.offset;
            for (int j = 0; j < g.h; ++j)
                for (int i = 0; i < g.w; ++i)
                {
                    const int index = j * g.w + i;
                    const unsigned a = (index & 1) ? mask[index / 2] & 15u : mask[index / 2] >> 4;
                    if (a) blend (x + g.x + i, y + g.y + j, colour, a == 15 ? 256 : a * 17);
                }
            x += g.advance;
        }
    }

    void right (int end, int y, const char* s, Ink colour, Type size = Label) const
    {
        text (end - measure (s, size), y, s, colour, size);
    }

    void center (int x, int y, int width, const char* s, Ink colour, Type size = Label) const
    {
        text (x + (width - measure (s, size)) / 2, y, s, colour, size);
    }
};
} // namespace anvil::sp16
