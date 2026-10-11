"""Lyrics translation for mousiki's READ LYRICS overlay (CTRL+L).

    python3 translate_lyrics.py <request.json>

The request file is written by the app:

    {"service": "google" | "mymemory",
     "from": "auto" | "de" | ..., "to": "en" | ...,
     "lines": ["first line", "", "second line", ...]}

and ONE JSON object is printed on stdout:

    {"ok": true,  "lang": "de", "lines": ["translated 1", "", "translated 2", ...]}
    {"ok": true,  "lang": "en", "same": true}    the lyrics already are in the target language
    {"ok": false, "error": "RATE_LIMIT" | "NETWORK" | "SERVICE" | ..., "detail": "..."}

"lines" has exactly as many entries as the request; empty lines stay empty, so the app can show every
translation right below the line it belongs to.

Services (all reached with urllib only -- no extra Python packages):
  google         the key-less web endpoint of Google Translate (client "gtx"). Unofficial and rate limited: a burst
                 of requests is answered with HTTP 429, so the lyrics are sent in a FEW big requests (many lines
                 each) and a 429 is retried with a growing pause. Detects the language itself.
  mymemory       MyMemory (api.mymemory.translated.net), key-less, about 5000 characters a day per address
                 (MOUSIKI_MYMEMORY_EMAIL raises that). One request per line. The language is detected with
                 "Autodetect"; if that fails, choose the language of the lyrics in the overlay.

The addresses of google / mymemory can be overridden with MOUSIKI_TRANSLATE_URL / MOUSIKI_MYMEMORY_URL (used by the
tests to point at a fake service).
"""

import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# Windows picks the ANSI code page for a redirected stdout, which would crash the moment a translated line holds a
# character that page cannot represent. Force UTF-8 like the other helpers.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except Exception:
        pass

GOOGLE_URL = os.environ.get("MOUSIKI_TRANSLATE_URL") or "https://translate.googleapis.com/translate_a/single"
MYMEMORY_URL = os.environ.get("MOUSIKI_MYMEMORY_URL") or "https://api.mymemory.translated.net/get"
USER_AGENT = "Mozilla/5.0 (compatible; MousikiPlus/3.2 lyrics-translate)"
TIMEOUT = 20
CHUNK_CHARS = 3500          # per Google request
PAUSE = float(os.environ.get("MOUSIKI_TRANSLATE_PAUSE", "0.4"))   # between two requests of one run


class TranslateError(Exception):
    def __init__(self, code, detail):
        Exception.__init__(self, detail)
        self.code = code
        self.detail = detail


def http(req):
    """Opens a request with up to 4 attempts; 429 / 5xx wait longer each time. Returns the body text."""
    last = None
    code = "NETWORK"
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                return resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            last = "HTTP %d" % e.code
            code = "RATE_LIMIT" if e.code == 429 else ("SERVICE" if e.code >= 500 or e.code in (400, 401, 403) else "NETWORK")
            if e.code == 429 or e.code >= 500:
                wait = 0
                try:
                    wait = float(e.headers.get("Retry-After") or 0)
                except (TypeError, ValueError):
                    pass
                time.sleep(min(10.0, max(wait, 1.5 * (2 ** attempt))))
                continue
            try:
                body = e.read().decode("utf-8", "replace")[:160]
                if body:
                    last += ": " + body
            except Exception:
                pass
            break
        except (urllib.error.URLError, OSError) as e:
            last = str(getattr(e, "reason", e))
            code = "NETWORK"
            time.sleep(0.5 * (attempt + 1))
    raise TranslateError(code, last or "request failed")


# ---------------------------------------------------------------------------------------------------------------
# Google
# ---------------------------------------------------------------------------------------------------------------

def google_call(text, src, dst):
    params = urllib.parse.urlencode({"client": "gtx", "sl": src, "tl": dst, "dt": "t"})
    body = urllib.parse.urlencode({"q": text}).encode("utf-8")
    req = urllib.request.Request(GOOGLE_URL + "?" + params, data=body,
                                 headers={"User-Agent": USER_AGENT, "Content-Type": "application/x-www-form-urlencoded"})
    try:
        data = json.loads(http(req))
        out = "".join(seg[0] for seg in (data[0] or []) if seg and seg[0])
        lang = data[2] if len(data) > 2 and isinstance(data[2], str) else ""
    except (ValueError, IndexError, TypeError) as e:
        raise TranslateError("SERVICE", "unexpected answer from the service (%s)" % e)
    return out, lang


