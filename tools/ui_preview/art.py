"""Reduce the original chrome masthead to native pixels and a fixed RGB565 palette."""
from pathlib import Path
from PIL import Image, ImageFont, ImageDraw

root = Path(__file__).resolve().parents[2]
source = Image.open(root/'assets/explore/acid-print.png').convert('RGB')
# Bounds measured on the 1520 x 1035 design. Keep all five letters and the wire ring.
mast = source.crop((0, 0, 1284, 313)).resize((238, 55), Image.Resampling.LANCZOS)
inks = ['0c0710','20132e','3f2459','603986','9054ba','bc86da','7d738a','a89fae',
        'c6bfd0','dad6e0','f5f2e8','466600','719600','a7d700','c6f62c','e3fa98']
colours = [tuple(bytes.fromhex(c)) for c in inks]
swatches = Image.new('P',(1,1))
swatches.putpalette(bytes(c for rgb in colours for c in rgb)*16)
out = mast.quantize(palette=swatches,dither=Image.Dither.NONE)
# Collapse duplicated palette entries, preserve the knife edges and specular pin.
out = out.point([i%16 for i in range(256)])
out.putpalette(bytes(c for rgb in colours for c in rgb))
out.putpixel((180,11),10)
# Remove the tip of the mock's neighbouring position digit from the right edge.
out.paste(0,(235,39,238,47))
out.save(root/'assets/art/anvil-masthead.png')
im = Image.open(root/'assets/art/anvil-masthead.png')
words = []
for y in range(im.height):
    row = [im.getpixel((x,y)) for x in range(im.width)]
    words.append('{' + ','.join(f'0x{a*16+b:02x}' for a,b in zip(row[::2],row[1::2])) + '}')
lines = ['// Generated from assets/art/anvil-masthead.png by tools/ui_preview/art.py.',
         '#pragma once','#include <cstdint>','namespace hdui::art {',
         'inline constexpr uint32_t inks[] = {' + ','.join('0xff'+c for c in inks) + '};',
         'inline constexpr uint8_t masthead[][119] = {', ',\n'.join(words), '};','}']
(root/'src/ui/identity.h').write_text('\n'.join(lines)+'\n')

face = ImageFont.truetype(str(root/'assets/fonts/DepartureMono-Regular.otf'),11)
glyphs = []
for i in range(32,128):
    tile = Image.new('L',(7,12))
    ImageDraw.Draw(tile).text((0,-3),chr(i) if i < 127 else '·',font=face,fill=255)
    glyphs.append('{' + ','.join(hex(sum(1 << x for x in range(7) if tile.getpixel((x,y))>=90))
                                 for y in range(12)) + '}')
(root/'src/ui/font.h').write_text('\n'.join(['// Departure Mono. See licenses/OFL-departure-mono.txt.',
    '#pragma once','#include <cstdint>','namespace hdui::font {',
    'constexpr int glyphWidth = 7, glyphHeight = 12;', 'inline constexpr uint8_t glyphs[][12] = {',
    ',\n'.join(glyphs), '};','}'])+'\n')
