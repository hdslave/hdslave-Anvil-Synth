"""Build native RGB565 proofs and a motion loop from the plug-in's own C++ renderer."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[2]
out = root/'assets/previews'
frames = root/'build/ui-preview/frames'
chrome = Image.open(out/'melodee/1.png').convert('RGB').crop((0,0,320,22))
face = str(root/'assets/fonts/DepartureMono-Regular.otf')
labels = [('playing','FM Perc / playing'), ('six-steps','Ping-Pong Toms / six steps'),
          ('vco','VCO / editing FM'), ('mix','Mix / Noise Hat'), ('vcf','VCF / high pass'),
          ('vca','VCA / slow attack'), ('patch','Patch / Random Zap'), ('seq','Seq / swing'),
          ('shift','Shift controls'), ('pickup','K2 awaiting pickup'), ('stopped','Stopped / Pad Kit'),
          ('buttons','Seq / trigger')]


def panel(im):
    channels = []
    for ch,bits in zip(im.convert('RGB').split(),(5,6,5)):
        values = [n >> (8-bits) for n in range(256)]
        channels.append(ch.point([(n << (8-bits)) | (n >> (2*bits-8)) for n in values]))
    return Image.merge('RGB',channels)


def composite(page):
    im = Image.new('RGB',(320,240))
    im.paste(panel(chrome),(0,0))
    im.paste(page,(0,22))
    return im


def scale(im,s):
    return im.resize((im.width*s,im.height*s), Image.Resampling.NEAREST)


sheet = Image.new('RGB',(1972,4*534+16),'#0c0710')
draw = ImageDraw.Draw(sheet)
font = ImageFont.truetype(face,22)
for i,(name,label) in enumerate(labels):
    rgb = Image.open(frames/(name+'.ppm'))
    p = panel(rgb)
    dest = out/'states'/name
    dest.mkdir(parents=True,exist_ok=True)
    rgb.save(dest/'native.png')
    p.save(dest/'panel.png')
    scale(rgb,3).save(dest/'3x.png')
    page = composite(p)
    page.save(dest/'composite.png')
    x,y = 12+(i%3)*654,12+(i//3)*534
    sheet.paste(scale(page,2),(x,y))
    draw.text((x,y+486),label,font=font,fill='#d5b5ee')
sheet.save(out/'contact-sheet-2x.png')
loop = [composite(panel(Image.open(frames/f'step-{i}.ppm'))) for i in range(1,9)]
strip = Image.new('RGB',(4*320,240))
for i,frame in enumerate(loop[2:6]):
    strip.paste(frame,(i*320,0))
strip.save(out/'step-strip.png')
motion = [scale(im,2) for im in loop]
motion[0].save(out/'anvil-playing.gif',save_all=True,append_images=motion[1:],duration=125,loop=0,disposal=1,optimize=False)