def google_translate(lines, src, dst):
    """lines: list of non-empty strings -> (translations, detected language)."""
    chunks, cur, size = [], [], 0
    for i, t in enumerate(lines):
        if cur and size + len(t) + 1 > CHUNK_CHARS:
            chunks.append(cur)
            cur, size = [], 0
        cur.append(i)
        size += len(t) + 1
    if cur:
        chunks.append(cur)
    result = [""] * len(lines)
    lang = ""
    for n, chunk in enumerate(chunks):
        if n:
            time.sleep(PAUSE)
        text, l = google_call("\n".join(lines[i] for i in chunk), src, dst)
        lang = lang or l
        if n == 0 and not src_is_explicit(src) and base(lang) == base(dst):
            return None, lang                     # already in the target language
        parts = text.split("\n")
        if len(parts) == len(chunk):
            for i, p in zip(chunk, parts):
                result[i] = p.strip()
        else:                                     # the lines did not stay apart: one request per line for this chunk
            for i in chunk:
                time.sleep(PAUSE)
                result[i] = google_call(lines[i], src, dst)[0].strip()
    return result, lang


# ---------------------------------------------------------------------------------------------------------------
# MyMemory
# ---------------------------------------------------------------------------------------------------------------

def mymemory_translate(lines, src, dst):
    email = os.environ.get("MOUSIKI_MYMEMORY_EMAIL", "")
    pair_src = "Autodetect" if not src_is_explicit(src) else src
    result = []
    lang = ""
    for n, t in enumerate(lines):
        if n:
            time.sleep(PAUSE)
        q = {"q": t[:450], "langpair": "%s|%s" % (pair_src, dst)}
        if email:
            q["de"] = email
        data = json.loads(http(urllib.request.Request(MYMEMORY_URL + "?" + urllib.parse.urlencode(q),
                                                      headers={"User-Agent": USER_AGENT})))
        status = int(data.get("responseStatus", 200) or 200)
        if status == 429 or data.get("quotaFinished"):
            raise TranslateError("RATE_LIMIT", "MyMemory's daily limit is used up")
        if status != 200:
            raise TranslateError("SERVICE", str(data.get("responseDetails") or ("status %d" % status)))
        out = (data.get("responseData") or {}).get("translatedText") or ""
        lang = lang or str(data.get("detectedLanguage") or (data.get("responseData") or {}).get("detectedLanguage") or "")
        if n == 0 and not src_is_explicit(src) and lang and base(lang) == base(dst):
            return None, lang
        result.append(out.strip())
    return result, lang


# ---------------------------------------------------------------------------------------------------------------

def base(code):
    return (code or "").split("-")[0].split("_")[0].lower()


def src_is_explicit(src):
    return bool(src) and src.lower() != "auto"


def main():
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "USAGE", "detail": "usage: translate_lyrics.py <request.json>"}))
        return
    try:
        with open(sys.argv[1], "r", encoding="utf-8") as f:
            req = json.load(f)
        service = str(req.get("service") or "google").lower()
        src = str(req.get("from") or "auto")
        dst = str(req.get("to") or "en")
        lines = [str(x) for x in req.get("lines", [])]
    except Exception as e:  # unreadable request file
        print(json.dumps({"ok": False, "error": "REQUEST", "detail": str(e)}, ensure_ascii=False))
        return

    todo = [i for i, t in enumerate(lines) if t.strip()]
    if not todo:
        print(json.dumps({"ok": False, "error": "EMPTY", "detail": "there is nothing to translate"}))
        return
    if src_is_explicit(src) and base(src) == base(dst):
        print(json.dumps({"ok": True, "same": True, "lang": src}))
        return

    texts = [lines[i] for i in todo]
    try:
        if service == "mymemory":
            out, lang = mymemory_translate(texts, src, dst)
        else:
            out, lang = google_translate(texts, src, dst)
        if out is None:
            print(json.dumps({"ok": True, "same": True, "lang": lang}))
        else:
            result = [""] * len(lines)
            for i, t in zip(todo, out):
                result[i] = t
            print(json.dumps({"ok": True, "lang": lang or (src if src_is_explicit(src) else ""), "lines": result},
                             ensure_ascii=False))
    except TranslateError as e:
        print(json.dumps({"ok": False, "error": e.code, "detail": e.detail}, ensure_ascii=False))
    except Exception as e:  # never leave the app without an answer
        print(json.dumps({"ok": False, "error": "FAILED", "detail": str(e)}, ensure_ascii=False))
    sys.stdout.flush()


if __name__ == "__main__":
    main()
