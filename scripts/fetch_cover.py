#!/usr/bin/env python3
"""Fetches the FRONT cover of an album from the Cover Art Archive (found through MusicBrainz).

usage: fetch_cover.py <album> <artist or ""> <output path without extension> [song title ...]

The release is searched on MusicBrainz by album title (and artist when one is given), first as the exact title, then with MusicBrainz' fuzzy search (typos); the best matches are tried in
order until one has a front cover in the Cover Art Archive. Only the front image is taken, in a lightweight size
(the 500 pixel JPEG thumbnail when there is one, else the next smaller / larger one, else the original). The file is
written as <output>.jpg (or .png when that is what the archive has) and one JSON line is printed:

  {"ok": true, "file": "...", "release": "Title", "artist": "Artist", "mbid": "...", "match": 0.93}
  {"ok": false, "error": "..."}

MusicBrainz asks for a descriptive User-Agent and at most one request per second; both are kept. Standard library only.
"""
import difflib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

USER_AGENT = "MousikiPlus/4.6 (https://github.com/StSchwerdtfeger/mousiki-plus; cover art fetch)"
MB_API = "https://musicbrainz.org/ws/2/release/"
CAA = "https://coverartarchive.org/"
TIMEOUT = 25
MIN_MATCH = 0.60        # how close the release title has to be to the album asked for (MusicBrainz often answers with something else)
MAX_BYTES = 6 * 1024 * 1024
_last = [0.0]


def pace():
    wait = 1.1 - (time.time() - _last[0])
    if wait > 0:
        time.sleep(wait)
    _last[0] = time.time()


def get(url, accept="application/json", binary=False):
    """The body of a URL (str, or bytes with binary=True) and its content type; None, "" on 404."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
    for attempt in range(3):
        pace()
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                ctype = resp.headers.get("Content-Type", "")
                data = resp.read(MAX_BYTES + 1)
                if len(data) > MAX_BYTES:
                    raise RuntimeError("picture too large")
                return (data if binary else data.decode("utf-8", "replace")), ctype
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None, ""
            if e.code in (429, 500, 502, 503, 504) and attempt < 2:
                time.sleep(2.5 * (attempt + 1))
                continue
            raise RuntimeError("HTTP %d" % e.code)
        except (urllib.error.URLError, OSError):
            if attempt < 2:
                time.sleep(2.0)
                continue
            raise RuntimeError("not reachable")
    raise RuntimeError("not reachable")


def norm(s):
    return re.sub(r"[^0-9a-z]+", " ", (s or "").lower()).strip()


def closeness(a, b):
    a, b = norm(a), norm(b)
    if not a or not b:
        return 0.0
    if a == b:
        return 1.0
    return difflib.SequenceMatcher(None, a, b).ratio()


def lucene(s):
    return re.sub(r'([+\-&|!(){}\[\]^"~*?:\\/])', r"\\\1", s)


def loose_terms(s):
    """Every word of the title as a fuzzy Lucene term (word~ finds typos, up to two letters off); short words stay exact."""
    out = []
    for w in re.findall(r"[^\W_]+", s or "", re.UNICODE):
        out.append(w + "~" if len(w) >= 4 else w)
    return " ".join(out)


def search_releases(album, artist, loose=False, titles=()):
    if loose:
        q = "release:(%s)" % loose_terms(album)
        if artist:
            q += " AND artist:(%s)" % loose_terms(artist)
    else:
        q = 'release:"%s"' % lucene(album)
        if artist:
            q += ' AND artist:"%s"' % lucene(artist)
    if titles:   # the release has to hold at least one of the songs: that confirms it is the right one
        q += " AND (%s)" % " OR ".join('recording:"%s"' % lucene(t) for t in titles)
    url = MB_API + "?" + urllib.parse.urlencode({"query": q, "fmt": "json", "limit": "10"})
    body, _ = get(url)
    if not body:
        return []
    try:
        data = json.loads(body)
    except ValueError:
        raise RuntimeError("unreadable answer from MusicBrainz")
    out = []
    for rel in data.get("releases") or []:
        title = rel.get("title") or ""
        names = " ".join((c.get("name") or "") for c in (rel.get("artist-credit") or []) if isinstance(c, dict))
        out.append({
            "confirmed": bool(titles),
            "mbid": rel.get("id"),
            "title": title,
            "artist": names,
            "group": (rel.get("release-group") or {}).get("id"),
            "match": closeness(title, album),
            "score": int(rel.get("score") or 0),
        })
    out.sort(key=lambda r: (-r["match"], -r["score"]))
    return out


def front_url(listing):
    """The URL of the lightest good front image of a Cover Art Archive listing, or None."""
    for img in listing.get("images") or []:
        if not (img.get("front") or "Front" in (img.get("types") or [])):
            continue
        th = img.get("thumbnails") or {}
        for size in ("500", "large", "250", "small", "1200"):
            if th.get(size):
                return th[size]
        if img.get("image"):
            return img["image"]
    return None


def caa_listing(kind, mbid):
    body, _ = get(CAA + kind + "/" + mbid)
    if not body:
        return None
    try:
        return json.loads(body)
    except ValueError:
        return None


def main():
    if len(sys.argv) < 4:
        print(json.dumps({"ok": False, "error": "usage: fetch_cover.py <album> <artist> <output>"}))
        return 2
    album, artist, out_stem = sys.argv[1].strip(), sys.argv[2].strip(), sys.argv[3]
    titles = [t.strip() for t in sys.argv[4:] if t.strip()][:3]
    if not album:
        print(json.dumps({"ok": False, "error": "no album name"}))
        return 1
    try:
        # Album + artist first (the tags are right already: the album may well be a compilation, and a song title would pull in the
        # compilations that contain it). Only when that finds nothing the song titles are added as a fallback.
        attempts = [(False, artist, False)]
        if artist:
            attempts.append((False, "", False))     # the artist tag may be spelled differently on MusicBrainz
        attempts.append((True, artist, False))      # MusicBrainz' fuzzy search (typos, a word missing)
        if artist:
            attempts.append((True, "", False))
        if titles:
            attempts.append((False, artist, True))
            attempts.append((True, artist, True))
        good = []
        for loose, art, with_titles in attempts:
            releases = search_releases(album, art, loose=loose, titles=titles if with_titles else ())
            good = [r for r in releases if r["match"] >= MIN_MATCH]
            if good:
                break
        if not good:
            print(json.dumps({"ok": False, "error": "no release called like that on MusicBrainz"}))
            return 0
        tried = 0
        for rel in good[:6]:
            url = None
            for kind, mbid in (("release", rel["mbid"]), ("release-group", rel["group"])):
                if not mbid:
                    continue
                tried += 1
                lst = caa_listing(kind, mbid)
                if lst:
                    url = front_url(lst)
                if url:
                    break
            if not url:
                continue
            data, ctype = get(url, accept="image/*", binary=True)
            if not data or len(data) < 1000:
                continue
            ext = ".png" if "png" in ctype.lower() or data[:4] == b"\x89PNG" else ".jpg"
            os.makedirs(os.path.dirname(out_stem) or ".", exist_ok=True)
            with open(out_stem + ext, "wb") as f:
                f.write(data)
            print(json.dumps({"ok": True, "file": out_stem + ext, "release": rel["title"], "artist": rel["artist"],
                              "mbid": rel["mbid"], "match": round(rel["match"], 2), "confirmed": rel["confirmed"]}))
            return 0
        print(json.dumps({"ok": False, "error": "no front cover in the Cover Art Archive"}))
    except RuntimeError as e:
        print(json.dumps({"ok": False, "error": str(e)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
