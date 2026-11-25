#!/usr/bin/env python3
"""Builds packaging/fonts/MousikiCorners.ttf: a tiny font with the THICK ROUND frame (corners that Unicode does not have, and the lines that join them)
(Cascadia Mono, the font Mousiki is made for, has thin round corners  ╭ ╮ ╰ ╯  and heavy sharp ones  ┏ ┓ ┗ ┛  only).

    python3 tools/make_corner_font.py [path/to/CascadiaMono-Regular.ttf]

The geometry is measured from Cascadia Mono itself (cell width, height of the horizontal line, how far the lines reach into the next cell), so the
pieces join each other and the neighbouring cells without a seam. The font holds the frame glyphs of the "thick round" look at the private-use
code points U+E1C0 ... U+E1D0 (see CODEPOINTS; corners.h maps the light box glyphs to them); everything else comes from the main font, this one is
listed next to it (Windows Terminal: "Cascadia Mono, Mousiki Corners"; fontconfig / CoreText find it by themselves when installed).
The outlines are drawn here, nothing of Cascadia's outlines is copied, so the file is Mousiki's own (Apache-2.0, like the program; see LICENSE).
Needs fontTools (pip install fonttools); without a Cascadia file the measured values of Cascadia Mono 2407.24 are used.
"""
import math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "packaging", "fonts", "MousikiCorners.ttf")
# Every frame glyph of the "thick round" look comes from this one font (a terminal draws its own box lines a pixel or two off from a font's
# round corners, so mixing them never joined well): the four round corners, the four sharp ones, the two lines, the T pieces, the cross, the stubs.
CODEPOINTS = {"corner_tl": 0xE1C0, "corner_tr": 0xE1C1, "corner_bl": 0xE1C2, "corner_br": 0xE1C3,
              "sharp_tl": 0xE1C4, "sharp_tr": 0xE1C5, "sharp_bl": 0xE1C6, "sharp_br": 0xE1C7,
              "line_h": 0xE1C8, "line_v": 0xE1C9, "tee_r": 0xE1CA, "tee_l": 0xE1CB, "tee_d": 0xE1CC, "tee_u": 0xE1CD, "cross": 0xE1CE,
              "stub_d": 0xE1CF, "stub_u": 0xE1D0}

