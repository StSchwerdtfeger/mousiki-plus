#!/usr/bin/env python3
import os, sys, re, shutil, time, tempfile
"""Dev tool (not run by CI): drives the music player with ~300 scripted keys (every overlay, menu and Settings tab) and writes the screen after
every key, with the animated parts and the numbers masked, plus the files it wrote, to a transcript. Use it for a change of the code that must
not change what the program does (moving members, splitting a class ...): run it with the old and the new binary and diff the two transcripts
(they are identical when nothing changed). Run it on an idle machine; run the OLD binary twice first -- two identical transcripts show that the
walk itself is steady.
  python3 tools/app_compare.py build/mousiki transcript.txt [scripts-dir]"""
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))
import ui_smoke as U

ESC = b"\x1b"
UP, DOWN, RIGHT, LEFT = b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D"
HOME_, END_, PGUP, PGDN = b"\x1b[H", b"\x1b[F", b"\x1b[5~", b"\x1b[6~"
SHL, SHR, SHD, SHU = b"\x1b[1;2D", b"\x1b[1;2C", b"\x1b[1;2B", b"\x1b[1;2A"
TAB, ENTER, BKSP, SPACE = b"\t", b"\r", b"\x7f", b" "

def seq():
    s = []
    def g(name, *keys):
        s.append((name, list(keys)))
    g("list-walk", DOWN, DOWN, UP, PGDN, PGUP, END_, HOME_, DOWN)
    g("cards", TAB, DOWN, UP, TAB, TAB, TAB)
    g("queue-keys", b"a", DOWN, b"a", b"e", b"4", b"5", b"!", b"$", b"%", b"d", b"X")
    g("search", b"/", b"al", BKSP, b"b", ENTER, ESC, ESC)
    g("search2", b"/", b"zzz", ESC, ESC)
    g("filter-folder", b"f", DOWN, ENTER, b"c")
    g("sort", b"T", b"T", b"T", b"T")
    g("play-modes", b"m", b"j", b"m", b"j")   # (back to where it was: shuffle would pick the next track by chance)
    g("volume", b"+", b"+", b"-", b"x", b"x")
    g("play", ENTER, b"p", b"p", b"n", b"b", RIGHT, LEFT)
    g("view-toggles", b"w", b"w", b".", b".", b",", b"k", b"k", b"*", b"_", b"r")
    g("mute-norm", b"V", DOWN, RIGHT, LEFT, ESC)
    g("cheat", b"?", DOWN, DOWN, PGDN, PGUP, UP, ESC)
    g("wizard", b"W", RIGHT, RIGHT, LEFT, ESC)
    g("history", b"H", DOWN, DOWN, UP, ENTER, ESC, ESC)
    g("console", b"t", UP, DOWN, ESC)
    g("menus", b"<", RIGHT, RIGHT, DOWN, LEFT, UP, ESC, ESC)
    g("menus-enter", b"<", DOWN, ENTER, ESC, ESC)
    g("audio-device", b"&", DOWN, UP, ESC)
    g("bars-menu", b"B", DOWN, RIGHT, LEFT, ESC)
    g("osc-menu", b"O", DOWN, RIGHT, LEFT, UP, ESC)
    g("spectro-menu", b"I", DOWN, RIGHT, LEFT, ESC)
    g("spectro-full", b"U", DOWN, UP, RIGHT, LEFT, ESC)
    g("scope-big", b"=", RIGHT, LEFT, UP, DOWN, ESC)
    g("scope-win", b")", ESC, b"(", ESC)
    g("list-overlay", b"L", DOWN, UP, TAB, ESC)
    g("folder-overlay", b"F", DOWN, UP, ESC)
    g("convert-overlay", b"C", DOWN, ESC)
    g("queue-overlay", b"K", DOWN, UP, ESC)
    g("playlist", b"P", DOWN, UP, b"n", b"abc", ENTER, ESC, ESC)
    g("playlist-add", b"A", DOWN, ESC, ESC)
    g("equaliser", b"E", RIGHT, RIGHT, UP, DOWN, LEFT, TAB, ESC)
    g("sleep", b"Z", DOWN, DOWN, ENTER, ESC, b"Z", ESC)
    g("creator", b"^", DOWN, TAB, ESC, ESC)
    g("rescan", b"R")
    g("style-options", b"S", DOWN, RIGHT, LEFT, ESC)
    g("bulk-add", b"G", ESC, ESC)
    g("meta-only", b"N", b"N")
    g("meta-editor", b"M", TAB, TAB, DOWN, DOWN, UP, ENTER, b"x", BKSP, ESC, ESC, ESC)
    g("meta-editor2", b"M", RIGHT, LEFT, DOWN, ESC, ESC)
    g("lyrics-retry", b"l", ESC)
    g("settings-walk", b"s", DOWN, DOWN, DOWN, RIGHT, LEFT, UP, TAB, DOWN, RIGHT, TAB, DOWN, DOWN, RIGHT, TAB, DOWN, RIGHT, TAB, DOWN, ENTER, ESC, ESC, ESC)
    g("settings-tabs", b"s", TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, ESC, ESC)
    g("settings-tabs-back", b"s", b"\x1b[Z", b"\x1b[Z", b"\x1b[Z", ESC, ESC)
    g("settings-edit", b"s", DOWN, DOWN, ENTER, b"abc", BKSP, ESC, ESC, ESC)
    g("settings-page", b"s", SHD, SHD, SHU, PGDN, PGUP, ESC, ESC)
    g("settings-theme", b"s", ENTER, DOWN, DOWN, UP, RIGHT, ESC, ESC, ESC)
    g("settings-theme2", b"s", b"l", ENTER, DOWN, RIGHT, LEFT, TAB, ESC, ESC, ESC, ESC)
    g("settings-theme-edit", b"s", b"e", DOWN, RIGHT, ENTER, ESC, ESC, ESC)
    g("settings-theme-io", b"s", b"E", b"x", ESC, b"I", ESC, ESC)
    g("settings-colors", b"s", TAB, DOWN, DOWN, RIGHT, LEFT, ENTER, ESC, ESC, ESC)
    g("settings-paths", b"s", TAB, TAB, TAB, DOWN, ENTER, b"xy", LEFT, BKSP, ESC, ESC, ESC)
    g("settings-keys", b"s", TAB, TAB, TAB, TAB, DOWN, ENTER, b"z", ESC, DOWN, ENTER, ESC, ESC, ESC)
    g("settings-ref", b"s", TAB, TAB, TAB, TAB, TAB, DOWN, DOWN, UP, ESC, ESC)
    g("settings-about", b"s", TAB, TAB, TAB, TAB, TAB, TAB, DOWN, DOWN, UP, ESC, ESC)
    g("settings-leave", b"s", DOWN, RIGHT, ESC, b"n", ESC, ESC)
    g("settings-leave2", b"s", DOWN, RIGHT, ESC, b"y")
    g("settings-ctrl", b"s", b"\x15", b"j", b"k", b"h", b"l", ESC, ESC)
    g("modes", b"*", b"_")
    g("end", b"p", b"p")
    return s

