#!/usr/bin/env python3
"""Bakes the three audio book animations into src_audio_book/book_art_data.inc.

Input : PNG frame folders rendered from the Lottie files (tools/render_lottie.js writes them).
Output: src_audio_book/book_art_data.inc -- per animation a palette (index 0 = transparent) and the frames as run length
        encoded palette indices, written as C string hex text. src_audio_book/book_art.cpp decodes them at start-up.

  python3 tools/make_book_art.py book:44x44 circ:60x60 stack:36x56 > src_audio_book/book_art_data.inc
(arguments: <frame folder>:<width>x<height> in DOTS = 2 per cell wide, 4 per cell high)
"""
import sys, glob, os
from PIL import Image

def pixels(im):
    return list(im.get_flattened_data()) if hasattr(im, 'get_flattened_data') else list(im.getdata())

def load(folder):
    return [Image.open(f).convert("RGBA") for f in sorted(glob.glob(os.path.join(folder, "*.png")))]

def union_bbox(frames):
    bb = None
    for im in frames:
        a = im.split()[3].point(lambda v: 255 if v > 20 else 0)
        b = a.getbbox()
        if b: bb = b if bb is None else (min(bb[0], b[0]), min(bb[1], b[1]), max(bb[2], b[2]), max(bb[3], b[3]))
    return bb

def lum(p):
    return (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0

def mono(canv, name):
    """Strong-contrast black and white version of the frames (the colour wave of the program is applied to it later).
    Book (an open book on a round mint ground): white pages stay white, the red cover is mid grey, the ground is dim, the stars and
    sparkles are dark, the turning page (pink / cyan) is a shade darker than the pages.
    Wave (the circular audio spectrum behind the book): every bar is plain white; the program draws it dim.
    Stack: a contrast stretch of the picture (outlines black, pages white)."""
    out = []
    for im in canv:
        o = Image.new("RGBA", im.size, (0, 0, 0, 0))
        res = []
        for p in pixels(im):
            if not p[3]:
                res.append((0, 0, 0, 0)); continue
            L = lum(p)
            if name == "Wave":
                g = 1.0
            elif name == "Book":
                # a photo of an open book with a teal ribbon (the sheet cut out): the green cover is the bright outline, the pages
                # are bright and solid (no holes), the ribbon a mid tone
                r, gg, b_ = p[0], p[1], p[2]
                if L > 0.99: g = 1.0                                                      # (a picture made of plain white dots: lit as they are)
                elif b_ > r + 8 and gg > r and L < 0.62: g = 0.62                           # the ribbon
                elif L < 0.40: g = 1.0                                                    # the cover edge: the outline
                else: g = 0.82 + 0.10 * min(1.0, max(0.0, (L - 0.5) / 0.4))              # the pages
            else:
                g = min(1.0, max(0.0, (L - 0.12) / 0.88)) ** 0.7        # outlines black, covers mid grey, pages white
            v = int(round(g * 255))
            res.append((v, v, v, 255))
        o.putdata(res)
        out.append(o)
    return out

def bake(folder, W, H, name):
    frames = load(folder)
    bb = union_bbox(frames)
    bw, bh = bb[2] - bb[0], bb[3] - bb[1]
    s = min(W / bw, H / bh)
    if "exact" in os.path.basename(folder.rstrip("/")): s = 1.0   # a picture drawn dot by dot (from a braille text): not scaled
    nw, nh = max(1, round(bw * s)), max(1, round(bh * s))
    ox, oy = (W - nw) // 2, H - nh
    if s == 1.0 and nw == bw and nh == bh: ox, oy = (W - nw) // 2, (H - nh) // 2          # centred, resting on the bottom edge
    canv = []
    for im in frames:
        c = im.crop(bb)
        # premultiplied resize so the edges do not pick up the colour of the transparent black
        px = c.load()
        pm = Image.new("RGBA", c.size)
        pp = pm.load()
        for y in range(c.size[1]):
            for x in range(c.size[0]):
                r, g, b, a = px[x, y]
                pp[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)
        pm = pm.resize((nw, nh), Image.LANCZOS)
        out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        q = pm.load(); o = out.load()
        for y in range(nh):
            for x in range(nw):
                r, g, b, a = q[x, y]
                if a < (30 if name == 'Wave' else 110): continue
                k = 255 / a
                o[ox + x, oy + y] = (min(255, int(r * k)), min(255, int(g * k)), min(255, int(b * k)), 255)
        canv.append(out)
    canv = mono(canv, name)
    # one palette for every frame (median cut over a mosaic of all opaque pixels)
    cols = [p[:3] for im in canv for p in pixels(im) if p[3]]
    mosaic = Image.new("RGB", (len(cols), 1)); mosaic.putdata(cols)
    pal_img = mosaic.quantize(colors=47, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = pal_img.getpalette()[:47 * 3]
    entries = [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for i in range(len(pal) // 3)]   # (fewer than 47 when the picture has few colours)
    def nearest(c):
        best, bi = 1e9, 0
        for i, e in enumerate(entries):
            d = (e[0] - c[0]) ** 2 + (e[1] - c[1]) ** 2 + (e[2] - c[2]) ** 2
            if d < best: best, bi = d, i
        return bi + 1
    cache = {}
    enc = []
    prev = None
    for im in canv:
        idx = []
        for p in pixels(im):
            if not p[3]: idx.append(0); continue
            k = p[:3]
            if k not in cache: cache[k] = nearest(k)
            idx.append(cache[k])
        # Delta against the previous frame: (255, n) = n pixels unchanged, (idx, n) = n pixels of palette index idx.
        ref = prev if prev is not None else [0] * len(idx)
        runs = []
        i = 0
        while i < len(idx):
            j = i
            if idx[i] == ref[i]:
                while j < len(idx) and idx[j] == ref[j] and j - i < 255: j += 1
                runs.append((255, j - i))
            else:
                while j < len(idx) and idx[j] == idx[i] and idx[j] != ref[j] and j - i < 255: j += 1
                runs.append((idx[i], j - i))
            i = j
        prev = idx
        enc.append("".join("%02x%02x" % r for r in runs))
    print("// %s: %d frames of %dx%d dots (generated by tools/make_book_art.py -- do not edit)" % (name, len(canv), W, H))
    print("static const int k%sW = %d, k%sH = %d, k%sFrames = %d;" % (name, W, name, H, name, len(canv)))
    print("static const char* const k%sPalette = \"%s\";" % (name, "".join("%02x%02x%02x" % e for e in entries)))
    print("static const char* const k%sData[%d] = {" % (name, len(enc)))
    for e in enc: print('    "%s",' % e)
    print("};")
    sys.stderr.write("%s: bbox %s scale %.3f -> %dx%d, %d chars\n" % (name, bb, s, nw, nh, sum(map(len, enc))))

if __name__ == "__main__":
    print("// Generated file: the baked audio book animations. See tools/make_book_art.py.")
    for a in sys.argv[1:]:
        folder, size = a.split(":"); w, h = map(int, size.split("x"))
        base = os.path.basename(folder.rstrip("/"))
        bake(folder, w, h, "Stack" if "stack" in base else "Wave" if "circ" in base else "Book")
