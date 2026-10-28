import sys, zlib, struct
def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
for f in sys.argv[1:]:
    d = open(f, 'rb').read(); nl = d.index(b'\n'); w, h = map(int, d[:nl].split())
    px = bytearray(d[nl+1:nl+1+w*h*4])
    for i in range(3, len(px), 4): px[i] = 255
    png(f[:-5] + '.png', w, h, bytes(px))
