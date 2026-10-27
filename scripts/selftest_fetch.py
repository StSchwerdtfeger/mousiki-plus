#!/usr/bin/env python3
"""Fetch health check: is everything Mousiki downloads or asks the internet for still working?

usage: selftest_fetch.py [--offline] [--verify-pins] [--only lyrics,cover,update,tools,pins,scripts] [--json]
       mousiki --selftest-fetch [the same options]

  lyrics   Better Lyrics and LRCLIB answer a known lookup (the two lyrics sources)
  cover    MusicBrainz finds a known album and the Cover Art Archive lists its front cover
  update   the GitHub releases page used by the update check answers
  tools    Python, ffmpeg, ffprobe, yt-dlp (and fpcalc, optional) are there and run
  pins     the files pinned in kanji_pins.json (and packaging/pins.json in a source checkout) still exist on their servers;
           with --verify-pins they are downloaded and compared with their pinned SHA-256 (a few MB)
  scripts  the helper scripts of this folder compile

Each line says OK, WARN (works partly or is optional), FAIL (broken) or SKIP. Exit code 0 = no FAIL, 1 = at least one FAIL.
Nothing is sent but the lookups of a famous song and album (Rick Astley - Never Gonna Give You Up; The Beatles - Abbey Road).
--offline runs only the checks that need no internet (tools, scripts, the pin files' syntax). Standard library only."""
import hashlib
import json
import os
import py_compile
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

OK, WARN, FAIL, SKIP = "OK", "WARN", "FAIL", "SKIP"
results = []   # (status, group, name, detail)


def add(status, group, name, detail=""):
    results.append((status, group, name, detail))


def timed(fn):
    t = time.time()
    try:
        return fn(), None, time.time() - t
    except Exception as e:   # noqa: BLE001 -- every failure is a result, not a crash
        return None, e, time.time() - t


def short(e):
    text = str(e).replace("\n", " ")
    return (type(e).__name__ + ": " + text)[:160]


# ---- scripts ---------------------------------------------------------------------------------------------------------
def check_scripts():
    names = sorted(f for f in os.listdir(HERE) if f.endswith(".py"))
    bad = []
    for n in names:
        try:
            py_compile.compile(os.path.join(HERE, n), cfile=os.path.join(tempfile.gettempdir(), "mousiki_selftest_" + n + "c"), doraise=True)
        except py_compile.PyCompileError as e:
            bad.append("%s (%s)" % (n, str(e).strip().splitlines()[-1][:80]))
    if bad:
        add(FAIL, "scripts", "helper scripts compile", "; ".join(bad))
    else:
        add(OK, "scripts", "helper scripts compile", "%d files" % len(names))
    v = sys.version_info
    add(OK if v >= (3, 8) else FAIL, "scripts", "Python version", "%d.%d.%d%s" % (v[0], v[1], v[2], "" if v >= (3, 8) else " (3.8 or newer is needed)"))
    try:
        import lrc   # noqa: F401
        add(OK, "scripts", "lrc.py loads (certificates, User-Agent)")
    except Exception as e:   # noqa: BLE001
        add(FAIL, "scripts", "lrc.py loads", short(e))


# ---- lyrics ----------------------------------------------------------------------------------------------------------
def check_lyrics():
    try:
        import lrc
    except Exception as e:   # noqa: BLE001
        add(FAIL, "lyrics", "lyrics module", short(e))
        return
    r, err, dt = timed(lambda: lrc.get_lrclib_lyrics("Never Gonna Give You Up", "Rick Astley"))
    if err:
        add(FAIL, "lyrics", "LRCLIB (line-synced lyrics)", short(err))
    else:
        n = len((r.get("lrc") or "").splitlines())
        add(OK if n > 10 else WARN, "lyrics", "LRCLIB (line-synced lyrics)", "%d lines, %.1f s" % (n, dt))
    r, err, dt = timed(lambda: lrc.get_better_lyrics("Never Gonna Give You Up", "Rick Astley"))
    if err:
        # Better Lyrics is the first source but LRCLIB takes over: only a warning while LRCLIB works
        lr_ok = any(s == OK and n.startswith("LRCLIB") for s, g, n, d in results)
        add(WARN if lr_ok else FAIL, "lyrics", "Better Lyrics (word-synced lyrics)", short(err) + ("  (LRCLIB takes over)" if lr_ok else ""))
    else:
        n = len((r.get("enhanced_lrc") or r.get("lrc") or "").splitlines())
        add(OK if n > 10 else WARN, "lyrics", "Better Lyrics (word-synced lyrics)", "%d lines, %.1f s" % (n, dt))


