# brand_logo.py tex.bin clut.bin outdir preview.png
# Makes texpacks/branding: The Sims 2's title logo (T8 256x256 + its 16x16 CT32 CLUT, from a PS2X_TEX_DUMP run)
# with a "RECOMP" ribbon across the bottom. Both replacement files keep the original uploads' names (hashes).
# Also writes the high-resolution picture for the GPU renderer (HD x the texture, ribbon drawn at that size;
# the logo itself is the original upscaled, the game has no larger one).
import sys, os
from PIL import Image, ImageDraw, ImageFont

FONT = 'C:/Windows/Fonts/ariblk.ttf'
S = 4  # supersampling
HD = 4  # high-resolution picture: pixels per texel
BLUE_DARK = (14, 42, 112, 255)
BLUE = (30, 92, 196, 255)
WHITE = (255, 255, 255, 255)

def swz(i):
    return (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)

def load(tex_path, clut_path):
    tex = open(tex_path, 'rb').read()
    cl = open(clut_path, 'rb').read()
    raw = [tuple(cl[i*4:i*4+4]) for i in range(256)]
    pal = [raw[swz(i)] for i in range(256)]
    img = Image.new('RGBA', (256, 256))
    px = img.load()
    for y in range(256):
        for x in range(256):
            r, g, b, a = pal[tex[y * 256 + x]]
            px[x, 255 - y] = (r, g, b, min(255, a * 2))  # stored upside down, alpha 0x80 = opaque
    return img

def ribbon(size=256):
    S = globals()['S'] * size // 256  # coordinates below are in 256-texel units
    W, H = size * 4, size * 4
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = 30 * S, 208 * S, 226 * S, 246 * S
    d.rounded_rectangle((x0 - 3 * S, y0 - 3 * S, x1 + 3 * S, y1 + 3 * S), 12 * S, fill=WHITE)
    d.rounded_rectangle((x0, y0, x1, y1), 10 * S, fill=BLUE_DARK)
    d.rounded_rectangle((x0 + 3 * S, y0 + 3 * S, x1 - 3 * S, y1 - 3 * S), 8 * S, fill=BLUE)
    label = 'RECOMP'
    size_px = 34 * S
    while True:
        font = ImageFont.truetype(FONT, size_px)
        l, t, r, b = d.textbbox((0, 0), label, font=font, stroke_width=3 * S)
        if r - l <= (x1 - x0) - 14 * S or size_px < 10 * S:
            break
        size_px -= S
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    d.text((cx - (l + r) / 2, cy - (t + b) / 2), label, font=font, fill=WHITE,
           stroke_width=3 * S, stroke_fill=BLUE_DARK)
    layer = layer.rotate(4, resample=Image.BICUBIC, center=(W / 2, 227 * S))
    return layer.resize((size, size), Image.LANCZOS)

def encode(img):
    q = img.quantize(256, method=Image.Quantize.FASTOCTREE)
    pal = q.getpalette('RGBA')[:1024]
    pal += [0] * (1024 - len(pal))
    clut = [tuple(pal[i*4:i*4+4]) for i in range(256)]
    px = q.load()
    idx = bytearray(256 * 256)
    for y in range(256):
        for x in range(256):
            idx[(255 - y) * 256 + x] = px[x, y]
    raw = bytearray(1024)
    for i, (r, g, b, a) in enumerate(clut):
        j = swz(i)
        raw[j*4:j*4+4] = bytes((r, g, b, (a + 1) // 2))
    return bytes(idx), bytes(raw)

def main():
    tex_path, clut_path, outdir, preview = sys.argv[1:5]
    original = load(tex_path, clut_path)
    small = original.resize((206, 206), Image.LANCZOS)  # room for the ribbon under the "2"
    logo = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    logo.alpha_composite(small, (25, 2))
    logo.alpha_composite(ribbon())
    big = Image.new('RGBA', (256 * HD, 256 * HD), (0, 0, 0, 0))
    big.alpha_composite(original.resize((206 * HD, 206 * HD), Image.BICUBIC), (25 * HD, 2 * HD))
    big.alpha_composite(ribbon(256 * HD))
    hd_name = os.path.basename(tex_path)[:-4] + '_' + os.path.basename(clut_path).split('_')[0] + '.png'
    idx, raw = encode(logo)
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, os.path.basename(tex_path)), 'wb').write(idx)
    open(os.path.join(outdir, os.path.basename(clut_path)), 'wb').write(raw)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from kbm_glyphs import bleed
    bleed(big).transpose(Image.FLIP_TOP_BOTTOM).save(os.path.join(outdir, hd_name))  # row 0 = texel row 0
    # Preview of what the GS will see (decoded back from the written files).
    back = load(os.path.join(outdir, os.path.basename(tex_path)), os.path.join(outdir, os.path.basename(clut_path)))
    bg = Image.new('RGBA', back.size, (128, 128, 128, 255))
    bg.alpha_composite(back)
    bg.resize((512, 512), Image.NEAREST).save(preview)

main()
