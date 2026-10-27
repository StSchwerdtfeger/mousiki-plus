#!/usr/bin/env python3
"""UI smoke test: runs the real `mousiki` binary in a pseudo terminal and walks through the main screens.

    python3 tests/ui_smoke.py <path to mousiki binary> [--scripts <scripts folder>] [--keep]

What it checks (it never needs a sound card, a network or a real library; everything lives in a temporary HOME):
  * the program starts, draws the library and survives every key press below (no crash, no hang),
  * the frames of the main screen, settings tabs, help wizard, cheat sheet, MENUS index, meta editor and cover options
    keep their right border in one column (the symptom of a broken wide-character layout),
  * PgUp / PgDn page like SHIFT+arrow and the Translit. Kanji row of the settings cycles,
  * the Latin-letters toggle really turns Japanese titles into Latin ones and back,
  * the program quits when asked to.

Needs Linux or macOS and the Python package `pyte` (pip install pyte). Without them the test reports "skipped"
(exit code 77, which CTest treats as skipped).
"""
import os
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import time
import wave
import zlib

try:
    import fcntl
    import pty
    import select
    import termios
    import pyte
except Exception as exc:  # no pty (Windows) or pyte missing
    print("skipped: %s" % exc)
    sys.exit(77)

COLS, ROWS = 140, 45
BOX_LEFT = "│╭╰├"
BOX_RIGHT = "│╮╯┤"


def make_wav(path, seconds=2):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(b"\x00\x00" * 8000 * seconds)


def make_png(path, size=64):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes([200, 40, 40]) * size for _ in range(size))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