# ---- cover -----------------------------------------------------------------------------------------------------------
def check_cover():
    try:
        import fetch_cover
    except Exception as e:   # noqa: BLE001
        add(FAIL, "cover", "fetch_cover.py loads", short(e))
        return

    def find():
        found = fetch_cover.search_releases("Abbey Road", "The Beatles")
        if not found:
            raise RuntimeError("MusicBrainz found no release")
        return found

    found, err, dt = timed(find)
    if err:
        add(FAIL, "cover", "MusicBrainz release search", short(err))
        add(SKIP, "cover", "Cover Art Archive front cover", "needs a release")
        return
    add(OK, "cover", "MusicBrainz release search", "%d releases, %.1f s" % (len(found), dt))
    first = found[0]
    for kind, ident in (("release", first.get("mbid")), ("release-group", first.get("group"))):
        if not ident:
            continue
        listing, err, dt = timed(lambda k=kind, i=ident: fetch_cover.caa_listing(k, i))
        if not err and listing:
            url = fetch_cover.front_url(listing)
            add(OK if url else WARN, "cover", "Cover Art Archive front cover", ("%s listing, %.1f s" % (kind, dt)) if url else "listed, but no front image")
            return
    add(WARN, "cover", "Cover Art Archive front cover", "no listing for the test release (the archive may be busy)")


# ---- update check ----------------------------------------------------------------------------------------------------
def check_update():
    try:
        import lrc
    except Exception as e:   # noqa: BLE001
        add(FAIL, "update", "update check", short(e))
        return
    data, err, dt = timed(lambda: lrc.http_get_json("https://api.github.com/repos/StSchwerdtfeger/mousiki-plus/releases/latest", {}, timeout=10))
    if err and getattr(err, "code", None) == 404:
        add(OK, "update", "GitHub releases page", "reachable (no release published yet)")
    elif err:
        add(WARN, "update", "GitHub releases page", short(err) + "  (only the update notice is affected)")
    else:
        add(OK, "update", "GitHub releases page", "latest: %s, %.1f s" % (data.get("tag_name", "?"), dt))


# ---- tools -----------------------------------------------------------------------------------------------------------
def find_tool(name):
    exe = name + (".exe" if os.name == "nt" else "")
    for d in (os.path.join(HERE, "..", "bin"), os.path.join(HERE, "..", "bin", "ffmpeg"), os.path.join(HERE, "..")):
        p = os.path.abspath(os.path.join(d, exe))
        if os.path.isfile(p):
            return p
    return shutil.which(name)


def check_tools():
    for name, args, required in (("ffmpeg", ["-version"], True), ("ffprobe", ["-version"], True),
                                 ("yt-dlp", ["--version"], False), ("fpcalc", ["-version"], False)):
        path = find_tool(name)
        if not path:
            why = {"yt-dlp": "online sources and downloads need it", "fpcalc": "optional: audio fingerprint lookup (SHIFT+b)"}.get(name, "playback of many formats needs it")
            add(FAIL if required else WARN, "tools", name, "not found (%s)" % why)
            continue
        try:
            p = subprocess.run([path] + args, capture_output=True, text=True, timeout=20)
            first = (p.stdout or p.stderr).strip().splitlines()[:1]
            add(OK if p.returncode == 0 else WARN, "tools", name, "%s  %s" % (path, first[0][:60] if first else ""))
        except Exception as e:   # noqa: BLE001
            add(WARN, "tools", name, "%s does not run: %s" % (path, short(e)))


