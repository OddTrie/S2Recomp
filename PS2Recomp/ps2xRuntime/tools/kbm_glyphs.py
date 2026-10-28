# kbm_glyphs.py dumpdir outdir preview.png [name:tex.bin:clut.bin:label ...]
# kbm_glyphs.py --hd outdir [name:tex.bin:clut.bin:label ...]   (only the high-resolution pictures)
# Makes the texpacks/kbm pack: keyboard keycaps in place of The Sims 2's PS2 button icons.
# dumpdir holds the icon uploads from a PS2X_TEX_DUMP run (dumpdir/pairs.txt names each texture's
# CLUT). Without specs it makes the icons below (keys as in ps2_pad.cpp).
import sys, os
from PIL import Image, ImageDraw, ImageFont

FONT = 'C:/Windows/Fonts/arialbd.ttf'
S = 8  # supersampling
HD = 8  # high-resolution pictures (GPU renderer): this many pixels per texel

def clut256(data):
    raw = [tuple(data[i*4:i*4+4]) for i in range(256)]
    return [raw[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)] for i in range(256)]

def keycap(img, d, box, label, small=False):
    x0, y0, x1, y1 = [v * S for v in box]
    r = (2 if small else 3) * S
    side = 2 * S if small else 3 * S
    d.rounded_rectangle((x0, y0, x1, y1), r, fill=(40, 40, 48, 255))                       # outline
    d.rounded_rectangle((x0 + S, y0 + S, x1 - S, y1 - S), r - S, fill=(150, 152, 165, 255))  # side
    top = (x0 + S + S // 2, y0 + S, x1 - S - S // 2, y1 - side)
    d.rounded_rectangle(top, r - S, fill=(236, 237, 243, 255))                               # top
    ink = (30, 32, 44, 255)
    cx, cy = (top[0] + top[2]) / 2, (top[1] + top[3]) / 2
    if label in ('up', 'down', 'left', 'right'):
        a = (top[3] - top[1]) * 0.36
        pts = {'up': [(cx, cy - a), (cx - a, cy + a * 0.7), (cx + a, cy + a * 0.7)],
               'down': [(cx, cy + a), (cx - a, cy - a * 0.7), (cx + a, cy - a * 0.7)],
               'left': [(cx - a, cy), (cx + a * 0.7, cy - a), (cx + a * 0.7, cy + a)],
               'right': [(cx + a, cy), (cx - a * 0.7, cy - a), (cx - a * 0.7, cy + a)]}[label]
        d.polygon(pts, fill=ink)
        return
    size = int((top[3] - top[1]) * (0.95 if small else 0.75 if len(label) == 1 else 0.5 if len(label) <= 3 else 0.36))
    while True:
        font = ImageFont.truetype(FONT, size)
        l, t, rr, b = d.textbbox((0, 0), label, font=font)
        if rr - l <= (top[2] - top[0]) - (S // 4 if small else 3 * S) or size < 2 * S:
            break
        size -= S // 2
    d.text((cx - (l + rr) / 2, cy - (t + b) / 2), label, font=font, fill=ink)

def cluster(img, d, keys):
    # Three 10 px keys side by side, all inside the 32x32 texture (half a pixel of margin, so no outline
    # gets cut off at the texture edge; the old layout ran from x=-1 to 33).
    keycap(img, d, (11, 0.5, 21, 15.5), keys[0], True)
    for i, ch in enumerate(keys[1:]):
        keycap(img, d, (0.5 + i * 10.5, 16, 10.5 + i * 10.5, 31.5), ch, True)

def draw_big(label, w=32, h=32):
    img = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if label in ('WASD', 'IJKL'):
        cluster(img, d, label)
    elif label == 'ARROWS':
        cluster(img, d, ('up', 'left', 'down', 'right'))
    elif w == 64:
        keycap(img, d, (6, 16, 58, 50), label)  # where the SELECT icon sits in its 64x64 texture
    elif len(label) > 3:
        keycap(img, d, (0, 6, 32, 26), label)
    else:
        keycap(img, d, (3, 3, 29, 29), label)
    return img

def draw(label, w=32, h=32):
    return draw_big(label, w, h).resize((w, h), Image.LANCZOS)

def bleed(img):
    # Transparent pixels take the colour of the nearest opaque ones, so filtering adds no dark fringe.
    import numpy as np
    a = np.asarray(img).astype(np.float32)
    al = a[..., 3:4] / 255.0
    acc, wt = a[..., :3] * al, al.copy()
    out = a.copy()
    todo = al[..., 0] == 0
    for _ in range(24):
        p_acc = np.pad(acc, ((1, 1), (1, 1), (0, 0)), mode='edge')
        p_wt = np.pad(wt, ((1, 1), (1, 1), (0, 0)), mode='edge')
        h, w = wt.shape[:2]
        acc = sum(p_acc[y:y + h, x:x + w] for y in range(3) for x in range(3))
        wt = sum(p_wt[y:y + h, x:x + w] for y in range(3) for x in range(3))
        got = todo & (wt[..., 0] > 0)
        out[got, :3] = acc[got] / wt[got]
        todo &= ~got
        if not todo.any():
            break
    return Image.fromarray(out.clip(0, 255).astype('uint8'), 'RGBA')

def hd_picture(label, w, h):
    # Row 0 = texel row 0: the icons are stored upside down.
    big = draw_big(label, w, h)
    if S != HD:
        big = big.resize((w * HD, h * HD), Image.LANCZOS)
    return bleed(big).transpose(Image.FLIP_TOP_BOTTOM)

def hd_name(tex, clut):
    # <texture upload>_<w>x<h>_<psm>_<CLUT upload hash>.png, both as the dump names them.
    return os.path.basename(tex)[:-4] + '_' + os.path.basename(clut).split('_')[0] + '.png'

def quantize(img):
    # Own 256-colour palette (PS2 alpha 0x80 = opaque).
    q = img.quantize(256, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette('RGBA')[:1024]
    pal += [0] * (1024 - len(pal))
    clut = [tuple(pal[i*4:i*4+4]) for i in range(256)]
    clut = [(r, g, b, (a + 1) // 2) for r, g, b, a in clut]
    px = q.load()
    w, h = img.size
    idx = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            idx[(h - 1 - y) * w + x] = px[x, y]  # stored upside down
    raw = bytearray(1024)
    for i, c in enumerate(clut):
        j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)
        raw[j*4:j*4+4] = bytes(c)
    return bytes(idx), clut, bytes(raw)

def render(idx, clut, w=32, h=32):
    im = Image.new('RGBA', (w, h))
    for y in range(h):
        for x in range(w):
            r, g, b, a = clut[idx[(h - 1 - y) * w + x]]
            im.putpixel((x, y), (r, g, b, min(255, a * 2)))
    return im

SPECS = ['cross:250c4221033679a2_32x32_19.bin:1a999e03845f1778_16x16_0.bin:Space', 'tri:952a2c96ef4ab339_32x32_19.bin:717d4d6c58ac3c31_16x16_0.bin:F', 'lstick:1b4895b919c389b0_32x32_19.bin:f658966d6cd9805f_16x16_0.bin:WASD', 'r1:58e1caa54936af3d_32x32_19.bin:c2346697af892351_16x16_0.bin:E', 'l1:e08e99c4ab91e816_32x32_19.bin:032523d1b9dd2ea9_16x16_0.bin:Q', 'dpad:778091a323c2aa6d_32x32_19.bin:5edffea00e2c5f80_16x16_0.bin:ARROWS', 'select:83884502a870870b_64x64_19.bin:aab41da04a757490_16x16_0.bin:Tab']

if __name__ == '__main__' and sys.argv[1] == '--hd':
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for spec in sys.argv[3:] or SPECS:
        name, tex, clut, label = spec.split(':', 3)
        w, h = map(int, tex.split('_')[1].split('x'))
        hd_picture(label, w, h).save(os.path.join(outdir, hd_name(tex, clut)))
elif __name__ == '__main__':
    dumpdir, outdir, preview = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    tiles = []
    for spec in sys.argv[4:] or SPECS:
        name, tex, clut, label = spec.split(':', 3)
        tex, clut = os.path.join(dumpdir, tex), os.path.join(dumpdir, clut)
        c = clut256(open(clut, 'rb').read())
        w, h = map(int, os.path.basename(tex).split('_')[1].split('x'))
        idx, newClut, rawClut = quantize(draw(label, w, h))
        open(os.path.join(outdir, os.path.basename(tex)), 'wb').write(idx)
        open(os.path.join(outdir, os.path.basename(clut)), 'wb').write(rawClut)
        hd_picture(label, w, h).save(os.path.join(outdir, hd_name(tex, clut)))
        tiles += [render(open(tex, 'rb').read(), c, w, h).resize((32, 32)), render(idx, newClut, w, h).resize((32, 32))]
    sheet = Image.new('RGBA', (len(tiles) * 32, 32), (120, 140, 200, 255))
    for i, t in enumerate(tiles):
        sheet.alpha_composite(t, (i * 32, 0))
    sheet.resize((sheet.width * 4, 128), Image.NEAREST).save(preview)
