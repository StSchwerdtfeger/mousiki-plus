#!/usr/bin/env python3
"""Bakes the audio book picture (tools/book_picture.png, a colour picture with a transparent background) into src_audio_book/book_pic_data.inc:
a grey version in which the transparent parts are 0 and everything else is at least a dark grey (lum * alpha, lifted a little so
the black parts of the picture still show), zlib-compressed. The program scales it to the size of the field (an area average,
so the tones stay) and draws it as a real transparent pixel image when the terminal speaks Kitty graphics or Sixel; its colours
are the program's gradient wave. For the braille fallback see tools/make_book_art.py (a dithered 60 x 60 dot version).
Original image by Kamara Rahmat (iyaniyan) from Unsplash (https://unsplash.com/de/grafiken/young-woman-reading-a-blue-book-on-a-blanket-outdoors-brNfZ1TdBN0)
Usage:  python3 tools/make_book_pic.py > src_audio_book/book_pic_data.inc        (needs Pillow)"""
import os, sys, zlib
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
im = Image.open(os.path.join(here, "book_picture.png")).convert("RGBA")
w, h = im.size
data = bytearray()
for r, g, b, a in im.getdata():
    lum = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0
    data.append(int(round((a / 255.0) * (0.15 + 0.85 * lum) * 255)))
z = zlib.compress(bytes(data), 9)
print("// Original image by Kamara Rahmat (iyaniyan) from Unsplash (https://unsplash.com/de/grafiken/young-woman-reading-a-blue-book-on-a-blanket-outdoors-brNfZ1TdBN0)")
print("// Generated file: the audio book picture, grey, zlib-compressed (see tools/make_book_pic.py -- do not edit).")
print("static const int kBookPicW = %d, kBookPicH = %d, kBookPicZLen = %d;" % (w, h, len(z)))
hexs = z.hex()
chunks = [hexs[i:i + 120] for i in range(0, len(hexs), 120)]
print("static const char* const kBookPicZ[%d] = {" % len(chunks))
for c in chunks: print('    "%s",' % c)
print("};")