# ---- pins ------------------------------------------------------------------------------------------------------------
def load_pin_files():
    """[(label, {name: {url, sha256}})]: kanji_pins.json next to this script, packaging/pins.json in a source checkout."""
    out = []
    kp = os.path.join(HERE, "kanji_pins.json")
    if os.path.isfile(kp):
        try:
            with open(kp, encoding="utf-8") as fh:
                pk = json.load(fh).get("packages") or {}
            out.append(("kanji_pins.json", {v.get("file", k): v for k, v in pk.items()}))
        except Exception as e:   # noqa: BLE001
            add(FAIL, "pins", "kanji_pins.json", "unreadable: " + short(e))
    else:
        add(WARN, "pins", "kanji_pins.json", "missing: the kanji reader would take the newest files from pypi.org, checked only against PyPI's own hash")
    pp = os.path.join(HERE, "..", "packaging", "pins.json")
    if os.path.isfile(pp):
        try:
            with open(pp, encoding="utf-8") as fh:
                data = json.load(fh)
            tools = data.get("tools", data) if isinstance(data, dict) else {}
            out.append(("packaging/pins.json", {k: v for k, v in tools.items() if isinstance(v, dict) and "url" in v}))
        except Exception as e:   # noqa: BLE001
            add(FAIL, "pins", "packaging/pins.json", "unreadable: " + short(e))
    return out


def check_pins(offline, verify):
    UA = {"User-Agent": "mousiki-selftest"}
    for label, pins in load_pin_files():
        for name, pin in sorted(pins.items()):
            url, want = pin.get("url", ""), (pin.get("sha256") or "").lower()
            if len(want) != 64:
                add(FAIL, "pins", "%s: %s" % (label, name), "no valid SHA-256 pinned")
                continue
            if offline:
                add(SKIP, "pins", "%s: %s" % (label, name), "offline: pin present")
                continue
            if verify:
                def get(u=url):
                    with urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=120) as r:
                        return r.read()
                data, err, dt = timed(get)
                if err:
                    add(FAIL, "pins", "%s: %s" % (label, name), "download failed: " + short(err))
                    continue
                got = hashlib.sha256(data).hexdigest()
                if got == want:
                    add(OK, "pins", "%s: %s" % (label, name), "downloaded, SHA-256 matches (%d bytes, %.1f s)" % (len(data), dt))
                else:
                    add(FAIL, "pins", "%s: %s" % (label, name),
                        "SHA-256 DIFFERS: pinned %s, got %s. The file at its address has changed (or the download was damaged): "
                        "run the check again; if it stays, the pin must be updated (release.py pin)" % (want[:16] + "...", got[:16] + "..."))
            else:
                def head(u=url):
                    req = urllib.request.Request(u, headers=UA, method="HEAD")
                    with urllib.request.urlopen(req, timeout=30) as r:
                        return r.status
                st, err, dt = timed(head)
                if err:
                    add(FAIL, "pins", "%s: %s" % (label, name), "the pinned address does not answer: " + short(err))
                else:
                    add(OK, "pins", "%s: %s" % (label, name), "address answers (HTTP %s, %.1f s); --verify-pins also checks the hash" % (st, dt))


GROUPS = ("scripts", "tools", "lyrics", "cover", "update", "pins")


def main(argv):
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0
    offline = "--offline" in argv
    verify = "--verify-pins" in argv
    only = set(GROUPS)
    if "--only" in argv:
        i = argv.index("--only")
        only = set((argv[i + 1] if i + 1 < len(argv) else "").split(","))
        unknown = only - set(GROUPS)
        if unknown:
            print("unknown group(s): %s (choose from %s)" % (", ".join(sorted(unknown)), ", ".join(GROUPS)))
            return 2
    plan = (("scripts", check_scripts, False), ("tools", check_tools, False), ("lyrics", check_lyrics, True),
            ("cover", check_cover, True), ("update", check_update, True))
    for group, fn, needs_net in plan:
        if group not in only:
            continue
        if needs_net and offline:
            add(SKIP, group, group, "offline")
            continue
        fn()
    if "pins" in only:
        check_pins(offline, verify)

    if "--json" in argv:
        print(json.dumps([{"status": s, "group": g, "name": n, "detail": d} for s, g, n, d in results], ensure_ascii=False))
    else:
        print("Mousiki fetch health check%s" % (" (offline)" if offline else ""))
        last = None
        for s, g, n, d in results:
            if g != last:
                print("\n[%s]" % g)
                last = g
            print("  %-4s %s%s" % (s, n, ("  -  " + d) if d else ""))
        counts = {k: sum(1 for r in results if r[0] == k) for k in (OK, WARN, FAIL, SKIP)}
        print("\n%d ok, %d warnings, %d failed, %d skipped" % (counts[OK], counts[WARN], counts[FAIL], counts[SKIP]))
        if counts[FAIL]:
            print("A FAIL means that part of Mousiki cannot do its job right now. Network failures can also be temporary: run it again later.")
    return 1 if any(r[0] == FAIL for r in results) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
