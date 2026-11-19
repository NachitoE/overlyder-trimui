#!/usr/bin/env python3
"""Generate alternative app icons that follow the stock TrimUI / CrossMix style.

Stock app icons (SystemTools, user_guide, MusicPlayer, ...) are 300x300 PNGs
with the actual tile in the upper ~180x180 area and transparent padding below.
The tile is a glossy, saturated rounded square with a thin light outline and a
single bold, centered glyph. This script renders several options in that
language so they can be compared and one can be promoted to app/icon.png.

Requires Pillow (pip install Pillow). Uses the bundled Material Icons font.
"""
import os
from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, 'app', 'resources', 'MaterialIcons-Regular.ttf')
OUTDIR = os.path.join(ROOT, 'docs', 'icon-options')
os.makedirs(OUTDIR, exist_ok=True)

S = 3  # supersample factor
W = 300 * S
# Tile box measured from the stock icons: x 60..240, y 19..199 in a 300px canvas.
TILE = (60 * S, 19 * S, 240 * S, 199 * S)
RADIUS = 40 * S
CENTER = (150 * S, 109 * S)

OPTIONS = [
    # key, file label, top color, bottom color, glyph codepoint, glyph color,
    # disc?, center accent color, glyph size (in final pixels)
    ('A', 'A · Guía (azul)', (79, 195, 247), (25, 118, 210),
     0xE429, (25, 118, 210), True, None, 92),
    ('B', 'B · Herramientas (rojo)', (245, 82, 82), (194, 24, 31),
     0xE429, (248, 249, 252), False, (214, 32, 45), 100),
    ('C', 'C · Consola (mando)', (45, 212, 191), (14, 143, 139),
     0xEA28, (248, 249, 252), False, None, 104),
    ('D', 'D · Sonido (eco)', (167, 107, 239), (106, 63, 192),
     0xE1B8, (248, 249, 252), False, None, 104),
    ('E', 'E · Switch oscuro', (52, 56, 72), (20, 22, 31),
     0xE429, (248, 249, 252), False, (230, 32, 55), 100),
]

font_cache = {}


def font(px):
    px = int(px * S)
    if px not in font_cache:
        font_cache[px] = ImageFont.truetype(FONT, px)
    return font_cache[px]


def rounded_mask(size, box, radius):
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius, fill=255)
    return m


def blend(img, draw_fn):
    """Draw semi-transparent shapes on a separate layer, then composite them."""
    layer = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    img.alpha_composite(layer)


