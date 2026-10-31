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


# --- word spacing with janome (optional, Apache-2.0, pure Python, its dictionary is inside the package) -----------------------------
# Japanese is written without spaces, so romaji needs the words cut first: tokens by janome, the reading of each by pykakasi.
# Particles stand alone ("watashi wa anata ga suki desu"), endings and auxiliary verbs stay with their verb ("tabemashita").
# は / へ / を as particles are read wa / e / o.
COPULA = {"\u3067\u3059", "\u3060", "\u3067\u3057\u305f", "\u3067\u3057\u3087\u3046", "\u3060\u3063\u305f"}   # です だ でした でしょう だった: separate words


def kana_to_hepburn(kks, kana):
    return "".join(i.get("hepburn", "") for i in kks.convert(kana))


def read_words(kks, tok, text):
    pieces = []   # [romaji, glue_to_previous]
    prev_p0 = prev_p1 = ""
    for t in tok.tokenize(text):
        surf = t.surface
        pos = t.part_of_speech.split(",")
        p0, p1 = pos[0], pos[1] if len(pos) > 1 else ""
        if not has_cjk(surf):
            seg = unicodedata.normalize("NFKC", surf)
            seg = seg.replace("\u300c", '"').replace("\u300d", '"').replace("\u300e", '"').replace("\u300f", '"')
            if p0 == "\u8a18\u53f7" or not seg.strip():
                pieces.append([seg.strip(), "punct"])
            else:
                pieces.append([seg, False])
            continue
        if p0 == "\u52a9\u8a5e" and surf in ("\u306f", "\u3078", "\u3092"):
            seg = {"\u306f": "wa", "\u3078": "e", "\u3092": "o"}[surf]
        else:
            rd = t.reading if t.reading and t.reading != "*" else ""
            # a small tsu at the end of a word doubles the next consonant (言っ|て = itte): pykakasi alone would write "itsu"
            geminate = rd[-1:] in ("\u30c3", "\u3063") and len(rd) > 1
            seg = kana_to_hepburn(kks, rd[:-1] if geminate else rd) if rd else ""
            if geminate and seg:
                seg += "~"
            if not seg:
                seg = kana_to_hepburn(kks, surf)
        if not seg:
            continue
        glue = ((p0 == "\u52a9\u52d5\u8a5e" and surf not in COPULA) or (p0 == "\u52d5\u8a5e" and p1 in ("\u975e\u81ea\u7acb", "\u63a5\u5c3e"))
                or (p0 == "\u540d\u8a5e" and p1 == "\u63a5\u5c3e") or (p0 == "\u52a9\u8a5e" and p1 == "\u63a5\u7d9a\u52a9\u8a5e")
                or surf in ("\u3063", "\u30c3"))
        # ...てはいけない: the verb after a particle (は, が, ...) is a word of its own, not an ending of the word before
        if glue and prev_p0 == "\u52a9\u8a5e" and prev_p1 != "\u63a5\u7d9a\u52a9\u8a5e" and p0 == "\u52d5\u8a5e":
            glue = False
        if pieces and pieces[-1][0].endswith("~"):
            glue = True   # the doubled consonant belongs to the next syllable
        pieces.append([seg, glue])
        prev_p0, prev_p1 = p0, p1
    out = ""
    for seg, glue in pieces:
        if not seg:
            continue
        if out.endswith("~"):   # the doubled consonant: the first letter of what follows (ch -> t: itchi)
            first = seg[0] if seg[0].isalpha() else ""
            out = out[:-1] + ("t" if seg.startswith("ch") else first)
        if glue == "punct":
            out += seg
        elif out and not glue and not out.endswith(('"', "(")):
            out += " " + seg
        else:
            out += seg
    return " ".join(out.replace("~", "").split())


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
        tok = None
        try:
            from janome.tokenizer import Tokenizer
            tok = Tokenizer()
        except Exception:
            tok = None
        res = {}
        for t in texts:
            r = ""
            if tok is not None:
                try:
                    r = read_words(kks, tok, t)
                except Exception:
                    r = ""
            if not r and has_kanji(t):   # (kana-only text without janome: nothing to add, the program's own romaji stays)
                r = read(kks, t)
            if r:
                res[t] = r
        print(json.dumps({"ok": True, "readings": res}, ensure_ascii=False))
    except Exception as e:
        print(json.dumps({"ok": False, "error": str(e)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
