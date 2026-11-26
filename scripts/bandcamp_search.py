#!/usr/bin/env python3
"""Bandcamp track search for Mousiki's /b: prefix.

yt-dlp downloads Bandcamp tracks but has no Bandcamp search, so this script asks Bandcamp itself and prints one JSON
object per track (the field names yt-dlp uses: title, uploader, webpage_url, duration), which the player parses the
same way as yt-dlp's output.

    python3 bandcamp_search.py "query" [count]

Two ways, the first that finds something wins:
  1. the search API behind bandcamp.com's search box (JSON),
  2. the search results page (https://bandcamp.com/search?q=...&item_type=t), parsed.
Nothing is printed when both fail (no network, Bandcamp changed its pages ...); the player then says "no online results".
Only the standard library is used.
"""
import html
import json
import re
import sys
import urllib.parse
import urllib.request

MAX_BODY = 20_000_000   # bytes: an answer of a web service is read up to this size, never without a limit

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"


def clean_url(u):
    """Drop the tracking query (?from=search...) so the same track always has the same id."""
    if not u:
        return ""
    u = html.unescape(u.strip())
    return u.split("?", 1)[0]


def via_api(query, count):
    url = "https://bandcamp.com/api/bcsearch_public_api/1/autocomplete_elastic"
    body = json.dumps({"search_text": query, "search_filter": "t", "full_page": True, "fan_id": None}).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json", "User-Agent": UA})
    with urllib.request.urlopen(req, timeout=8) as r:
        data = json.loads(r.read(MAX_BODY).decode("utf-8", "replace"))
    results = (data.get("auto") or {}).get("results") or data.get("results") or []
    out = []
    for it in results:
        if it.get("type") not in ("t", None):
            continue
        link = clean_url(it.get("item_url_path") or it.get("item_url") or "")
        if "/track/" not in link:
            continue
        title = it.get("name") or ""
        if not title:
            continue
        out.append({"title": title, "uploader": it.get("band_name") or "", "webpage_url": link})
        if len(out) >= count:
            break
    return out


def via_page(query, count):
    url = "https://bandcamp.com/search?" + urllib.parse.urlencode({"q": query, "item_type": "t"})
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=8) as r:
        page = r.read(MAX_BODY).decode("utf-8", "replace")
    out = []
    # one <li class="searchresult ..."> per hit: a heading link (title), a subhead ("from <album> by <artist>")
    # and the item URL
    for block in re.findall(r'<li class="searchresult.*?</li>', page, re.S):
        m_link = re.search(r'<div class="itemurl">\s*<a[^>]*>([^<]+)</a>', block, re.S) or \
                 re.search(r'<div class="heading">\s*<a href="([^"]+)"', block, re.S)
        m_title = re.search(r'<div class="heading">\s*<a[^>]*>(.*?)</a>', block, re.S)
        m_sub = re.search(r'<div class="subhead">(.*?)</div>', block, re.S)
        if not m_link or not m_title:
            continue
        link = clean_url(m_link.group(1))
        if "/track/" not in link:
            continue
        title = html.unescape(re.sub(r"\s+", " ", m_title.group(1))).strip()
        artist = ""
        if m_sub:
            sub = html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", m_sub.group(1)))).strip()
            m_by = re.search(r"\bby (.+)$", sub)
            artist = m_by.group(1).strip() if m_by else ""
        out.append({"title": title, "uploader": artist, "webpage_url": link})
        if len(out) >= count:
            break
    return out


def main():
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        return
    query = sys.argv[1].strip()
    count = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 15
    results = []
    for way in (via_api, via_page):
        try:
            results = way(query, count)
        except Exception as e:  # no network, changed page, ... -> try the other way
            print(f"bandcamp search ({way.__name__}): {e}", file=sys.stderr)
            results = []
        if results:
            break
    seen = set()
    for r in results:
        if r["webpage_url"] in seen:
            continue
        seen.add(r["webpage_url"])
        r["duration"] = None
        print(json.dumps(r, ensure_ascii=True))


if __name__ == "__main__":
    main()
