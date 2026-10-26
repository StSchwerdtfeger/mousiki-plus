import json
import os
import re
import time
import signal
import sys

# Windows-only bug, and a real one: when this script's stdout/stderr are a
# pipe (which they always are here -- mousiki reads them via CreateProcess),
# Python picks a text encoding for them from the OS's locale settings. On
# Linux/macOS that's effectively always UTF-8, because a desktop session's
# LANG/LC_ALL (e.g. "en_US.UTF-8") is inherited by every child process,
# mousiki included, right down to this script. Windows has no equivalent
# environment variable, so Python falls back to the ANSI code page --
# which, unless the user has opted into the (off-by-default) "Use Unicode
# UTF-8 for worldwide language support" setting, cannot represent most
# non-Latin text. A track title in Japanese, Cyrillic, or anything outside
# that code page then throws UnicodeEncodeError the moment this script
# tries to print it -- which happens even on a lookup MISS, since the
# "no lyrics found for: <title>" message below embeds the original title.
# The process dies with a traceback on stderr (discarded) and nothing on
# stdout, which mousiki's C++ side reports as "lyrics helper script
# produced no output" -- indistinguishable, from the UI, from the script
# not existing at all. Forcing UTF-8 here removes the whole failure class
# regardless of the box's regional settings; errors="replace" is a last
# resort so a truly unencodable byte degrades to a replacement character
# instead of taking the whole fetch down with it.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# lrc.py (same directory) does the actual fetching: Better Lyrics first
# (word-level TTML, converted to enhanced LRC here in lrc.py itself),
# falling back to LRCLIB (line-synced only) if Better Lyrics has nothing.
# This script's only job is to call it and translate the result into the
# flat JSON object lyrics_fetcher.cpp already knows how to parse --
# {"ok", "enhanced", "lrc", "source"} on success, {"ok": false, "error",
# "detail"} on failure -- so nothing on the C++ side had to change to
# pick up the new source.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def load_lrc_module():
    """lrc.py (same directory) uses only Python's standard library -- no pip package is needed any more. None only
    when lrc.py itself cannot be loaded (a damaged installation)."""
    try:
        import lrc
        return lrc
    except ImportError:
        return None


def has_word_timestamps(lrc_text):
    """Check if LRC text actually contains word-level <mm:ss.xx> timestamps."""
    if not lrc_text:
        return False
    for line in lrc_text.split("\n"):
        bracket_end = line.find("]")
        if bracket_end < 0:
            continue
        rest = line[bracket_end + 1:]
        if "<" in rest and ">" in rest:
            return True
    return False


def clean_youtube_title(text):
    if '|' in text:
        text = text.split('|')[0]
    text = re.sub(r'\(.*?\)', '', text)
    text = re.sub(r'\[.*?\]', '', text)
    return text.strip()


def has_cjk(text):
    for ch in text:
        o = ord(ch)
        if 0x3040 <= o <= 0x30FF or 0x3400 <= o <= 0x9FFF or 0xAC00 <= o <= 0xD7A3 or 0xFF66 <= o <= 0xFF9F:
            return True
    return False


def script_parts(text):
    """(latin part, cjk part) of a name such as "Lemon (レモン)", "米津玄師 Kenshi Yonezu" or "夜に駆ける - Yoru ni kakeru".
    A part is "" when the name has none of that kind."""
    pieces = re.split(r"[\(\[（【\)\]）】]|\s[-–—|/]\s", text)
    latin, cjk = [], []
    for piece in pieces:
        piece = piece.strip()
        if not piece:
            continue
        if has_cjk(piece):
            tokens = piece.split()
            lt = [t for t in tokens if not has_cjk(t)]
            ct = [t for t in tokens if has_cjk(t)]
            if ct:
                cjk.append(" ".join(ct))
            if lt:
                latin.append(" ".join(lt))
        else:
            latin.append(piece)
    return " ".join(latin[:1]), " ".join(cjk[:1])


def name_variants(title, artist, alt_title, alt_artist, raw_title=""):
    """Further (label, (title, artist)) searches to try, most likely first, without repeats. At most 7."""
    t_lat, t_cjk = script_parts(raw_title or title)   # (the raw title still has the brackets that clean_youtube_title() removes)
    a_lat, a_cjk = script_parts(artist)
    cands = []
    if alt_title or alt_artist:
        cands.append(("Latin spelling", (alt_title or title, alt_artist or artist)))
    if t_lat and t_lat != title:
        cands.append(("title without the Japanese part", (t_lat, a_lat or alt_artist or artist)))
    if t_cjk and t_cjk != title:
        cands.append(("title without the Latin part", (t_cjk, a_cjk or artist)))
    if a_lat != artist or a_cjk != artist:
        if a_lat and a_lat != artist:
            cands.append(("artist without the Japanese part", (title, a_lat)))
        if a_cjk and a_cjk != artist:
            cands.append(("artist without the Latin part", (title, a_cjk)))
    cands.append(("title only", (title, "")))
    if alt_title:
        cands.append(("Latin title only", (alt_title, "")))
    seen = {(title.lower(), artist.lower())}
    out = []
    for label, (song, performer) in cands:
        key = (song.strip().lower(), performer.strip().lower())
        if not key[0] or key in seen:
            continue
        seen.add(key)
        out.append((label, (song.strip(), performer.strip())))
        if len(out) == 7:
            break
    return out