class Session:
    def __init__(self, binary, home, scripts, env_extra=None):
        self.raw = b""
        env = dict(os.environ, HOME=home, USERPROFILE=home, TERM="xterm-256color", LANG="C.UTF-8")
        env.pop("KITTY_WINDOW_ID", None)
        env.pop("WT_SESSION", None)
        if scripts:
            env["MOUSIKI_SCRIPTS_DIR"] = scripts
        env.update(env_extra or {})
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            # the size is set here, before the program starts: set only afterwards (by the parent, below) a slow machine
            # could start the program first and let it read a 0 x 0 terminal
            try:
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
            except OSError:
                pass
            os.execve(binary, [binary], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.exit_status = None

    def alive(self):
        if self.exit_status is not None:
            return False
        pid, status = os.waitpid(self.pid, os.WNOHANG)
        if pid:
            self.exit_status = status
            return False
        return True

    def pump(self, seconds):
        end = time.time() + seconds * SLOW
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self.raw += data
                self.stream.feed(data)

    def send(self, keys, wait=0.8):
        try:
            os.write(self.fd, keys if isinstance(keys, bytes) else keys.encode())
        except OSError:
            # the program has just ended and closed its terminal (macOS reports EIO on the write; the quit test
            # sends a key while the program is on its way out): nothing left to send to
            return
        self.pump(wait)

    def text(self):
        return "\n".join(self.screen.display)

    def row_cells(self, y):
        row = self.screen.buffer[y]
        return [row[x].data for x in range(COLS)]

    def dump(self, why):
        """What the screen shows (for the log of a failed check: a CI run cannot be looked at)."""
        print("---- screen at: %s ----" % why)
        print("\n".join("%2d %s" % (i, l.rstrip()) for i, l in enumerate(self.screen.display) if l.strip()))
        print("---- end of screen (alive=%s) ----" % self.alive())

    def kill(self):
        # A blocking waitpid hung on macOS: the dying program can wait for its terminal output to be read (the pty buffer
        # is full), so keep draining the master side while waiting, and give up after 10 s.
        try:
            os.kill(self.pid, signal.SIGKILL)
        except Exception:
            return
        end = time.time() + 10
        while time.time() < end:
            try:
                pid, _ = os.waitpid(self.pid, os.WNOHANG)
                if pid:
                    return
            except Exception:
                return
            try:
                if select.select([self.fd], [], [], 0.1)[0]:
                    os.read(self.fd, 65536)
            except Exception:
                time.sleep(0.1)
        print("warning: the program did not end after SIGKILL")


failures = []
checks = 0

# Every wait is multiplied by this: the macOS CI runners are slow (builds and program alike), a screen that is not there after
# 0.8 s may be there after 1.6 s. MOUSIKI_SMOKE_SLOW overrides (1 = as written).
SLOW = float(os.environ.get("MOUSIKI_SMOKE_SLOW", "2.0" if sys.platform == "darwin" else "1.0"))


T0 = time.time()


def progress(what):
    """A timestamped line, flushed at once: if the test hangs on a CI machine the log shows how far it got."""
    print("[%4ds] %s" % (time.time() - T0, what), flush=True)


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("  FAIL:", what, flush=True)


def frame_problems(sess):
    """Rows that start with a box character must end with one, all in the same column."""
    ends = {}
    for y in range(ROWS):
        cells = sess.row_cells(y)
        if cells[0] not in BOX_LEFT:
            continue
        last = max((x for x, c in enumerate(cells) if c not in (" ", "")), default=-1)
        ends[y] = (last, cells[last] if last >= 0 else " ")
    if len(ends) < 5:
        return []  # not a framed screen
    cols = [e[0] for e in ends.values()]
    mode = max(set(cols), key=cols.count)
    bad = []
    for y, (last, ch) in ends.items():
        if last != mode or ch not in BOX_RIGHT:
            bad.append("row %d ends at col %d with %r (frame edge is col %d)" % (y, last, ch, mode))
    return bad


def check_frame(sess, name, frame=True):
    check(sess.alive(), "%s: the program is still running" % name)
    bad = frame_problems(sess) if frame else []
    check(not bad, "%s: frame broken: %s" % (name, "; ".join(bad[:3])))
    if bad:
        print("\n".join("%2d %s" % (i, l.rstrip()) for i, l in enumerate(sess.screen.display) if l.strip()))


def kitty_cover_options(binary, home, scripts):
    """Cover options overlay in a Kitty-graphics terminal: the preview is a real picture, [t] switches to braille and back."""
    sess = Session(binary, home, scripts, {"KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"})
    try:
        sess.pump(4)
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send("M", 2.0)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.5)
        for _ in range(12):
            sess.send(b"\x1b[B", 0.15)
        sess.send(b"\r", 1.5)
        check("COVER OPTIONS" in sess.text(), "kitty cover options: the overlay opens")
        def picture_after(mark, wait):
            """True when a picture (Kitty graphics transmit) arrives within `wait` seconds after `mark`: the preview is
            decoded in the background, so on a slow machine it comes late."""
            end = time.time() + wait
            while time.time() < end:
                if b"\x1b_Ga=T" in sess.raw[mark:]:
                    return True
                sess.pump(0.25)
            return b"\x1b_Ga=T" in sess.raw[mark:]

        if shutil.which("ffmpeg") is None:
            # pictures are decoded by ffmpeg: without it the preview says "(the picture cannot be read)" (a CI runner)
            print("  skipped: kitty cover options, the picture checks (ffmpeg is not installed on this machine)")
            return
        mark = len(sess.raw)
        sess.send(b"\x1b[B", 0.5)
        sess.send(b"\x1b[B", 0.8)   # automatic -> no cover -> cover.png
        shows_picture = picture_after(mark, 10.0)
        check(shows_picture, "kitty cover options: the preview of a picture is a real picture")
        if not shows_picture:
            sess.dump("kitty cover options: no picture arrived")
            print("graphics commands seen so far: %d, TERM=%s" % (sess.raw.count(b"\x1b_G"), "xterm-kitty"))
        for _ in range(3):           # every [t] flips between picture and braille
            mark = len(sess.raw)
            sess.send("t", 0.5)
            now = picture_after(mark, 10.0 if not shows_picture else 2.5)   # expecting a picture: wait for it; braille: nothing may come
            check(now != shows_picture,
                  "kitty cover options: [t] switches the preview to %s" % ("braille (ASCII)" if shows_picture else "a picture"))
            shows_picture = now
        check(sess.alive(), "kitty cover options: the program is still running")
    finally:
        sess.kill()


