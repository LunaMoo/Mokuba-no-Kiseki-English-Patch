#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make an HD replacement for the font atlas of the PPSSPP edition of the Mokuba no Kiseki patch.

A ready-made HD atlas ships with the release (hd_font/textures/). Use this script only to
rebuild it, e.g. with another font or scale.

1. Patch the game with mokuba_english_ppsspp.patch and run it in PPSSPP with texture dumping on.
2. Find the dumped 512x512 font atlas (PSP/TEXTURES/NPJH50442/new/<hash>.png: white glyphs in
   16x16 cells, top-left of an otherwise empty texture).
3. python make_hd_font.py <dumped atlas.png> [--scale 4] [--font X.ttf/.ttc --index N] [--out DIR]
   -> DIR/<same name>.png; copy it into PSP/TEXTURES/NPJH50442/ next to textures.ini.

Each HD glyph is fitted to the original: same ink box inside its cell, and its stroke weight is
searched so that, scaled back down to 16x16, it matches the game's own glyph as closely as
possible. Needs Python 3, Pillow and numpy; cellmap.json must be next to this script.
"""
import argparse, json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
# heavy sans faces close to the game's FOT-NewRodin DB; the first one found is used
LATIN_FONTS = [                # Helvetica-like bold for Latin-1, like the game's LT6SSB
    ('/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf', 0),
    (os.path.join(HERE, 'LiberationSans-Bold.ttf'), 0),
    ('C:/Windows/Fonts/arialbd.ttf', 0),
]
DEFAULT_FONTS = [
    ('/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc', 0),
    (os.path.join(HERE, 'NotoSansJP-Bold.ttf'), 0),
    ('C:/Windows/Fonts/NotoSansJP-Bold.ttf', 0),
    ('C:/Windows/Fonts/YuGothB.ttc', 0),
    ('C:/Windows/Fonts/meiryob.ttc', 0),
]
R = 192                       # master render size
SUBST = {'\u00ad': '-'}       # soft hyphen has no outline in most fonts
STROKES = [0, 2, 4, 6, 8, 10, 13, 16]   # extra stroke at master size


def master(font, ch, stroke):
    im = Image.new('L', (R * 3, R * 3), 0)
    ImageDraw.Draw(im).text((R, R), ch, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
    b = im.getbbox()
    return im.crop(b) if b else None


def edges(a):
    """sub-pixel ink extents of a 16x16 alpha cell: (x0, y0, x1, y1) in pixels."""
    a = a.astype(float) / 255
    def ext(p):
        nz = np.nonzero(p > 0.05)[0]
        if len(nz) == 0: return None
        i0, i1 = nz[0], nz[-1]
        return i0 + 1 - min(1, p[i0]), i1 + min(1, p[i1])
    cx, cy = ext(a.max(0)), ext(a.max(1))
    if not cx or not cy: return None
    return cx[0], cy[0], cx[1], cy[1]


def place(g, box, cell, S, keep):
    """resize master glyph g into box (cell pixels, float) on an HD cell canvas.
    keep: 0 = stretch to the box, 1 = keep the font's aspect (fit, centred). Returns (canvas, distortion)."""
    x0, y0, x1, y1 = [v * S for v in box]
    bw, bh = x1 - x0, y1 - y0
    sx, sy = bw / g.size[0], bh / g.size[1]
    if keep:
        f = min(sx, sy)
        if f / max(sx, sy) < 0.8: return None, 0     # would leave the glyph much smaller than the original
        w, h = g.size[0] * f, g.size[1] * f
        x0 += (bw - w) / 2; y0 += (bh - h) / 2; sx = sy = f
    else:
        w, h = bw, bh
    gi = g.resize((max(1, round(w)), max(1, round(h))), Image.LANCZOS)
    c = Image.new('L', (cell * S, cell * S), 0)
    c.paste(gi, (round(x0), round(y0)))
    return c, abs(np.log(sx / sy))


def down(c, S):
    a = np.asarray(c, dtype=float)
    n = a.shape[0] // S
    return a.reshape(n, S, n, S).mean((1, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('atlas'); ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--font'); ap.add_argument('--index', type=int, default=0)
    ap.add_argument('--latin-font', help='font for Latin-1 characters (default: Arial/Liberation Sans Bold)')
    ap.add_argument('--out', default=os.path.join(HERE, 'hd'))
    ap.add_argument('--cellmap', default=os.path.join(HERE, 'cellmap.json'))
    ap.add_argument('--preview', help='also write a side-by-side preview PNG')
    a = ap.parse_args()
    cm = json.load(open(a.cellmap, encoding='utf-8'))
    table, cell, cols = cm['table'], cm['cell'], cm['cols']
    fp = (a.font, a.index) if a.font else next((f for f in DEFAULT_FONTS if os.path.exists(f[0])), None)
    if not fp: sys.exit('no suitable font found, pass --font (a bold/heavy sans, e.g. Noto Sans JP Bold)')
    font = ImageFont.truetype(fp[0], R, index=fp[1])
    lp = (a.latin_font, 0) if a.latin_font else next((f for f in LATIN_FONTS if os.path.exists(f[0])), fp)
    lfont = ImageFont.truetype(lp[0], R, index=lp[1])
    print('fonts:', ' '.join(lfont.getname()), '/', ' '.join(font.getname()))

    src = Image.open(a.atlas).convert('RGBA')
    assert src.size == tuple(cm['atlas']), f'atlas must be {cm["atlas"]}, got {src.size}'
    A = np.array(src)[..., 3]
    S = a.scale
    out = Image.new('L', (src.size[0] * S, src.size[1] * S), 0)
    errs = []
    for k, ch in enumerate(table):
        x, y = (k % cols) * cell, (k // cols) * cell
        o = A[y:y + cell, x:x + cell]
        box = edges(o)
        if box is None or ch.isspace(): continue
        rch = SUBST.get(ch, ch)
        best = None
        for st in STROKES:
            g = master(lfont if ord(rch) < 0x2000 else font, rch, st)
            if g is None: break
            for sh in (0.0, 0.2, 0.4):
              for keep in (0, 1):
                bx = (box[0] + sh, box[1] + sh, box[2] - sh, box[3] - sh)
                if bx[2] - bx[0] < 0.5 or bx[3] - bx[1] < 0.5: continue
                c, dist = place(g, bx, cell, S, keep)
                if c is None: continue
                # match the game's glyph; mildly prefer undistorted shapes
                e = float(((down(c, S) - o) ** 2).mean()) * (1 + 0.5 * max(0, dist - 0.15))
                if best is None or e < best[0]: best = (e, c)
        if best is None: continue
        errs.append(best[0] ** 0.5)
        out.paste(best[1], (x * S, y * S))
    print(f'{len(errs)} glyphs, rms error vs original {np.mean(errs):.1f}/255')
    white = Image.new('L', out.size, 255)
    res = Image.merge('RGBA', [white, white, white, out])
    os.makedirs(a.out, exist_ok=True)
    dst = os.path.join(a.out, os.path.basename(a.atlas))
    res.save(dst, optimize=True)
    print('->', dst, res.size)
    if a.preview:
        h = 6 * cell
        ob = Image.fromarray(A[:h, :]).resize((src.size[0] * S, h * S), Image.NEAREST)
        pv = Image.new('L', (src.size[0] * S, h * S * 2 + 8), 40)
        pv.paste(ob, (0, 0)); pv.paste(out.crop((0, 0, src.size[0] * S, h * S)), (0, h * S + 8))
        pv.save(a.preview)


if __name__ == '__main__':
    main()
