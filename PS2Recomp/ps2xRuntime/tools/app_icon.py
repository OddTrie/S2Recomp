# app_icon.py
# Makes the program icon and the installer's wizard pictures from the branding texture pack's
# high-resolution logo (texpacks/branding, written by brand_logo.py):
#   ps2xRuntime/assets/s2recomp.ico   the exes' icon (window, taskbar, Explorer, shortcuts, Setup)
#   ps2xRuntime/assets/s2recomp.png   the Linux window and .desktop icon
#   installer/windows/wizard*.png     Inno Setup's wizard pictures
# 16-48 px leave out the "RECOMP" ribbon, which is unreadable that small.
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(HERE)
INSTALLER = os.path.join(RUNTIME, '..', 'installer', 'windows')
SOURCE = os.path.join(RUNTIME, 'texpacks', 'branding', '76e3b045c1a8492c_256x256_19_51f52905b7ed3998.png')
BLUE_DARK = (14, 42, 112)
BLUE = (30, 92, 196)

def square(img, pad=0.0):
    img = img.crop(img.getbbox())
    w, h = img.size
    s = int(max(w, h) * (1 + pad))
    out = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    out.alpha_composite(img, ((s - w) // 2, (s - h) // 2))
    return out

def gradient(w, h):
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(round(BLUE[i] + (BLUE_DARK[i] - BLUE[i]) * t) for i in range(3)) + (255,)
        for x in range(w):
            px[x, y] = c
    return img

def main():
    hd = Image.open(SOURCE).convert('RGBA').transpose(Image.FLIP_TOP_BOTTOM)  # stored upside down
    bare = hd.crop((0, 0, hd.width, 796))
    bare.paste((0, 0, 0, 0), (700, 780, hd.width, 796))  # the ribbon's top corner
    full, bare = square(hd), square(bare)

    small = [bare.resize((s, s), Image.LANCZOS) for s in (16, 24, 32, 48)]
    large = [full.resize((s, s), Image.LANCZOS) for s in (64, 128, 256)]
    os.makedirs(os.path.join(RUNTIME, 'assets'), exist_ok=True)
    large[-1].save(os.path.join(RUNTIME, 'assets', 's2recomp.ico'), sizes=[i.size for i in small + large],
                   append_images=small + large[:-1])
    large[-1].save(os.path.join(RUNTIME, 'assets', 's2recomp.png'))

    # Modern wizard: 164x314 side picture and 55x55 top-right picture at 100%, plus larger ones for high DPI.
    for scale in (1, 2):
        w, h = 164 * scale, 314 * scale
        side = gradient(w, h)
        logo = full.resize((w - 24 * scale, w - 24 * scale), Image.LANCZOS)
        side.alpha_composite(logo, (12 * scale, (h - logo.height) // 2))
        side.convert('RGB').save(os.path.join(INSTALLER, f'wizard_{scale}x.png'))
        bare.resize((55 * scale, 55 * scale), Image.LANCZOS).save(os.path.join(INSTALLER, f'wizard_small_{scale}x.png'))

if __name__ == '__main__':
    main()
