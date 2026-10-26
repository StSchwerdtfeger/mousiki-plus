#!/usr/bin/env python3
"""Reads Japanese text (kanji and kana) in Latin letters with pykakasi, for the Latin letters display of Mousiki.

usage: kanji_reading.py <file with one text per line, UTF-8>

pykakasi is NOT part of Mousiki (it is licensed GPL-3.0-or-later, Mousiki is Apache-2.0): it is an optional package you install
yourself with `pip install pykakasi`; this script only calls it as a separate program, on your computer, nothing is sent anywhere.
One JSON line is printed:

  {"ok": true, "readings": {"夜に駆ける": "yoru ni kakeru", ...}}
  {"ok": false, "missing": true, "error": "pykakasi is not installed (pip install pykakasi)"}
  {"ok": false, "error": "..."}
"""
import json
import sys
import os
import unicodedata

# the Windows installer's optional step puts pykakasi into <install folder>\pylib (scripts\install_kanji_reader.py)
_PYLIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pylib")
if os.path.isdir(_PYLIB) and _PYLIB not in sys.path:
    sys.path.append(_PYLIB)


# the output is JSON with the characters themselves: UTF-8 whatever the console code page is (Windows pipes default to cp1252 / cp850)
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def has_cjk(s):
    for ch in s:
        o = ord(ch)
        if 0x3040 <= o <= 0x30FF or 0x3400 <= o <= 0x9FFF or 0xFF66 <= o <= 0xFF9F:
            return True
    return False


PARTICLES = {"\u306e", "\u306b", "\u3067", "\u3092", "\u306f", "\u304c", "\u3068", "\u3082", "\u3078", "\u3084", "\u304b\u3089", "\u307e\u3067", "\u3088\u308a", "\u3063\u3066"}


def hiragana(s):
    return bool(s) and all(0x3041 <= ord(ch) <= 0x309F for ch in s)


def has_kanji(s):
    return any(0x3400 <= ord(ch) <= 0x9FFF for ch in s)


def read(kks, text):
    out = ""
    prev_cjk = False
    prev_kanji = False
    for item in kks.convert(text):
        orig = item.get("orig", "")
        cjk = has_cjk(orig)
        seg = item.get("hepburn", "") if cjk else unicodedata.normalize("NFKC", orig)
        seg = seg.replace("「", '"').replace("」", '"').replace("『", '"').replace("』", '"')
        if not seg:
            continue
        glue = cjk and prev_kanji and hiragana(orig) and orig not in PARTICLES   # 駆け|る: the ending of the word before
        if out and not out.endswith(" ") and not glue and ((cjk and out[-1].isalnum()) or (prev_cjk and seg[0].isalnum())):
            out += " "
        out += seg
        prev_cjk = cjk
        prev_kanji = has_kanji(orig) or (glue and prev_kanji)
    return " ".join(out.split())


def main():
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "usage: kanji_reading.py <file>"}))
        return 2
    try:
        import pykakasi
    except Exception:
        print(json.dumps({"ok": False, "missing": True, "error": "pykakasi is not installed (pip install pykakasi)"}))
        return 0
    try:
        with open(sys.argv[1], encoding="utf-8") as f:
            texts = [line.rstrip("\r\n") for line in f if line.strip()]
        kks = pykakasi.kakasi()
        res = {}
        for t in texts:
            r = read(kks, t)
            if r:
                res[t] = r
        print(json.dumps({"ok": True, "readings": res}, ensure_ascii=False))
    except Exception as e:
        print(json.dumps({"ok": False, "error": str(e)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
