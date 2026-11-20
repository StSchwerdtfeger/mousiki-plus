#!/usr/bin/env python3
"""The "Default values" chapters of the manual (chapter 16) and of the help wizard (chapter 6).

They are generated from the real defaults, so they cannot drift:

    DUMP_DEFAULTS=/tmp/defaults.txt ./build/mousiki_tests          # tests/test_dump_defaults.cpp writes the default config.txt
    python3 tools/defaults_overview.py /tmp/defaults.txt           # rewrites the two chapters in place

Everything from the marker  <!-- DEFAULTS BEGIN -->  to  <!-- DEFAULTS END -->  in the manual, and everything from the page that
starts with "6 DEFAULT VALUES" to the end of the wizard text, is replaced. The table of contents entries are written once by hand.
"""
import os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/defaults.txt"

SECTIONS = [("COLORS", "Colors"), ("GENERAL", "General settings"), ("ANIMA", "Anima/Viz, emoji, marquee, sound"), ("KEYS", "Hotkeys")]
rows = {k: [] for k, _ in SECTIONS}
cur = "COLORS"
for line in open(src, encoding="utf-8").read().splitlines():
    m = re.match(r"##\s+PANEL (\d)", line)
    if m:
        cur = {"1": "COLORS", "2": "GENERAL", "3": "ANIMA", "4": "REFERENCE"}[m.group(1)]
        continue
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*=", line):
        continue
    key, val = line.split("=", 1)
    val = val.strip().rstrip(";")
    if len(val) >= 2 and val[0] == val[-1] == '"':
        val = val[1:-1]
    if key.startswith("HKey"):
        rows["KEYS"].append((key[4:], val))
    elif cur == "REFERENCE":
        continue                                   # the font and frame characters: not a setting to look up
    else:
        rows[cur].append((key, val if val else "(empty)"))

# ---- manual -------------------------------------------------------------------------------------------------------
intro = {
    "COLORS": "Colour numbers of the 256-colour palette (`ColorCodes=palette`).",
    "GENERAL": "Switches, numbers and texts of the GENERAL tab and the files they belong to.",
    "ANIMA": "ANIMA/VIZ tab: visuals, video, MilkDrop, the field, emoji, marquee, stereo and loudness normalization.",
    "KEYS": "The hotkeys as they are bound without a `HKey...=` line in `config.txt` (the word after `HKey` is the action).",
}
esc = lambda s: s.replace("|", "\\|")
fmt = lambda s: s if s == "(empty)" else f"`{esc(s)}`"
md = ["<!-- DEFAULTS BEGIN -->", "## 16. Default values", "",
      "What every setting is when `config.txt` does not mention it (a new installation, or a line you delete). The list is generated from the "
      "program's own defaults, so it is always current; the key names are the ones of `config.txt`. The radio keeps its own file "
      "(`radio_config.txt`) with the same names for the rows it shares, and the audio book mode keeps its own look in "
      "`audiobook_config.txt`.", ""]
for k, title in SECTIONS:
    md += [f"### {title}", "", intro[k], "", "| Key | Default | Key | Default |", "|---|---|---|---|"]
    r = rows[k]
    half = (len(r) + 1) // 2
    for i in range(half):
        a = r[i]
        b = r[i + half] if i + half < len(r) else ("", "")
        md.append(f"| `{esc(a[0])}` | {fmt(a[1])} | " + (f"`{esc(b[0])}` | {fmt(b[1])} |" if b[0] else " | |"))
    md.append("")
md.append("<!-- DEFAULTS END -->")
p = os.path.join(root, "Mousiki_User_Manual", "Mousiki_User_Guide.md")
s = open(p, encoding="utf-8").read()
block = "\n".join(md)
if "<!-- DEFAULTS BEGIN -->" in s:
    s = re.sub(r"<!-- DEFAULTS BEGIN -->.*?<!-- DEFAULTS END -->", lambda _m: block, s, flags=re.S)
else:
    s = s.replace("---\n\n## About this app", block + "\n\n---\n\n## About this app", 1)
open(p, "w", encoding="utf-8").write(s)

# ---- wizard -------------------------------------------------------------------------------------------------------
pages = []
n = 0
for k, title in SECTIONS:
    n += 1
    out = [f"6.{n} DEFAULT VALUES: {title.upper()} [[Header]]", "", intro[k].replace("`", ""), ""]
    for key, val in rows[k]:
        out.append(f"{key[:34]:<35}{val[:34]}")
    pages.append("\n".join(out))
head = ("6 DEFAULT VALUES [[Header]]\n\nWhat each setting is when config.txt does not mention it. Generated from the program's own defaults.\n"
        "The radio (radio_config.txt) and the audio book mode (audiobook_config.txt) keep their own files.\n")
# the first page doubles as the chapter title page
text = "[[New Page]]\n" + pages[0].replace(f"6.1 DEFAULT VALUES: {SECTIONS[0][1].upper()} [[Header]]", head + "\n6.1 DEFAULT VALUES: COLORS [[Header]]", 1)
for pg in pages[1:]:
    text += "\n\n[[New Page]]\n" + pg
text += "\n"
p = os.path.join(root, "Mousiki_Help_Wizard.txt")
w = open(p, encoding="utf-8").read()
i = w.find("[[New Page]]\n6 DEFAULT VALUES")
w = (w[:i] if i >= 0 else w.rstrip("\n") + "\n\n") + text
open(p, "w", encoding="utf-8").write(w)
print({k: len(v) for k, v in rows.items()})
