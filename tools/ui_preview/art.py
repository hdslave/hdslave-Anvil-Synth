"""Reduce the original chrome masthead to native pixels and a fixed RGB565 palette."""
from math import floor, ceil, cos, hypot, sin
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
# Lift the baked highlight off the print plate. The L's narrow silver bevel
# starts on row 14. The live torus supplies the wires behind the exposed tip.
im.paste(0, (152,0,199,14))
im.paste(0, (175,14,186,24))
words = []
for y in range(im.height):
    row = [im.getpixel((x,y)) for x in range(im.width)]
    words.append('{' + ','.join(f'0x{a*16+b:02x}' for a,b in zip(row[::2],row[1::2])) + '}')
# Opaque pieces that cross in front of the wire torus, in masthead pixels.
# The lettering occludes the wires even along its dark edges.
front = Image.new('1', im.size)
draw = ImageDraw.Draw(front)
draw.polygon([(0,0),(158,0),(158,13),(174,14),(169,20),(166,25),
              (160,32),(158,36),(164,38),(183,38),(183,39),(170,53),
              (167,54),(159,48),(150,47),(0,54)], fill=1)
# Extract the visible folds from the original print. Each column has a single
# vertical run of cloth; its black checks belong to the ribbon too.
ribbon = Image.new('1', im.size)
cloth = ImageDraw.Draw(ribbon)
cloth.polygon([(0,0),(42,0),(46,2),(53,7),(59,11),(58,12),(52,14),
               (47,16),(41,19),(35,22),(32,23),(25,22),(19,20),(10,20),
               (6,21),(2,23),(0,25)], fill=1)
cloth.polygon([(0,18),(29,18),(34,21),(34,23),(31,24),(23,23),(18,22),
               (8,22),(3,24),(0,26)], fill=1)
cloth.polygon([(36,0),(45,0),(57,8),(52,8)], fill=1)
# The short folds appear through the V and below the I. Trace their rows
# at the type's edge to keep the chrome tips on the stationary layer.
for first_y, runs in [(36, [(94,95),(92,97),(92,99),(93,101),(91,101),(90,101),
                           (90,101),(90,101),(91,100),(93,99),(95,99),(98,99)]),
                      (44, [(112,119),(111,148),(108,148),(106,148),(106,146),
                           (105,144),(106,141),(111,137),(117,132)])]:
    for y,(left,right) in enumerate(runs, first_y):
        cloth.line((left,y,right,y), fill=1)
cloth.polygon([(161,36),(168,31),(174,28),(180,24),(188,22),(193,21),
               (199,21),(205,23),(209,25),(214,29),(213,32),(208,36),
               (205,38),(203,36),(198,32),(194,32),(188,34),(183,37),
               (180,38),(174,37),(167,37),(162,37)], fill=1)
cloth.line((160,38,183,38), fill=0)
cloth.line((179,37,187,37), fill=1)

def ease(t):
    t = max(0, min(1, t))
    return t*t*(3-2*t)

# Trace the fabric in material space, extending the edges under the lettering.
# Stations trace the upper and lower edges in the resting pose.
stations = [
    (0, 0, 25), (6, -3, 21), (14, -5, 19.8), (21, -5, 20.7),
    (32, -4, 24), (42, -0.5, 27.8), (53, 6.7, 31.8), (60, 11.5, 35),
    (88, 34.5, 42), (95, 36.7, 45), (103, 40.5, 48.4), (112, 43.2, 51.3),
    (122, 45, 52.2), (131, 45.7, 52), (141, 45.4, 49.2), (150, 43.5, 45.9),
    (161, 36.8, 44.6), (173, 28.1, 40.1), (184, 23.2, 36.0), (193, 21.1, 31.8),
    (199, 21.4, 32.3), (205, 23.0, 36.5), (210, 25.7, 39.3), (215, 30.1, 42),
    (220, 34.2, 45.7), (226, 37.9, 48.9), (232, 39.9, 50.4), (239, 40.2, 50.6),
    (247, 39.1, 49.7), (258, 35.5, 46.7), (267, 33.2, 44.9), (275, 32.7, 44.6),
    (284, 33.6, 46.1), (294, 36.5, 48.8), (303, 38.7, 50.5), (311, 39.1, 50.4),
    (320, 37.3, 48.2),
]


def edge_samples(edge):
    # Interpolate each traced edge with shared tangents. A crest or trough
    # has a flat tangent so a turning fold never overshoots the tracing.
    slopes = [(b[edge]-a[edge])/(b[0]-a[0]) for a,b in zip(stations, stations[1:])]
    tangents = [slopes[0]]
    for left,right in zip(slopes, slopes[1:]):
        tangents.append(2*left*right/(left+right) if left*right > 0 else 0)
    tangents.append(slopes[-1])
    values = []
    for i,(a,b) in enumerate(zip(stations, stations[1:])):
        width = b[0]-a[0]
        for x in range(width):
            t = x/width
            blend = ease(t)
            values.append((1-blend)*a[edge] + blend*b[edge] +
                          width*t*(1-t)*((1-t)*tangents[i] - t*tangents[i+1]))
    return values + [stations[-1][edge]]


tops, bottoms = edge_samples(1), edge_samples(2)
centres = [(a+b)/2 for a,b in zip(tops, bottoms)]
# Space checks by distance down the fabric. At a narrow turn the cross-grain
# foreshortens, while the length of a check stays legible at native size.
rows = 3
distance = [0.0]
for x,(a,b) in enumerate(zip(centres, centres[1:])):
    step = hypot(1, b-a)
    width = (bottoms[x]-tops[x] + bottoms[x+1]-tops[x+1]) / (2*step)
    pitch = max(5.6, width/rows)
    distance.append(distance[-1] + step/pitch)