# Cascadia Mono 2407.24, units per em 2048 (measured, see measure() below)
# The stroke of the thick corners: Cascadia's own heavy lines (━ ┃) are 416 units, but Windows Terminal draws box lines itself, a little thinner
# than that, and all lines now come from this font, so the width is a matter of taste: 300 units (about 70 % of Cascadia's heavy lines).
STROKE = 300
# How far a glyph reaches into the next cell (units): just enough to hide the rounding of the cell size to whole pixels. Cascadia's own lines reach 104 sideways
# and 326 up; where two glyphs overlap, their soft edge pixels are drawn twice and look heavier, so the overlap is kept small.
OVERLAP = 40
M = dict(upm=2048, ascent=1900, descent=-480, adv=1200, cy=707, half=STROKE // 2, reach=OVERLAP, down=-480 - OVERLAP, up=1900 + OVERLAP, radius=600)


def measure(path):
    """Reads the numbers of M from a Cascadia Mono file (the heavy lines are the model: ━ U+2501, ┃ U+2503, round corner U+256D)."""
    from fontTools.ttLib import TTFont
    from fontTools.pens.boundsPen import BoundsPen
    f = TTFont(path)
    gs, cm = f.getGlyphSet(), f.getBestCmap()
    def bounds(cp):
        bp = BoundsPen(gs); gs[cm[cp]].draw(bp); return bp.bounds
    h, v, r = bounds(0x2501), bounds(0x2503), bounds(0x256D)
    m = dict(M)
    m.update(upm=f["head"].unitsPerEm, ascent=f["hhea"].ascent, descent=f["hhea"].descent, adv=gs[cm[0x2501]].width, cy=(h[1] + h[3]) // 2)
    m["reach"], m["down"], m["up"] = OVERLAP, m["descent"] - OVERLAP, m["ascent"] + OVERLAP
    # the light round corner is a quarter ring around (adv + reach - ..): its outer radius is the distance from the corner's far
    # edge to the line's middle; the middle line of the arc has the radius adv/2
    m["radius"] = m["adv"] // 2
    m["half"] = STROKE // 2
    return m


def ring_quarter(cx, cy, r, a0, a1, steps=4):
    """Points of the circle (cx, cy, r) from angle a0 to a1 (degrees) as quadratic segments: [(off-curve, on-curve), ...]."""
    segs = []
    da = (a1 - a0) / steps
    k = 1.0 / math.cos(math.radians(abs(da) / 2))
    for i in range(steps):
        am = math.radians(a0 + da * (i + 0.5))
        ae = math.radians(a0 + da * (i + 1))
        segs.append(((cx + r * k * math.cos(am), cy + r * k * math.sin(am)), (cx + r * math.cos(ae), cy + r * math.sin(ae))))
    return segs


def corner_glyph(m, vert_down, horz_right, sharp=False):
    """One thick round corner. vert_down: the vertical line goes down from the arc (╭ ╮) or up (╰ ╯); horz_right: the horizontal line goes
    right (╭ ╰) or left (╮ ╯). Returns a list of closed contours of ('M', pt) / ('L', pt) / ('Q', off, on)."""
    adv, cy, reach, rad = m["adv"], m["cy"], m["reach"], m["radius"]
    hw = m["half"]                       # half the stroke (the stroke is 2 * hw = STROKE; Cascadia's own heavy lines are 416)
    # work for the ╭ corner (vertical tail down, horizontal tail right), mirror for the others
    mid = adv // 2                       # x of the vertical middle line
    cx_arc = mid + rad                   # centre of the arc circle: right of the vertical line ...
    cy_arc = cy - rad                    # ... and below the horizontal line (╭: the arc turns from down to right)
    ro, ri = rad + hw, rad - hw          # outer and inner radius
    tail_dn = m["down"]                  # bottom of the vertical tail (it reaches into the cell below)
    tail_rt = adv + reach                # right end of the horizontal tail (it reaches into the next cell)
    if sharp:
        c = [("M", (mid - hw, tail_dn)), ("L", (mid - hw, cy + hw)), ("L", (tail_rt, cy + hw)), ("L", (tail_rt, cy - hw)),
             ("L", (mid + hw, cy - hw)), ("L", (mid + hw, tail_dn)), ("Z",)]
    else:
        c = [("M", (mid - hw, tail_dn)), ("L", (mid - hw, cy_arc))]
        for off, on in ring_quarter(cx_arc, cy_arc, ro, 180, 90, 4):   # outer edge: from the left of the arc to its top
            c.append(("Q", off, on))
        c.append(("L", (tail_rt, cy + hw)))
        c.append(("L", (tail_rt, cy - hw)))
        c.append(("L", (cx_arc, cy - hw)))
        for off, on in ring_quarter(cx_arc, cy_arc, ri, 90, 180, 4):   # inner edge: back from the top to the left
            c.append(("Q", off, on))
        c.append(("L", (mid + hw, tail_dn)))
        c.append(("Z",))

    def tf(p):
        x, y = p
        if not horz_right: x = adv - x
        if not vert_down:  y = 2 * cy - y
        return (round(x), round(y))
    out = []
    for seg in c:
        if seg[0] == "Z": out.append(seg); continue
        out.append((seg[0],) + tuple(tf(p) for p in seg[1:]))
    if not vert_down:
        # the mirrored tail would end at 2 * cy - down; the glyph must reach the top of the cell above like the line glyphs do
        shift = m["up"] - (2 * cy - tail_dn)
        out2 = []
        for seg in out:
            if seg[0] == "Z": out2.append(seg); continue
            pts = []
            for p in seg[1:]:
                x, y = p
                pts.append((x, y + shift if y >= 2 * cy - tail_dn else y))
            out2.append((seg[0],) + tuple(pts))
        out = out2
    return out


def polygons(m, name):
    """The straight pieces as single closed outlines (clockwise), one contour each: no overlapping contours, so no renderer can fill the overlap twice."""
    adv, cy, hw = m["adv"], m["cy"], m["half"]
    mid = adv // 2
    left, right, down, up = -m["reach"], adv + m["reach"], m["down"], m["up"]
    x0, x1, y0, y1 = mid - hw, mid + hw, cy - hw, cy + hw
    return {
        "line_h": [(left, y0), (left, y1), (right, y1), (right, y0)],
        "line_v": [(x0, down), (x0, up), (x1, up), (x1, down)],
        "tee_r": [(x0, down), (x0, up), (x1, up), (x1, y1), (right, y1), (right, y0), (x1, y0), (x1, down)],
        "tee_l": [(x0, down), (x0, y0), (left, y0), (left, y1), (x0, y1), (x0, up), (x1, up), (x1, down)],
        "tee_d": [(left, y0), (left, y1), (right, y1), (right, y0), (x1, y0), (x1, down), (x0, down), (x0, y0)],
        "tee_u": [(left, y0), (left, y1), (x0, y1), (x0, up), (x1, up), (x1, y1), (right, y1), (right, y0)],
        "cross": [(x0, down), (x0, y0), (left, y0), (left, y1), (x0, y1), (x0, up), (x1, up), (x1, y1), (right, y1), (right, y0), (x1, y0), (x1, down)],
        "stub_d": [(x0, down), (x0, y1), (x1, y1), (x1, down)],
        "stub_u": [(x0, y0), (x0, up), (x1, up), (x1, y0)],
    }[name]



def pin_bounds(pen, m):
    """Two points at opposite corners of the box every glyph shares, so that all glyphs have the same bounding box. A renderer that places a glyph's
    bitmap by the rounded corner of its bounding box (Windows Terminal seems to) then shifts none of them against the others: the right corners and
    walls, whose boxes differed from the plain wall's, were a pixel off before. Each point is a contour of no area (a line there and back): it has no
    ink, which a one-unit square did have in some renderers (faint dots next to the lines, at the corners of the cells)."""
    x0, y0, x1, y1 = -m["reach"], m["down"], m["adv"] + m["reach"], m["up"]
    for (ax, ay) in ((x0, y0), (x1, y1)):
        pen.moveTo((ax, ay)); pen.lineTo((ax, ay)); pen.closePath()


def build(m, out_path):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    names = [".notdef"] + list(CODEPOINTS)
    fb = FontBuilder(m["upm"], isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({cp: n for n, cp in CODEPOINTS.items()})
    spec = {"corner_tl": (True, True, False), "corner_tr": (True, False, False), "corner_bl": (False, True, False), "corner_br": (False, False, False),
            "sharp_tl": (True, True, True), "sharp_tr": (True, False, True), "sharp_bl": (False, True, True), "sharp_br": (False, False, True)}
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    for n in names[1:]:
        if n in spec: continue
        pen = TTGlyphPen(None)
        pts = polygons(m, n)
        pen.moveTo(pts[0])
        for pt in pts[1:]: pen.lineTo(pt)
        pen.closePath()
        pin_bounds(pen, m)
        glyphs[n] = pen.glyph()
    for n, (vd, hr, sharp) in spec.items():
        pen = TTGlyphPen(None)
        # a mirrored contour runs the other way round: reverse it so the fill stays right (TrueType: clockwise = filled)
        rp = ReverseContourPen(pen) if (vd != hr) else pen
        for seg in corner_glyph(m, vd, hr, sharp):
            if seg[0] == "M": rp.moveTo(seg[1])
            elif seg[0] == "L": rp.lineTo(seg[1])
            elif seg[0] == "Q": rp.qCurveTo(seg[1], seg[2])
            elif seg[0] == "Z": rp.closePath()
        pin_bounds(pen, m)
        glyphs[n] = pen.glyph()
    fb.setupGlyf(glyphs)
    # the left side bearing of a glyph must equal its xMin, or the renderer shifts the outline by the difference
    fb.setupHorizontalMetrics({n: (m["adv"], getattr(glyphs[n], "xMin", 0) if glyphs[n].numberOfContours else 0) for n in names})
    fb.setupHorizontalHeader(ascent=m["ascent"], descent=m["descent"])
    fb.setupNameTable({"familyName": "Mousiki Corners", "styleName": "Regular", "uniqueFontIdentifier": "MousikiCorners-1.0",
                       "fullName": "Mousiki Corners", "psName": "MousikiCorners-Regular", "version": "Version 1.000",
                       "licenseDescription": "Apache License 2.0, see the Mousiki Plus repository.", "copyright": "Mousiki Plus contributors, Apache-2.0"})
    # a complete OS/2 table (Windows is stricter than FreeType): regular weight and style, a monospaced PANOSE, the private-use range
    fb.setupOS2(sTypoAscender=m["ascent"], sTypoDescender=m["descent"], usWinAscent=m["ascent"] + 100, usWinDescent=-m["descent"] + 100, sTypoLineGap=0,
                xAvgCharWidth=m["adv"], fsType=0, fsSelection=0x40, achVendID="MSKI", usWeightClass=400, usWidthClass=5,
                panose=dict(bFamilyType=2, bSerifStyle=11, bWeight=5, bProportion=9, bContrast=2, bStrokeVariation=2, bArmStyle=3, bLetterForm=2, bMidline=2, bXHeight=4),
                ulUnicodeRange1=0, ulUnicodeRange2=1 << 28, ulUnicodeRange3=0, ulUnicodeRange4=0, ulCodePageRange1=1, ulCodePageRange2=0,
                usFirstCharIndex=0xE1C0, usLastCharIndex=0xE1D0, sxHeight=1100, sCapHeight=1400, usDefaultChar=0, usBreakChar=32, usMaxContext=0)
    fb.setupPost(isFixedPitch=1)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    fb.save(out_path)
    return out_path


def write_inc(ttf_path, inc_path):
    """The font as a C array: the program carries the 1.5 KB font itself and installs it for the user when "thick round" is chosen."""
    data = open(ttf_path, "rb").read()
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 24]) + ",")
    with open(inc_path, "w", newline="\n") as f:
        f.write("// Generated by tools/make_corner_font.py from packaging/fonts/MousikiCorners.ttf -- do not edit.\n")
        f.write("static const unsigned char kCornerFontData[] = {\n" + "\n".join(lines) + "\n};\n")


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else None
    m = measure(src) if src else M
    print("measured" if src else "built-in", m)
    print(build(m, OUT), os.path.getsize(OUT), "bytes")
    inc = os.path.join(os.path.dirname(HERE), "src", "corner_font_data.inc")
    write_inc(OUT, inc)
    print(inc)