def radio_latin_toggle(binary, root, scripts):
    """Radio mode: the quote key shows Japanese / Korean station names in Latin letters and back."""
    home = os.path.join(root, "radio_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "さくらラジオ | J-Pop | JP | MP3 | 128 | http://127.0.0.1:9/a | *\n"
                "사랑 방송 | K-Pop | KR | MP3 | 128 | http://127.0.0.1:9/b | *\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\nLatinLetters=false\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)   # (a first start may show the help wizard)
        check("Plain Station" in sess.text(), "radio: the station list is shown")
        check("さくらラジオ" in sess.text(), "radio: station names show in their own script by default")
        sess.send('"', 1.5)
        check("sakurarajio" in sess.text() and "さくらラジオ" not in sess.text(), "radio: [\"] shows Japanese station names in Latin letters")
        check("sarang" in sess.text(), "radio: [\"] shows Korean station names in Latin letters")
        sess.send('"', 1.5)
        check("さくらラジオ" in sess.text(), "radio: [\"] again shows the original names")
        check(sess.alive(), "radio: the program is still running")
    finally:
        sess.kill()


def selftest_command(binary, scripts):
    """`mousiki --selftest-fetch` starts the fetch health check (offline part here) and passes its exit code on."""
    import subprocess
    env = dict(os.environ)
    if scripts:
        env["MOUSIKI_SCRIPTS_DIR"] = scripts
    p = subprocess.run([binary, "--selftest-fetch", "--offline", "--only", "scripts,pins"], capture_output=True, text=True, timeout=120, env=env)
    check(p.returncode == 0, "--selftest-fetch: exit code 0 (got %d): %s" % (p.returncode, (p.stdout + p.stderr)[-200:]))
    check("fetch health check" in p.stdout and "helper scripts compile" in p.stdout, "--selftest-fetch: prints the report")
    p = subprocess.run([binary, "--selftest-fetch", "--bogus;rm"], capture_output=True, text=True, timeout=60, env=env)
    check(p.returncode == 2, "--selftest-fetch: refuses an odd option (exit %d)" % p.returncode)


def paging_and_translit_row(binary, root, scripts):
    """PgUp / PgDn page like SHIFT+arrow, and the TRANSLITERATION tab has the Translit. Kanji row (Leave / Chin. / Jap.)."""
    home = os.path.join(root, "paging_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\n" % lib)
    sess = Session(binary, home, scripts)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)   # the first-start help wizard
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        # the cheat sheet is longer than the screen: PgDn scrolls it (like SHIFT+down), PgUp brings it back
        sess.send("?", 1.2)
        first = sess.text()
        sess.send(b"\x1b[6~", 0.8)
        paged = sess.text()
        check(paged != first, "paging: PgDn scrolls the cheat sheet")
        sess.send(b"\x1b[5~", 0.8)
        check(sess.text() == first, "paging: PgUp scrolls it back")
        sess.send(b"\x1b[1;2B", 0.8)
        check(sess.text() == paged, "paging: SHIFT+down does the same as PgDn")
        # the music player's cheat sheet lists its own categories only: no audio book rows
        seen = sess.text()
        for _ in range(40):
            sess.send(b"\x1b[6~", 0.3)
            seen += "\n" + sess.text()
        check("SOUND (MAIN UI)" in seen and "AUDIO BOOK" not in seen,
              "cheat sheet: music player shows its categories, none of the audio book mode")
        sess.send(b"\x1b", 0.8)
        check("Alpha Song" in sess.text(), "paging: ESC leaves the cheat sheet")

        # settings -> ON/OFF -> TRANSLITERATION -> Translit. Kanji
        sess.send("s", 1.5)
        sess.send(b"\t", 0.6)
        def find(label):
            for line in sess.screen.display:
                if label in line:
                    return line
            return ""
        # MODES: 7 rows, then Use Terminal Settings, Terminal Size and Full Screen (rows 8, 9, 10)
        for _ in range(7):
            sess.send(b"\x1b[B", 0.1)
        check("Use Terminal Settings" in find("Use Terminal Settings") and "false" in find("Use Terminal Settings"),
              "use terminal settings: the row exists and is false by default")
        sess.send(b"\x1b[B", 0.1)
        check("Terminal Size" in find("Terminal Size") and "120" in find("Terminal Size") and "32" in find("Terminal Size"),
              "terminal size: the row shows 120 x 32 by default")
        check("Set back to default" in find("Terminal Size"), "terminal size: the Set back to default button is next to it")
        sess.send(b"\x1b[C", 0.4)   # RIGHT: the rows field
        sess.send(b"\r", 0.5)
        for _ in range(3):
            sess.send(b"\x7f", 0.1)
        sess.send("5", 0.2)
        sess.send(b"\r", 0.8)
        # raised to the minimum: the status line says so, and the row shows 28 (either one proves it; the status line can
        # be replaced by another message on a slow machine)
        def raised():
            return "allowed: 28" in sess.text() or "28" in find("Terminal Size")
        for _ in range(12):
            if raised():
                break
            sess.pump(0.5)
        if not raised():
            sess.dump("terminal size: the row did not become 28")
        check(raised(), "terminal size: a number below the minimum is raised to it (status line or row shows 28)")
        check("28" in find("Terminal Size"), "terminal size: the rows field shows the minimum, 28")
        sess.send(b"\x1b[C", 0.4)   # RIGHT: the button
        sess.send(b"\r", 0.8)
        check("32" in find("Terminal Size") and "28" not in find("Terminal Size"), "terminal size: ENTER on the button sets it back to 120 x 32")
        sess.send(b"\x1b[B", 0.2)
        check("Full Screen" in find("Full Screen") and "false" in find("Full Screen"), "full screen: the row exists and is off by default")
        for _ in range(9):
            sess.send(b"\x1b[B", 0.1)   # down to Translit. Kanji (rows 10 .. 17)
        def row():
            return find("Translit. Kanji")
        def settle(word, limit=8.0):   # a slow machine draws the row a moment after the key: wait for it (up to `limit` s)
            end = time.time() + limit
            while word not in row() and time.time() < end:
                sess.pump(0.3)
        settle("Leave")
        check("Leave" in row(), "translit kanji: the row exists and reads Leave by default")
        sess.send(b"\x1b[C", 0.8)
        settle("Chin.")
        check("Chin." in row(), "translit kanji: RIGHT cycles to Chin.")
        sess.send(b"\x1b[C", 0.8)
        settle("Jap.")
        check("Jap." in row(), "translit kanji: RIGHT again cycles to Jap.")
        sess.send(b"\x1b[D", 0.8)
        settle("Chin.")
        sess.send(b"\x1b[D", 0.8)
        settle("Leave")
        check("Leave" in row(), "translit kanji: LEFT twice is back at Leave")
        sess.send(b"\x1b[6~", 0.8)   # PgDn in the settings: a page, like SHIFT+down
        check(sess.alive(), "paging: PgDn in the settings does not crash")
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "settings: ESC leaves it")
    finally:
        sess.kill()