def render(top, bottom, cp, glyph_rgb, disc, accent, glyph_px):
    img = Image.new('RGBA', (W, W), (0, 0, 0, 0))

    # Very subtle drop shadow; the stock tiles barely cast one.
    shadow = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    box = (TILE[0] + 4 * S, TILE[1] + 4 * S, TILE[2] - 4 * S, TILE[3] + 4 * S)
    sd.rounded_rectangle(box, RADIUS, fill=(0, 0, 0, 55))
    shadow = shadow.filter(ImageFilter.GaussianBlur(3 * S))
    img.alpha_composite(shadow)

    # Glossy vertical gradient tile.
    grad = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    gp = grad.load()
    for y in range(TILE[1], TILE[3]):
        t = (y - TILE[1]) / (TILE[3] - TILE[1] - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(TILE[0], TILE[2]):
            gp[x, y] = (*c, 255)
    img.paste(grad, (0, 0), rounded_mask((W, W), TILE, RADIUS))

    # Gloss: a soft sheen fading in from the top, a faint shade at the bottom.
    def shade(y0, y1, a0, a1, color):
        band = Image.new('RGBA', (TILE[2] - TILE[0], TILE[3] - TILE[1]), (0, 0, 0, 0))
        p = band.load()
        for y in range(max(0, y0 - TILE[1]), min(band.height, y1 - TILE[1])):
            t = (y - (y0 - TILE[1])) / max(1, (y1 - y0))
            a = int(a0 + (a1 - a0) * t)
            for x in range(band.width):
                p[x, y] = (*color, a)
        mask = rounded_mask(band.size, (0, 0, band.width - 1, band.height - 1), RADIUS)
        band.putalpha(ImageChops.multiply(band.getchannel('A'), mask))
        img.alpha_composite(band, (TILE[0], TILE[1]))

    shade(TILE[1], TILE[1] + 66 * S, 70, 0, (255, 255, 255))
    shade(TILE[3] - 44 * S, TILE[3], 0, 34, (0, 0, 0))

    d = ImageDraw.Draw(img)
    cx, cy = CENTER
    if accent:
        r = 27 * S
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(*accent, 255))
    if disc:
        r = 57 * S
        blend(img, lambda l: l.ellipse(
            (cx - r, cy - r + 5 * S, cx + r, cy + r + 5 * S),
            outline=(0, 0, 0, 30), width=5 * S))
        d = ImageDraw.Draw(img)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(248, 249, 252, 255))
        blend(img, lambda l: l.ellipse(
            (cx - r + 3 * S, cy - r + 3 * S, cx + r - 3 * S, cy + r - 3 * S),
            outline=(255, 255, 255, 150), width=2 * S))
        d = ImageDraw.Draw(img)

    # Thin light outline, like the stock tiles.
    blend(img, lambda l: l.rounded_rectangle(
        TILE, RADIUS, outline=(255, 255, 255, 170), width=2 * S))
    d = ImageDraw.Draw(img)

    glyph = chr(cp)
    f = font(glyph_px)
    bbox = d.textbbox((0, 0), glyph, font=f)
    gx, gy = cx - (bbox[2] - bbox[0]) / 2 - bbox[0], cy - (bbox[3] - bbox[1]) / 2 - bbox[1]
    if not disc:
        # Subtle dark offset copy gives the glyph a bit of depth.
        blend(img, lambda l: l.text((gx + 2 * S, gy + 2 * S), glyph,
                                    font=f, fill=(0, 0, 0, 60)))
        d = ImageDraw.Draw(img)
    d.text((gx, gy), glyph, font=f, fill=(*glyph_rgb, 255))

    return img.resize((300, 300), Image.LANCZOS)


def label(text, size=22):
    for path in (r'C:\Windows\Fonts\segoeui.ttf', 'arial.ttf'):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    renders = []
    for key, label_text, top, bottom, cp, gcol, disc, accent, gpx in OPTIONS:
        im = render(top, bottom, cp, gcol, disc, accent, gpx)
        path = os.path.join(OUTDIR, f'option-{key}.png')
        im.save(path)
        renders.append((key, label_text, im))
        print(path, os.path.getsize(path), 'bytes')

    # Comparison sheet: options on top, real stock icons below for reference.
    sheet = Image.new('RGB', (1560, 700), (32, 34, 43))
    d = ImageDraw.Draw(sheet)
    d.text((40, 24), 'Opciones de icono - estilo TrimUI / CrossMix',
           font=label(30), fill=(240, 240, 245))
    f_lbl = label(19)
    for i, (key, text, im) in enumerate(renders):
        x = 40 + i * 300
        sheet.paste(im, (x, 80), im)
        d.text((x + 150, 390), text, font=f_lbl, fill=(225, 225, 232), anchor='mm')

    # Optional: drop real stock icons here to include them in the sheet.
    refdir = os.path.join(OUTDIR, 'reference')
    refs = [
        ('SystemTools', os.path.join(refdir, 'SystemTools.png')),
        ('User Guide', os.path.join(refdir, 'user_guide.png')),
        ('MusicPlayer', os.path.join(refdir, 'MusicPlayer.png')),
    ]
    d.text((40, 445), 'Referencia: iconos stock reales',
           font=label(24), fill=(160, 200, 240))
    for i, (name, path) in enumerate(refs):
        if os.path.exists(path):
            im = Image.open(path).convert('RGBA').resize((190, 190), Image.LANCZOS)
            x = 40 + i * 300
            sheet.paste(im, (x, 485), im)
            d.text((x + 95, 675), name, font=f_lbl, fill=(200, 200, 210), anchor='mm')

    sheet_path = os.path.join(OUTDIR, 'preview-grid.png')
    sheet.save(sheet_path)
    print(sheet_path, os.path.getsize(sheet_path), 'bytes')


if __name__ == '__main__':
    main()