def wave_gain(x):
    # Each fold is held where it disappears behind the type. The free edge
    # across the torus gets the full movement.
    if x < 60:
        return ease((60-x)/24)
    elif x < 103:
        return .55 * ease((x-88)/5) * ease((103-x)/6)
    elif x < 155:
        return .8 * ease((x-103)/10) * ease((151-x)/9)
    else:
        return ease((x-160)/25)


columns = []
# The print starts at screen x=1. Carry the continuous material coordinates
# through the aperture and off the rightmost pixel of the 320-pixel panel.
for x in range(320 - 1):
    if x < 200:
        ys = [y for y in range(im.height) if ribbon.getpixel((x,y))]
        if not ys:
            columns.append('{}')
            continue
        visible_top, visible_end = min(ys), max(ys)+1
    top,bottom = tops[x],bottoms[x]
    prev = max(0,x-1)
    du = (distance[x+1]-distance[prev]) / (x+1-prev)
    # A cross-seam spans a whole cell. Use its neighbourhood to turn the
    # seam smoothly across a broad fold, including its pinned ends.
    radius = max(2, round((bottom-top)/rows))
    left,right = max(0,x-radius), min(len(centres)-1,x+radius)
    slope = (centres[right]-centres[left]) / (right-left)
    gain = wave_gain(x)
    gain_slope = (wave_gain(right)-wave_gain(left)) / (right-left)
    if x >= 200:
        visible_top, visible_end = floor(top), ceil(bottom)+1
    fields = (visible_top, visible_end, round(255*gain), round(top*16), round(bottom*16),
              round(distance[x]*256), round(slope*128), round(du*256), round(gain_slope*1024))
    columns.append('{' + ','.join(map(str,fields)) + '}')
cover = []
for y in range(im.height):
    row = [sum(front.getpixel((x+b,y)) << b for b in range(8) if x+b < im.width)
           for x in range(159, im.width, 8)]
    cover.append('{' + ','.join(f'0x{v:02x}' for v in row) + '}')


def unit(value):
    return min(1, max(0, value))


def contour_coverage(x, y):
    # The bands across the lettering curl into an elliptical fingerprint.
    bend = y + 3.6*sin(x*.072 + y*.028) + 7.5*sin(x*.034 - .6 + y*.039) + 3.5*sin(x*.109 - y*.045)
    band = bend*.110 + x*.024 + sin(x*.027 + y*.029)
    dx, dy = x-202, y-30
    radius = hypot(.82*dx - .48*dy, 1.30*(.45*dx + 1.03*dy))
    ring = (radius + 2.6*sin(dx*.081 + dy*.073) + 1.3*sin(dy*.15 - dx*.026))*.141

    def line(f):
        return unit((.18 - abs(f-round(f)))*6.4)

    strata = line(band) * unit(1.3 + .9*cos(x*.036 + y*.055))
    rings = line(ring) * unit((radius-25)/14) * unit((83-radius)/22)
    blend = ease((x-133)/30)
    readout = 1 - unit((x-228)/14) * (1 - unit((y-30)/10))
    return ((1-blend)*strata + blend*rings) * readout * unit((59-y)/4)


def contour_ink(x, y):
    samples = ((-.27,-.27), (.27,-.27), (-.27,.27), (.27,.27))
    return round(3 * sum(contour_coverage(x+dx, y+dy) for dx,dy in samples) / len(samples))


# The extra plate margin lets the sampler drift past every screen edge.
contours = []
for y in range(-3,62):
    groups = [sum(contour_ink(x+i,y) << (2*i) for i in range(4)) for x in range(-4,324,4)]
    contours.append('{' + ','.join(f'0x{value:02x}' for value in groups) + '}')

def colour_ramp(name, stops):
    colours = []
    for (start,a),(end,b) in zip(stops,stops[1:]):
        first,last = bytes.fromhex(a),bytes.fromhex(b)
        for x in range(start,end):
            t = (x-start)/(end-start)
            rgb = bytes(round(v+(w-v)*t) for v,w in zip(first,last))
            colours.append('0xff'+rgb.hex())
    colours.append('0xff'+stops[-1][1])
    rows = [','.join(colours[i:i+10]) for i in range(0,len(colours),10)]
    return 'inline constexpr uint32_t '+name+'[] = {\n'+',\n'.join(rows)+'};'

lines = ['// Generated from assets/art/anvil-masthead.png by tools/ui_preview/art.py.',
         '#pragma once','#include <cstdint>','namespace hdui::art {',
         'inline constexpr uint32_t inks[] = {' + ','.join('0xff'+c for c in inks) + '};',
         colour_ramp('foil', [(0,inks[3]), (25,inks[4]), (48,inks[5]), (64,inks[4]),
                              (76,inks[8]), (92,inks[4]), (111,inks[3])]),
         colour_ramp('fabric', [(0,inks[9]), (36,inks[10]), (94,inks[8]), (153,inks[10]),
                                (185,inks[9]), (223,inks[10]), (266,inks[8]), (319,inks[10])]),
         'inline constexpr uint8_t contours[][82] = {', ',\n'.join(contours), '};',
         'inline constexpr uint8_t masthead[][119] = {', ',\n'.join(words), '};',
         'inline constexpr uint8_t torusCover[][10] = {', ',\n'.join(cover), '};',
         f'inline constexpr int checkerRows = {rows};',
         'struct RibbonSpan { uint8_t top, end, gain; int16_t edgeTop16, edgeBottom16, u256, slope128; uint8_t du256; int8_t gainSlope1024; };', 'inline constexpr RibbonSpan ribbon[] = {',
         ',\n'.join(','.join(columns[x:x+12]) for x in range(0, len(columns), 12)), '};','}']
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
