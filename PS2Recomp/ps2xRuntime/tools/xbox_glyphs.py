# xbox_glyphs.py outdir preview.png [name:tex.bin:clut.bin:label ...]
# Makes the texpacks/xbox pack: Xbox controller buttons in place of The Sims 2's PS2 button icons, shown
# while a non-PlayStation controller is in use (ps2_pad.cpp). Same icon uploads as kbm_glyphs.py (its
# SPECS name them); labels A B X Y LB RB LT RT LS RS DPAD VIEW MENU. No dump is needed: each file is named
# after the upload it replaces and holds new texels and a new CLUT.
import sys, os
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kbm_glyphs import FONT, S, HD, SPECS, bleed, hd_name, quantize, render

FACE = {'A': (96, 178, 56), 'B': (214, 58, 50), 'X': (44, 118, 222), 'Y': (236, 186, 30)}
DARK = (40, 42, 50, 255)
EDGE = (16, 16, 20, 255)
WHITE = (246, 246, 250, 255)

def text(d, cx, cy, label, size, fill, box_w):
    while True:
        font = ImageFont.truetype(FONT, size)
        l, t, r, b = d.textbbox((0, 0), label, font=font)
        if r - l <= box_w or size < 2 * S:
            break
        size -= S // 2
    d.text((cx - (l + r) / 2, cy - (t + b) / 2), label, font=font, fill=fill)

def face(d, box, label):
    x0, y0, x1, y1 = [v * S for v in box]
    d.ellipse((x0, y0, x1, y1), fill=EDGE)
    d.ellipse((x0 + S, y0 + S, x1 - S, y1 - S), fill=FACE[label] + (255,))
    ink = (34, 30, 18, 255) if label == 'Y' else WHITE
    text(d, (x0 + x1) / 2, (y0 + y1) / 2 + S / 4, label, int((y1 - y0) * 0.62), ink, x1 - x0)

def shoulder(d, box, label):
    # Bumpers: a flat bar; triggers: a taller shape rounded at the top.
    x0, y0, x1, y1 = [v * S for v in box]
    if label in ('LT', 'RT'):
        x0, x1 = x0 + 3 * S, x1 - 3 * S
        d.rounded_rectangle((x0, y0, x1, y1), 7 * S, fill=EDGE)
        d.rounded_rectangle((x0 + S, y0 + S, x1 - S, y1 - S), 6 * S, fill=DARK)
    else:
        y0, y1 = y0 + 5 * S, y1 - 5 * S
        d.rounded_rectangle((x0, y0, x1, y1), 4 * S, fill=EDGE)
        d.rounded_rectangle((x0 + S, y0 + S, x1 - S, y1 - S), 3 * S, fill=DARK)
    text(d, (x0 + x1) / 2, (y0 + y1) / 2, label, int((y1 - y0) * 0.55), WHITE, x1 - x0 - 4 * S)

def stick(d, box, label):
    x0, y0, x1, y1 = [v * S for v in box]
    d.ellipse((x0, y0, x1, y1), fill=EDGE)
    d.ellipse((x0 + S, y0 + S, x1 - S, y1 - S), fill=DARK)
    m = (x1 - x0) * 0.17
    d.ellipse((x0 + m, y0 + m, x1 - m, y1 - m), outline=(120, 122, 132, 255), width=S)
    text(d, (x0 + x1) / 2, (y0 + y1) / 2, label, int((y1 - y0) * 0.36), WHITE, x1 - x0 - 8 * S)

def dpad(d, box):
    x0, y0, x1, y1 = [v * S for v in box]
    cx, cy, w = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) * 0.36
    for o, fill in ((0, EDGE), (S, DARK)):
        d.rounded_rectangle((cx - w / 2 + o, y0 + o, cx + w / 2 - o, y1 - o), 2 * S, fill=fill)
        d.rounded_rectangle((x0 + o, cy - w / 2 + o, x1 - o, cy + w / 2 - o), 2 * S, fill=fill)
    a = w * 0.28
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        tx, ty = cx + dx * (x1 - x0) * 0.33, cy + dy * (y1 - y0) * 0.33
        px, py = -dy, dx  # perpendicular
        d.polygon([(tx + dx * a, ty + dy * a), (tx - dx * a * 0.6 + px * a, ty - dy * a * 0.6 + py * a),
                   (tx - dx * a * 0.6 - px * a, ty - dy * a * 0.6 - py * a)], fill=WHITE)

