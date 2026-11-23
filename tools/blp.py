# mod-ground-telegraphs - Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
"""BLP2 DXT5 writer (full mip chain), pure Python + Pillow. Usage: from blp import dxt5_blp; data = dxt5_blp(rgba_image)
DXT5 = 8-bit smooth alpha; the colour blocks are 4-colour DXT1 blocks (c0 >= c1, as DXT5 always reads them)."""
import struct

from PIL import Image


def _565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def _from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _block(px):
    """px: 16 (r, g, b) tuples, row-major -> 8 bytes DXT1 (4-colour mode)."""
    # axis = the channel pair with the largest spread; endpoints = extremes along luminance-weighted projection
    mr = [sum(p[i] for p in px) / 16.0 for i in range(3)]
    cov = [[sum((p[i] - mr[i]) * (p[j] - mr[j]) for p in px) for j in range(3)] for i in range(3)]
    ax = [1.0, 1.0, 1.0]
    for _ in range(4):   # power iteration for the principal axis
        nx = [sum(cov[i][j] * ax[j] for j in range(3)) for i in range(3)]
        n = max(abs(v) for v in nx) or 1.0
        ax = [v / n for v in nx]
    proj = [sum((p[i] - mr[i]) * ax[i] for i in range(3)) for p in px]
    lo, hi = px[proj.index(min(proj))], px[proj.index(max(proj))]
    c0, c1 = _565(hi), _565(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    a, b = _from565(c0), _from565(c1)
    pal = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)), tuple((a[i] + 2 * b[i]) // 3 for i in range(3))]
    idx = 0
    for k, p in enumerate(px):
        best = min(range(4), key=lambda j: (p[0] - pal[j][0]) ** 2 + (p[1] - pal[j][1]) ** 2 + (p[2] - pal[j][2]) ** 2)
        idx |= best << (2 * k)
    return struct.pack('<HHI', c0, c1, idx)


def _alpha_block(al):
    """al: 16 alpha values (0-255), row-major -> 8 bytes DXT5 alpha block (8-value mode, a0 > a1)."""
    a0, a1 = max(al), min(al)
    if a0 == a1:
        return struct.pack('<BB', a0, a1) + bytes(6)
    pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
    bits = 0
    for k, a in enumerate(al):
        best = min(range(8), key=lambda j: abs(a - pal[j]))
        bits |= best << (3 * k)
    return struct.pack('<BB', a0, a1) + bits.to_bytes(6, 'little')


def _dxt5(img):
    w, h = img.size
    W, H = max(4, w), max(4, h)
    if (W, H) != (w, h):
        big = Image.new('RGBA', (W, H))
        for y in range(0, H, h):
            for x in range(0, W, w):
                big.paste(img, (x, y))
        img = big
    d = img.load()
    out = bytearray()
    for by in range(0, H, 4):
        for bx in range(0, W, 4):
            px = [d[bx + x, by + y] for y in range(4) for x in range(4)]
            out += _alpha_block([p[3] for p in px]) + _block([p[:3] for p in px])
    return bytes(out)


def dxt5_blp(img):
    img = img.convert('RGBA')
    w, h = img.size
    mips, cur = [], img
    while True:
        mips.append(_dxt5(cur))
        if cur.size == (1, 1):
            break
        cur = cur.copy().resize((max(1, cur.size[0] // 2), max(1, cur.size[1] // 2)), Image.Resampling.BOX)
    offs, sizes, pos = [], [], 1172
    for m in mips:
        offs.append(pos); sizes.append(len(m)); pos += len(m)
    head = b'BLP2' + struct.pack('<IBBBBII', 1, 2, 8, 7, 1, w, h)
    head += struct.pack('<16I', *(offs + [0] * (16 - len(offs)))) + struct.pack('<16I', *(sizes + [0] * (16 - len(sizes))))
    return head + bytes(1024) + b''.join(mips)