NORM_BRAILLE = re.compile("[⠀-⣿▀-▟■-◿]")
def norm(t):
    t = NORM_BRAILLE.sub(" ", t)
    t = re.sub(r"/tmp/appcmp_\w+", "/tmp/appcmp_X", t)
    t = re.sub(r"[0-9]", "#", t)
    t = re.sub(r"[ ]+", " ", t)
    return "\n".join(l.rstrip() for l in t.split("\n")).strip()

def settle(sess, first=0.35, limit=3.0):
    """Wait until the (masked) screen stops changing: a fixed sleep is too short for a load and too long for the rest."""
    sess.pump(first)
    last = norm(sess.text()); t0 = time.time()
    while time.time() - t0 < limit:
        sess.pump(0.25)
        cur = norm(sess.text())
        if cur == last: return cur
        last = cur
    return last

def main(binary, out, scripts=None):   # (scripts-dir argument kept for the same call as radio_compare.py; not used)
    binary = os.path.abspath(binary)
    root = tempfile.mkdtemp(prefix="appcmp_")
    scripts = os.path.join(root, "no_scripts")   # empty: no lyrics / cover / update lookup starts (they would answer after a time that varies)
    os.makedirs(scripts)
    home = os.path.join(root, "home"); lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    for n in ("Alpha Song", "Beta Song", "Gamma Song"): U.make_wav(os.path.join(lib, "Alpha Artist", "First Album", n + ".wav"), 900)
    U.make_wav(os.path.join(lib, "Misc", "さくら.wav"), 900); U.make_wav(os.path.join(lib, "Misc", "사랑해.wav"), 900)   # (long: a track that ends during the walk would make it depend on the time)
    U.make_png(os.path.join(lib, "Alpha Artist", "First Album", "cover.png"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("# test\nLocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    U.COLS, U.ROWS = 160, 50
    sess = U.Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1", "MOUSIKI_NO_MEDIA": "1"}, quiet_stderr=True)
    log = []
    try:
        for _ in range(40):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text(): break
        log.append("=== first start\n%s\n" % norm(sess.text()))
        for k in (RIGHT, RIGHT, RIGHT, ESC):
            sess.send(k, 0.4); log.append("=== wizard %r\n%s\n" % (k, norm(sess.text())))
        for _ in range(40):
            sess.pump(0.5)
            if "Alpha Song" in sess.text() and "さくら" in sess.text(): break
        sess.pump(1.0)
        log.append("=== library\n%s\n" % settle(sess))
        for name, keys in seq():
            for i, k in enumerate(keys):
                sess.send(k, 0.0)
                scr = settle(sess)
                alive = sess.alive()
                log.append("=== %s #%d %r alive=%s\n%s\n" % (name, i, k, alive, scr))
                if not alive: break
            if not sess.alive(): break
            sess.send(ESC, 0.3); sess.send(ESC, 0.3)
        sess.send(b"q", 1.0)
        log.append("=== after q alive=%s" % sess.alive())
    finally:
        sess.kill()
        for dp, dn, fn in sorted(os.walk(home)):
            for f in sorted(fn):
                fp = os.path.join(dp, f)
                if "cache" in fp or "control" in fp or f.endswith((".pcm", ".png", ".jpg")): continue
                try: body = open(fp, encoding="utf-8", errors="replace").read()
                except OSError: continue
                log.append("=== FILE %s\n%s\n" % (os.path.relpath(fp, home), re.sub(r"/tmp/appcmp_\w+", "/tmp/appcmp_X", body)))
        shutil.rmtree(root, ignore_errors=True)
    open(out, "w", encoding="utf-8").write("\n".join(log))
    print(out, len(log), "snapshots")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], *(sys.argv[3:4] or []))