def main():
    if len(sys.argv) < 2:
        emit({
            "ok": False,
            "error": "EXCEPTION",
            "detail": "usage: fetch_lyrics.py <title> [artist]"
        })

    raw_title = sys.argv[1]
    title = clean_youtube_title(raw_title)
    artist = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else ""
    # optional: --variants  (try other spellings of the name when the first search finds nothing),
    #           --alt-title X --alt-artist Y  (the Latin-letters spelling Mousiki shows, made by the program itself)
    extra = sys.argv[2:]
    variants_on = "--variants" in extra
    alt_title = alt_artist = ""
    for i, a in enumerate(extra):
        if a == "--alt-title" and i + 1 < len(extra):
            alt_title = extra[i + 1]
        elif a == "--alt-artist" and i + 1 < len(extra):
            alt_artist = extra[i + 1]

    lrc = load_lrc_module()
    if lrc is None:
        emit({
            "ok": False,
            "error": "MODULE_MISSING",
            "detail": "scripts/lrc.py could not be loaded -- reinstall Mousiki"
        })

    # Hard 35-second alarm: two sources (Better Lyrics, then LRCLIB), each
    # with its own 10s network timeout inside lrc.py, times up to two
    # attempts (original query, then swapped title/artist below) -- worst
    # case that's up to ~40s of genuine network waiting. The alarm is a
    # last-resort backstop so a truly wedged call still returns *something*
    # to the C++ caller instead of hanging the lyrics fetch forever.
    try:
        signal.alarm(80 if variants_on else 35)
    except (AttributeError, OSError):
        pass  # Windows or restricted env -- no alarm, best-effort

    def do_fetch(song, performer):
        return lrc.get_lyrics(song, performer)

    result = None
    last_error = None
    used = ""

    try:
        result = do_fetch(title, artist)
    except Exception as e:
        last_error = str(e)

    # Retry with swapped Title/Artist if it failed and the title looks
    # like a YouTube-style "Artist - Title" string with no artist tag of
    # its own -- same heuristic the old syncedlyrics-backed version used.
    if result is None and "-" in title and not artist:
        parts = [p.strip() for p in title.split("-", 1)]
        if len(parts) == 2 and parts[0] and parts[1]:
            try:
                result = do_fetch(parts[1], parts[0])
                used = "swapped title / artist"
            except Exception as e:
                last_error = str(e)

    # Other spellings of the same names (Settings > ON/OFF > MODES > Lyrics Name Variants): lyrics sites often know a song under
    # only one of its names -- Latin letters or the original script -- so a search with a mixed or unusual spelling finds nothing
    # while the same search without the Latin or without the Japanese part does.
    if result is None and variants_on:
        started = time.time()
        for label, (song, performer) in name_variants(title, artist, alt_title, alt_artist, raw_title):
            if time.time() - started > 40:
                break
            try:
                result = do_fetch(song, performer)
            except Exception as e:
                last_error = str(e)
                result = None
            if result is not None and (result.get("enhanced_lrc") or result.get("lrc")):
                used = label
                break
            result = None

    if result is None:
        detail = last_error or f"no lyrics found for: {title} {artist}".strip()
        error = "EXCEPTION" if last_error else "NOT_FOUND"
        emit({"ok": False, "error": error, "detail": detail})

    # lrc.py hands back {"source", "ttml", "lrc", "enhanced_lrc"}. Prefer
    # the word-level enhanced LRC (only Better Lyrics provides one); fall
    # back to the plain line-synced LRC (LRCLIB, or Better Lyrics if its
    # TTML somehow had no per-word spans). Re-checking for actual "<...>"
    # timestamps rather than trusting which field was non-empty mirrors
    # the same "don't trust the flag, check the text" caution the old
    # script applied to syncedlyrics' `enhanced` request flag.
    lrc_text = result.get("enhanced_lrc") or result.get("lrc") or ""
    if not lrc_text:
        emit({
            "ok": False,
            "error": "NOT_FOUND",
            "detail": f"no lyrics found for: {title} {artist}".strip()
        })

    emit({
        "ok": True,
        "enhanced": has_word_timestamps(lrc_text),
        "lrc": lrc_text,
        "source": result.get("source", "lrc.py"),
        "variant": used,
    })


if __name__ == "__main__":
    main()