def menu_button(d, box, label):
    # View (two overlapping windows) or Menu (three lines) on a small dark circle.
    x0, y0, x1, y1 = [v * S for v in box]
    d.ellipse((x0, y0, x1, y1), fill=EDGE)
    d.ellipse((x0 + S, y0 + S, x1 - S, y1 - S), fill=DARK)
    cx, cy, u = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 12
    if label == 'VIEW':
        d.rounded_rectangle((cx - 3.2 * u, cy - 2.6 * u, cx + 1.2 * u, cy + 0.8 * u), u * 0.6, outline=WHITE, width=max(S, int(u * 0.8)))
        d.rounded_rectangle((cx - 1.2 * u, cy - 0.8 * u, cx + 3.2 * u, cy + 2.6 * u), u * 0.6, fill=WHITE)
    else:
        for k in (-1.6, 0, 1.6):
            d.rounded_rectangle((cx - 3 * u, cy + k * u - 0.5 * u, cx + 3 * u, cy + k * u + 0.5 * u), u * 0.4, fill=WHITE)

def draw_big(label, w=32, h=32):
    img = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    box = (15, 16, 49, 50) if w == 64 else (2, 2, 30, 30)  # where the SELECT icon sits in its 64x64 texture
    if label in FACE:
        face(d, box, label)
    elif label in ('LB', 'RB', 'LT', 'RT'):
        shoulder(d, (0.5, 4, 31.5, 28) if w == 32 else box, label)
    elif label in ('LS', 'RS'):
        stick(d, box, label)
    elif label == 'DPAD':
        dpad(d, box)
    else:
        menu_button(d, box, label)
    return img

def draw(label, w, h):
    return draw_big(label, w, h).resize((w, h), Image.LANCZOS)

def hd_picture(label, w, h):
    big = draw_big(label, w, h)
    if S != HD:
        big = big.resize((w * HD, h * HD), Image.LANCZOS)
    return bleed(big).transpose(Image.FLIP_TOP_BOTTOM)  # row 0 = texel row 0: stored upside down

# The kbm icons as Xbox buttons (Cross = A, Triangle = Y, L1/R1 = LB/RB, Select = View).
XBOX = {'cross': 'A', 'tri': 'Y', 'lstick': 'LS', 'r1': 'RB', 'l1': 'LB', 'dpad': 'DPAD', 'select': 'VIEW'}

if __name__ == '__main__':
    outdir, preview = sys.argv[1], sys.argv[2]
    specs = sys.argv[3:] or [':'.join(s.split(':')[:3] + [XBOX[s.split(':')[0]]]) for s in SPECS]
    os.makedirs(outdir, exist_ok=True)
    tiles = []
    for spec in specs:
        name, tex, clut, label = spec.split(':', 3)
        w, h = map(int, tex.split('_')[1].split('x'))
        idx, newClut, rawClut = quantize(draw(label, w, h))
        open(os.path.join(outdir, tex), 'wb').write(idx)
        open(os.path.join(outdir, clut), 'wb').write(rawClut)
        hd_picture(label, w, h).save(os.path.join(outdir, hd_name(tex, clut)))
        tiles += [render(idx, newClut, w, h).resize((64, 64), Image.NEAREST), hd_picture(label, w, h).transpose(Image.FLIP_TOP_BOTTOM).resize((64, 64), Image.LANCZOS)]
    sheet = Image.new('RGBA', (len(tiles) * 64, 64), (120, 140, 200, 255))
    for i, t in enumerate(tiles):
        sheet.alpha_composite(t, (i * 64, 0))
    sheet.resize((sheet.width * 2, 128), Image.NEAREST).save(preview)
