#!/usr/bin/env python3
"""Looks up the Latin-letter name of an artist (written in Chinese characters, kana or Hangul) on MusicBrainz.

usage: lookup_artist_name.py <artist name>

MusicBrainz knows most artists by their original name and lists the English / Latin names as aliases ("Kenshi Yonezu" for
米津玄師). The artist is searched by name; the answer counts only when the name asked for is the artist's own name or one of its
aliases (so a different artist with a similar name is never taken). The Latin name is, in this order: the primary English alias,
another Latin alias of the type "Artist name", the artist's own name when that is Latin, the sort name ("Yonezu, Kenshi" ->
"Kenshi Yonezu") of a person. One JSON line is printed:

  {"ok": true, "latin": "Kenshi Yonezu", "mbid": "...", "matched": "米津玄師"}
  {"ok": false, "notfound": true}
  {"ok": false, "error": "..."}

MusicBrainz asks for a descriptive User-Agent and at most one request per second. Standard library only.
"""
import json
import re
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

USER_AGENT = "MousikiPlus/4.6 (https://github.com/StSchwerdtfeger/mousiki-plus; artist name lookup)"
MB_API = "https://musicbrainz.org/ws/2/artist/"
TIMEOUT = 25


# the output is JSON with the characters themselves: UTF-8 whatever the console code page is (Windows pipes default to cp1252 / cp850)
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def fold(s):
    return unicodedata.normalize("NFKC", s or "").casefold().strip()


def is_latin(s):
    """Only Latin letters (with accents), digits and the usual punctuation, and at least two letters."""
    s = s or ""
    letters = 0
    for ch in s:
        if ch.isalpha():
            if ord(ch) > 0x24F:
                return False
            letters += 1
        elif not (ch.isdigit() or ch in " .,'&!-+/()"):
            return False
    return letters >= 2


def latin_name(artist, wanted):
    """The Latin name from one MusicBrainz artist (a dict) if `wanted` is its name or an alias; else None."""
    w = fold(wanted)
    aliases = artist.get("aliases") or []
    names = [artist.get("name") or ""] + [a.get("name") or "" for a in aliases] + [a.get("sort-name") or "" for a in aliases]
    if w not in {fold(n) for n in names}:
        return None
    if w != fold(artist.get("name")):   # only an alias of the artist (a stage name ...): its own Latin form, not the artist's
        for a in aliases:
            if w in (fold(a.get("name")), fold(a.get("sort-name"))) and is_latin(a.get("sort-name") or ""):
                return a["sort-name"]
        return None
    best = None
    for a in aliases:
        n = a.get("name") or ""
        if not is_latin(n):
            continue
        rank = 0
        if a.get("primary"):
            rank += 2
        if (a.get("locale") or "").startswith("en"):
            rank += 2
        if a.get("type") == "Artist name":
            rank += 1
        if best is None or rank > best[0]:
            best = (rank, n)
    if best and best[0] >= 2:
        return best[1]
    if is_latin(artist.get("name") or ""):
        return artist["name"]
    if best:
        return best[1]
    sort = artist.get("sort-name") or ""
    if is_latin(sort):
        if artist.get("type") == "Person" and sort.count(",") == 1:
            last, first = [p.strip() for p in sort.split(",")]
            return (first + " " + last).strip()
        return sort
    return None


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                return resp.read(2 * 1024 * 1024).decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503, 504) and attempt < 2:
                time.sleep(3.0 * (attempt + 1))
                continue
            raise RuntimeError("HTTP %d" % e.code)
        except (urllib.error.URLError, OSError):
            if attempt < 2:
                time.sleep(2.0)
                continue
            raise RuntimeError("not reachable")
    raise RuntimeError("not reachable")


def lucene(s):
    return re.sub(r'([+\-&|!(){}\[\]^"~*?:\\/])', r"\\\1", s)


def lookup(name):
    q = 'artist:"%s" OR alias:"%s"' % (lucene(name), lucene(name))
    url = MB_API + "?" + urllib.parse.urlencode({"query": q, "fmt": "json", "limit": "8"})
    data = json.loads(get(url) or "{}")
    for artist in data.get("artists") or []:
        if int(artist.get("score") or 0) < 80:
            continue
        latin = latin_name(artist, name)
        if latin:
            return {"ok": True, "latin": latin, "mbid": artist.get("id"), "matched": name}
    return {"ok": False, "notfound": True}


def main():
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        print(json.dumps({"ok": False, "error": "no name"}))
        return 2
    try:
        out = lookup(sys.argv[1].strip())
    except RuntimeError as e:
        out = {"ok": False, "error": str(e)}
    except ValueError:
        out = {"ok": False, "error": "unreadable answer from MusicBrainz"}
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