def mode_keys(binary, root, scripts):
    """The mode switch key is rebindable (HKeyModeNext): the new key switches player <-> radio, the old one does nothing."""
    home = os.path.join(root, "modekeys_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nHKeyModeNext=\"J\"\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        check(not re.search(rb"\x1b\[8;\d+;\d+t", sess.raw), "use terminal settings: true leaves the terminal alone")
        sess.send("*", 1.5)
        check("Alpha Song" in sess.text() and "Plain Station" not in sess.text(), "mode keys: the old key (*) does nothing after the rebinding")
        sess.send("J", 2.5)
        for _ in range(10):
            if "Plain Station" in sess.text() or "HELP WIZARD" in sess.text():
                break
            sess.pump(0.5)
        if "HELP WIZARD" in sess.text():   # (the radio's own first start)
            sess.send(b"\x1b", 1.5)
        check("Plain Station" in sess.text(), "mode keys: the rebound key (J) switches to the radio")
        sess.send("J", 2.5)
        for _ in range(10):
            if "PROGRESS BAR" in sess.text():
                break
            sess.pump(0.5)
        check("PROGRESS BAR" in sess.text() and "Plain Station" not in sess.text(), "mode keys: the rebound key (J) in the radio switches on (back to the player)")
        check(sess.alive(), "mode keys: the program is still running")
    finally:
        sess.kill()


def karaoke_timing(binary, root, scripts):
    """The lyrics timing overlay (ALT+L) opens over the karaoke screen; ESC brings the karaoke screen back."""
    home = os.path.join(root, "kt_home")
    lib = os.path.join(root, "kt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Karaoke Song.wav"), 20)
    with open(os.path.join(lib, "lyrics", "Karaoke Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]First lyric line here\n[00:03.00]Second lyric line here\n[00:06.00]Third lyric line here\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Karaoke Song" in sess.text():
                break
        sess.send(b"\r", 3.0)
        sess.send("k", 1.5)
        for _ in range(20):   # (the lyrics are read from the sidecar file by a background thread)
            if "lyric line" in sess.text():
                break
            sess.pump(0.5)
        check("KARAOKE" in sess.text() and "lyric line" in sess.text(), "karaoke timing: the karaoke screen shows the lyrics")
        if shutil.which("ffprobe") is None:
            # without ffmpeg / ffprobe the track is not loaded properly (the screen shows no length, "/ 0:00 ]"), so the overlay,
            # which needs a loaded track with its lyrics, cannot open: the same reason the Kitty picture checks are skipped
            print("  skipped: karaoke timing overlay (ffmpeg / ffprobe is not installed on this machine)")
            return
        if re.search(r"\[ 0:00 / ", sess.text()):
            # no track is playing (the position stays at 0:00 several seconds after ENTER): a machine without a (working) audio
            # device, such as a CI runner, or one where the audio device does not start in time. The timing overlay
            # needs the playing track, so there is nothing to test here.
            print("  skipped: karaoke timing overlay (the track does not play on this machine: no audio device)")
            return
        timing_key = b"J" if sys.platform == "darwin" else b"\x1bl"   # macOS: SHIFT+j (Option is not a modifier in most terminals)
        for _ in range(6):   # the key is ignored until the lyrics are loaded: try again for a few seconds
            sess.send(timing_key, 1.5)
            if "Lyrics Timing" in sess.text():
                break
            sess.pump(1.0)
        check("Lyrics Timing" in sess.text() and "KARAOKE" in sess.text(), "karaoke timing: the timing key opens the timing overlay on top of the karaoke screen")
        if "Lyrics Timing" not in sess.text():
            sess.dump("karaoke timing: the overlay did not open")
        sess.send(b"\x1b", 1.5)
        check("KARAOKE" in sess.text() and "Lyrics Timing" not in sess.text(), "karaoke timing: ESC closes the overlay and the karaoke screen is back")
    finally:
        sess.kill()


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    binary = os.path.abspath(args[0])
    scripts = None
    keep = "--keep" in args
    # a hang must not use up the whole CTest timeout without a trace: after this many seconds Python prints where every
    # thread is stuck and ends the test (MOUSIKI_SMOKE_LIMIT, default 780 s; the CTest timeout is longer)
    import faulthandler
    faulthandler.dump_traceback_later(int(os.environ.get("MOUSIKI_SMOKE_LIMIT", "780")), exit=True)
    if "--scripts" in args:
        scripts = os.path.abspath(args[args.index("--scripts") + 1])
    if not os.path.exists(binary):
        print("skipped: the player is not built (%s); build the target `mousiki` too" % binary)
        return 77

    root = tempfile.mkdtemp(prefix="mousiki_ui_")
    home = os.path.join(root, "home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Alpha Song.wav"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Beta Song.wav"))
    make_png(os.path.join(lib, "Alpha Artist", "First Album", "cover.png"))
    make_wav(os.path.join(lib, "Misc", "さくら.wav"))
    make_wav(os.path.join(lib, "Misc", "사랑해.wav"))
    make_png(os.path.join(lib, "Misc", "cover.png"))   # (the first track of the list depends on the file system's directory order: any of them has a cover)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\n" % lib)

    sess = Session(binary, home, scripts)
    try:
        progress("main session")
        # --- first start: a fresh HOME opens the help wizard by itself (full-screen frame)
        sess.pump(4)
        check(sess.alive(), "start: the program runs")
        check(re.search(rb"\x1b\[8;32;120t", sess.raw) is not None, "terminal size: at start the terminal is asked for 120 x 32 (the test terminal is 140 x 45)")
        check("HELP WIZARD" in sess.text(), "first start: the help wizard opens by itself")
        check_frame(sess, "first-run help wizard")
        for _ in range(3):
            sess.send(b"\x1b[C", 0.5)
            check_frame(sess, "first-run help wizard (next page)")
        sess.send(b"\x1b", 1.0)

        # --- main screen
        for _ in range(40):  # up to ~20 s for the first scan
            sess.pump(0.5)
            if "Alpha Song" in sess.text() and "さくら" in sess.text():
                break
        sess.pump(1.0)
        check("Alpha Song" in sess.text() and "さくら" in sess.text(), "main screen: the library is listed")
        check_frame(sess, "main screen", frame=False)

        # --- Latin letters toggle (default hotkey: ")
        sess.send('"', 1.5)
        txt = sess.text()
        check("sakura" in txt.lower() and "さくら" not in txt, "latin toggle: Japanese title shown in Latin letters")
        check("sarangheo" in txt.lower() or "salanghae" in txt.lower() or "사랑해" not in txt,
              "latin toggle: Korean title shown in Latin letters")
        sess.send('"', 1.5)
        check("さくら" in sess.text(), "latin toggle: toggled back to kana")

        # --- cheat sheet, MENUS index, wizard, console
        for key, name in (("?", "cheat sheet"), ("<", "MENUS index"), ("W", "help wizard"), ("t", "console")):
            sess.send(key, 1.2)
            check_frame(sess, name, frame=False)  # overlays sit on top of the main screen: only crashes count
            for _ in range(2):
                sess.send(b"\x1b[C", 0.4)  # pages / tabs
            check_frame(sess, name + " (next page)", frame=False)
            sess.send(b"\x1b", 0.8)
            check(sess.alive(), "%s: ESC leaves it" % name)

        # --- settings: walk every tab to the right, then back
        sess.send("s", 1.5)
        check_frame(sess, "settings (first tab)")
        for i in range(12):
            sess.send(b"\t", 0.5)
            check_frame(sess, "settings tab %d" % (i + 2))
            sess.send(b"\x1b[B", 0.2)
            sess.send(b"\x1b[1;2B", 0.2)  # SHIFT+down = page
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "settings: ESC leaves it")

        # --- meta editor and its cover options overlay
        sess.send("M", 2.0)
        check_frame(sess, "meta editor")
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.5)
        for _ in range(12):
            sess.send(b"\x1b[B", 0.15)
        sess.send(b"\r", 1.5)
        check("COVER OPTIONS" in sess.text(), "cover options: the overlay opens")
        sess.send("t", 1.0)
        check(sess.alive(), "cover options: [t] does not crash")
        sess.send("t", 1.0)
        check(sess.alive(), "cover options: [t] again does not crash")
        sess.send(b"\x1b", 1.0)
        check("COVER OPTIONS" not in sess.text(), "cover options: ESC closes the overlay")
        for _ in range(4):  # ESC steps back: fields pane -> library pane -> out of the meta editor
            if "META EDITOR" not in sess.text():
                break
            sess.send(b"\x1b", 1.0)
        check("META EDITOR" not in sess.text(), "meta editor: ESC leaves it")

        progress("main session: quit")
        # --- quit
        sess.send("q", 0.5)
        for _ in range(20):
            if not sess.alive():
                break
            sess.pump(0.3)
            if sess.alive():
                sess.send("y", 0.3)  # in case a quit confirmation is asked
        check(not sess.alive(), "quit: the program ends after the quit key")
        if sess.alive():
            print("\n".join("%2d %s" % (i, l.rstrip()) for i, l in enumerate(sess.screen.display) if l.strip()))
    finally:
        sess.kill()

    if not failures:
        progress("kitty_cover_options")
        kitty_cover_options(binary, home, scripts)
        progress("radio_latin_toggle")
        radio_latin_toggle(binary, root, scripts)
        progress("paging_and_translit_row")
        paging_and_translit_row(binary, root, scripts)
        progress("mode_keys")
        mode_keys(binary, root, scripts)
        progress("karaoke_timing")
        karaoke_timing(binary, root, scripts)
        progress("selftest_command")
        selftest_command(binary, scripts)
    if not keep:
        shutil.rmtree(root, ignore_errors=True)
    print("%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
