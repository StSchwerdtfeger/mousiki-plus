#!/usr/bin/env python3
"""Mousiki_Help_Wizard.txt  ->  src/wizard_text.inc

The help wizard's text is a plain .txt file (Mousiki_Help_Wizard.txt in the repository root). This script compiles it into
src/wizard_text.inc, which src/wizard.cpp includes:

    static const char* const kWizardTextChunks[] = { R"WIZ(...)WIZ", ..., nullptr };

The text is cut into chunks of about 8000 bytes (at line ends) because some compilers limit the length of one string literal.

Markup (see src/wizard.h): [[New Page]], [[Header]] / [[Legend]] (the capitalised words in front of it),
[[Header_line]] (anywhere in a line: the whole line in the header color), [KEY], {VERSION}, =>>> notes.

Usage:  python3 tools/wizard_to_inc.py [input.txt] [output.inc]
        (defaults: Mousiki_Help_Wizard.txt  and  src/wizard_text.inc, relative to the repository root)
"""
import os
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "Mousiki_Help_Wizard.txt")
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "src", "wizard_text.inc")

with open(src, "rb") as f:
    raw = f.read()
if raw.startswith(b"\xef\xbb\xbf"):          # a BOM from Windows Notepad
    raw = raw[3:]
text = raw.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
if ')WIZ"' in text:
    sys.exit("the text contains the sequence )WIZ\" which ends a raw string literal")

# The two welcome pages (the first two pages) are drawn beside the spinning disk: their text may be 72 columns wide at
# most (kWelcomeTextWidth in src/wizard.h), a wider line would be wrapped by the program.
WELCOME_WIDTH = 72
import re
pages, page = [], None
for ln in text.split("\n"):
    s = ln.strip()
    if s.startswith("=>>>"):
        continue
    if s.lower() == "[[new page]]":
        page = []
        pages.append(page)
    elif page is not None:
        page.append(ln)
for pi, pg in enumerate(pages[:2], 1):
    for ln in pg:
        vis = re.sub(r"\[\[[^\]]*\]\]", "", ln).replace("{VERSION}", "9.9.9").rstrip()
        if len(vis) > WELCOME_WIDTH:
            print("warning: welcome page %d: a line is %d columns wide (limit %d): %s" % (pi, len(vis), WELCOME_WIDTH, vis.strip()[:60]))
if len(pages) < 3:
    print("warning: fewer than 3 pages: the first two are the welcome pages, then the table of content should follow")

lines = text.split("\n")
chunks, cur, size = [], [], 0
for line in lines:
    cur.append(line)
    size += len(line.encode("utf-8")) + 1
    if size >= 8000:
        chunks.append("\n".join(cur) + "\n")
        cur, size = [], 0
if cur:
    chunks.append("\n".join(cur))

with open(dst, "w", encoding="utf-8", newline="\n") as f:
    f.write("// Generated from Mousiki_Help_Wizard.txt by tools/wizard_to_inc.py -- edit the .txt, not this file.\n")
    f.write("static const char* const kWizardTextChunks[] = {\n")
    for c in chunks:
        f.write('R"WIZ(' + c + ')WIZ",\n')
    f.write("nullptr };\n")
print("%s: %d bytes in %d chunks -> %s" % (os.path.basename(src), len(raw), len(chunks), dst))
