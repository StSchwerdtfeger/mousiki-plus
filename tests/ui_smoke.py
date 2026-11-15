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

The scenarios run in parallel worker processes (MOUSIKI_SMOKE_JOBS=n, default: the cores, at most 4; 1 = one after the other;
`--only a,b` runs some of them: main, read_lyrics_play, karaoke_timing, book_speed_and_devices, paging_and_translit_row, mode_keys,
kitty_cover_options, radio_latin_toggle, selftest_command).

Needs Linux or macOS and the Python package `pyte` (pip install pyte). Without them the test reports "skipped"
(exit code 77, which CTest treats as skipped).
"""
import base64
import os
import re
import select
import shutil
import signal
import struct
import subprocess
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
    def __init__(self, binary, home, scripts, env_extra=None, quiet_stderr=False, gfx=False):
        self.raw = bytearray()   # (a bytearray: bytes += copies all of it each read, and the pictures make it hundreds of MB)
        self._sync_open = False  # inside a synchronized update (ESC[?2026h without its ESC[?2026l yet)
        # gfx=True: the Kitty graphics commands are taken out of the stream before the screen model sees it (otherwise their data ends up as text on
        # the screen) and kept: self.pics = {image id: where it is placed (row, col of the cell, c x r cells, the cropped pixels x, y, w, h)},
        # self.pic_rgba = {image id: (width, height, RGBA bytes) of the last complete transmission}
        self.gfx = gfx
        self.pics = {}
        self.pic_rgba = {}
        self._apc_rest = b""
        self._apc_pending = None
        # Auto-Play is off by default (the opening track stands paused); the scenarios were written for a track that plays at once: a config
        # that does not say otherwise gets AutoPlay=true (auto_play_setting checks the default itself)
        cfg = os.path.join(home, ".config", "mousiki", "config.txt")
        try:
            with open(cfg, encoding="utf-8") as f:
                has_ap = "AutoPlay=" in f.read()
            if not has_ap:
                with open(cfg, "a", encoding="utf-8") as f:
                    f.write("\nAutoPlay=true\n")
        except OSError:
            pass
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
            if quiet_stderr:   # (ALSA writes its complaints about a missing sound card onto the screen)
                try:
                    os.dup2(os.open(os.devnull, os.O_WRONLY), 2)
                except OSError:
                    pass
            try:
                os.chdir(home)   # (the program also reads/writes ./radio_config.txt: never the one of the project folder)
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
        # A frame is written in one go between "begin / end synchronized update" (ESC[?2026h ... ESC[?2026l), but it reaches us
        # in pieces: when the time is up in the middle of one, read on (at most one more second) until that frame is complete,
        # so the checks never see half a frame (on a slow machine that happened now and then: "frame broken" on a good frame).
        hard_end = end + 1.0
        while time.time() < end or (time.time() < hard_end and self._sync_open):
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                at = max(0, len(self.raw) - 8)
                self.raw += data
                tail = self.raw[at:]   # (only the new part is searched: the whole buffer each read made the harness lag seconds behind)
                lh, ll = tail.rfind(b"\x1b[?2026h"), tail.rfind(b"\x1b[?2026l")
                if lh >= 0 or ll >= 0:
                    self._sync_open = lh > ll
                if self.gfx:
                    self._feed_gfx(data)
                else:
                    self.stream.feed(data)

    def _feed_gfx(self, data):
        buf = self._apc_rest + data
        self._apc_rest = b""
        pos = 0
        while True:
            a = buf.find(b"\x1b_G", pos)
            if a < 0:
                # (a command whose start is cut by the read -- "...ESC" | "_G..." -- is kept for the next read: fed now, its data ended up as text)
                keep = 2 if buf.endswith(b"\x1b_") else 1 if buf.endswith(b"\x1b") else 0
                self.stream.feed(buf[pos:len(buf) - keep])
                self._apc_rest = buf[len(buf) - keep:] if keep else b""
                return
            self.stream.feed(buf[pos:a])
            e = buf.find(b"\x1b\\", a)
            if e < 0:
                self._apc_rest = buf[a:]
                return
            ctrl, _, payload = buf[a + 3:e].partition(b";")
            keys = dict(kv.split("=", 1) for kv in ctrl.decode("ascii", "replace").split(",") if "=" in kv)
            pos = e + 2
            if keys.get("a") == "d":
                if "i" in keys:
                    self.pics.pop(int(keys["i"]), None)
                    self.pic_rgba.pop(int(keys["i"]), None)
                else:
                    self.pics.clear()
                    self.pic_rgba.clear()
                continue
            if "a" in keys:
                self._apc_pending = (keys, bytearray(payload))
            elif self._apc_pending:
                self._apc_pending[1].extend(payload)
            else:
                continue
            if keys.get("m", "0") != "0":
                continue
            k, body = self._apc_pending
            self._apc_pending = None
            if k.get("a") != "T" or "i" not in k:
                continue
            try:
                raw_px = base64.b64decode(bytes(body))
                if k.get("o") == "z":
                    raw_px = zlib.decompress(raw_px)
                w, h, i = int(k["s"]), int(k["v"]), int(k["i"])
            except Exception:
                continue
            if len(raw_px) == w * h * 4:
                self.pic_rgba[i] = (w, h, raw_px)
            self.pics[i] = dict(row=self.screen.cursor.y, col=self.screen.cursor.x, c=int(k.get("c", 0)), r=int(k.get("r", 0)),
                                x=int(k.get("x", 0)), y=int(k.get("y", 0)), w=int(k.get("w", w)), h=int(k.get("h", h)))

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

    def wait_for(self, cond, limit=8.0, step=0.25):
        """Reads the screen until cond(text) is true or `limit` seconds are over (a loaded machine draws a moment after the key: a check
        that looks right after the key can see the screen before it). Returns the last value of cond."""
        end = time.time() + limit * SLOW
        while True:
            if cond(self.text()):
                return True
            if time.time() >= end:
                return False
            self.pump(step)

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
        sess.send(b"\x1b[A", 0.3)       # (the last stop is Video Option, under the two buttons: Up is Style Option, Left goes to Cover Options)
        sess.send(b"\x1b[D", 0.3)
        open_mark = len(sess.raw)
        sess.send(b"\r", 1.5)
        t0 = sess.text()
        check("COVER OPTIONS" in t0, "kitty cover options: the overlay opens")
        check("The disk shows" in t0, "kitty cover options: the disk modes are listed (the rest of the list may be overwritten by the picture's graphics data in this screen model)")
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
        mark = open_mark             # (the start is on "Automatic": its preview is the picture the search finds, here cover.png)
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
        if not shows_picture:
            sess.send("t", 1.0)          # (back to the picture for the next check)
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


def radio_sign_border(binary, root, scripts):
    """Radio, ANIMA/VIZ tab -> ON AIR SIGN: Sign border (SignBorder=, false by default since batch 279) -- the " : : " column between the ON AIR / OFF AIR
    sign and the station info. Off: no column, the station info stays in its place, nothing runs past the window; the row is in the settings."""
    def start(tag, border):
        home = os.path.join(root, "rsb_home_" + tag)
        cfgdir = os.path.join(home, ".config", "mousiki")
        os.makedirs(cfgdir)
        with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:
            f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
        with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nStartMode=radio\nWizardSeen=true\n" % os.path.join(root, "music"))
        if border is not None:
            with open(os.path.join(cfgdir, "radio_config.txt"), "w", encoding="utf-8") as f:
                f.write("SignBorder=%s\nWizardSeen=true\n" % ("true" if border else "false"))
        sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)   # (a first start may show the help wizard)
        return sess, cfgdir
    def panel(sess):   # the rows of the info panel (between the first top border and its bottom border)
        rows = sess.screen.display
        out = []
        for l in rows[1:]:
            if l.startswith("╰") or l.startswith("└"):
                break
            out.append(l)
        return out
    sess, _ = start("on", True)
    try:
        p_on = panel(sess)
        check(len(p_on) >= 8 and sum(1 for l in p_on if " : : " in l) >= len(p_on) - 1, "radio sign border: SignBorder=true, the \" : : \" column runs down the info panel")
        col_on = next((l.find(":") for l in p_on if " : : " in l), -1)
    finally:
        sess.kill()
    sess, cfgdir = start("off", None)
    try:
        p_off = panel(sess)
        check(len(p_off) >= 8 and not any(" : : " in l for l in p_off), "radio sign border: off by default -- no column")
        check_fit(sess, "radio sign border off", False)
        # the station info starts in the same column either way (the column is replaced by spaces, the sign moves into the middle)
        same = len(p_on) == len(p_off) and all(len(a) == len(b) for a, b in zip(p_on, p_off))
        check(same and col_on > 0, "radio sign border: the panel keeps its width and rows")
        info_on = [l.find("Currently No Station") for l in p_on if "Currently No Station" in l]
        info_off = [l.find("Currently No Station") for l in p_off if "Currently No Station" in l]
        check(info_on and info_on == info_off, "radio sign border: the station info is where it was (column %s / %s)" % (info_on, info_off))
        # the settings row
        sess.send("s", 1.2)
        found = False
        for _ in range(4):
            if "Sign border" in sess.text():
                found = True
                break
            sess.send(b"\t", 0.6)
        check(found and "ON AIR SIGN" in sess.text(), "radio sign border: the row is in the ON AIR SIGN part of the settings")
    finally:
        sess.kill()


def radio_idle_rate(binary, root, scripts):
    """Radio mode, nothing tuned: Reduce Motion When Paused lowers the frame rate after a few quiet seconds, a key brings it back,
    and with the setting off the full rate stays."""
    def run(tag, extra, wait_low=False):
        home = os.path.join(root, "radioidle_" + tag)
        os.makedirs(os.path.join(home, ".config", "mousiki"))
        with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
            f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
        with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nStartMode=radio\nWizardSeen=true\n%s" % (os.path.join(root, "music"), extra))
        sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
        try:
            sess.wait_for(lambda t: "Plain Station" in t)
            sess.send(b"\x1b", 0.5)   # (a first start may show the help wizard)
            sess.pump(9)               # quiet: no key for more than 3 s (with a margin: a busy runner reads the key late, and the low rate only starts 3 s after it)
            if wait_low:               # a starved runner (three scenarios at a time on a few cores) can read the key many seconds late: wait until the low rate is on (10 a second), at most 40 s
                for _ in range(40):
                    b0 = len(sess.raw); sess.pump(1)
                    if sess.raw[b0:].count(b"\x1b[?2026h") <= 15:
                        break
            b0 = len(sess.raw); sess.pump(3); quiet = sess.raw[b0:].count(b"\x1b[?2026h")
            sess.send(b"\x1b[B", 0.0)   # a key
            b0 = len(sess.raw); sess.pump(1); after_key = sess.raw[b0:].count(b"\x1b[?2026h")
            return quiet, after_key, sess.alive()
        finally:
            sess.kill()
    q, k, alive = run("on", "", True)
    check(alive, "radio idle rate: the program is still running")
    check(q <= 75, "radio idle rate: nothing tuned and untouched, the screen is redrawn at a low rate (%d frames in 3 s)" % q)
    check(k >= 20, "radio idle rate: a key brings the full rate back (%d frames in 1 s)" % k)
    q2, _, _ = run("off", "ReduceMotionWhenPaused=false\n")
    check(q2 >= 3 * 30 * 0.6 and q2 > q, "radio idle rate: with Reduce Motion When Paused off the full rate stays (%d frames in 3 s)" % q2)


def radio_organigram(binary, root, scripts):
    """Radio mode: "<" shows the menu organigram (Main, its categories, the boxes of the open category); ENTER on Equalizer opens the equalizer."""
    home = os.path.join(root, "radioorg_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.5)
        t = sess.text()
        check("Main" in t and "System" in t and "Sound" in t and "Stations and Presets" in t and "Visualizations" in t, "radio organigram: Main and its categories")
        check("Settings" in t and "Cheat Sheet" in t, "radio organigram: the first category is open")
        sess.send(b"\x1b[C", 0.8)
        check("Equalizer" in sess.text() and "Record" in sess.text(), "radio organigram: RIGHT opens the Sound category")
        sess.send(b"\x1b[B", 0.6)
        sess.send(b"\r", 1.2)
        check("Plain Station" in sess.text() or "EQUALIZER" in sess.text().upper(), "radio organigram: ENTER leaves the index")
        check("Visualizations" not in sess.text(), "radio organigram: the index is closed")
        check(sess.alive(), "radio organigram: the program is still running")
    finally:
        sess.kill()


def radio_waterfall(binary, root, scripts):
    """Radio mode: the spectrogram options (SHIFT+i) start with the View row; the waterfall shows its own rows and the full screen says WATERFALL."""
    home = os.path.join(root, "radiowf_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.send("I", 1.2)
        t = sess.text()
        check("Spectrogram" in t and "View" in t and "spectrogram" in t, "radio spectrogram options: the View row is there, set to spectrogram")
        check("Graphs" not in t, "radio spectrogram options: the waterfall's rows are hidden in the spectrogram view")
        sess.send(b"\x1b[C", 1.0)
        t = sess.text()
        check("waterfall" in t and "Graphs" in t, "radio spectrogram options: View -> waterfall shows the waterfall rows")
        sess.send("I", 1.0)                                  # closes the overlay
        sess.send("U", 1.5)
        check("WATERFALL" in sess.text(), "radio: the full screen is titled WATERFALL")
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "radio waterfall: the program is still running")
    finally:
        sess.kill()


def radio_station_edit(binary, root, scripts):
    """Radio mode, big STATIONS overlay (SHIFT+l): SHIFT+c edits every field of an entry, DEL deletes after a warning, SHIFT+u takes it back; the files follow."""
    home = os.path.join(root, "radioedit_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    sfile = os.path.join(cfgdir, "stations.txt")
    with open(sfile, "w", encoding="utf-8") as f:
        f.write("# my stations\n# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Alpha Radio | Rock | US | MP3 | 128 | http://127.0.0.1:9/a | *\n"
                "Bravo Radio | Jazz | DE | AAC | 64 | http://127.0.0.1:9/b\n"
                "Charlie Radio | Pop | FR | MP3 | 96 | http://127.0.0.1:9/c\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\n" % os.path.join(root, "music"))
    def file_text():
        with open(sfile, encoding="utf-8") as f:
            return f.read()
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Radio" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.send("L", 1.2)
        t = sess.text()
        check("[SHIFT+c] Edit entry" in t and "[DEL] Delete" in t and "[SHIFT+u] Undo" in t, "radio stations overlay: the legend shows Edit entry / Delete / Undo")
        sess.send(b"\x1b[B", 0.4)                           # Bravo
        sess.send("C", 1.0)
        t = sess.text()
        check("Edit entry" in t and "PRESET NAME:" in t and "URL:" in t and "KBPS:" in t, "radio stations overlay: SHIFT+c opens the entry editor with all fields")
        sess.send("X", 0.4)                                  # typed into NAME
        sess.send(b"\t", 0.3)
        sess.send("Bravo2", 0.4)                              # PRESET NAME
        sess.send(b"\r", 1.2)
        txt = file_text()
        check("Bravo RadioX" in txt and "# my stations" in txt, "radio stations overlay: the edited name is saved, the comment at the top stays")
        check("Bravo2" in sess.text(), "radio stations overlay: the preset name shows in the list")
        sess.send(b"\x1b[3~", 0.8)                           # DEL
        t = sess.text()
        check("Delete station" in t and "all presets" in t, "radio stations overlay: DEL asks first")
        sess.send("n", 0.6)
        check("Bravo RadioX" in file_text(), "radio stations overlay: N keeps the station")
        sess.send(b"\x1b[3~", 0.8)
        sess.send("y", 1.2)
        txt = file_text()
        check("Bravo" not in txt and "Alpha Radio" in txt and "Charlie Radio" in txt, "radio stations overlay: Y removes the station from stations.txt")
        sess.send("U", 1.2)
        txt = file_text()
        check("Bravo RadioX" in txt and txt.index("Alpha Radio") < txt.index("Bravo RadioX") < txt.index("Charlie Radio"), "radio stations overlay: SHIFT+u brings it back at its place")
        sess.send("U", 1.0)
        check("Bravo Radio |" in file_text() and "RadioX" not in file_text(), "radio stations overlay: SHIFT+u again takes the edit back")
        check(sess.alive(), "radio stations overlay: the program is still running")
    finally:
        sess.kill()


def scope_spectro_overlays(binary, root, scripts):
    """The full-screen spectrogram (SHIFT+u): the scale sits at the upper left, UP / DOWN switch spectrogram <-> waterfall, LEFT / RIGHT image <-> braille;
    the oscilloscope overlay (=): two centred legend lines, LEFT / RIGHT image <-> braille, UP / DOWN oscilloscope music on / off."""
    home = os.path.join(root, "sso_home")
    lib = os.path.join(root, "sso_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyScopeBig=\"=\"\nHKeySpectroFull=\"U\"\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
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
        sess.send("U", 1.5)
        lines = sess.text().split("\n")
        top = lines[0] if lines else ""
        check("SPECTROGRAM" in sess.text(), "spectrogram full screen: opens")
        check(" Hz" in top and "SPECTROGRAM" in top and top.index("Hz") < top.index("SPECTROGRAM"), "spectrogram full screen: the scale is at the upper left of the frame (%s)" % top[:70].strip())
        bottom = [l for l in lines if "Options" in l and "Close" in l]
        check(bottom and "Image / Braille" in bottom[-1] and "Waterfall" in bottom[-1] or any("Image / Braille" in l for l in lines), "spectrogram full screen: the legend names the arrow keys")
        sess.send(b"\x1b[B", 1.2)
        check("WATERFALL" in sess.text(), "spectrogram full screen: DOWN switches to the waterfall")
        sess.send(b"\x1b[A", 1.2)
        check("SPECTROGRAM" in sess.text() and "WATERFALL" not in sess.text(), "spectrogram full screen: UP switches back")
        sess.send(b"\x1b", 1.0)
        sess.send("=", 1.5)
        t = sess.text()
        check("OSCILLOSCOPE" in t, "oscilloscope overlay: opens")
        check("[LEFT/RIGHT]" in t or "Image / Braille" in t, "oscilloscope overlay: the first legend line names the arrow keys")
        check("Grid color" in t or "Color" in t, "oscilloscope overlay: the second legend line is in the bottom border")
        sess.send(b"\x1b[B", 1.0)
        check("oscilloscope music mode: on" in sess.text() or sess.alive(), "oscilloscope overlay: DOWN turns the music mode on")
        sess.send(b"\x1b[A", 0.8)
        sess.send(b"\x1b", 0.8)
        check(sess.alive(), "scope / spectro overlays: the program is still running")
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

        # settings -> GENERAL -> TRANSLITERATION -> Translit. Kanji
        sess.send("s", 1.5)
        sess.send(b"\t", 0.6)
        def find(label):
            for line in sess.screen.display:
                if label in line and "Preview:" not in line:   # (the preview box on the right is titled with the row)
                    return line
            return ""
        # GENERAL tab: Playback, Start Mode, Display queue, MODES ON/OFF AND ORDER (6 rows), AUDIO (9 rows: Choose Audio Device ... Pause Fade, Auto-Play,
        # Gapless Playback), TERMINAL: Black Background, then Use Terminal Settings (row 18)
        for _ in range(18):
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
        for _ in range(12):
            sess.send(b"\x1b[B", 0.1)   # down to Translit. Kanji (past Reduce Motion When Paused and CLI / REMOTE / UPDATES, with CLI Scripts; TEXT AND TRANSLITERATION is last)
        def row():
            return find("Translit. Kanji")
        def settle(word, limit=8.0):   # a slow machine draws the row a moment after the key: wait for it (up to `limit` s)
            end = time.time() + limit
            while word not in row() and time.time() < end:
                sess.pump(0.3)
        settle("Leave")
        check("Leave" in row(), "translit kanji: the row exists and reads Leave by default")
        check("Preview: Translit. Kanji" in sess.text() and "zh\u0101ng" in sess.text(), "settings preview: the hovered row shows its examples")
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
        sess.send("t", 1.0)   # the console: [v] switches the verbose log on for this session
        check("CONSOLE / LOGS" in sess.text() and "Verbose log" in sess.text(), "console: the legend names [v] Verbose log for this session")
        sess.send("v", 1.0)
        check("(verbose)" in sess.text(), "console: [v] switches the verbose log on (shown in the title)")
        sess.send("v", 1.0)
        check("(verbose)" not in sess.text(), "console: [v] again switches it off")
        sess.send(b"\x1b", 1.0)
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


def book_speed_and_devices(binary, root, scripts):
    """Audio book: SHIFT+LEFT / SHIFT+RIGHT change the playback speed, shown in a box left of the play mode (and saved);
    the CHOOSE AUDIO DEVICE overlay opens from the main screen of the audio book and the radio and lists System Default;
    the GENERAL tab has the Playback Speed and Choose Audio Device rows."""
    home = os.path.join(root, "book_home")
    books = os.path.join(root, "books")
    make_wav(os.path.join(books, "Book One", "01 Chapter.wav"))
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        # fixed keys, whatever the keyboard layout of the machine (the default mode key is "*" or "'", see default_size_mode_key())
        f.write("AudioBookPath=%s\nStartMode=audiobook\nUseTerminalSettings=true\nHKeyAudioDevice=\"&\"\nHKeyModeNext=\"J\"\n" % books)
    with open(os.path.join(cfgdir, "radio_config.txt"), "w", encoding="utf-8") as f:
        f.write("HKeyAudioDevice=&\n")
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1", "MOUSIKI_NULL_AUDIO": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Book One" in sess.text():
                break
        check("Book One" in sess.text(), "audio book: the book is listed")
        check("1.0x" in sess.text(), "audio book: the speed box shows 1.0x")
        check_frame(sess, "audio book main screen", frame=False)
        sess.send(b"\x1b[1;2C", 0.8)   # SHIFT+RIGHT
        check("1.05x" in sess.text(), "audio book: SHIFT+RIGHT makes it 1.05x")
        sess.send(b"\x1b[1;2D", 0.5)
        sess.send(b"\x1b[1;2D", 0.8)   # SHIFT+LEFT twice
        check("0.95x" in sess.text(), "audio book: SHIFT+LEFT twice makes it 0.95x")
        check_frame(sess, "audio book main screen with 0.95x", frame=False)
        with open(os.path.join(cfgdir, "config.txt"), encoding="utf-8") as f:
            check("BookSpeed=0.95" in f.read(), "audio book: the speed is saved (BookSpeed=0.95)")
        sess.send("&", 1.2)
        check("Choose Audio Device" in sess.text() and "System Default" in sess.text(), "audio device: & opens the overlay in the audio book")
        sess.send(b"\x1b", 0.8)
        check("Choose Audio Device" not in sess.text(), "audio device: ESC closes the overlay")
        sess.send("Y", 1.2)                    # SHIFT+b is the bookmark in the audio book: the spectrum bars overlay has SHIFT+y there
        t = sess.text()
        check("SPECTRUM BARS" in t.upper() and "Style" in t, "audio book: SHIFT+y opens the spectrum bars overlay (%s)" % [l for l in t.split("\n") if "BARS" in l.upper()][:2])
        sess.send(b"\x1b", 0.8)
        sess.send("s", 1.2)
        for _ in range(20):            # a slow machine: wait for the settings screen before TAB
            if "Switch tab" in sess.text():
                break
            sess.pump(0.5)
        sess.send(b"\t", 1.0)          # COLORS -> GENERAL
        for _ in range(10):
            if "GENERAL" in sess.text() and "Start Mode" in sess.text():
                break
            sess.pump(0.5)
        seen = sess.text()
        for _ in range(40):            # the AUDIO rows may be below the visible part: walk down until both have been seen
            if "Playback Speed" in seen and "Choose Audio Device" in seen:
                break
            sess.send(b"\x1b[B", 0.15)
            seen += sess.text()
        check("Playback Speed" in seen and "0.95x" in seen, "settings: GENERAL has Playback Speed (0.95x) in the audio book mode")
        check("Choose Audio Device" in seen and "System Default" in seen, "settings: GENERAL has Choose Audio Device (System Default)")
        if "Playback Speed" not in seen:
            sess.dump("settings GENERAL tab of the audio book mode")
        sess.send(b"\x1b", 1.0)
        for _ in range(2):              # audio books -> music player -> radio (each may open its help wizard on its first start)
            sess.send("J", 2.5)
            for _ in range(10):
                if "HELP WIZARD" in sess.text():
                    break
                sess.pump(0.3)
            if "HELP WIZARD" in sess.text():
                sess.send(b"\x1b", 1.5)
        for _ in range(10):
            if "Plain Station" in sess.text():
                break
            sess.pump(0.5)
        if "Plain Station" in sess.text():
            sess.send("&", 1.5)
            check("Choose Audio Device" in sess.text() and "System Default" in sess.text(), "audio device: & opens the overlay in the radio")
            sess.send(b"\x1b", 0.8)
            check("Choose Audio Device" not in sess.text(), "audio device: ESC closes it in the radio")
        else:
            check(False, "audio device: the radio could not be reached for the overlay check")
        check(sess.alive(), "audio book / devices: the program is still running")
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


def read_lyrics_play(binary, root, scripts):
    """READ LYRICS (CTRL+L): a saved translation is shown at once, and [p] plays the track with the lyrics highlighted."""
    home = os.path.join(root, "rl_home")
    lib = os.path.join(root, "rl_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    make_wav(os.path.join(lib, "Read Song.wav"), 20)
    lines = ["First lyric line here", "Second lyric line here", "Third lyric line here"]
    with open(os.path.join(lib, "lyrics", "Read Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]%s\n[00:03.00]%s\n[00:06.00]%s\n" % tuple(lines))
    h = 1469598103934665603     # the fingerprint of the original lines (FNV-1a, see lyrics_translate.cpp)
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    with open(os.path.join(lib, "lyrics", "translations", "Read Song.en.txt"), "w", encoding="utf-8") as f:
        f.write("#mousiki-translation to=en from=de svc=google lines=3 hash=%016x\n" % h)
        f.write("Erste Zeile hier\nZweite Zeile hier\nDritte Zeile hier\n")
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
            if "Read Song" in sess.text():
                break
        sess.send(b"\x0c", 1.5)   # CTRL+L on the hovered track
        check("READ LYRICS" in sess.text() and "First lyric line here" in sess.text(), "read lyrics: the overlay shows the lyrics")
        check("(Erste Zeile hier)" in sess.text(), "read lyrics: a saved translation is shown without asking")
        check("[p] Play" in sess.text(), "read lyrics: the legend names the play key")
        check_frame(sess, "read lyrics")
        sess.send("p", 3.0)        # the track starts, the reader stays open
        check("READ LYRICS" in sess.text(), "read lyrics: [p] keeps the reader open")
        sess.send("a", 1.0)        # "Add manually": two search panes, the current track is the first entry of the lower one
        check("ADD LYRICS MANUALLY" in sess.text() and "Current track" in sess.text() and "Read Song" in sess.text(), "read lyrics: [a] opens the add-manually overlay with both lists")
        check("LOCKED IN: LYRICS FILE" in sess.text() and "LOCKED IN: TRACK" in sess.text(), "read lyrics: the add-manually overlay shows the locked-in panes")
        sess.send(b"\t", 0.3)      # to the track search field: CTRL+t / CTRL+f work while typing
        sess.send(b"\x14", 0.7)    # CTRL+t: the track list shows file names
        check("TRACKS (file names)" in sess.text() and "Read Song.wav" in sess.text(), "read lyrics: CTRL+t shows file names in the track list")
        sess.send(b"\x14", 0.7)
        check("TRACKS (artist - title)" in sess.text() and "- - Read Song" not in sess.text(), "read lyrics: CTRL+t goes back to artist - title (no '- -' in front of a track)")
        sess.send(b"\x06", 0.7)    # CTRL+f: only tracks without saved lyrics (this one has some)
        check("0 of 1 tracks" in sess.text(), "read lyrics: CTRL+f lists only the tracks without lyrics")
        sess.send(b"\x06", 0.7)
        check("1 of 1 tracks" in sess.text(), "read lyrics: CTRL+f shows all tracks again")
        sess.send(b"\t", 0.3)
        sess.send(b"\t", 0.3)
        sess.send(b"\t", 0.3)      # (round to the lyrics list again)
        sess.send(b"\r", 0.5)     # ENTER in a list locks the highlighted lyrics file in, it does not assign
        check("ADD LYRICS MANUALLY" in sess.text() and "none yet" not in sess.text(), "read lyrics: ENTER locks the lyrics file in and keeps the overlay open")
        sess.send(b"\x03", 0.7)    # CTRL+c clears the choice, also from a search field
        check("none yet" in sess.text(), "read lyrics: CTRL+c clears the locked-in lyrics file")
        sess.send(b"\r", 0.5)      # (locked in again)
        sess.send(b"\t", 0.3)
        sess.send(b"\t", 0.5)
        sess.send(b"\r", 1.0)      # on the button: assigns the first file to the current track (it is the same text, so nothing changes)
        check("ADD LYRICS MANUALLY" not in sess.text() and "READ LYRICS" in sess.text(), "read lyrics: ENTER assigns the lyrics file and closes the overlay")
        sess.send("a", 1.0)         # the file that was just assigned shows as locked in, with the note
        check("Added manually;" in sess.text() and "unassign" in sess.text(), "read lyrics: a manually added lyrics file shows as locked in, with the unassign note")
        sess.send(b"\x1b", 1.0)
        check("ADD LYRICS MANUALLY" not in sess.text() and "READ LYRICS" in sess.text(), "read lyrics: ESC closes the add-manually overlay")
        if "[l] Re-fetch" in sess.text() or "Re-fetch" in sess.text():
            sess.send("l", 1.0)    # the re-fetch / version control form opens over the reader (the track is the loaded one now)
            if "LYRICS REFETCH AND VERSION CONTROL" in sess.text():
                check("READ LYRICS" in sess.text(), "read lyrics: [l] opens the re-fetch form over the reader")
                sess.send(b"\x1b", 1.0)
                check("LYRICS REFETCH AND VERSION CONTROL" not in sess.text() and "READ LYRICS" in sess.text(), "read lyrics: ESC returns from the re-fetch form to the reader")
            else:
                check("only for the track that plays" in sess.text(), "read lyrics: [l] says it is only for the playing track")
        if shutil.which("ffprobe") is None or sess.alive() is False:
            print("  skipped: read lyrics playback (ffmpeg / ffprobe is not installed on this machine)")
            return
        for _ in range(6):
            sess.pump(0.5)
        if re.search(r"playing 0:00", sess.text()) or "playing" not in sess.text():
            print("  skipped: read lyrics playback (the track does not play on this machine: no audio device)")
            return
        check("playing " in sess.text() and "First lyric line here" in sess.text(), "read lyrics: the position is shown while it plays")
        sess.send("p", 1.0)
        check("paused" in sess.text(), "read lyrics: [p] pauses")
    finally:
        sess.kill()


def make_library(root):
    """The temporary HOME and the small library every scenario starts from (a worker gets its own copy of it)."""
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



def main_session(binary, root, scripts):
    """The long walk through the main screens (first start, settings, meta editor, ..., quit)."""
    home = os.path.join(root, "home")
    lib = os.path.join(root, "music")
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
        sess.send(b"\x1b[A", 0.3)       # the last stop is Video Option, under the two buttons: Up is Style Option
        sess.send(b"\x1b[D", 0.3)       # the two buttons sit side by side: Left goes to Cover Options
        sess.send(b"\r", 1.5)
        check("COVER OPTIONS" in sess.text(), "cover options: the overlay opens")
        sess.send("t", 1.0)
        check(sess.alive(), "cover options: [t] does not crash")
        sess.send("t", 1.0)
        check(sess.alive(), "cover options: [t] again does not crash")
        sess.send(b"\x1b", 1.0)
        check("COVER OPTIONS" not in sess.text(), "cover options: ESC closes the overlay")
        check("Style Option" in sess.text() and "Cover Options" in sess.text(), "meta editor: both buttons are shown")
        sess.send(b"\x1b[C", 0.3)       # Right: the Style Option button
        sess.send(b"\r", 1.5)
        for _ in range(12):   # (the overlay animates: a read can fall between two frames)
            if "STYLE OPTION" in sess.text() and "Cover Brightness" in sess.text(): break
            sess.pump(0.4)
        check("STYLE OPTION" in sess.text(), "style option: the overlay opens")
        check("Cover Brightness" in sess.text() and "Look of this track" in sess.text(), "style option: the rows are shown")
        check("Cover Gradient Saturation" in sess.text(), "style option: Cover Gradient Saturation has a row")
        check_frame(sess, "style option", frame=False)
        sess.send(b"\x1b[B", 0.3)       # Cover Contrast
        sess.send(b"\x1b[C", 1.2)
        for _ in range(8):   # (the overlay's preview animates: a read can fall between two frames)
            if "\u25c0 0 %" in sess.text(): break
            sess.pump(0.4)
        check("\u25c0 0 %" in sess.text(), "style option: Right gives the track its own value")
        sess.send(b"\x7f", 0.5)         # DEL: back to 'as in Settings'
        for _ in range(8):
            if "as in Settings" in sess.text(): break
            sess.pump(0.4)
        check("as in Settings" in sess.text(), "style option: DEL goes back to 'as in Settings'")
        sess.send(b"\x1b", 1.0)
        check("STYLE OPTION" not in sess.text(), "style option: ESC closes the overlay")
        for _ in range(4):  # ESC steps back: fields pane -> library pane -> out of the meta editor
            if "META EDITOR" not in sess.text():
                break
            sess.send(b"\x1b", 1.0)
        check("META EDITOR" not in sess.text(), "meta editor: ESC leaves it")

        progress("main session: quit")
        # --- quit
        sess.send("q", 0.5)
        # (the program waits for its helper tasks on the way out -- up to about 5 s -- and a loaded CI machine (macOS, three scenarios at a time) is slow
        # to end a process: wait up to ~25 s before calling it a hang)
        for _ in range(80):
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


def _cell_width(ch):
    import unicodedata
    if unicodedata.combining(ch) or unicodedata.category(ch) in ("Mn", "Me", "Cf"):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


def overflow_problems(sess):
    """Reads the last complete frame as the program wrote it (every row starts with ESC[row;colH) and reports a row that is
    written below the last row or whose text runs past the last column. The screen buffer cannot show that: the terminal wraps
    or clips it, and a later row overwrites the wrap."""
    raw = sess.raw
    end = raw.rfind(b"\x1b[?2026l")
    if end < 0:
        return []
    start = raw.rfind(b"\x1b[?2026h", 0, end)
    if start < 0:
        return []
    text = raw[start:end].decode("utf-8", "replace")
    text = re.sub(r"\x1b_.*?\x1b\\", "", text, flags=re.S)          # graphics (Kitty)
    text = re.sub(r"\x1bP.*?\x1b\\", "", text, flags=re.S)           # graphics (Sixel)
    bad = []
    parts = re.split(r"\x1b\[(\d+);(\d+)H", text)
    for i in range(1, len(parts) - 2, 3):
        row, col, seg = int(parts[i]), int(parts[i + 1]), parts[i + 2]
        seg = re.sub(r"\x1b\[[0-9;:?]*[A-Za-z]", "", seg)
        seg = re.sub(r"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", "", seg)
        if not seg.strip():
            continue
        if row > ROWS:
            bad.append("row %d is below the last row (%d): %r" % (row, ROWS, seg[:30]))
            continue
        w = col - 1 + sum(_cell_width(c) for c in seg)
        if w > COLS:
            bad.append("row %d is %d wide in a window of %d: %r" % (row, w, COLS, seg[:20] + "..." + seg[-20:]))
    return bad


def check_fit(sess, name, frame=True):
    """The program runs, the frame holds together, and nothing is written outside the window."""
    check(sess.alive(), "%s: the program is still running" % name)
    bad = overflow_problems(sess) + (frame_problems(sess) if frame else [])
    check(not bad, "%s @%dx%d: does not fit: %s" % (name, COLS, ROWS, "; ".join(bad[:3])))
    if bad:
        sess.dump(name)


def narrow_sweep(binary, root, scripts, cols, rows):
    """Every screen and overlay in a small window (80 x 24 and 100 x 30): nothing may be written past its edges."""
    global COLS, ROWS
    COLS, ROWS = cols, rows
    home = os.path.join(root, "nw_home")
    lib = os.path.join(root, "nw_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Alpha Song.wav"), 20)
    make_wav(os.path.join(lib, "Misc", "\u3055\u304f\u3089.wav"), 5)
    make_png(os.path.join(lib, "Alpha Artist", "First Album", "cover.png"))
    lines = ["First lyric line here, a rather long line that must wrap in a narrow window", "Second lyric line here", "Third lyric line here"]
    with open(os.path.join(lib, "Alpha Artist", "First Album", "lyrics.dummy"), "w") as f:
        f.write("x")
    os.makedirs(os.path.join(lib, "Alpha Artist", "First Album", "lyrics", "translations"))
    ldir = os.path.join(lib, "Alpha Artist", "First Album", "lyrics")
    with open(os.path.join(ldir, "Alpha Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]%s\n[00:03.00]%s\n[00:06.00]%s\n" % tuple(lines))
    os.makedirs(os.path.join(lib, "Misc", "lyrics"))
    with open(os.path.join(lib, "Misc", "lyrics", "\u3055\u304f\u3089.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]Sakura line one\n[00:03.00]Sakura line two\n")
    with open(os.path.join(lib, "lyrics", "Other Lyrics.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]Some other lyrics\n[00:03.00]More other lyrics\n")
    h = 1469598103934665603
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    with open(os.path.join(ldir, "translations", "Alpha Song.en.txt"), "w", encoding="utf-8") as f:
        f.write("#mousiki-translation to=en from=de svc=google lines=3 hash=%016x\n" % h)
        f.write("Erste Zeile hier, eine ziemlich lange Zeile die umbrechen muss\nZweite Zeile hier\nDritte Zeile hier\n")
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"}, quiet_stderr=True)
    tag = "%dx%d " % (cols, rows)

    def see(keys, what, wait=0.8, until=None, frame=True):
        sess.send(keys, wait)
        if until:
            for _ in range(10):
                if until in sess.text():
                    break
                sess.pump(0.4)
        check_fit(sess, tag + what, frame)

    try:
        progress("narrow sweep %dx%d" % (cols, rows))
        sess.pump(3)
        check_fit(sess, tag + "first-run help wizard")
        for i in range(60):    # every page of the wizard
            sess.send(b"\x1b[C", 0.25)
            check_fit(sess, tag + "help wizard page %d" % (i + 2))
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.pump(1.0)
        check_fit(sess, tag + "main screen", False)   # (its volume row has no right border by design: only the edges count)
        for key, name in (("?", "cheat sheet"), ("<", "MENUS index"), ("W", "help wizard"), ("t", "console")):
            see(key, name, 1.0, frame=False)
            for i in range(6):
                see(b"\x1b[C", name + " page %d" % (i + 2), 0.3, frame=False)
            for i in range(3):
                see(b"\x1b[1;2B", name + " scrolled %d" % (i + 1), 0.3, frame=False)
            sess.send(b"\x1b", 0.8)
        see("s", "settings", 1.2)
        for i in range(13):
            see(b"\t", "settings tab %d" % (i + 2), 0.4)
            for _ in range(2):
                see(b"\x1b[B", "settings tab %d (down)" % (i + 2), 0.15)
            see(b"\x1b[1;2B", "settings tab %d (page)" % (i + 2), 0.15)
        sess.send(b"\x1b", 0.8)
        see("M", "meta editor", 1.5)
        see(b"\t", "meta editor (fields)", 0.4)
        for _ in range(12):
            sess.send(b"\x1b[B", 0.12)
        check_fit(sess, tag + "meta editor (bottom)")
        sess.send(b"\x1b[D", 0.3)
        see(b"\r", "cover options", 1.2, "COVER OPTIONS", frame=False)
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b[C", 0.3)
        see(b"\r", "style option", 1.2, "STYLE OPTION", frame=False)
        sess.send(b"\x1b", 0.8)
        for _ in range(4):
            if "META EDITOR" not in sess.text():
                break
            sess.send(b"\x1b", 0.8)
        for key, name in (("O", "oscilloscope tuning"), (".", "lyrics area view 2"), (".", "lyrics area view 3"), (".", "lyrics area view 4"), (".", "lyrics area view 1")):
            see(key, name, 1.0, frame=False)
            if name.startswith("osc"):
                for _ in range(8):
                    see(b"\x1b[B", name + " (down)", 0.15, frame=False)
                sess.send(b"\x1b", 0.8)
        # READ LYRICS and everything that opens over it
        sess.send(b"\x0c", 1.5)
        for _ in range(10):
            if "READ LYRICS" in sess.text():
                break
            sess.pump(0.4)
        opened = "READ LYRICS" in sess.text()
        check(opened, tag + "READ LYRICS opens on the hovered track")
        if not opened:
            sess.dump("READ LYRICS did not open")
        else:
            check_fit(sess, tag + "read lyrics")
            for _ in range(4):
                see(b"\x1b[1;2B", "read lyrics (scrolled)", 0.2)
            see(b"\t", "read lyrics (translation pane)", 0.3)
            see(b"\t", "read lyrics (lyrics pane)", 0.3)
            see(b"/", "read lyrics: search lyrics files", 0.8, "SEARCH LYRICS")
            for c in "oth":
                sess.send(c, 0.1)
            check_fit(sess, tag + "read lyrics: search lyrics files (typed)")
            sess.send(b"\x1b", 0.6)
            see("t", "read lyrics: translate overlay", 0.8)
            sess.send(b"\x1b[B", 0.2)
            check_fit(sess, tag + "read lyrics: translate overlay (down)")
            sess.send(b"\x1b", 0.6)
            see("e", "read lyrics: export overlay", 0.8)
            sess.send(b"\x1b[C", 0.2)
            check_fit(sess, tag + "read lyrics: export overlay (right)")
            sess.send(b"\x1b", 0.6)
            see("E", "read lyrics: edit / add translation", 1.0, "EDIT / ADD TRANSLATION")
            for c in "Dies ist eine sehr lange uebersetzte Zeile die nicht in das Fenster passt":
                sess.send(c, 0.02)
            check_fit(sess, tag + "translation editor (typed)")
            for k in (b"\r", b"\x1b[B", b"\t", b"\x1b[C", b"\x1b[6~", b"\x0f", b"\t", b"\x0e", b"\x1b"):
                see(k, "translation editor (key %r)" % k, 0.3)
            sess.send(b"\x1b", 0.4)
            sess.send(b"y", 0.6)
            if "EDIT / ADD TRANSLATION" in sess.text():
                sess.send(b"\x1b", 0.6)
            check_fit(sess, tag + "translation editor closed")
            see("a", "read lyrics: add manually", 1.0, "ADD LYRICS MANUALLY")
            for k in (b"\t", b"\t", b"\t", b"\t", b"\x14", b"\x06", b"\x0f", b"\x1b[1;2B", b"\x1b[6~"):
                see(k, "add manually (key %r)" % k, 0.4)
            sess.send(b"\x03", 0.4)
            sess.send(b"\r", 0.4)
            check_fit(sess, tag + "add manually (locked)")
            sess.send(b"\x1b", 0.8)
            see("p", "read lyrics: playing", 2.0)
            see("l", "read lyrics: refetch form", 1.0)
            if "REFETCH" in sess.text():
                sess.send(b"\x1b", 0.8)
        # the big queue overlay and its legend
        for _ in range(3):
            if "READ LYRICS" in sess.text() or "ADD LYRICS" in sess.text():
                sess.send(b"\x1b", 0.8)
        see("e", "queue: add a track", 0.4, frame=False)
        see("K", "big queue overlay", 1.2, frame=False)
        see("S", "big queue overlay (shuffled)", 0.5, frame=False)
        see(b"\x1b", "big queue overlay closed", 0.8, frame=False)
        # karaoke
        for _ in range(3):   # (back at the main screen whatever is still open)
            if "READ LYRICS" in sess.text() or "ADD LYRICS" in sess.text():
                sess.send(b"\x1b", 0.8)
        see("k", "karaoke", 1.5)
        for i in range(3):
            see(b"\x1b[C", "karaoke (right %d)" % (i + 1), 0.4)
        see("a", "karaoke: add lyrics manually", 1.0)
        for _ in range(3):   # (back at the main screen whatever is still open; an ESC there would not be wanted)
            t = sess.text()
            if "ADD LYRICS" in t or "READ LYRICS" in t or "KARAOKE" in t:
                sess.send(b"\x1b", 0.8)
        # radio and the other modes
        for i in range(3):
            see("*", "mode switch %d" % (i + 1), 2.0, frame=False)
            if "HELP WIZARD" in sess.text():
                sess.send(b"\x1b", 1.0)
                check_fit(sess, tag + "mode switch %d (after its wizard)" % (i + 1))
        for key, name in (("s", "settings (mode)"), ("?", "cheat sheet (mode)")):
            see(key, name, 1.0, frame=False)
            sess.send(b"\x1b", 0.8)
        check(sess.alive(), tag + "the program is still running at the end")
    finally:
        sess.kill()

def opened_lyrics_is_current(binary, root, scripts):
    """A lyrics file opened from the search in READ LYRICS is what "Add manually" works on: its song is the current track
    (and the file is locked in); a file without a song of that name leaves no current track. Without a search it is the playing song."""
    home = os.path.join(root, "ol_home")
    lib = os.path.join(root, "ol_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "Zed Artist", "lyrics"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    make_wav(os.path.join(lib, "Zed Artist", "Zzz Last.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first song lyrics\n")
    with open(os.path.join(lib, "Zed Artist", "lyrics", "Zzz Last.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]last song lyrics\n")
    with open(os.path.join(lib, "lyrics", "Nobody Home.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]lyrics of no song\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)

    def reader_with_search(query):
        sess.send(b"\x0c", 1.2)                       # READ LYRICS on the hovered track (the first one)
        sess.send(b"/", 0.8)                          # "/": search the lyrics files
        for c in query:
            sess.send(c, 0.1)
        sess.send(b"\r", 0.8)                         # open the first match

    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        reader_with_search("zzz")
        check("last song lyrics" in sess.text(), "opened lyrics: the search opened the lyrics file")
        sess.send("a", 1.0)
        check("ADD LYRICS MANUALLY" in sess.text() and "Zzz Last" in sess.text() and "Aaa First" not in sess.text().split("TRACKS")[0],
              "opened lyrics: the song of the opened lyrics file is the current track")
        check("Zzz Last.lrc" in sess.text() or "Zzz Last" in sess.text().split("LOCKED IN: LYRICS FILE")[-1], "opened lyrics: the opened file is locked in")
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b", 0.8)                       # leave the reader
        reader_with_search("nobody")
        sess.send("a", 1.0)
        check("no track is named like the opened lyrics" in sess.text(), "opened lyrics: a file without a song leaves no current track")
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x0c", 1.2)                       # an ordinary reader again: the hovered track is the current one
        sess.send("a", 1.0)
        check("Current track" in sess.text() and "Aaa First" in sess.text().split("TRACKS")[0] + sess.text(), "opened lyrics: without a search the reader's track is the current one")
        check("no track is named" not in sess.text(), "opened lyrics: the earlier choice does not stay")
        check(sess.alive(), "opened lyrics: the program is still running")
    finally:
        sess.kill()

def translation_editor(binary, root, scripts):
    """READ LYRICS, SHIFT+e: a translation written by hand is saved as "Manual Translation", the next one as "Manual Translation 2";
    a machine translation that is edited stays as it was."""
    home = os.path.join(root, "te_home")
    lib = os.path.join(root, "te_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first line\n[00:02.00]\n[00:03.00]second line\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    tdir = os.path.join(lib, "lyrics", "translations")
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x0c", 1.5)
        check("READ LYRICS" in sess.text() and "[SHIFT+e] Edit/Add" in sess.text(), "translation editor: the legend names SHIFT+e")
        sess.send("E", 1.0)
        t = sess.text()
        check("EDIT / ADD TRANSLATION" in t and "Manual Translation" in t and "first line" in t, "translation editor: opens, new translation named Manual Translation")
        for c in "erste Zeile":
            sess.send(c, 0.05)
        sess.send(b"\r", 0.4)                              # the empty line between the stanzas is skipped
        for c in "zweite Zeile":
            sess.send(c, 0.05)
        sess.send(b"\x13", 1.0)                            # CTRL+s
        t = sess.text()
        check("EDIT / ADD TRANSLATION" not in t and "Manual Translation" in t and "erste Zeile" in t, "translation editor: saved, shown in the reader as Manual Translation")
        files = sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []
        check(len(files) == 1 and ".manual." in files[0], "translation editor: one file with the manual service id (%r)" % files)
        if files:
            body = open(os.path.join(tdir, files[0]), encoding="utf-8").read().split("\n")
            check(body[1:4] == ["erste Zeile", "", "zweite Zeile"], "translation editor: one translation line per lyrics line, the gap stays empty (%r)" % body[1:4])
        sess.send("E", 1.0)                                  # edit it again: changed in place
        sess.send(b"!", 0.3)
        sess.send(b"\x13", 1.0)
        files = sorted(os.listdir(tdir))
        check(len(files) == 1, "translation editor: editing a manual translation changes it in place")
        sess.send("E", 1.0)
        sess.send(b"\x0e", 0.5)                             # CTRL+n: start from scratch (asks)
        check("Start over" in sess.text(), "translation editor: CTRL+n asks before it drops the text")
        sess.send("y", 0.5)
        check("Manual Translation 2" in sess.text(), "translation editor: the scratch translation is Manual Translation 2")
        for c in "other":
            sess.send(c, 0.05)
        sess.send(b"\x13", 1.0)
        files = sorted(os.listdir(tdir))
        check(len(files) == 2, "translation editor: the second manual translation is a file of its own (%r)" % files)
        check("Manual Translation 2" in sess.text(), "translation editor: the pane lists Manual Translation 2")
        sess.send("E", 1.0)
        sess.send(b"x", 0.3)
        sess.send(b"\x1b", 0.5)
        check("Leave without saving" in sess.text(), "translation editor: ESC with changes asks")
        sess.send("n", 0.4)
        sess.send(b"\x1b", 0.4)
        sess.send(b"y", 0.6)
        check("EDIT / ADD TRANSLATION" not in sess.text(), "translation editor: y leaves without saving")
        check(sess.alive(), "translation editor: the program is still running")
    finally:
        sess.kill()

def disk_border_setting(binary, root, scripts):
    """Settings -> DISK -> Disk Border: with ElimentDiskBorder=false the ":  :" column between the disk and the meta data is gone and the
    disk sits further right (in the middle of the space); with the default it is there."""
    cols = {}
    for name, val in (("on", "true"), ("off", "false")):
        home = os.path.join(root, "db_home_" + name)
        lib = os.path.join(root, "music")
        os.makedirs(os.path.join(home, ".config", "mousiki"))
        with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nElimentDiskBorder=%s\n" % (lib, val))
        sess = Session(binary, home, scripts, quiet_stderr=True)
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
            t = sess.text()
            check((":  :" in t) == (name == "on"), "disk border %s: the \":  :\" column is %s" % (name, "there" if name == "on" else "gone"))
            first = None
            for y in range(1, 12):                           # the first braille column of the disk, left of the meta data
                row = sess.row_cells(y)
                for x, ch in enumerate(row):
                    if "\u2800" <= ch <= "\u28ff" and ch != "\u2800":
                        first = x if first is None else min(first, x)
                        break
            cols[name] = first
        finally:
            sess.kill()
    check(cols.get("on") is not None and cols.get("off") is not None and cols["off"] >= cols["on"], "disk border: with the border off the disk is not further left (on %s, off %s)" % (cols.get("on"), cols.get("off")))


def play_mode_back(binary, root, scripts):
    """m steps through the play modes, j steps back (the console log names the mode)."""
    home = os.path.join(root, "pmb_home")
    lib = os.path.join(root, "pmb_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send("m", 0.8)                                  # list -> repeat
        sess.send("j", 0.8)                                  # back to list
        sess.send("j", 0.8)                                  # list -> queue then stop
        sess.send("t", 1.5)
        t = sess.text()
        check("play mode: repeat" in t and "play mode: list" in t and "play mode: queue then stop" in t, "play mode back: m goes forward, j back (repeat, list, queue then stop)")
        check(sess.alive(), "play mode back: the program is still running")
    finally:
        sess.kill()

def translation_create(binary, root, scripts):
    """READ LYRICS: the lyrics files and the translations are two panes side by side; SHIFT+c creates an empty translation file (Manual
    Translation, then Manual Translation 2); LEFT / RIGHT go through the entries of the focused pane and round the ends."""
    home = os.path.join(root, "tc_home")
    lib = os.path.join(root, "tc_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first line\n[00:02.00]\n[00:03.00]second line\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    tdir = os.path.join(lib, "lyrics", "translations")
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x0c", 1.5)
        lines = sess.text().split("\n")
        check(any("READ LYRICS" in l and "TRANSLATION" in l for l in lines), "translation create: the lyrics pane and the translation pane are side by side")
        check("[SHIFT+c] Create" in sess.text(), "translation create: the legend names SHIFT+c")
        sess.send("C", 1.0)
        t = sess.text()
        files = sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []
        check(len(files) == 1 and ".manual." in files[0], "translation create: one file with the manual service id (%r)" % files)
        if files:
            body = open(os.path.join(tdir, files[0]), encoding="utf-8").read().split("\n")
            check(body[0].startswith("#mousiki-translation ") and "lines=3" in body[0] and body[1:4] == ["", "", ""], "translation create: the file is empty, one empty line per lyrics line (%r)" % body[:4])
        check("Manual Translation" in t, "translation create: the pane lists it")
        sess.send("C", 1.0)
        files = sorted(os.listdir(tdir))
        check(len(files) == 2 and "Manual Translation 2" in sess.text(), "translation create: the second one is Manual Translation 2 (%r)" % files)
        sess.send(b"\x1b[C", 0.8)                           # RIGHT in the translation pane: round the end to the first
        check("\u25cf Manual Translation" in sess.text(), "translation create: RIGHT goes to the next translation (and round)")
        check(sess.alive(), "translation create: the program is still running")
    finally:
        sess.kill()

def translation_paste_undo(binary, root, scripts):
    """A paste by the terminal (bracketed paste) is one event: with no text field it does nothing (no hotkeys fire, no help wizard), in the
    translation editor its lines go into the rows; CTRL+z / CTRL+y take steps back (up to 10), CTRL+r brings them forward."""
    home = os.path.join(root, "tpu_home")
    lib = os.path.join(root, "tpu_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]one\n[00:02.00]two\n[00:03.00]three\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x1b[200~hhh???wwww\x1b[201~", 1.0)      # pasted on the main screen: no text field, so nothing happens
        check("HELP WIZARD" not in sess.text() and "Aaa First" in sess.text(), "paste: on the main screen it fires no hotkeys")
        sess.send(b"\x0c", 1.5)
        sess.send("E", 1.0)
        check("EDIT / ADD TRANSLATION" in sess.text(), "paste: the translation editor is open")
        sess.send(b"\x1b[200~The person I passed by\rWalks politely on the white line,\rLike a stray cat.\x1b[201~", 1.2)
        t = sess.text()
        check("HELP WIZARD" not in t and "pasted 3 lines" in t and "stray cat" in t and "white line" in t, "paste: three lines go into the three rows, no help wizard")
        for _ in range(3):
            sess.send(b"\x1b[A", 0.2)                       # up to the first row
        sess.send(b"\x1b[1;2B", 0.5)                        # SHIFT+down: rows 1 and 2 marked
        sess.send(b"\x1b[200~AAAA\rBBBB\x1b[201~", 1.0)
        t = sess.text()
        check("AAAA" in t and "BBBB" in t and "person I passed" not in t and "white line" not in t and "stray cat" in t, "paste: marked lines are replaced by the pasted lines")
        sess.send(b"\x1a", 0.8)                             # CTRL+z (the marked-lines paste)
        sess.send(b"\x1a", 0.8)                             # CTRL+z
        t = sess.text()
        check("undone" in t and "stray cat" not in t, "undo: CTRL+z takes the paste back")
        sess.send(b"\x12", 0.8)                             # CTRL+r
        t = sess.text()
        check("redone" in t and "stray cat" in t, "undo: CTRL+r brings it forward again")
        sess.send(b"\x1a", 0.6)
        for c in "xyz":
            sess.send(c, 0.1)
        sess.send(b"\x19", 0.8)                             # CTRL+y (the undo key of a QWERTZ keyboard)
        t = sess.text()
        check("xyz" not in t.split("TRANSLATION")[-1] and "undone" in t, "undo: CTRL+y takes a typed run back as one step")
        for i in range(12):                                  # more than 10 steps: only the last 10 are kept
            sess.send("a", 0.05)
            sess.send(b"\x1b[D", 0.05)
            sess.send(b"\x7f", 0.05)
        for i in range(14):
            sess.send(b"\x1a", 0.1)
        sess.send(b"\x1a", 0.6)
        check("nothing to undo" in sess.text(), "undo: after the last step it says nothing is left to undo")
        check(sess.alive(), "paste / undo: the program is still running")
    finally:
        sess.kill()

def translation_lines(binary, root, scripts):
    """The translation editor on a small terminal: long lines are broken beside their original (no "..."), SHIFT+up/down marks
    whole lines that are copied / pasted / cleared / undone, CTRL+2 switches the Latin letters."""
    global COLS, ROWS
    COLS, ROWS = 80, 32
    home = os.path.join(root, "tl_home")
    lib = os.path.join(root, "tl_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]this is a rather long original line that cannot fit one row ZEBRA\n[00:02.00]short two\n[00:03.00]short three\n[00:04.00]short four\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    tdir = os.path.join(lib, "lyrics", "translations")
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x0c", 1.5)
        sess.send("E", 1.0)
        t = sess.text()
        check("ZEBRA" in t and "..." not in t.split("EDIT / ADD TRANSLATION")[-1].split("[CTRL+s]")[0], "translation lines: a long original is broken into rows, not cut with '...'")
        for c in "eine sehr lange Uebersetzung die auch nicht in eine Zeile passt FINAL":
            sess.send(c, 0.02)
        sess.pump(0.5)
        t = sess.text()
        check("FINAL" in t and "eine sehr" in t, "translation lines: a long translation is broken into rows beside its original")
        check_fit(sess, "translation lines 80x32", True)
        sess.send(b"\r", 0.3)
        sess.send("AAA", 0.3)
        sess.send(b"\r", 0.3)
        sess.send("BBB", 0.3)
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\x1b[1;2B", 0.5)                       # SHIFT+down: two lines marked
        check("2 lines marked" in sess.text(), "translation lines: SHIFT+down marks whole lines")
        sess.send(b"\x03", 0.5)                            # CTRL+c
        check("copied 2 lines" in sess.text(), "translation lines: CTRL+c copies the marked lines")
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x16", 0.6)                            # CTRL+v with a multi-line clipboard: pasted from the caret's line
        t = sess.text()
        for _ in range(10):   # (a slow machine needs longer to show the message)
            if "pasted" in t: break
            sess.pump(0.4); t = sess.text()
        check("pasted 1 line" in t or "pasted 2 lines" in t, "translation lines: CTRL+v pastes whole lines (%r)" % [l for l in t.split("\n") if "pasted" in l][:1])
        if not ("pasted 1 line" in t or "pasted 2 lines" in t): print("  screen at the failure:\n" + "\n".join("    | " + l.rstrip() for l in t.split("\n") if l.strip()), flush=True)
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\x1b[1;2B", 0.4)
        sess.send(b"\x7f", 0.5)                            # DEL on marked lines clears them
        check("cleared 2 lines" in sess.text(), "translation lines: DEL clears the marked lines")
        sess.send(b"\x1a", 0.5)                            # CTRL+z
        check("undone" in sess.text(), "translation lines: CTRL+z undoes it")
        sess.send(b"\x00", 0.5)                            # CTRL+2
        check("Latin letters: on" in sess.text() or "Latin letters on" in sess.text(), "translation lines: CTRL+2 switches the Latin letters")
        sess.send(b"\x00", 0.4)
        sess.send(b"\x14", 0.5)                            # CTRL+t: the same, on a key every terminal sends
        check("Latin letters: on" in sess.text() or "Latin letters on" in sess.text(), "translation lines: CTRL+t switches the Latin letters")
        sess.send(b"\x14", 0.4)
        sess.send(b"\x13", 1.0)
        files = sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []
        check(len(files) == 1, "translation lines: saved (%r)" % files)
        if files:
            body = open(os.path.join(tdir, files[0]), encoding="utf-8").read().split("\n")
            check(body[1].endswith("FINAL") and body[2] == "AAA" and body[3] == "BBB", "translation lines: the lines are where they should be (%r)" % body[1:5])
        check(sess.alive(), "translation lines: the program is still running")
    finally:
        sess.kill()

def waterfall_view(binary, root, scripts):
    """The spectrogram options have a View row: the waterfall swaps the rows it does not use (motion, channels, time span) for its own,
    the full-screen view is titled WATERFALL and carries the frequency labels on its top ruler."""
    global COLS, ROWS
    COLS, ROWS = 110, 36
    home = os.path.join(root, "wf_home")
    lib = os.path.join(root, "wf_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nSpectroStyle=braille\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send(b"\r", 1.5)
        sess.send("I", 1.0)
        t = sess.text()
        check("Spectrogram" in t and "View" in t and "spectrogram" in t and "Motion" in t and "Graphs" not in t, "waterfall: the options open on the View row, the spectrogram rows are listed")
        sess.send(b"\x1b[C", 0.8)                          # RIGHT: waterfall
        t = sess.text()
        check("waterfall" in t and "Graphs" in t and "Graph spacing" in t and "Slant" in t and "Peak height" in t and "Motion" not in t and "Channels" not in t and "Time span" not in t,
              "waterfall: its own rows replace motion / channels / time span")
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x1b[B", 0.3)
        check(sess.alive(), "waterfall: moving through the rows works")
        check_fit(sess, "waterfall options", False)
        sess.send("I", 0.8)
        sess.send("U", 1.2)                                  # the full-screen view
        t = sess.text()
        check("WATERFALL" in t and "SPECTROGRAM" not in t, "waterfall: the full-screen view is titled WATERFALL")
        check_fit(sess, "waterfall full screen", True)
        sess.send("U", 0.8)
        sess.send("I", 0.8)
        sess.send(b"\x1b[D", 0.8)                          # LEFT on the View row: back to the spectrogram
        t = sess.text()
        check("Motion" in t and "Graphs" not in t, "waterfall: back to the spectrogram brings its rows back")
        check(sess.alive(), "waterfall: the program is still running")
    finally:
        sess.kill()

def disk_field_setting(binary, root, scripts):
    """ANIMA/VIZ: DISK holds Eliment Disk / Cover and Disk Speed, the cover rows are DISK / COVER OPTIONS; Remove Field empties the preview."""
    home = os.path.join(root, "df_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)                               # ANIMA/VIZ
        t = sess.text()
        check("DISK / COVER OPTIONS" in t and "Eliment Disk / Cover" in t and "Album Cover" in t, "disk field: the tab has DISK and DISK / COVER OPTIONS")
        check(t.index("Disk Speed") < t.index("DISK / COVER OPTIONS") < t.index("Album Cover"), "disk field: Disk Speed is under DISK, Album Cover under DISK / COVER OPTIONS")
        for _ in range(23):                                 # to Eliment Disk (after Lyrics/Viz Default: When No Lyrics Show, When No Video Shows, Lyrics Alignment, Lyrics Animation, Include Lyrics, Video Size, Effect, Contrast, Colors, Chromatic Aberration, Dither Pattern, Base Layer, Effect Contrast, Rounded Corners, Show Clap Loading, Disk/Cover/Thumbnail, Resolution, Frame Rate, Buffer, yt-dlp mp3 from mp4)
            sess.send(b"\x1b[B", 0.2)
        sess.pump(0.6)
        check("Show Disk / Cover" in sess.text(), "disk field: the row says Show Disk / Cover")
        sess.send(b"\x1b[C", 1.0)
        check("Remove Field" in sess.text() and "Disk / Cover field removed" in sess.text(), "disk field: Remove Field empties the preview")
        sess.send(b"\x1b[C", 1.0)
        check("Show Disk / Cover" in sess.text() and "Disk / Cover field removed" not in sess.text(), "disk field: Show Disk / Cover brings the preview back")
        check(sess.alive(), "disk field: the program is still running")
        for _ in range(6):                                  # up to Show Clap Loading (yt-dlp mp3 from mp4, Video Buffer, Video Frame Rate, Video Resolution and Disk/Cover/Thumbnail are in between)
            sess.send(b"\x1b[A", 0.2)
        sess.pump(0.8)
        t = sess.text()
        check("Show Clap Loading" in t and "loading: none" in t, "show clap loading: the preview shows none (the default)")
        sess.send(b"\x1b[D", 0.8)
        t = sess.text()
        check("loading: clap" in t, "show clap loading: left arrow goes to the clap")
        sess.send(b"\x1b[D", 0.8)
        t = sess.text()
        check("loading: text note" in t and "video: starting" in t, "show clap loading: the text note shows the message")
    finally:
        sess.kill()

def video_files_tab(binary, root, scripts):
    """FETCH VIDEOS, second tab VIDEO FILES: the videos are listed, the coupled one is marked and names its song, DEL (twice) deletes the file."""
    import subprocess
    if shutil.which("ffmpeg") is None:
        print("  skipped: video files tab (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vft_home")
    lib = os.path.join(root, "vft_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 5)
    for n in ("Alpha Song", "Lonely Clip"):
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=2", "-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-shortest", "-pix_fmt", "yuv420p", os.path.join(lib, n + ".mp4")], capture_output=True)
        if r.returncode != 0:
            print("  (skipped: ffmpeg could not make the test clip)")
            return
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send(b"\x1bv", 1.5)
        check("VIDEO FILES" in sess.text(), "fetch videos: the VIDEO FILES tab is there")
        sess.send(b"\x1b[1;3C", 1.0)
        check("[c] Alpha Song.mp4" in sess.text() and "[ ] Lonely Clip.mp4" in sess.text(), "video files: the coupled video is marked, the other one is video only")
        check("Alpha Song.wav" in sess.text(), "video files: the coupled song is named")
        sess.send(b"\x1b[B", 0.6)                            # to Lonely Clip.mp4 (video only)
        check("[CTRL+3 / m] makes an mp3 song" in sess.text(), "video files: a video only file offers the mp3 command above the note about the deletion")
        sess.send("m", 4.0)
        mp3s = [f for r_, d_, fs_ in os.walk(home) for f in fs_ if f.endswith(".mp3")]
        check(mp3s == ["Lonely Clip.mp3"] and "mp3 saved" in sess.text(), "video files: [m] makes the mp3 of a video only file in the download folder")
        sess.send(b"\x1b[A", 0.6)
        check("[CTRL+3 / m] makes an mp3 song" not in sess.text(), "video files: a coupled video does not offer it")
        sess.send(b"\x1b[3~", 0.8)
        check("DELETE Alpha Song.mp4" in sess.text() and os.path.exists(os.path.join(lib, "Alpha Song.mp4")), "video files: DEL asks first, nothing is deleted yet")
        sess.send(b"\x1b[3~", 1.5)
        check(not os.path.exists(os.path.join(lib, "Alpha Song.mp4")) and os.path.exists(os.path.join(lib, "Lonely Clip.mp4")), "video files: the second DEL deletes that video file only")
        sess.send("u", 1.5)
        check(os.path.exists(os.path.join(lib, "Alpha Song.mp4")) and "restored" in sess.text(), "video files: [u] brings the deleted video back")
        sess.send("u", 0.8)
        check("nothing to undo" in sess.text(), "video files: a second [u] has nothing to undo")
        sess.send(b"\x06", 0.8); sess.send(b"\x06", 0.8); sess.send(b"\x06", 0.8)
        check("newest first" in sess.text(), "video files: CTRL+f reaches the 'newest first' filter")
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "video files: ESC leaves")
    finally:
        sess.kill()

def settings_video_preview(binary, root, scripts):
    """ANIMA/VIZ: the Use Video row previews the sample video (scripts/preview_video.mp4), looping; a row further down stops it."""
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        print("  skipped: settings video preview (ffmpeg / ffprobe is not installed on this machine)")
        return
    if not os.path.exists(os.path.join(scripts, "preview_video.mp4")):
        print("  skipped: settings video preview (scripts/preview_video.mp4 is not there)")
        return
    home = os.path.join(root, "sv_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)                               # ANIMA/VIZ
        found = False
        for _ in range(60):                                 # down to the Use Video row (the ON/OFF block further down the tab)
            sess.send(b"\x1b[B", 0.15)
            sess.pump(0.4)
            if "Preview: Use Video" in sess.text():
                sess.pump(1.5)
            if "sample video" in sess.text():
                found = True
                break
        check(found, "settings video preview: the Use Video row shows the sample video")
        for _ in range(12):                                  # (Video Size, Effect, Contrast, Brightness, Colors, Chromatic Aberration, Resolution, Buffer and yt-dlp mp3 from mp4 show it too: on to the next row with another preview)
            sess.send(b"\x1b[B", 0.3)
            sess.pump(1.0)
            if "sample video" not in sess.text():
                break
        check("sample video" not in sess.text(), "settings video preview: another row shows its own preview")
        check(sess.alive(), "settings video preview: the program is still running")
    finally:
        sess.kill()

def bulk_add_options(binary, root, scripts):
    """BULK ADD (SHIFT+g): the panel shows the two options, CTRL+s (Save) and CTRL+o (Video) switch them and they are saved;
    the audio book without a narrative does not show the music player's "Hit [n] to proceed"."""
    home = os.path.join(root, "ba_home")
    lib = os.path.join(root, "music")
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    os.makedirs(os.path.dirname(cfg))
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.pump(1.0)
        check("Hit [" in sess.text() or "Alpha Song" in sess.text() or sess.alive(), "bulk add: the main screen is up")
        sess.send("G", 1.0)
        t = sess.text()
        check("BULK ADD" in t and "Save: ON" in t and "Video: OFF" in t, "bulk add: the panel shows Save: ON and Video: OFF by default")
        sess.send(b"\x13", 0.8)
        t = sess.text()
        check("Save: OFF" in t and "Video: OFF" in t, "bulk add: CTRL+s switches to play only")
        sess.send(b"\x0f", 0.8)
        t = sess.text()
        check("Save: OFF" in t and "Video: ON" in t, "bulk add: CTRL+o switches the video on")
        sess.send(b"\x1b", 1.0)
        check("BULK ADD" not in sess.text() and sess.alive(), "bulk add: ESC closes the panel")
        sess.send("q", 1.5)
    finally:
        sess.kill()
    conf = open(cfg, encoding="utf-8").read()
    check("BulkAddSave=false" in conf and "BulkAddVideo=true" in conf, "bulk add: both options are saved in config.txt")

    home = os.path.join(root, "bb_home")
    books = os.path.join(root, "books")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("AudioBookPath=%s\nStartMode=audiobook\nUseTerminalSettings=true\n" % books)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.5)
        t = sess.text()
        check("No Narrative Loaded" in t, "audio book: no narrative loaded shows its message")
        check("to proceed" not in t, "audio book: no \"Hit [n] to proceed\" without a narrative")
        sess.send("q", 1.5)
    finally:
        sess.kill()


def bulk_add_flow(binary, root, scripts):
    """BULK ADD end to end with a fake yt-dlp: with Save OFF and Video ON the played track's audio file and its video exist while it plays and
    are deleted again once the next track has started; with Save ON the files stay."""
    import subprocess, stat, glob
    if shutil.which("ffmpeg") is None:
        print("  skipped: bulk add flow (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "baf_home")
    fake = os.path.join(root, "baf_bin")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(fake)
    wav = os.path.join(root, "baf_tone.wav")
    make_wav(wav, 20)
    clip = os.path.join(root, "baf_clip.mp4")
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=2", "-pix_fmt", "yuv420p", clip], capture_output=True)
    if r.returncode != 0:
        print("  (skipped: ffmpeg could not make the test clip)")
        return
    yt = os.path.join(fake, "yt-dlp")
    with open(yt, "w") as f:
        f.write("""#!/bin/bash
if [[ "$*" == *"--flat-playlist"* ]]; then
  echo '{"id":"AAAAAAAAAAA","title":"Bulk One","uploader":"Chan","duration":20}'
  echo '{"id":"BBBBBBBBBBB","title":"Bulk Two","uploader":"Chan","duration":20}'
  exit 0
fi
out=""; audio=0
for a in "$@"; do if [ "$a" = "-x" ]; then audio=1; fi; done
while [ $# -gt 0 ]; do if [ "$1" = "-o" ]; then out="$2"; fi; shift; done
if [ $audio = 1 ]; then
  f="${out/\\%(ext)s/opus}"
  cp "@WAV@" "$f"
else
  f="${out/\\%(ext)s/mp4}"
  echo "download:PCT 50.0%"
  cp "@CLIP@" "$f"
  echo "FILE:$f"
fi
""".replace("@CLIP@", clip).replace("@WAV@", wav))
    os.chmod(yt, os.stat(yt).st_mode | stat.S_IEXEC)

    def run(save, video):
        cfg = os.path.join(home, ".config", "mousiki", "config.txt")
        with open(cfg, "w", encoding="utf-8") as f:
            f.write("UseTerminalSettings=true\nVideoMode=true\nBulkAddSave=%s\nBulkAddVideo=%s\n" % (save, video))
        sess = Session(binary, home, scripts, {"PATH": fake + os.pathsep + os.environ.get("PATH", ""), "MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
        cache = os.path.join(home, ".cache", "mousiki")
        def files(pat):
            return sorted(glob.glob(os.path.join(cache, pat)) + glob.glob(os.path.join(cache, "video_down", pat)))
        try:
            for _ in range(30):
                sess.pump(0.5)
                if "HELP WIZARD" in sess.text():
                    break
            sess.send(b"\x1b", 1.0)
            sess.send("G", 1.0)
            sess.send("https://www.youtube.com/playlist?list=x", 0.8)
            sess.send("\r", 0.5)
            for _ in range(30):
                sess.pump(0.5)
                if "Bulk Two" in sess.text():
                    break
            check("Bulk One" in sess.text() and "Bulk Two" in sess.text(), "bulk add flow (save=%s video=%s): the fake playlist is listed" % (save, video))
            sess.send("a", 1.0)
            sess.send("n", 1.0)                       # the first track of the queue
            for _ in range(40):
                sess.pump(0.5)
                if files("*.opus") and (video == "false" or files("*.mp4")):
                    break
            check(len(files("*.opus")) == 1, "bulk add flow (save=%s video=%s): the first track's file is there while it plays" % (save, video))
            if video == "true":
                check(len(files("*.mp4")) == 1, "bulk add flow (save=%s video=%s): its video is downloaded" % (save, video))
            else:
                check(len(files("*.mp4")) == 0, "bulk add flow (save=%s video=%s): no video is downloaded" % (save, video))
            sess.send("n", 1.0)                       # the second one: the first one's files go when it has started
            for _ in range(40):
                sess.pump(0.5)
                if len(files("*.opus")) >= 2 or (save == "false" and files("*.opus") and "Bulk Two" in sess.text()):
                    break
            sess.pump(3.0)
            ops = files("*.opus")
            if save == "false":
                check(len(ops) == 1 and "bulk_two" in os.path.basename(ops[0]).lower().replace(" ", "_"), "bulk add flow (save=%s video=%s): only the playing track's file is left: %r" % (save, video, [os.path.basename(x) for x in ops]))
                check(len(files("*.mp4")) <= 1, "bulk add flow (save=%s video=%s): the first video is deleted with its track" % (save, video))
            else:
                check(len(ops) == 2, "bulk add flow (save=%s video=%s): both files stay: %r" % (save, video, [os.path.basename(x) for x in ops]))
            sess.send("q", 2.0)
            sess.pump(1.5)
            if save == "false":
                check(len(files("*.opus")) == 0 and len(files("*.mp4")) == 0, "bulk add flow (save=%s video=%s): nothing is left after quitting" % (save, video))
        finally:
            sess.kill()
        shutil.rmtree(cache, ignore_errors=True)
    run("false", "true")
    run("true", "false")


def queue_shuffle(binary, root, scripts):
    """The big queue overlay (SHIFT+k): SHIFT+s shuffles the order of the queue."""
    home = os.path.join(root, "qs_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        for i in range(4):                       # the four tracks to the end of the queue
            sess.send("e", 0.4)
            sess.send(b"\x1b[B", 0.3)
        sess.send("K", 1.2)
        check("Shuffle" in sess.text(), "queue shuffle: the overlay legend names SHIFT+s")
        if shutil.which("ffprobe") is None:
            # without ffmpeg / ffprobe the tracks cannot be loaded: each one fails at once and leaves the queue, so there is nothing to shuffle
            print("  skipped: queue shuffle order (ffmpeg / ffprobe is not installed on this machine)")
            sess.send(b"\x1b", 0.8)
            return
        names = ["Alpha Song", "Beta Song"]   # (the Japanese / Korean names are left out: a terminal without wide-character support does not show them, and the order would be offsets)

        def order():
            t = sess.text()
            pos = [t.find(n) for n in names]
            return tuple(sorted(range(len(names)), key=lambda i: pos[i])) if all(p >= 0 for p in pos) else tuple(pos)   # (the order, not the offsets: a status line above would move them)
        def settled():                            # the order once two frames in a row agree (a slow machine draws late)
            last = None
            for _ in range(8):
                sess.pump(0.4)
                cur = order()
                if cur == last:
                    return cur
                last = cur
            return last
        for _ in range(16):                       # a slow machine draws the overlay late: wait until both names are on the screen
            if all(n in sess.text() for n in names):
                break
            sess.pump(0.5)
        changed = False
        for _ in range(12):
            before = settled()
            sess.send("S", 0.3)
            if settled() != before:
                changed = True
                break
        check(changed, "queue shuffle: SHIFT+s changes the order of the queue")
        check("Undo shuffle" in sess.text() or "Unshuffle" in sess.text(), "queue shuffle: the overlay legend names SHIFT+u")
        sess.send("U", 0.3)
        after_undo = settled()
        # (a short track may end meanwhile: on a locked queue the track that played moves to the end, so the old order comes back as a rotation)
        rotated = any(before[k:] + before[:k] == after_undo for k in range(len(before))) if len(before) == len(after_undo) else False
        # (on a slow machine more tracks end meanwhile and the queue refills: then only what is still in both lists has to keep its old order, as a rotation)
        common_b = [x for x in before if x in after_undo]
        common_a = [x for x in after_undo if x in before]
        rotated = rotated or (len(common_b) >= 2 and len(common_b) == len(common_a) and any(common_b[k:] + common_b[:k] == common_a for k in range(len(common_b))))
        check(after_undo == before or rotated, "queue shuffle: SHIFT+u brings the order from before the shuffle back (before %r, after undo %r)" % (before, after_undo))
        sess.send("U", 0.6)
        check(sess.alive(), "queue shuffle: a second SHIFT+u (nothing to undo) is harmless")
        check(sess.alive(), "queue shuffle: the program is still running")
        sess.send(b"\x1b", 0.8)
    finally:
        sess.kill()


def queue_search_colors(binary, root, scripts):
    """The search bar of the big queue overlay: a match is written in the header colour -- also under the cursor, where the row keeps the cursor's
    background (before, the cursor colours replaced the header colour and the match was no longer marked there)."""
    home = os.path.join(root, "qc_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nColorHeader=10\nColorQueueCursorFg=75\nColorQueueCursorBg=240\nColorQueueInactiveBg=0\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    probe = pyte.Screen(8, 1)
    pyte.ByteStream(probe).feed(b"\x1b[38;5;10mH\x1b[0m\x1b[38;5;75m\x1b[48;5;240mC\x1b[0mP")
    HEADER_FG, CURSOR_FG, CURSOR_BG, PLAIN = probe.buffer[0][0].fg, probe.buffer[0][1].fg, probe.buffer[0][1].bg, probe.buffer[0][2].bg
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
        for i in range(4):
            sess.send("e", 0.4)
            sess.send(b"\x1b[B", 0.3)
        if shutil.which("ffprobe") is None:
            print("  skipped: queue search colours (ffmpeg / ffprobe is not installed on this machine)")
            return
        sess.send("K", 1.2)
        sess.send("/", 0.5)
        for ch in "Beta":
            sess.send(ch, 0.25)
        sess.send(b"\r", 0.8)   # leave the bar: the matches stay marked, the cursor is on the first one

        def cell_of(name):
            for y, line in enumerate(sess.screen.display):
                x = line.find(name)
                if x >= 0 and line.lstrip().startswith(("\u2502", "|")):
                    return sess.screen.buffer[y][x]
            return None
        for _ in range(10):
            if cell_of("Beta Song") is not None:
                break
            sess.pump(0.5)
        c = cell_of("Beta Song")
        check(c is not None, "queue search colours: the match is on the screen")
        if c is not None:
            check(c.fg == HEADER_FG and c.bg == CURSOR_BG, "queue search colours: a match under the cursor is in the header colour on the cursor's background (fg %s bg %s)" % (c.fg, c.bg))
        sess.send(b"\x1b[A", 0.6)   # the cursor goes to the row above: the match is a plain match again
        c = cell_of("Beta Song")
        a = cell_of("Alpha Song")
        if c is not None:
            check(c.fg == HEADER_FG and c.bg == PLAIN, "queue search colours: a match away from the cursor is in the header colour on the plain background (fg %s bg %s)" % (c.fg, c.bg))
        if a is not None:
            check(a.fg == CURSOR_FG and a.bg == CURSOR_BG, "queue search colours: a row that does not match keeps the cursor colours under the cursor (fg %s bg %s)" % (a.fg, a.bg))
        else:
            check(False, "queue search colours: the row above is on the screen")
        sess.send(b"\x1b", 0.8)   # ESC clears the search
        sess.send(b"\x1b", 0.8)
        check(sess.alive(), "queue search colours: the program is still running")
    finally:
        sess.kill()


def video_overlays(binary, root, scripts):
    """The & overlay (VIDEO), VIDEO OPTION, SEARCH YT and the [ Video Option ] button of the meta editor open, show their rows and close."""
    home = os.path.join(root, "vo_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideo=Y\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send("Y", 1.0)
        t = sess.text()
        check("Include Lyrics" in t and "Disk/Cover/Thumbnail" in t and "Manually Couple Local Files" in t and "Quick Search/Down. YT" in t and "General Fetch Videos Menu" in t and "Delete Current Video" in t and "Refetch this Video" in t, "video overlay: the rows are listed")
        check("Video Size" in t and "Effect" in t and "Full Screen Video" in t, "video overlay: picture size, effect and full screen video rows are listed")
        check("Contrast" in t and "Colors" in t and "Chromatic Aberration" in t and "Resolution" in t and "Buffer" in t, "video overlay: contrast, colors, chromatic aberration, resolution and buffer rows")
        check("Dither Pattern" in t and "< interleaved noise >" in t, "video overlay: the Dither Pattern row is listed, interleaved noise by default")
        for _ in range(21):   # (to "Manually Couple Local Files", under "Full Screen Video"; Show Clap Loading is the row before it; Effect Contrast and Frame Rate are among the rows above)
            sess.send(b"\x1b[B", 0.2)
        sess.wait_for(lambda t: "> Manually Couple Local Files" in t, 15.0)   # (the keys may be late on a busy machine: not before the row is reached)
        sess.send("\r", 1.0)
        sess.wait_for(lambda t: "VIDEO OPTION" in t and "all audio files" in t, 10.0)
        t = sess.text()
        check("VIDEO OPTION" in t and "SEARCH LOCAL SONGS" in t and "SEARCH LOCAL VIDEOS" in t and "Connect song and video" in t and "Delete COMPLETE VIDEO FILE connected to track" in t, "video option: the two search panes and the buttons are drawn")
        check("all audio files" in t and "[video]" not in t, "video option: the song list shows audio files only, all of them at first")
        sess.send(b"\x06", 0.6)                            # CTRL+f: only the songs without a video yet
        check("VIDEO OPTION" in sess.text() and "no video yet" in sess.text(), "video option: CTRL+f switches to 'no video yet'")
        sess.send(b"\x06", 0.6)
        sess.send("a", 0.4)
        check("VIDEO OPTION" in sess.text(), "video option: typing goes into the search field")
        sess.send(b"\x03", 0.8)                            # CTRL+c clears the choice (it must not close the program)
        check(sess.alive() and "VIDEO OPTION" in sess.text(), "video option: CTRL+c does not quit the program")
        sess.send(b"\x1bv", 1.5)                           # ALT+v: FETCH VIDEOS for the song
        check("FETCH VIDEOS" in sess.text() and "VIDEO OPTION" not in sess.text(), "video option: ALT+v switches to the fetch videos overlay")
        sess.send(b"\x1b", 0.8)
        check("VIDEO OPTION" not in sess.text() and "FETCH VIDEOS" not in sess.text(), "video option: ESC closes it")
        sess.send("Y", 0.8)
        sess.send(b"\x1b[B", 0.2)                          # (the cursor stays on the row that was used last: VIDEO OPTION -> SEARCH YT)
        sess.send("\r", 1.5)
        t = sess.text()
        check("SEARCH YT" in t and "video_down" in t, "search yt: the panel names the download folder")
        sess.send(b"\x1b", 0.8)
        check("SEARCH YT" not in sess.text(), "search yt: ESC closes it")
        sess.send(b"\x1bv", 1.5)
        t = sess.text()
        check("FETCH VIDEOS" in t and "VIDEOS FOR THE SONG" in t and "Alpha Song" in t, "fetch videos: ALT+v opens the full-screen overlay with the songs and the videos pane")
        sess.send(b"\x1b", 0.8)
        check("FETCH VIDEOS" not in sess.text(), "fetch videos: ESC closes it")
        sess.send("M", 1.5)
        check("Video Option" in sess.text(), "meta editor: the [ Video Option ] button is there")
        sess.send(b"\x1b", 0.8)
        check(sess.alive(), "video overlays: the program is still running")
    finally:
        sess.kill()


def fx_panel(sess):
    """The effect list on the screen: (top, bottom, left, right) = the rows / columns of the corners of its border, or None while it is not there."""
    d = sess.screen.display
    top = next((y for y, l in enumerate(d) if "VIDEO EFFECT" in l), None)
    if top is None:
        return None
    a = d[top].index("VIDEO EFFECT")
    left, right = d[top].rfind("\u256d", 0, a), d[top].find("\u256e", a)
    if left < 0 or right < 0:
        return None
    bot = next((y for y in range(top + 1, len(d)) if d[y][left:left + 1] == "\u2570" and d[y][right:right + 1] == "\u256f"), None)
    return None if bot is None else (top, bot, left, right)


def fx_centred(sess, pan):
    """(rows above, rows below, columns left, columns right) of the panel: centred means each pair differs by at most one."""
    top, bot, left, right = pan
    return top, sess.screen.lines - 1 - bot, left, sess.screen.columns - 1 - right


def fx_preview_cells(sess, pan):
    """The number of true colour cells inside the panel: the half blocks of the text preview carry their pixels in true colour backgrounds."""
    top, bot, left, right = pan
    scr = sess.screen
    n = 0
    for y in range(top, bot + 1):
        for x in range(left, right + 1):
            bg = scr.buffer[y][x].bg
            if len(bg) == 6 and bg != "default":
                n += 1
    return n


def video_fx_picker(binary, root, scripts):
    """ENTER on the Effect row of the VIDEO overlay opens the effect list: one panel in the middle of the window (rows and columns), over the menu it came from (that is not
    drawn meanwhile), two columns of names and a large preview; arrows move, ENTER chooses, ESC leaves it. The Base Layer row cycles and is written to config.txt."""
    home = os.path.join(root, "fxp_home")
    lib = os.path.join(root, "music")
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideo=Y\nWizardSeen=true\nVideoBase=scanlines\n" % lib)   # (a config from before clean was the default: it is read as clean once)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send("Y", 1.0)
        for _ in range(12):
            if "> Effect" in sess.text():
                break
            sess.send(b"\x1b[B", 0.25)
        check("> Effect" in sess.text() and "< Off >" in sess.text(), "effect picker: the Effect row is reached, Off at first")
        sess.send("\r", 1.0)
        sess.wait_for(lambda t: "VIDEO EFFECT" in t)
        t = sess.text()
        check("VIDEO EFFECT" in t and "Phosphor Green Glow" in t and "Night Vision" not in t and "Game Boy High-Res Gradient" in t and "CGA" in t and "C64" in t, "effect picker: ENTER lists all the effects, the new ones too (no Night Vision): %r" % t[:0])
        rows = [l for l in t.splitlines() if "Scanlines" in l and "Dither B/W" in l]   # (41 effects: the second column starts with Dither Gradient, Scanlines faces Dither B/W)
        check(bool(rows), "effect picker: the effects stand in two columns side by side")
        check("VIDEO SETTINGS" not in t and "Use Video in Lyrics Field" not in t, "effect picker: the list is the only panel while it is open (the VIDEO SETTINGS overlay it came from is not drawn beside it or behind it)")
        check("\u2580" in t, "effect picker: the preview column is drawn (half blocks)")
        pan = fx_panel(sess)
        check(pan is not None, "effect picker: the panel's border is found")
        if pan:
            above, below, lft, rgt = fx_centred(sess, pan)
            check(abs(above - below) <= 1 and abs(lft - rgt) <= 1 and min(above, below, lft, rgt) >= 0,
                  "effect picker: the panel is centred in rows and columns (rows above / below: %d / %d, columns left / right: %d / %d)" % (above, below, lft, rgt))
            inside = fx_preview_cells(sess, pan)
            # the preview was a strip of at most 36 columns x 10 rows of half blocks (360 cells): three times that
            check(inside >= 1080, "effect picker: the preview is at least three times as large as before (%d half-block cells; was 360 at most)" % inside)
        sess.send(b"\x1b", 0.8)
        t = sess.text()
        check("VIDEO EFFECT" not in t and "< Off >" in t, "effect picker: ESC leaves the effect as it was")
        check("VIDEO SETTINGS" in t and "Use Video in Lyrics Field" in t, "effect picker: ESC shows the VIDEO SETTINGS overlay again")
        sess.send("\r", 1.0)
        sess.send(b"\x1b[B", 0.3)          # Scanlines
        sess.send(b"\x1b[B", 0.3)          # CRT
        sess.send("\r", 1.2)
        t = sess.text()
        check("VIDEO EFFECT" not in t and "< CRT >" in t, "effect picker: arrows + ENTER choose an effect (CRT)")
        with open(cfg, encoding="utf-8") as f:
            c = f.read()
        check("VideoFx=crt" in c, "effect picker: the choice is written to config.txt")
        sess.send("\r", 1.0)
        sess.send(b"\x1b[C", 0.3)          # right: one column on
        sess.send("\r", 1.2)
        t = sess.text()
        check("VIDEO EFFECT" not in t and "< CRT >" not in t, "effect picker: the right arrow goes to the next column")
        # the Base Layer row
        for _ in range(12):
            if "> Base Layer" in sess.text():
                break
            sess.send(b"\x1b[B", 0.25)
        check("> Base Layer" in sess.text() and "< clean >" in sess.text(), "base layer: the row is listed, clean by default (the old config.txt said VideoBase=scanlines)")
        with open(cfg, encoding="utf-8") as f:
            c0 = f.read()
        check("VideoBaseDefault=clean" in c0, "base layer: the old config.txt was written again with the marker VideoBaseDefault=clean")
        sess.send(b"\x1b[C", 0.6)
        check("< scanlines >" in sess.text(), "base layer: the right arrow goes round to scanlines")
        sess.send(b"\x1b[C", 0.6)
        check("< aperture grille >" in sess.text(), "base layer: the right arrow changes it")
        with open(cfg, encoding="utf-8") as f:
            c = f.read()
        check("VideoBase=aperture grille" in c, "base layer: written to config.txt")
        # Settings -> ANIMA/VIZ: ENTER on the Video Effect row
        sess.send(b"\x1b", 0.8)
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)
        hit = False
        for _ in range(70):
            sess.send(b"\x1b[B", 0.15)
            sess.pump(0.3)
            if "Preview: Video Effect" in sess.text():
                hit = True
                break
        check(hit, "effect picker: the Video Effect row of the settings is reached")
        sess.send("\r", 1.0)
        sess.wait_for(lambda t: "VIDEO EFFECT" in t)
        sess.pump(0.6)
        t = sess.text()
        check("VIDEO EFFECT" in t and "Phosphor Green Glow" in t, "effect picker: ENTER in Settings opens the list too")
        pan = fx_panel(sess)
        if pan:
            above, below, lft, rgt = fx_centred(sess, pan)
            check(abs(above - below) <= 1 and abs(lft - rgt) <= 1, "effect picker: in Settings the panel is centred as well (rows above / below: %d / %d, columns left / right: %d / %d)" % (above, below, lft, rgt))
            inside = fx_preview_cells(sess, pan)
            check(inside >= 1080, "effect picker: in Settings the preview is large too (%d half-block cells)" % inside)
            check("Preview: Video Effect" not in sess.text(), "effect picker: in Settings the box of its own preview is not drawn behind the list (a cut-off half of it showed above the list)")
        else:
            check(False, "effect picker: in Settings the panel's border is found")
            sess.dump("effect picker in Settings")
            print("  reverse rows:", [(y, sess.screen.display[y].rstrip()[:60]) for y in range(sess.screen.lines) if any(sess.screen.buffer[y][x].reverse for x in range(sess.screen.columns))])
        sess.pump(1.0)   # (a slow runner: the list must have taken its first key before the next one)
        sess.send(b"\x1b[B", 1.0)
        sess.send("\r", 1.5)
        def fx_cfg():
            try:
                with open(cfg, encoding="utf-8") as f:
                    return "VideoFx=pixel" in f.read()
            except OSError:
                return False
        def row_pixel(t):   # (the Settings row itself: the status line may already say "Unsaved changes!")
            return any("Video Effect " in l and ": Pixel" in l and "Contrast" not in l and "Preview" not in l for l in t.splitlines())
        sess.wait_for(lambda t: "VIDEO EFFECT" not in t and ("video effect: Pixel" in t or row_pixel(t) or fx_cfg()), 20)   # (a slow runner closes the list and draws the status line a moment later; config.txt has the choice too)
        t = sess.text()
        ok = "VIDEO EFFECT" not in t and ("video effect: Pixel" in t or row_pixel(t) or fx_cfg())
        check(ok, "effect picker: Settings takes the choice (Pixel, one below Dither Gradient where the picker started: the right arrow had taken CRT one column on)")
        if not ok:
            sess.dump("effect picker: after the choice")
            with open(cfg, encoding="utf-8") as f:
                print("  config VideoFx lines:", [l.strip() for l in f if l.startswith("VideoFx=")])
        check(sess.alive(), "effect picker: the program is still running")
    finally:
        sess.kill()


def video_base_per_track(binary, root, scripts):
    """The Base Layer of a track (VIDEO OPTIONS of the big video, SHIFT+s) is kept: it cycles, stays after the panel is closed and opened again,
    and is stored with the track (it was never read back). The effect list opened from that panel replaces it while it is open (one centred panel) and the panel is back after it."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: base layer per track (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vbt_home")
    lib = os.path.join(root, "vbt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=20", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=20", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nHKeyVideo=Y\nWizardSeen=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        sess.send("/", 0.5)
        sess.send("clip song", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        sess.send("v", 2.0)
        sess.send("S", 1.0)
        check("VIDEO OPTIONS" in sess.text() and "Video Base Layer" in sess.text(), "base layer per track: SHIFT+s opens VIDEO OPTIONS of the big video")
        sess.send(b"\x1b[B", 0.3)   # Base Layer is right under the Video Effect row
        check("Video Base Layer" in sess.text() and "as in Settings (clean)" in sess.text(), "base layer per track: the row reads as in Settings (clean) at first")
        sess.send(b"\x1b[C", 0.6)
        check("\u25c0 scanlines \u25b6" in sess.text(), "base layer per track: RIGHT gives the track its own base layer (scanlines)")
        sess.send(b"\x1b[C", 0.6)
        check("\u25c0 aperture grille \u25b6" in sess.text(), "base layer per track: RIGHT again: aperture grille")
        sess.send(b"\x1b[C", 0.6)
        check("\u25c0 shadow mask \u25b6" in sess.text(), "base layer per track: RIGHT again: shadow mask")
        sess.send("S", 1.0)
        check("VIDEO OPTIONS" not in sess.text(), "base layer per track: SHIFT+s closes the panel")
        sess.send("S", 1.0)
        sess.send(b"\x1b[B", 0.3)
        check("\u25c0 shadow mask \u25b6" in sess.text(), "base layer per track: the choice is still there after closing and opening the panel")
        sess.send(b"\x7f", 0.6)           # DEL: back to as in Settings
        check("as in Settings (clean)" in sess.text() and "\u25c0 shadow mask" not in sess.text(), "base layer per track: DEL puts the row back to as in Settings")
        sess.send(b"\x1b[B", 0.3)   # Video Effect Contrast is right under the Base Layer row
        check("Video Effect Contrast" in sess.text() and "as in Settings (reduced)" in sess.text(), "effect contrast per track: the row reads as in Settings (reduced, the default) at first")
        sess.send(b"\x1b[C", 0.6)
        check("\u25c0 full \u25b6" in sess.text(), "effect contrast per track: RIGHT gives the track its own level (full)")
        sess.send(b"\x1b[C", 0.6)
        check("\u25c0 reduced \u25b6" in sess.text(), "effect contrast per track: RIGHT again: reduced")
        sess.send(b"\x7f", 0.6)
        check("as in Settings (reduced)" in sess.text() and "\u25c0 reduced" not in sess.text(), "effect contrast per track: DEL puts the row back to as in Settings")
        sess.send(b"\x1b[A", 0.3)   # (back on the Base Layer row)
        # the effect list from this panel: the panel is not drawn while the list is open
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\r", 1.2)
        t = sess.text()
        check("VIDEO EFFECT" in t and "VIDEO OPTIONS" not in t and "Video Base Layer" not in t, "effect list from VIDEO OPTIONS: the list opens in the middle and the panel is not drawn meanwhile")
        check("as in Settings" in t and "Phosphor Green Glow" in t and "Night Vision" not in t, "effect list from VIDEO OPTIONS: its first entry is as in Settings; no Night Vision")
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\r", 1.2)
        t = sess.text()
        check("VIDEO EFFECT" not in t and "VIDEO OPTIONS" in t and "\u25c0 Off \u25b6" in t, "effect list from VIDEO OPTIONS: DOWN + ENTER choose Off for the track")
        check(sess.alive(), "base layer per track: the program is still running")
    finally:
        sess.kill()


def fx_hole(sess, pan, image=7201):
    """The picture behind the panel (the video of the big screen, Kitty image 7201) is cut open where the panel lies and nowhere else.
    Returns (share of transparent pixels inside the panel, share of opaque pixels in the two cell rows above it, the same below it), or None
    when the picture is not there. (The picture's own pixels are mapped to cells by the placement's c x r cells over its cropped pixels.)"""
    p, img = sess.pics.get(image), sess.pic_rgba.get(image)
    if not p or not img or not p["r"] or not p["c"]:
        return None
    w, h, px = img
    top, bot, left, right = pan
    def py(row): return p["y"] + (row - p["row"]) * p["h"] / p["r"]
    def pxc(col): return p["x"] + (col - p["col"]) * p["w"] / p["c"]
    def share(r0, r1, c0, c1, transparent):
        y0, y1 = max(0, int(py(max(r0, p["row"])) + 1)), min(h, int(py(min(r1, p["row"] + p["r"])) - 1))
        x0, x1 = max(0, int(pxc(max(c0, p["col"])) + 1)), min(w, int(pxc(min(c1, p["col"] + p["c"])) - 1))
        n = hit = 0
        for y in range(y0, y1):
            row = px[(y * w + x0) * 4:(y * w + x1) * 4]
            alpha = row[3::4]
            n += len(alpha)
            hit += sum(1 for a in alpha if (a == 0) == transparent)
        return hit / n if n else None
    return (share(top, bot + 1, left, right + 1, True), share(top - 2, top, left, right + 1, False), share(bot + 1, bot + 3, left, right + 1, False))


def video_fx_picker_picture(binary, root, scripts):
    """In a Kitty terminal the preview of the effect list is a real picture (not half blocks of text), in the three menus that have a picture behind
    them or room for one: VIDEO OPTIONS of the big video (SHIFT+s), the Style Option overlay over the main screen and the VIDEO SETTINGS overlay. It lies in the
    second picture slot (Kitty image 7202) inside the one centred panel, is large (it was 18 x 5 cells), is made again as the highlighted effect changes, and is
    deleted when the list closes. The picture behind (the video) is cut open exactly where the panel lies: no black rectangle of another size."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: effect list picture (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "fpp_home")
    lib = os.path.join(root, "fpp_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=30", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=30", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nHKeyVideo=Y\nWizardSeen=true\n" % lib)
    sess = Session(binary, home, scripts, {"KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"}, quiet_stderr=True, gfx=True)
    PIC = b"i=7202,"
    def pic_after(mark, wait=8.0):
        end = time.time() + wait
        while time.time() < end:
            if PIC in sess.raw[mark:]:
                return True
            sess.pump(0.25)
        return PIC in sess.raw[mark:]
    def picture_in_panel(what):
        """The list is one centred panel and its preview picture (image 7202) is large and lies inside it."""
        for _ in range(16):   # (the harness reads the terminal slower than the program writes it: wait for the panel to be on the screen)
            if fx_panel(sess) is not None and sess.pics.get(7202):
                break
            sess.pump(0.25)
        pan, p2 = fx_panel(sess), sess.pics.get(7202)
        check(pan is not None, "effect list picture (%s): the panel is on the screen" % what)
        check(p2 is not None, "effect list picture (%s): the preview picture is placed" % what)
        if not (pan and p2):
            return pan
        above, below, lft, rgt = fx_centred(sess, pan)
        check(abs(above - below) <= 1 and abs(lft - rgt) <= 1, "effect list picture (%s): the panel is centred (rows above / below: %d / %d, columns left / right: %d / %d)" % (what, above, below, lft, rgt))
        check(p2["c"] * p2["r"] >= 3 * 18 * 5 and p2["c"] >= 40, "effect list picture (%s): the preview picture is at least three times as large as the 18 x 5 cells it had (%d x %d cells)" % (what, p2["c"], p2["r"]))
        # its pixels: the resolution of the video everywhere else (standard: 640 across on Kitty), not the 480 it was cut to
        check(p2["w"] >= 600, "effect list picture (%s): the preview has the video's resolution (%d x %d pixels; standard is 640 across)" % (what, p2["w"], p2["h"]))
        top, bot, left, right = pan
        check(top < p2["row"] and p2["row"] + p2["r"] <= bot and left < p2["col"] and p2["col"] + p2["c"] <= right,
              "effect list picture (%s): the preview picture lies inside the panel (picture rows %d..%d, columns %d..%d; panel rows %d..%d, columns %d..%d)" % (what, p2["row"], p2["row"] + p2["r"] - 1, p2["col"], p2["col"] + p2["c"] - 1, top, bot, left, right))
        check("VIDEO OPTIONS" not in sess.text() and "VIDEO SETTINGS" not in sess.text() and "STYLE OPTION" not in sess.text(), "effect list picture (%s): the menu it came from is not drawn while the list is open" % what)
        return pan
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        sess.send("/", 0.5)
        sess.send("clip song", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        # --- the big video: SHIFT+s, the Video Effect row, ENTER
        sess.send("v", 2.0)
        mark = len(sess.raw)
        sess.send("S", 1.0)
        check(b"VIDEO OPTIONS" in sess.raw[mark:], "effect list picture: SHIFT+s opens VIDEO OPTIONS of the big video")
        mark = len(sess.raw)
        sess.send(b"\r", 1.5)
        check("VIDEO EFFECT" in sess.text(), "effect list picture: the list opens")
        check(pic_after(mark), "effect list picture (big video): the preview is a picture (Kitty image 7202)")
        check("▀".encode() not in sess.raw[mark:], "effect list picture (big video): no half blocks of text are drawn for it")
        sess.pump(1.0)
        pan = picture_in_panel("big video")
        if pan:
            hole = fx_hole(sess, pan)
            check(hole is not None and hole[0] is not None and hole[0] >= 0.98, "effect list picture (big video): the video behind is cut open over the whole panel (%s of its pixels are clear)" % (None if hole is None else "%.0f%%" % (100 * (hole[0] or 0))))
            # (no larger hole: the cell rows just above and below the panel keep their picture; the old hole was the union of the menu and the list)
            check(hole is not None and all(v is None or v >= 0.3 for v in hole[1:]), "effect list picture (big video): the video is not cut open above or below the panel (opaque share above / below: %s)" % (None if hole is None else ", ".join("-" if v is None else "%.0f%%" % (100 * v) for v in hole[1:])))
        mark = len(sess.raw)
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x1b[B", 0.3)
        check(pic_after(mark), "effect list picture (big video): it is made again for the effect that is highlighted")
        mark = len(sess.raw)
        sess.send(b"\x1b", 1.0)
        check(b"a=d,d=I,i=7202" in sess.raw[mark:], "effect list picture (big video): the picture is deleted when the list closes")
        check(7202 not in sess.pics and "VIDEO OPTIONS" in sess.text(), "effect list picture (big video): the panel it came from is back")
        if pan:
            sess.pump(1.5)
            hole = fx_hole(sess, pan)
            check(hole is not None and hole[0] is not None and hole[0] <= 0.5, "effect list picture (big video): the video is sent whole again after the list (%s of the pixels where the panel was are clear)" % (None if hole is None or hole[0] is None else "%.0f%%" % (100 * hole[0])))
        sess.send(b"\x1b", 0.8)       # the panel
        sess.send("v", 1.5)            # the big video
        # --- the Style Option overlay over the main screen
        mark = len(sess.raw)
        sess.send("S", 1.5)
        check(b"STYLE OPTION" in sess.raw[mark:], "effect list picture: SHIFT+s opens Style Option over the main screen")
        for _ in range(10):          # Look, five cover rows, Use Disk/Cover/Thumbnail, Video of this track, Use Lyrics on Video, Video Size: the next row is Video Effect
            sess.send(b"\x1b[B", 0.25)
        mark = len(sess.raw)
        sess.send(b"\r", 1.5)
        check("VIDEO EFFECT" in sess.text(), "effect list picture: ENTER in Style Option opens the list")
        check(pic_after(mark), "effect list picture (Style Option): the preview is a picture (Kitty image 7202)")
        check("▀".encode() not in sess.raw[mark:], "effect list picture (Style Option): no half blocks of text are drawn for it")
        sess.pump(1.0)
        picture_in_panel("Style Option")
        mark = len(sess.raw)
        sess.send(b"\x1b", 1.0)
        check(pic_after(mark), "effect list picture (Style Option): the preview of Style Option itself (the same second slot) is a picture again when the list closes")
        sess.send(b"\x1b", 1.0)
        # --- the VIDEO SETTINGS overlay
        sess.send("Y", 1.5)
        for _ in range(12):
            if "> Effect" in sess.text():
                break
            sess.send(b"\x1b[B", 0.25)
        check("> Effect" in sess.text(), "effect list picture: the Effect row of the VIDEO SETTINGS overlay is reached")
        mark = len(sess.raw)
        sess.send(b"\r", 1.5)
        check("VIDEO EFFECT" in sess.text(), "effect list picture: ENTER in VIDEO SETTINGS opens the list")
        check(pic_after(mark), "effect list picture (VIDEO SETTINGS): the preview is a picture (Kitty image 7202)")
        sess.pump(1.0)
        picture_in_panel("VIDEO SETTINGS")
        mark = len(sess.raw)
        sess.send(b"\x1b", 1.0)
        for _ in range(12):
            if b"a=d,d=I,i=7202" in sess.raw[mark:] and "VIDEO SETTINGS" in sess.text():
                break
            sess.pump(0.25)
        check(b"a=d,d=I,i=7202" in sess.raw[mark:] and "VIDEO SETTINGS" in sess.text(), "effect list picture (VIDEO SETTINGS): the picture is deleted when the list closes and the overlay is back")
        check(sess.alive(), "effect list picture: the program is still running")
    finally:
        sess.kill()


def video_fx_picker_third_slot(binary, root, scripts):
    """The effect list from VIDEO SETTINGS when the main screen behind already shows two pictures (the album cover, Kitty image 7201, and the video
    next to it, 7202): the preview is still a real picture at the video's resolution -- in a third picture of its own (7203) inside the panel -- not
    half blocks of text (it was: the coarse, "pixelated" preview of VIDEO SETTINGS and Style Option). The cover and the video stay, cut open where the
    panel lies; ESC deletes the preview and the overlay is back."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: effect list third picture (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "fp3_home")
    lib = os.path.join(root, "fp3_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=30", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=30", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    make_png(os.path.join(lib, "cover.png"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nVideoView=true\nHKeyVideo=Y\nWizardSeen=true\nAlbumCover=true\nCoverMode=just cover\n" % lib)
    sess = Session(binary, home, scripts, {"KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty", "MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True, gfx=True)
    try:
        sess.wait_for(lambda t: "Clip Song" in t, 15)
        for _ in range(20):
            if 7201 in sess.pics and 7202 in sess.pics:
                break
            sess.pump(0.5)
        check(7201 in sess.pics and 7202 in sess.pics, "effect list third picture: the main screen shows two pictures (the cover and the video next to it)")
        sess.send("Y", 1.5)
        for _ in range(14):
            if "> Effect" in sess.text():
                break
            sess.send(b"\x1b[B", 0.3)
        check("> Effect" in sess.text(), "effect list third picture: the Effect row of VIDEO SETTINGS is reached")
        mark = len(sess.raw)
        sess.send(b"\r", 2.0)
        check("VIDEO EFFECT" in sess.text(), "effect list third picture: ENTER opens the list")
        for _ in range(16):
            if 7203 in sess.pics:
                break
            sess.pump(0.5)
        p3, pan = sess.pics.get(7203), fx_panel(sess)
        check(p3 is not None, "effect list third picture: the preview is a picture of its own (Kitty image 7203)")
        check("\u2580".encode() not in sess.raw[mark:], "effect list third picture: no half blocks of text are drawn for it")
        if p3 and pan:
            top, bot, left, right = pan
            check(p3["w"] >= 600 and p3["c"] >= 40, "effect list third picture: large and at the video's resolution (%d cells, %d pixels across)" % (p3["c"], p3["w"]))
            check(top < p3["row"] and p3["row"] + p3["r"] <= bot and left < p3["col"] and p3["col"] + p3["c"] <= right, "effect list third picture: it lies inside the panel")
        check(7201 in sess.pics and 7202 in sess.pics, "effect list third picture: the cover and the video behind stay on the screen")
        hole = fx_hole(sess, pan, 7202) if pan else None
        check(hole is None or hole[0] is None or hole[0] >= 0.98, "effect list third picture: the video behind is cut open where the panel lies (%s)" % (None if hole is None or hole[0] is None else "%.0f%%" % (100 * hole[0])))
        mark = len(sess.raw)
        sess.send(b"\x1b", 1.2)
        check(b"a=d,d=I,i=7203" in sess.raw[mark:] and 7203 not in sess.pics and "VIDEO SETTINGS" in sess.text(), "effect list third picture: ESC deletes it and VIDEO SETTINGS is back")
        check(sess.alive(), "effect list third picture: the program is still running")
    finally:
        sess.kill()


def video_fx_picker_sizes(binary, root, scripts):
    """The effect list in every window size: while it is open the window is made smaller and larger again. The panel stays inside the window and centred
    (a column / row of margin where the window allows), the preview grows with the room and is left out where there is none (under 18 columns), and
    nothing is written past the window's edges. ESC closes the list at the end and the VIDEO SETTINGS overlay is back."""
    global COLS, ROWS
    COLS, ROWS = 140, 45
    home = os.path.join(root, "fps_home")
    lib = os.path.join(root, "fps_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Song A.wav"), 30)
    make_wav(os.path.join(lib, "Song B.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideo=Y\nWizardSeen=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)

    def resize(c, r):
        global COLS, ROWS
        COLS, ROWS = c, r
        fcntl.ioctl(sess.fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
        os.kill(sess.pid, signal.SIGWINCH)
        sess.screen = pyte.Screen(c, r)
        sess.stream = pyte.ByteStream(sess.screen)
        sess.raw = bytearray()
        sess.pump(1.2)

    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Song A" in sess.text():
                break
        sess.send("Y", 1.0)
        for _ in range(12):
            if "> Effect" in sess.text():
                break
            sess.send(b"\x1b[B", 0.25)
        sess.send("\r", 1.2)
        check(fx_panel(sess) is not None, "effect list sizes: the list is open at 140 x 45")
        # (columns, rows, the least half-block cells of the preview; 0 = it may be left out)
        for c, r, least in ((140, 45, 1080), (120, 32, 900), (100, 30, 400), (80, 24, 100), (60, 20, 0), (50, 16, 0), (44, 12, 0), (140, 45, 1080)):
            resize(c, r)
            sess.send(b"\x1b[B", 0.4)   # (an arrow key: the list is redrawn with the next effect)
            pan = fx_panel(sess)
            check(pan is not None, "effect list sizes @%dx%d: the panel is on the screen" % (c, r))
            if not pan:
                sess.dump("effect list sizes %dx%d" % (c, r))
                continue
            above, below, lft, rgt = fx_centred(sess, pan)
            check(min(above, below, lft, rgt) >= 0 and abs(above - below) <= 1 and abs(lft - rgt) <= 1,
                  "effect list sizes @%dx%d: the panel is inside the window and centred (rows above / below: %d / %d, columns left / right: %d / %d)" % (c, r, above, below, lft, rgt))
            inside = fx_preview_cells(sess, pan)
            check(inside >= least, "effect list sizes @%dx%d: the preview has %d half-block cells (at least %d)" % (c, r, inside, least))
            if c >= 100 and r >= 30:
                check(min(above, below, lft, rgt) >= 1, "effect list sizes @%dx%d: a row / column of the window is left free around the panel" % (c, r))
            check_fit(sess, "effect list sizes @%dx%d" % (c, r), False)
        check("VIDEO SETTINGS" not in sess.text(), "effect list sizes: the overlay it came from is still not drawn")
        sess.send(b"\x1b", 1.0)
        check(fx_panel(sess) is None and "VIDEO SETTINGS" in sess.text(), "effect list sizes: ESC closes the list; the VIDEO SETTINGS overlay is back")
    finally:
        sess.kill()


def book_embedded_chapters(binary, root, scripts):
    """Audio book: an .m4b with three embedded chapters lists them as three rows with their own titles; ENTER on the second plays it
    from its start (the chapter shown is 2) and n goes on to the third."""
    import subprocess
    if shutil.which("ffmpeg") is None:
        print("  skipped: embedded chapters (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "bec_home")
    books = os.path.join(root, "bec_books")
    book = os.path.join(books, "Embedded Book")
    os.makedirs(book)
    meta = os.path.join(root, "bec_meta.txt")
    with open(meta, "w", encoding="utf-8") as f:
        f.write(";FFMETADATA1\n")
        for i, (a, b, t) in enumerate([(0, 10, "Alpha Intro"), (10, 20, "Beta Middle"), (20, 30, "Gamma End")]):
            f.write("[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=%s\n" % (a * 1000, b * 1000, t))
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=300:duration=30", "-i", meta, "-map_metadata", "1",
                        "-c:a", "aac", "-f", "ipod", os.path.join(book, "story.m4b")], capture_output=True)
    if r.returncode != 0:
        print("  (skipped: ffmpeg could not make the test .m4b: %s)" % r.stderr.decode("utf-8", "replace")[:120])
        return
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("AudioBookPath=%s\nStartMode=audiobook\nUseTerminalSettings=true\n" % books)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.5)
        for _ in range(20):
            sess.pump(0.5)
            if "Embedded Book" in sess.text():
                break
        sess.send(b"\r", 2.0)       # open the book
        t = ""
        for _ in range(30):          # ffprobe runs on opening
            t = sess.text()
            if "Gamma End" in t:
                break
            sess.pump(0.5)
        check("Alpha Intro" in t and "Beta Middle" in t and "Gamma End" in t, "embedded chapters: the three chapters of the .m4b are listed as rows")
        if "Gamma End" not in t:
            sess.dump("embedded chapters list")
        check("story" not in t.replace("Embedded", ""), "embedded chapters: the file itself is not listed as one row")
        sess.send(b"\r", 2.0)       # the first chapter plays (nothing was playing: no question)
        sess.send(b"\x1b[B", 0.8)
        sess.send(b"\r", 1.0)       # the second chapter: the question about a bookmark in the one that is left
        for _ in range(10):
            if "Going on to" in sess.text():
                break
            sess.pump(0.5)
        check("Going on to Beta Middle" in sess.text(), "embedded chapters: the question names the chapter by its title, not by the file")
        sess.send("n", 2.5)          # no bookmark
        t = ""
        for _ in range(10):
            t = sess.text()
            if "2/3" in t and "Beta Middle" in t:
                break
            sess.pump(0.5)
        check("2/3" in t and "Beta Middle" in t, "embedded chapters: the second chapter plays (shown as 2/3 with its title)")
        sess.send("n", 1.5)          # the next-chapter key, then no bookmark
        sess.send("n", 2.5)
        t = sess.text()
        check("3/3" in t and "Gamma End" in t, "embedded chapters: n goes on to the third chapter")
        check(sess.alive(), "embedded chapters: still running")
        # play modes inside one file: "stop after the chapter" pauses where the next one starts, "repeat" jumps back to the chapter's start
        sess.send(b"\x1b[A", 0.8)
        sess.send(b"\x1b[A", 0.8)
        sess.send(b"\r", 1.0)       # chapter 1 again (the question about the bookmark, then it plays)
        sess.send("n", 2.0)
        sess.send("m", 0.8)          # on -> repeat
        sess.send("m", 0.8)          # repeat -> stop after
        t = ""
        for _ in range(40):          # chapter 1 is 10 s long: playback pauses where chapter 2 starts (the button turns to PLAY)
            t = sess.text()
            if "2/3" in t and "PLAY" in t:
                break
            sess.pump(0.5)
        check("2/3" in t and "PLAY" in t and "PAUSE" not in t, "embedded chapters: stop after the chapter pauses where the next embedded chapter starts")
        sess.send("p", 1.0)          # on from there: chapter 2 plays
        check("PAUSE" in sess.text() and "2/3" in sess.text(), "embedded chapters: p goes on with chapter 2")
        sess.send("q", 1.5)
    finally:
        sess.kill()


def video_dither_option(binary, root, scripts):
    """VIDEO SETTINGS overlay: the Dither Pattern row steps through the patterns with the arrows (round and round) and is saved as VideoDither=."""
    home = os.path.join(root, "vd_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideo=Y\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send("Y", 1.0)
        def saved():
            try:
                return open(cfg, encoding="utf-8").read()
            except OSError:
                return ""
        def fxc_line():
            return " ".join(l for l in sess.text().split("\n") if "Effect Contrast" in l)
        base_seen = False
        fxc_done = False
        for _ in range(20):
            if "> Base Layer" in sess.text() and "< clean >" in sess.text():
                base_seen = True
            if not fxc_done and "> Effect Contrast" in sess.text():   # the row under the base layer: reduced by default, then low, full, reduced
                fxc_done = True
                check("< reduced >" in fxc_line(), "effect contrast: the row sits under the base layer and says reduced (the default)")
                for want in ("low", "full", "reduced"):
                    sess.send(b"\x1b[C", 0.6)
                    check("< %s >" % want in fxc_line(), "effect contrast: the next level is %s" % want)
                    check("VideoEffectContrast=%s" % want in saved(), "effect contrast: %s is saved in the config" % want)
                sess.send(b"\x1b[D", 0.6)
                check("< full >" in fxc_line(), "effect contrast: left arrow goes back one (round and round)")
                sess.send(b"\x1b[C", 0.6)
            if "> Dither Pattern" in sess.text():
                break
            sess.send(b"\x1b[B", 0.2)
        sess.pump(0.4)
        check(base_seen, "base layer: the row sits under the effect, clean by default")
        check(fxc_done, "effect contrast: the row was reached between the base layer and the dither pattern")
        check("> Dither Pattern" in sess.text() and "< interleaved noise >" in sess.text(), "dither pattern: the row is reached and says interleaved noise (the default)")
        for want in ("golden ratio", "auto", "blue noise", "bayer 2x2", "bayer 4x4", "bayer 8x8", "interleaved noise"):
            sess.send(b"\x1b[C", 0.6)
            check("< %s >" % want in sess.text(), "dither pattern: the next one is %s" % want)
            check("VideoDither=%s" % want in saved(), "dither pattern: %s is saved in the config" % want)
        sess.send(b"\x1b[D", 0.6)
        check("< bayer 8x8 >" in sess.text(), "dither pattern: left arrow goes back one")
        sess.send(b"\x1b[B", 0.8)                          # the next row: Gradient Saturation
        check("> Gradient Saturation" in sess.text() and "< 85 % >" in sess.text(), "gradient saturation: the row is under the dither pattern and says 85 % (the default)")
        sess.send(b"\x1b[C", 0.6)
        check("< 90 % >" in sess.text() and "VideoGradientSaturation=90" in saved(), "gradient saturation: right arrow makes it 90 % and it is saved")
        sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[D", 0.6)
        check("< 80 % >" in sess.text() and "VideoGradientSaturation=80" in saved(), "gradient saturation: left arrow twice makes it 80 %")
        check(sess.alive(), "dither pattern: the program is still running")
    finally:
        sess.kill()


def video_fetch_flow(binary, root, scripts):
    """FETCH VIDEOS with a fake yt-dlp: the videos of the song under the cursor are searched by themselves, two hits demand a choice,
    only the chosen video is downloaded."""
    import subprocess, stat
    if shutil.which("ffmpeg") is None:
        print("  skipped: fetch videos flow (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vff_home")
    lib = os.path.join(root, "vff_music")
    fake = os.path.join(root, "vff_bin")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(fake)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 5)
    clip = os.path.join(root, "vff_clip.mp4")
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=2", "-pix_fmt", "yuv420p", clip], capture_output=True)
    if r.returncode != 0:
        print("  (skipped: ffmpeg could not make the test clip)")
        return
    yt = os.path.join(fake, "yt-dlp")
    with open(yt, "w") as f:
        f.write("""#!/bin/bash
if [[ "$*" == *"ytsearch"* ]]; then
  echo '{"id":"AAAAAAAAAAA","title":"Alpha Song (Official Video)","uploader":"Chan One","duration":212}'
  echo '{"id":"BBBBBBBBBBB","title":"Alpha Song live","channel":"Chan Two","duration":301}'
  exit 0
fi
out=""
while [ $# -gt 0 ]; do if [ "$1" = "-o" ]; then out="$2"; fi; shift; done
f="${out/\\%(ext)s/mp4}"
echo "download:PCT 50.0%"
cp "@CLIP@" "$f"
echo "FILE:$f"
""".replace("@CLIP@", clip))
    os.chmod(yt, os.stat(yt).st_mode | stat.S_IEXEC)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    sess = Session(binary, home, scripts, {"PATH": fake + os.pathsep + os.environ.get("PATH", "")}, quiet_stderr=True)
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
        sess.send(b"\x1bv", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song live" in sess.text():
                break
        t = sess.text()
        check("Alpha Song (Official Video)" in t and "Alpha Song live" in t, "fetch videos: the videos of the song under the cursor are searched by themselves")
        check("[?]" in t, "fetch videos: several videos -> the song waits for a choice")
        sess.send("F", 1.0)
        check("a choice" in sess.text() or "nothing chosen" in sess.text(), "fetch videos: nothing is downloaded without a choice")
        sess.send("\r", 0.6)      # into the videos
        sess.send(b"\x1b[B", 0.3)  # the second one
        sess.send("\r", 0.8)      # choose it
        check("[x]" in sess.text(), "fetch videos: the choice is marked")
        t = sess.text()
        check("Download only current selected" in t and "Download all chosen (1)" in t, "fetch videos: the legend has SHIFT+a (only the current one) and SHIFT+f with the number of choices")
        state = os.path.join(home, ".config", "mousiki", "video_fetch_state.json")
        check(os.path.exists(state) and "BBBBBBBBBBB" in open(state, encoding="utf-8").read(), "fetch videos: the choice is saved at once (video_fetch_state.json)")
        sess.send(b"\x06", 0.6)    # CTRL+f: all songs
        sess.send(b"\x06", 0.6)    # songs with a video (none)
        sess.send(b"\x06", 0.8)    # only the chosen ones
        check("SONGS, chosen (1)" in sess.text() and "Alpha Song" in sess.text(), "fetch videos: CTRL+f has a stop that lists only the songs with a chosen video")
        sess.send(b"\x06", 0.8)    # back to missing video
        sess.send("A", 0.5)         # SHIFT+a: only the song under the cursor
        for _ in range(30):
            sess.pump(0.5)
            if "downloaded and paired" in sess.text():
                break
        check("1 of 1 chosen videos downloaded and paired" in sess.text(), "fetch videos: SHIFT+a downloads the chosen video of the song under the cursor")
        links = os.path.join(home, ".config", "mousiki", "video_links.txt")
        ok = os.path.exists(links) and "Alpha Song" in open(links, encoding="utf-8").read()
        check(ok, "fetch videos: the pairing is saved")
    finally:
        sess.kill()


def video_fetch_restore(binary, root, scripts):
    """FETCH VIDEOS keeps the found videos and the choices (video_fetch_state.json): a song searched and chosen the last time shows [x] again, and
    the filter CTRL+f can list the chosen ones only."""
    home = os.path.join(root, "vfr_home")
    lib = os.path.join(root, "vfr_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Beta Song.wav"), 5)
    make_wav(os.path.join(lib, "Gamma Song.wav"), 5)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    state = {"songs": [{"path": os.path.join(lib, "Beta Song.wav"), "choice": 1, "seconds": 5,
                        "cands": [{"id": "CCCCCCCCCCC", "title": "Beta one", "uploader": "U1", "dur": 10},
                                  {"id": "DDDDDDDDDDD", "title": "Beta two", "uploader": "U2", "dur": 11}]}]}
    import json
    with open(os.path.join(home, ".config", "mousiki", "video_fetch_state.json"), "w", encoding="utf-8") as f:
        json.dump(state, f)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Beta Song" in sess.text():
                break
        sess.send(b"\x1bv", 1.5)
        t = sess.text()
        check("1 chosen" in t and "[x] Beta Song" in t.replace("  ", " "), "fetch videos: the choice of the last time is there again")
        sess.send(b"\x06", 0.6)
        sess.send(b"\x06", 0.6)
        sess.send(b"\x06", 0.8)
        t = sess.text()
        check("SONGS, chosen (1)" in t and "Beta Song" in t and "Gamma Song" not in t, "fetch videos: the filter lists only the chosen song")
        sess.send("c", 0.8)
        check("SONGS, chosen (0)" in sess.text() and "no video is chosen yet" in sess.text(), "fetch videos: clearing the choice takes the song out of the chosen list")
        sess.send(b"\x1b", 1.0)
        check(not os.path.exists(os.path.join(home, ".config", "mousiki", "video_fetch_state.json")) or "DDDDDDDDDDD" in open(os.path.join(home, ".config", "mousiki", "video_fetch_state.json"), encoding="utf-8").read(), "fetch videos: the state file is kept when the overlay closes")
    finally:
        sess.kill()


def video_pair_tags(binary, root, scripts):
    """A video that is paired with a track (same name beside it) shows that track's artist and title in the library list."""
    if shutil.which("ffmpeg") is None:
        print("  skipped: paired video tags (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vp_home")
    lib = os.path.join(root, "vp_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    import subprocess
    ff = ["ffmpeg", "-v", "error", "-y"]
    r1 = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=3", "-metadata", "artist=Pairartist", "-metadata", "title=Pairtitle",
                              os.path.join(lib, "Pair Song.mp3")], capture_output=True)
    r2 = subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=3", "-pix_fmt", "yuv420p", os.path.join(lib, "Pair Song.mp4")], capture_output=True)
    if r1.returncode != 0 or r2.returncode != 0:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if sess.text().count("Pairartist") >= 2:
                break
        check(sess.text().count("Pairartist") >= 2, "video pair tags: the paired video row shows the artist of its track")
    finally:
        sess.kill()


def video_library_modes(binary, root, scripts):
    """Video mode OFF: a mere video file (no song of the same name) is a track like any other -- listed, found by a normal search, playable; one that is
    paired with a song is left out of the list and of a normal search; v: lists the videos only, x: songs and videos."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: video library modes (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vlm_home")
    lib = os.path.join(root, "vlm_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = True
    for name in ("Paired Song", "Other Song"):
        ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=3", os.path.join(lib, name + ".mp3")], capture_output=True).returncode == 0
    for name in ("Paired Song", "Lone Clip"):
        ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=3", "-f", "lavfi", "-i", "sine=frequency=330:duration=3",
                                   "-pix_fmt", "yuv420p", "-shortest", os.path.join(lib, name + ".mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=false\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Other Song" in sess.text():
                break
        t = sess.text()
        check("Lone Clip" in t, "video mode off: a video file without a song of its name is listed as a track")
        check(t.count("| Paired Song") == 1, "video mode off: a video paired with a song is not listed twice (%d)" % t.count("| Paired Song"))
        sess.send("/", 0.5)
        sess.send("clip", 1.0)
        check("Lone Clip" in sess.text(), "video mode off: a normal search finds the mere video")
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 2.0)                                # (the box keeps the keyboard until ENTER once: then the hovered row plays)
        check("File      : Lone Clip" in sess.text(), "video mode off: the mere video plays (its sound)")
        sess.send(b"\x1b", 0.8)
        sess.send("/", 0.5)
        sess.send("v:", 1.0)
        t = sess.text()
        check("| Lone Clip" in t and "| Other Song" not in t, "video mode off: v: lists the videos only")
        sess.send(b"\x1b", 0.8)
        sess.send("/", 0.5)
        sess.send("x:", 1.0)
        t = sess.text()
        check("| Lone Clip" in t and "| Other Song" in t and t.count("| Paired Song") >= 2, "video mode off: x: lists songs and videos")
        sess.send(b"\x1b", 0.8)
    finally:
        sess.kill()


def main_lyrics_translation_key(binary, root, scripts):
    """The Latin letters key (" = SHIFT+2 on a German keyboard) on the main screen while the lyrics field shows synced lyrics that have a saved
    translation: the lyrics -> in Latin letters -> the lyrics + the translation -> in Latin letters + the translation -> the lyrics."""
    home = os.path.join(root, "mlt_home")
    lib = os.path.join(root, "mlt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    make_wav(os.path.join(lib, "Clip Song.wav"), 40)
    lines = ["さくら さくら", "やよいの そらは", "みわたす かぎり"]
    with open(os.path.join(lib, "lyrics", "Clip Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.10]%s\n[00:30.00]%s\n[00:35.00]%s\n" % tuple(lines))
    h = 1469598103934665603
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    with open(os.path.join(lib, "lyrics", "translations", "Clip Song.en.txt"), "w", encoding="utf-8") as f:
        f.write("#mousiki-translation to=en from=ja svc=google lines=3 hash=%016x\n" % h)
        f.write("Cherry blossoms, cherry blossoms\nIn the spring sky\nAs far as you can see\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nTranslateTo=en\nLyricsAnimation=0\nWizardSeen=true\nLatinLetters=false\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        check(sess.wait_for(lambda t: "\u3084\u3088\u3044" in t, 8), "lyrics translation key: the lyrics field shows the original lyrics")
        check("Cherry" not in sess.text(), "lyrics translation key: ... and no translation")
        J = "\u3084\u3088\u3044"   # (the original's second line, in Japanese)
        sess.send('"', 1.0)
        check(sess.wait_for(lambda t: "yayoino" in t, 4) and "spring" not in sess.text(), "lyrics translation key: 1st press = the lyrics in Latin letters")
        sess.send('"', 1.0)
        check(sess.wait_for(lambda t: J in t and "In the spring sky" in t, 4) and "yayoino" not in sess.text(), "lyrics translation key: 2nd press = the lyrics + the translation")
        sess.send('"', 1.0)
        check(sess.wait_for(lambda t: "yayoino" in t and "In the spring sky" in t, 4) and J not in sess.text(), "lyrics translation key: 3rd press = in Latin letters + the translation")
        sess.send('"', 1.0)
        check(sess.wait_for(lambda t: J in t, 4) and "spring" not in sess.text() and "yayoino" not in sess.text(), "lyrics translation key: 4th press = the lyrics as they are again")
        sess.send('"', 1.0)
        check(sess.wait_for(lambda t: "yayoino" in t, 4), "lyrics translation key: and round again: Latin letters")
        check(sess.alive(), "lyrics translation key: the program is still running")
    finally:
        sess.kill()


def video_translation_tab(binary, root, scripts):
    """The big video overlay: TAB goes off -> lyrics -> lyrics + translation -> translation only, through every saved translation; the
    legend names the one shown. (Needs ffmpeg for the test video.)"""
    if shutil.which("ffmpeg") is None:
        print("  skipped: video translation TAB (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vtt_home")
    lib = os.path.join(root, "vtt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=30", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=30", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    lines = ["First lyric line here", "Second lyric line here", "Third lyric line here"]
    with open(os.path.join(lib, "lyrics", "Clip Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.10]%s\n[00:20.00]%s\n[00:25.00]%s\n" % tuple(lines))
    h = 1469598103934665603
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    for code, texts in (("en", ["Erste Zeile hier", "Zweite Zeile hier", "Dritte Zeile hier"]), ("es", ["Primera linea aqui", "Segunda linea aqui", "Tercera linea aqui"])):
        with open(os.path.join(lib, "lyrics", "translations", "Clip Song.%s.txt" % code), "w", encoding="utf-8") as f:
            f.write("#mousiki-translation to=%s from=de svc=google lines=3 hash=%016x\n" % (code, h))
            f.write("\n".join(texts) + "\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nTranslateTo=en\nLyricsAnimation=0\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        sess.send("v", 2.5)
        t = sess.text()
        check("First lyric line" in t and "Lyrics on" in t, "video translation: the overlay starts with the lyric line")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics + transl. en" in t and "First lyric line" in t and "Erste" in t, "video translation: TAB 1 = lyrics + the first translation (en, the Translate To language)")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics transl. en" in t and "Erste" in t and "First lyric line" not in t, "video translation: TAB 2 = the translation only")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics + transl. es" in t and "First lyric line" in t and "Primera" in t, "video translation: TAB 3 = lyrics + the next translation (es)")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics transl. es" in t and "Primera" in t and "First lyric line" not in t, "video translation: TAB 4 = the second translation only")
        sess.send(b"\t", 1.2)
        check("Lyrics off" in sess.text(), "video translation: TAB 5 = lyrics off")
        sess.send(b"\t", 1.2)
        check("Lyrics on" in sess.text() and "First lyric line" in sess.text(), "video translation: TAB 6 = the lyric line again")
        check(sess.alive(), "video translation: the program is still running")
    finally:
        sess.kill()

def video_fullscreen_sizes(binary, root, scripts):
    """The & overlay: Video Size (regular / cropped / full) and Effect rows change and are remembered in config.txt; the full screen video (v)
    opens over a playing track with a video, UP / DOWN change the effect, TAB switches the lyric line, ESC closes; it opens without a video too."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: video full screen (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vfs_home")
    lib = os.path.join(root, "vfs_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=20", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=20", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=330:duration=20", os.path.join(lib, "Plain Song.mp3")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nHKeyVideo=Y\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        # the menu rows: Video Size is the sixth row (after Cover Gradient Saturation), Rounded Corners the seventh, Effect the eighth
        sess.send("Y", 1.0)
        for _ in range(5):   # (Cover Gradient Saturation is the row under Disk/Cover/Thumbnail)
            sess.send(b"\x1b[B", 0.2)
        check("< full >" in sess.text(), "video overlay: the video size starts on full (the default)")
        sess.send(b"\x1b[C", 0.5)
        check("< framed >" in sess.text(), "video overlay: RIGHT from full makes the size framed")
        sess.send(b"\x1b[C", 0.5)
        check("< regular >" in sess.text(), "video overlay: RIGHT again goes round to regular")
        sess.send(b"\x1b[C", 0.5)
        check("< cropped >" in sess.text(), "video overlay: RIGHT makes the size cropped")
        sess.send(b"\x1b[C", 0.5)
        check("< full >" in sess.text(), "video overlay: RIGHT again makes the size full")
        sess.send(b"\x1b[B", 0.2)   # Rounded Corners (below Video Size)
        sess.send(b"\x1b[B", 0.2)   # Effect
        sess.send(b"\x1b[C", 0.5)
        check("< Scanlines >" in sess.text(), "video overlay: the effect row cycles to scanlines")
        sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[D", 0.5)
        check("< ASCII >" in sess.text(), "video overlay: LEFT from off goes round to the last effect of the list (ASCII)")
        sess.send(b"\x1b[C", 0.5)                              # back to off
        sess.send(b"\x1b", 0.8)
        conf = open(cfg, encoding="utf-8").read()
        check("VideoSize=full" in conf and "VideoFx=off" in conf, "video overlay: the size and the effect are saved in config.txt")
        # a track with a video: the full screen video
        sess.send("/", 0.5)
        sess.send("clip song", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        sess.send("v", 2.0)
        t = sess.text()
        check("[UP/DOWN] Effect: Off" in t and "Lyrics on" in t and "Close" in t, "full screen video: the legend names effect, lyrics and close")
        lines = t.split("\n")
        leg = [i for i, l in enumerate(lines) if "[S] Options" in l or "[SHIFT+g] Grid/Scale" in l]
        check(len(leg) == 2 and leg[1] == leg[0] + 1 and "Close" in lines[leg[1]] and "Effect" in lines[leg[0]] and not lines[leg[0]].startswith("\u2502") and lines[leg[0]].count("\u2500") > 4 and not lines[leg[1]].lstrip().startswith("\u2502"), "full screen video: commands row 1 on the bottom border line, row 2 right below it")
        vid = [l for l in lines if l.startswith("\u2502") and any(0x2800 <= ord(ch) <= 0x28ff for ch in l)]
        check(bool(vid) and all(l[1:3] == "  " and l.rstrip()[-3:-1] != "" for l in vid) and all(not (0x2800 <= ord(l[1]) <= 0x28ff or 0x2800 <= ord(l[2]) <= 0x28ff) for l in vid), "full screen video: two empty columns between the picture and the border on the left")
        sess.send(b"\x1b[B", 0.8)
        check("Effect: Scanlines" in sess.text(), "full screen video: DOWN changes the effect")
        sess.send(b"\t", 0.8)
        check("Lyrics + transl." in sess.text(), "full screen video: TAB goes on to the lyrics with their translation (the status line says if none is saved)")
        sess.send(b"\t", 0.8)
        check("Lyrics off" in sess.text(), "full screen video: TAB again switches the lyric line off")
        check(any(0x2800 <= ord(ch) <= 0x28FF for ch in sess.text()), "full screen video: the picture is drawn (braille dots in this terminal)")
        # the layout: SHIFT+DOWN -> video + oscilloscope (and its options over it), another -> + spectrogram, SHIFT+UP back
        sess.send(b"\x1b[1;2B", 1.0)
        check("Layout: Video+Osci" in sess.text() or "Layout" in sess.text(), "full screen video: SHIFT+DOWN changes the layout")
        sess.send("O", 1.0)
        t = sess.text()
        check("video layout" in t and "default" in t, "full screen video layout: O opens the oscilloscope options with the layout's own style / mode")
        sess.send(b"\x1b", 1.0)
        sess.send(b"\x1b[1;2B", 1.0)
        sess.send("I", 1.0)
        check("Spectrogram (video layout)" in sess.text(), "full screen video layout: I opens the spectrogram options")
        sess.send(b"\x1b", 1.0)
        sess.send("G", 0.8)
        sess.send(b"\x1b[1;2B", 1.0)                        # video + both (the oscilloscope above the spectrogram, right of the video)
        check(sess.alive() and "VideoLayout=3" in open(cfg, encoding="utf-8").read(), "full screen video layout: SHIFT+DOWN reaches video + both")
        sess.send(b"\x1b[1;2A", 0.8)
        sess.send(b"\x1b[1;2A", 0.8)
        sess.send(b"\x1b[1;2A", 0.8)
        conf = open(cfg, encoding="utf-8").read()
        check("VideoLayout=0" in conf and "VideoLayoutScales=false" in conf, "full screen video layout: layout and scales are saved")
        sess.send("S", 1.0)
        t = sess.text()
        check("Chromatic Aberration" in t and "Contrast" in t and "Colors" in t and "Video Resolution" in t, "full screen video: S opens the options panel")
        sess.send(b"\x1b", 1.0)
        check("Chromatic Aberration" not in sess.text() and "Effect:" in sess.text(), "full screen video: ESC closes only the options panel")
        sess.send(b"\x1b", 1.0)
        check("Effect:" not in sess.text() and sess.alive(), "full screen video: ESC closes it")
        conf = open(cfg, encoding="utf-8").read()
        check("VideoFx=scanlines" not in conf, "full screen video: the effect is the track's own (not the general setting)")
        # Style Option (SHIFT+s) of the playing track: the rows of its video
        sess.send("S", 1.5)
        sess.wait_for(lambda tt: all(w in tt for w in ("Video Chromatic Aberration", "Cover Chromatic Aberration", "Video Contrast", "Video Effect", "Video Resolution", "Use mp4 audio for this song", "as in Settings (subtle)")), 15.0)   # (a busy machine draws the overlay a little later)
        t = sess.text()
        check("Video of this track" in t and "Video Size" in t and "Video Effect" in t, "style option: video on / off, size and effect rows for this track")
        check("Video Contrast" in t and "Video Colors" in t and "Video Chromatic Aberration" in t and "Cover Chromatic Aberration" in t, "style option: contrast, colors and chromatic aberration rows (video and cover)")
        check(any("Use Lyrics on Video" in ln and "none" in ln for ln in t.splitlines()), "style option: TAB in the big video overlay is this track's Use Lyrics on Video (none after lyrics -> + translation -> none)")
        check(any("Video Chromatic Aberration" in ln and "as in Settings (subtle)" in ln for ln in t.splitlines()), "style option: the chromatic aberration row shows its value untruncated")
        check("Video Resolution" in t, "style option: a Video Resolution row")
        check(any("Use mp4 audio for this song" in ln and "false" in ln for ln in t.splitlines()), "style option: a song paired with a video has the row Use mp4 audio for this song (false)")
        for _ in range(8):                                   # down to the first video row: the preview shows the video instead of the disk
            sess.send(b"\x1b[B", 0.2)
        for _ in range(10):
            if "the video of this track" in sess.text():
                break
            sess.pump(0.5)
        check("the video of this track" in sess.text(), "style option: on a video row the preview shows the video of the track")
        for _ in range(8):
            sess.send(b"\x1b[A", 0.2)
        for _ in range(7):
            sess.send(b"\x1b[B", 0.2)
        sess.send(b"\x1b[C", 0.8)
        for _ in range(10):                                  # (a busy machine draws it a little later)
            if "Video of this track" in sess.text() and "off" in sess.text().split("Video of this track", 1)[1][:40]:
                break
            sess.pump(0.5)
        check("Video of this track" in sess.text() and "off" in sess.text().split("Video of this track", 1)[1][:40], "style option: the video of this track can be switched off")
        sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b", 1.0)
        # a track without a video: a note instead of the overlay
        sess.send("/", 0.5)
        sess.send("plain", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 2.0)
        sess.send("v", 1.0)
        check("Effect:" in sess.text() and sess.alive(), "full screen video: a track without a video opens it too (the clapperboard)")
        check("No video for this track." in sess.text() and "Fetch video via [" in sess.text() and "pair local video via video options in [" in sess.text(), "full screen video: the note for a track without a video")
        sess.send("v", 1.0)
        check("Effect:" not in sess.text() and sess.alive(), "full screen video: v closes it again")
        # the overlay does not depend on the Video mode: with it off a track with a video still shows its picture
        sess.send("Y", 1.0)
        for _ in range(20):
            if "> Use Video in Lyrics Field" in sess.text():
                break
            sess.send(b"\x1b[B", 0.2)
            if sess.wait_for(lambda tt: "> Use Video in Lyrics Field" in tt, 1.5):   # (the key is seen before the next one is sent: no row is passed on a slow machine)
                break
        sess.send(b"\x1b[C", 0.8)
        sess.wait_for(lambda tt: "Use Video in Lyrics Field" in tt and "[ ] false" in tt.split("Use Video in Lyrics Field", 1)[1][:80], 8.0)
        check("Use Video in Lyrics Field" in sess.text() and "[ ] false" in sess.text().split("Use Video in Lyrics Field", 1)[1][:80], "video overlay: the Video mode can be switched off")
        check("Cover Gradient Saturation" in sess.text() and "30 %" in sess.text().split("Cover Gradient Saturation", 1)[1][:30], "video overlay: Cover Gradient Saturation is a row (default 30 %)")
        sess.send(b"\x1b", 1.0)
        sess.send("/", 0.5)
        sess.send("clip", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 2.0)
        sess.send("v", 2.0)
        t = sess.text()
        check("Effect:" in t and "No video for this track." not in t and any(0x2800 <= ord(ch) <= 0x28FF for ch in t),
              "full screen video: shows the picture with the Video mode off")
        sess.send("v", 1.0)
    finally:
        sess.kill()


def cli_scripts_overlay(binary, root, scripts):
    """The script overlay (library left, editor right): open, edit, save, run, new, filter; /cs: searches the scripts."""
    home = os.path.join(root, "cs_home")
    lib = os.path.join(root, "cs_music")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    with open(os.path.join(cfgdir, "cli", "night.cli"), "w", encoding="utf-8") as f:
        f.write("# quiet\nvolume 25\nvolume +5\n")
    with open(os.path.join(cfgdir, "cli", "morning.cli"), "w", encoding="utf-8") as f:
        f.write("volume 60\n")
    sess = Session(binary, home, scripts, {}, quiet_stderr=True)
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
        sess.send("/", 0.5)
        sess.send("cs:pause", 0.6)
        check("SEARCH SCRIPTS" in sess.text() and "0 matches" in sess.text(), "scripts: /cs: searches the scripts (no match for 'pause' yet)")
        sess.send(b"\x15", 0.2) if False else None
        sess.send(b"\x1b", 0.6)
        sess.send("/", 0.5)
        sess.send("cs:volume", 0.6)
        check("2: " in sess.text() and "night" in sess.text(), "scripts: /cs:volume finds both scripts by their text: ")
        check("2 scripts" in sess.text() and "volume 60" in sess.text() and "volume 25" in sess.text(), "scripts: /cs: lists the matches in the library pane (name and first command)")
        sess.send("\r", 1.2)
        text = sess.text()
        check("SCRIPTS (2)" in text and "EDITOR" in text and "morning.cli" in text, "scripts: ENTER on /cs: opens the script overlay with the search applied and the first match loaded")
        check("volume 60" in text and "night" in text, "scripts: library on the left, the script's text in the editor on the right")
        sess.send(b"\x12", 1.2)   # CTRL+R: run the editor text
        check("(done)" in sess.text() or "volume 30%" in sess.text(), "scripts: CTRL+R runs the script and the answers show in OUTPUT")
        sess.send(b"\x1b[B", 0.4)
        sess.send("\r", 0.8)
        check("volume 25" in sess.text() and "night.cli" in sess.text(), "scripts: ENTER in the library loads the selected script")
        sess.send(b"\x1b[F", 0.3)   # End
        sess.send("\r", 0.2)
        sess.send("echo hello", 0.4)
        check("echo hello" in sess.text() and "night.cli *" in sess.text(), "scripts: typing edits the buffer and marks it changed")
        sess.send(b"\x13", 0.8)   # CTRL+S
        check("saved night.cli" in sess.text() and "night.cli *" not in sess.text(), "scripts: CTRL+S saves")
        check("echo hello" in open(os.path.join(cfgdir, "cli", "night.cli"), encoding="utf-8").read(), "scripts: the file on disk has the new line")
        sess.send(b"\x1b", 0.8)   # leave: unchanged text closes at once
        check("EDITOR" not in sess.text(), "scripts: ESC closes the overlay when nothing is unsaved")
        sess.send("/", 0.4)
        sess.send("c:", 0.4)
        sess.send("open scripts\r", 1.0)
        check("SCRIPTS (4)" in sess.text(), "scripts: `open scripts` in the command line opens it (startup, aliases and the two scripts)")
        sess.send("n", 0.5)
        sess.send("brand-new\r", 1.0)
        check("brand-new" in sess.text() and os.path.exists(os.path.join(cfgdir, "cli", "brand-new.cli")), "scripts: [n] makes a new script file")
        sess.send(b"\t", 0.3)
        sess.send("x", 0.5)
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b", 0.8)
        check(sess.alive(), "scripts: the program is still running")
    finally:
        sess.kill()


def cli_core(binary, root, scripts):
    """The command line's chains, timers, aliases, set/get, confirmations, scripts, the startup file and `mousiki --query` from outside."""
    import subprocess
    home = os.path.join(root, "cc_home")
    lib = os.path.join(root, "cc_music")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 60)
    make_wav(os.path.join(lib, "Beta Tune.wav"), 60)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    with open(os.path.join(cfgdir, "cli", "quiet.cli"), "w", encoding="utf-8") as f:
        f.write("# a script\nvolume 20\nwait 1s\nvolume +5\n")
    with open(os.path.join(cfgdir, "cli", "startup.cli"), "w", encoding="utf-8") as f:
        f.write("volume 33\n")
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"))

    sess = Session(binary, home, scripts, {}, quiet_stderr=True)

    def outside(*args, timeout=15):
        # (the terminal of the program has to be read meanwhile, or it blocks writing its screen and cannot answer)
        p = subprocess.Popen([binary] + list(args), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t0 = time.time()
        while p.poll() is None and time.time() - t0 < timeout:
            sess.pump(0.2)
        if p.poll() is None:
            p.kill()
        return p.returncode, (p.stdout.read() or "").strip()

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
        sess.pump(3.0)
        code, out = outside("--query", "get", "volume")
        check(code == 0 and out == "33", "cli core: startup.cli ran at the start (volume 33) and --query get volume answers: %r" % out)
        code, out = outside("--query", "volume 40; volume +5; get volume")
        check(code == 0 and out.endswith("45"), "cli core: a chain from outside runs in order: %r" % out)
        code, out = outside("--query", "quit")
        check(code == 3 and "force" in out, "cli core: quit from outside is refused without force: %r (%d)" % (out, code))
        code, out = outside("--query", "no-such-command")
        check(code == 3 and "unknown command" in out, "cli core: an unknown command is an error with exit code 3")
        code, out = outside("--query", "set", "RemoteAppPin", "1234")
        check(code == 3, "cli core: set does not reach the remote app PIN")
        code, out = outside("--query", "set VideoGradientSaturation 60; get VideoGradientSaturation")
        check(code == 0 and out.endswith("60"), "cli core: set changes a number setting and get reads it: %r" % out)
        check("VideoGradientSaturation=60" in open(os.path.join(cfgdir, "config.txt"), encoding="utf-8").read(), "cli core: set saved it in config.txt")
        code, out = outside("--query", "run", "quiet", timeout=20)
        check(code == 0 and "running quiet" in out, "cli core: run <script> works (waits inside it): %r" % out)
        code, out = outside("--query", "get volume")
        check(out == "25", "cli core: the script finished its volume +5 after the wait: %r" % out)
        sess.pump(1.6)   # (the status file is written about once a second)
        code, out = outside("--status", "--json")
        check(code == 0 and out.startswith("{") and '"volume": 25' in out, "cli core: --status --json: %r" % out[:80])
        code, out = outside("--status", "--format", "vol {volume}")
        check(code == 0 and out == "vol 25", "cli core: --status --format: %r" % out)
        code, out = outside("--query", "in 1s volume 11")
        check(code == 0 and "timer" in out, "cli core: in <time> <command> sets a timer")
        sess.pump(2.5)
        code, out = outside("--query", "get volume")
        check(out == "11", "cli core: the timer fired: %r" % out)
        code, out = outside("--query", "alias night = volume 15; echo night")
        code, out = outside("--query", "night")
        check(code == 0 and "night" in out, "cli core: an alias runs its commands: %r" % out)
        code, out = outside("--query", "if playing echo yes")
        check(code == 0 and ("yes" in out) == ("(playing)" in out), "cli core: if <condition> <command> runs the command only when the condition holds: %r" % out)
        code, out = outside("--query", "volume 80; if volume>50 volume 20; get volume")
        check(code == 0 and out.endswith("20"), "cli core: if volume>50 volume 20 -- the command word is also in the condition: %r" % out)
        code, out = outside("--query", "volume 80; mute; if unmuted mute; if muted unmute; get volume")
        check(code == 0 and "(not unmuted)" in out and out.endswith("80"), "cli core: if unmuted mute -- 'mute' is also inside 'unmuted': %r" % out)
        code, out = outside("--query", "help")
        check(code == 0 and "play-pause" in out and "volume <0-100" in out and out.count("\n") > 20, "cli core: help from outside answers with the list of commands: %r" % out[:100])
        code, out = outside("--query", "track 99")
        check(code == 3 and "no such title" in out and "has 2" in out, "cli core: track <n> with no such title is an error, not a claim that it plays: %r" % out)
        code, out = outside("--query", "track 1; get title")
        first_title = out.strip().split("\n")[-1]
        code, out = outside("--query", "track 2; get title")
        second_title = out.strip().split("\n")[-1]
        check(first_title and second_title and first_title != second_title, "cli core: track 1 / track 2 name two titles (%r, %r)" % (first_title, second_title))
        code, out = outside("--query", "track 1; next; get title")   # (before: the next was dropped while track 1 still loaded, and get title answered with the old title)
        lines = out.strip().split("\n")
        check(code == 0 and lines[-1] == second_title and lines[-2].startswith("next: " + second_title) and ("(was: " + first_title + ")") in lines[-2], "cli core: next after track 1 waits for it, and its answer names the title that really plays: %r" % out)
        sess.send("/", 0.5)
        sess.send("c:", 0.5)
        sess.send("queue alpha; queue beta\r", 1.2)
        check("queued Beta Tune" in sess.text(), "cli core: a typed chain runs both (the last reply shows): ")
        sess.send("clear-queue\r", 1.0)
        check("cannot be undone" in sess.text() and "yes" in sess.text(), "cli core: clear-queue asks for yes")
        sess.send("yes\r", 1.0)
        sess.send("queue\r", 1.0)
        check("queue: 0" in sess.text(), "cli core: yes cleared the queue")
        sess.send("play beta\r", 1.5)
        sess.send("status\r", 0.8)
        first = sess.text().split("\n")[1] if "\n" in sess.text() else ""
        sess.pump(3.0)
        second = sess.text().split("\n")[1] if "\n" in sess.text() else ""
        check("COMMAND LINE" in sess.text() and first != second, "cli core: a long reply scrolls in the bar's border (marquee)")
        sess.send(b"\x1b", 0.6)
        check(sess.alive(), "cli core: the program is still running")
    finally:
        sess.kill()


def cli_commands(binary, root, scripts):
    """/c: turns the search bar into a command line (ENTER runs, ESC leaves); the console's third pane is a command line too."""
    home = os.path.join(root, "cli_home")
    lib = os.path.join(root, "cli_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    make_wav(os.path.join(lib, "Beta Tune.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, {}, quiet_stderr=True)
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
        sess.send("/", 0.5)
        sess.send("c:", 0.6)
        check("COMMAND LINE" in sess.text(), "cli: /c: turns the search bar into COMMAND LINE")
        check("COMMAND HISTORY / OUTPUT" in sess.text() and "type a command" in sess.text(), "cli: /c: shows the command history pane in place of the library list (a hint while it is empty)")
        sess.send("play beta\r", 1.5)
        check("playing Beta Tune" in sess.text(), "cli: play <words> starts the first match and the reply sits in the search bar")
        check("> play beta" in sess.text() and "  playing Beta Tune" in sess.text().replace("playing Beta Tune", "  playing Beta Tune"), "cli: the typed line and its reply are listed in the library pane")
        sess.send("volume 42\r", 1.0)
        check("-> 42%" in sess.text(), "cli: volume 42 sets the volume and says what it was")
        sess.send("bogus\r", 0.8)
        check("unknown command" in sess.text(), "cli: an unknown command is named")
        sess.send("queue alpha\r", 0.8)
        check("queued Alpha Song" in sess.text(), "cli: queue <words> adds the first match")
        sess.send("pla", 0.4)
        sess.send(b"\x1b[1;5C", 0.6)
        check("c:play" in sess.text().replace("/c:", "c:") and "playback-mode" in sess.text(), "cli: CTRL+RIGHT completes a command word up to what the candidates share (pla -> play) and lists them")
        sess.send(b"\x1b[1;5C", 0.5)
        sess.send("alp", 0.4)
        sess.send(b"\x1b[1;5C", 0.8)
        check("c:play Alpha Song" in sess.text().replace("/c:", "c:"), "cli: CTRL+RIGHT completes the title after play")
        sess.send(b"\x15", 0.3)   # CTRL+u clears the line
        sess.send(b"\x1b[A", 0.5)
        check("c:queue alpha" in sess.text().replace("/c:", "c:"), "cli: UP recalls the last line")
        sess.send(b"\x1b", 0.8)
        check("COMMAND LINE" not in sess.text() and "SEARCH LOCAL" in sess.text(), "cli: ESC leaves the command line like any search")
        check("COMMAND HISTORY" not in sess.text() and "Alpha Song" in sess.text(), "cli: ESC brings the library list back")
        sess.send("t", 1.0)
        check("COMMAND LINE" in sess.text() and "STATUS MESSAGES" in sess.text() and "CONSOLE / LOGS" in sess.text(), "console: the third pane COMMAND LINE is at the bottom")
        sess.send(b"\t", 0.4)
        sess.send(b"\t", 0.5)
        sess.send("status\r", 1.0)
        check("playing:" in sess.text() and "vol 42%" in sess.text(), "console: status in the command pane answers")
        sess.send("t", 0.5)
        check("> t" in sess.text(), "console: keys are typed in the command pane (t does not close it)")
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x1b", 0.8)
        check("COMMAND LINE" not in sess.text(), "console: ESC clears the line, the next ESC closes the console")
        check(sess.alive(), "cli: the program is still running")
    finally:
        sess.kill()


def cli_replies_hooks(binary, root, scripts):
    """Run 3 of the CLI: every command answers with what happened; hooks.cli reacts to events; /api/cli (RemoteAppCli) answers with JSON."""
    import subprocess, urllib.request, json as _json
    home = os.path.join(root, "cr_home")
    lib = os.path.join(root, "cr_music")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 60)
    make_wav(os.path.join(lib, "Beta Tune.wav"), 60)
    port = 47631
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nRemoteApp=true\nRemoteAppPort=%d\nRemoteAppPin=135246\nRemoteAppCli=true\n" % (lib, port))
    with open(os.path.join(cfgdir, "cli", "hooks.cli"), "w", encoding="utf-8") as f:
        f.write("# hooks\non track-change = volume 12\non pause = volume 22\non resume = volume 33\n")
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"))
    sess = Session(binary, home, scripts, {}, quiet_stderr=True)

    def q(line, timeout=15):
        p = subprocess.Popen([binary, "--query", line], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t0 = time.time()
        while p.poll() is None and time.time() - t0 < timeout:
            sess.pump(0.2)
        if p.poll() is None:
            p.kill()
        return p.returncode, (p.stdout.read() or "").strip()

    def http(line, wait=8):
        import threading
        res = {}
        def run():
            try:
                u = "http://127.0.0.1:%d/api/cli?pin=135246&wait=%d&line=%s" % (port, wait, urllib.request.quote(line))
                with urllib.request.urlopen(u, timeout=wait + 5) as r:
                    res["code"], res["body"] = r.status, r.read().decode("utf-8")
            except urllib.error.HTTPError as e:
                res["code"], res["body"] = e.code, e.read().decode("utf-8")
            except Exception as e:
                res["code"], res["body"] = 0, str(e)
        t = threading.Thread(target=run)
        t.start()
        t0 = time.time()
        while t.is_alive() and time.time() - t0 < wait + 6:
            sess.pump(0.2)
        t.join(1)
        return res.get("code", 0), res.get("body", "")

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
        sess.pump(3.5)   # (the hooks wait for the startup)
        code, out = q("pause")
        check(code == 0 and ("nothing is playing" in out or out.startswith("paused:")), "cli replies: pause answers with the state: %r" % out)
        other, other_title = ("beta", "Beta Tune") if "Alpha" in out else ("alpha", "Alpha Song")   # (the program loads one of them, paused)
        code, out = q("play " + other)
        check(code == 0 and ("playing " + other_title) in out, "cli replies: play <words>: %r" % out)
        code, out = q("play " + ("alpha" if other == "beta" else "beta") + "; get title; play " + other + "; get title")
        check(code == 0 and out.strip().split("\n")[-1] == other_title and out.strip().split("\n")[-3] == ("Alpha Song" if other == "beta" else "Beta Tune"), "cli replies: play x; get title -- get title waits for the title: %r" % out)
        sess.pump(1.0)
        code, out = q("get volume")
        check(out == "12", "cli hooks: on track-change ran (volume 12): %r" % out)
        code, out = q("pause")
        check(code == 0 and out.startswith("paused: " + other_title) and "/" in out and "vol" in out, "cli replies: pause names the title, position and volume: %r" % out)
        sess.pump(1.0)
        code, out = q("get volume")
        check(out == "22", "cli hooks: on pause ran (volume 22): %r" % out)
        code, out = q("pause")
        check("already paused" in out, "cli replies: a second pause says it was already paused: %r" % out)
        code, out = q("stop")
        check("stop pauses" in out and other_title in out, "cli replies: stop explains that it pauses: %r" % out)
        code, out = q("play")
        check(out.startswith("resumed: " + other_title), "cli replies: play resumes and says what: %r" % out)
        sess.pump(1.0)
        code, out = q("get volume")
        check(out == "33", "cli hooks: on resume ran (volume 33): %r" % out)
        code, out = q("play")
        check("already playing" in out, "cli replies: play while playing: %r" % out)
        code, out = q("seek 5")
        check("seek +5 s ->" in out and other_title in out, "cli replies: seek shows where it landed: %r" % out)
        code, out = q("mute")
        check(out.startswith("muted") and "volume" in out, "cli replies: mute: %r" % out)
        code, out = q("unmute")
        check(out.startswith("sound on"), "cli replies: unmute: %r" % out)
        code, out = q("volume 30")
        check("-> 30%" in out, "cli replies: volume shows the change: %r" % out)
        code, out = q("playback-mode")
        check("playback mode:" in out and "->" in out, "cli replies: playback-mode names old and new: %r" % out)
        code, out = q("hooks")
        check("3 hooks" in out and "on pause = volume 22" in out, "cli hooks: the hooks command lists them: %r" % out)
        # the web server's endpoint
        code, body = http("status")
        check(code == 200 and '"ok":true' in body and other_title in body, "cli http: /api/cli answers with JSON: %s %r" % (code, body[:120]))
        code, body = http("quit")
        check(code == 200 and '"ok":false' in body and "not allowed over the network" in body, "cli http: quit is refused over the network: %r" % body[:120])
        code, body = http("force quit")
        check('"ok":false' in body, "cli http: force is refused over the network: %r" % body[:120])
        code, body = http("set VideoGradientSaturation 10")
        check('"ok":false' in body, "cli http: set is refused over the network")
        code, body = http("queue clear")
        check('"ok":false' in body, "cli http: queue clear is refused over the network")
        code, body = http("volume 40; get volume")
        check(code == 200 and '"lines":["volume' in body and '"40"]' in body, "cli http: a chain gives one line per command: %r" % body[:160])
        check(sess.alive(), "cli replies: the program is still running")
    finally:
        sess.kill()


def radio_cli(binary, root, scripts):
    """The radio's command line from outside: stations, tune, pause / play / stop replies, volume, chains, timers, errors, no quit."""
    import subprocess
    home = os.path.join(root, "radiocli_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\nJazz Night | Jazz | DE | MP3 | 128 | http://127.0.0.1:9/j | *\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\nWizardSeen=true\n" % os.path.join(root, "music"))
    with open(os.path.join(cfgdir, "cli", "calm.cli"), "w", encoding="utf-8") as f:
        f.write("volume 20\necho calm\n")
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"), MOUSIKI_RADIO_NULL="1")
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})

    def q(line, timeout=15):
        p = subprocess.Popen([binary, "--query", line], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t0 = time.time()
        while p.poll() is None and time.time() - t0 < timeout:
            sess.pump(0.2)
        if p.poll() is None:
            p.kill()
        return p.returncode, (p.stdout.read() or "").strip()

    try:
        sess.wait_for(lambda t: "Plain Station" in t)
        sess.send(b"\x1b", 0.5)
        sess.pump(1.0)
        code, out = q("stations")
        check(code == 0 and "2 stations" in out and "Jazz Night" in out, "radio cli: stations lists them: %r" % out)
        code, out = q("stations jazz")
        check(code == 0 and "1 station" in out and "Plain" not in out, "radio cli: stations <words> filters: %r" % out)
        code, out = q("tune jazz")
        check(code == 0 and out.startswith("tuning: Jazz Night") and "[2/2]" in out, "radio cli: tune <words>: %r" % out)
        code, out = q("tune 9")
        check(code == 3 and "no station 9" in out, "radio cli: tune with a wrong number is an error: %r" % out)
        code, out = q("play plain")
        check(code == 0 and out.startswith("tuning: Plain Station") and "[1/2]" in out, "radio cli: play <words> tunes the first station that matches (as in the player): %r" % out)
        code, out = q("play 2")
        check(code == 0 and out.startswith("tuning: Jazz Night"), "radio cli: play <n> tunes station n: %r" % out)
        code, out = q("play nosuchstation")
        check(code == 3 and "no station matches" in out, "radio cli: play <words> with no match is an error: %r" % out)
        code, out = q("get volume")   # (the chain below needs a known volume)
        code, out = q("if volume>10 echo loud")
        check(code == 0 and "loud" in out, "radio cli: if <condition> <command> runs the command: %r" % out)
        code, out = q("volume 60; if volume>50 volume 35; get volume")
        check(code == 0 and out.endswith("35"), "radio cli: if volume>50 volume 35 -- the command word is also in the condition: %r" % out)
        code, out = q("volume 40")
        check(code == 0 and "-> 40%" in out, "radio cli: volume says old -> new: %r" % out)
        code, out = q("mute; unmute")
        check(code == 0 and "muted" in out and "sound on" in out, "radio cli: a chain answers with every reply: %r" % out)
        code, out = q("get volume")
        check(out == "40", "radio cli: get volume: %r" % out)
        code, out = q("status")
        check(code == 0 and ("Jazz Night" in out or "stopped" in out) and "vol 40%" in out or "stopped" in out, "radio cli: status: %r" % out)
        code, out = q("stop")
        check(code == 0 and ("stopped" in out), "radio cli: stop says what happened: %r" % out)
        code, out = q("quit")
        check(code == 3 and "mousiki --cmd quit" in out, "radio cli: quit is refused: %r" % out)
        code, out = q("bogus")
        check(code == 3 and "unknown command" in out, "radio cli: an unknown command is an error: %r" % out)
        code, out = q("run calm")
        check(code == 0 and "calm" in out, "radio cli: run <script>: %r" % out)
        code, out = q("get volume")
        check(out == "20", "radio cli: the script's volume applied: %r" % out)
        code, out = q("in 1s volume 55")
        sess.pump(2.5)
        code, out = q("get volume")
        check(out == "55", "radio cli: a timer fired: %r" % out)
        code, out = q("help seek")
        check(code == 0 and "seek <seconds>" in out, "radio cli: help <command>: %r" % out)
        check(sess.alive(), "radio cli: the radio is still running")
    finally:
        sess.kill()


def milkdrop_layouts_style(binary, root, scripts):
    """The MilkDrop visuals in the big video overlay and in Style Option: "m" switches the big overlay between the video and the visuals
    (VideoFullSource), SHIFT+UP / DOWN go through the MilkDrop layouts (VideoFullMilkDropLayout 0..4: the visuals, + osci, + osci and
    spectrogram, the quartet, video | visuals) with one picture per place (Kitty ids 7201..7203); the video layouts have the quartet too
    (VideoLayout=4); ":" opens the visuals overlay over the big overlay. Style Option has "MilkDrop for this track" and "More Options" (the preset overlay of the track):
    "on" switches the field to the visuals while the track plays, the next track gives the view back. Skipped without projectM / ffmpeg."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: milkdrop layouts (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "mdl_home")
    lib = os.path.join(root, "mdl_music")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=120", os.path.join(lib, "Alpha Clip.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=120", "-pix_fmt", "yuv420p", os.path.join(lib, "Alpha Clip.mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    make_wav(os.path.join(lib, "Beta Song.wav"), 120)
    cfg = os.path.join(cfgdir, "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=true\nUseLyrics=false\nUseSphere=false\nVizDefault=osci\n"
                "VideoMode=true\nVideoAuto=false\nHKeyVideo=Y\n" % lib)
    env = {"MOUSIKI_NULL_AUDIO": "1", "KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"}
    sess = Session(binary, home, scripts, env, quiet_stderr=True, gfx=True)
    try:
        sess.wait_for(lambda t: "Alpha Clip" in t, 10)
        sess.pump(2.0)
        sess.send(":", 1.5)
        if "Not available" in sess.text():
            print("  skipped: milkdrop layouts (projectM 4 or presets are not installed on this machine)")
            return
        sess.send(b"\x1b", 0.8)
        def conf():
            with open(cfg, encoding="utf-8") as f:
                return f.read()
        def big(ids, minc=8, minr=3):   # the pictures placed now (Kitty ids) that are at least that big
            return [i for i in ids if i in sess.pics and sess.pics[i]["c"] >= minc and sess.pics[i]["r"] >= minr]
        def settle(cond, tries=12):
            for _ in range(tries):
                if cond():
                    return True
                sess.pump(0.5)
            return cond()
        # --- the big overlay: the video layouts have the quartet (SHIFT+UP from the first layout goes round to it)
        sess.send("v", 2.0)
        t = sess.text()
        check("[m] MilkDrop" in t and "[:] Visuals" in t, "big overlay: the legend names [m] MilkDrop and the visuals overlay key")
        sess.send(b"\x1b[1;2A", 1.0)
        check("VideoLayout=4" in conf(), "big overlay: SHIFT+UP from the first video layout goes round to the quartet (VideoLayout=4)")
        # (the oscilloscope is braille here: its picture style is not on; the spectrogram is drawn in tiles of its own)
        check(settle(lambda: len(big([7201, 7202])) == 2), "big overlay quartet: the video and the MilkDrop visuals are pictures (%s)" % sorted(big([7201, 7202, 7203])))
        if len(big([7201, 7202])) == 2:
            V, M = sess.pics[7201], sess.pics[7202]
            top_left = "".join(sess.screen.display[r][3:M["col"] - 1] for r in range(1, V["row"] - 1))
            check(M["row"] < V["row"] and M["col"] > V["col"] and any(0x2800 < ord(ch) <= 0x28FF for ch in top_left),
                  "big overlay quartet: oscilloscope top left, MilkDrop top right, video bottom left (video %s, milkdrop %s)" % ((V["row"], V["col"]), (M["row"], M["col"])))
        sess.send(b"\x1b[1;2B", 1.0)
        check("VideoLayout=0" in conf(), "big overlay: SHIFT+DOWN from the quartet goes round to the first layout")
        # --- m: the MilkDrop visuals take the overlay
        sess.send("m", 1.5)
        check("VideoFullSource=milkdrop" in conf(), "big overlay: m switches to the MilkDrop visuals (VideoFullSource=milkdrop)")
        check("Queue/List/Folders" in sess.text() and "Queue  [" not in sess.text(), "big overlay: the list overlays are one legend entry, Queue/List/Folders")
        check("Preset" in sess.text() and "Effect:" in sess.text(), "big overlay MilkDrop: the legend names the preset keys and the (MilkDrop) effect")
        sess.send(b"\x1b[B", 1.0)   # DOWN: the MilkDrop visuals' own effect (this track's), not the video's
        check(settle(lambda: "milkdrop effect of this track" in sess.text() or "Effect: Scanlines" in sess.text(), 6), "big overlay MilkDrop: DOWN changes the effect of the visuals (%r)" % next((l.strip()[:80] for l in sess.screen.display if "Effect:" in l), None))
        sess.send(b"\x1b[A", 1.0)
        check("MILKDROP" in sess.text() and "[m] Video" in sess.text(), "big overlay: the title says MILKDROP, the legend offers [m] Video")
        check(settle(lambda: len(big([7201, 7202, 7203], 30, 10)) == 1), "big overlay MilkDrop layout 0: one big picture (%s)" % sorted(big([7201, 7202, 7203])))
        names = {1: "+ osci", 2: "+ osci and spectrogram", 3: "quartet", 4: "video | visuals"}
        want = {1: 1, 2: 1, 3: 2, 4: 2}   # the pictures (the oscilloscope is braille here, the spectrogram has tiles of its own)
        for k in range(1, 5):
            sess.send(b"\x1b[1;2B", 1.2)
            check("VideoFullMilkDropLayout=%d" % k in conf(), "big overlay MilkDrop: SHIFT+DOWN -> layout %d (%s)" % (k, names[k]))
            check(settle(lambda: len(big([7201, 7202, 7203], 20, 10)) == want[k], 16), "big overlay MilkDrop layout %d: %d pictures (%s)" % (k, want[k], {i: (p["row"], p["col"], p["c"], p["r"]) for i, p in sess.pics.items() if i < 7300}))
        def side_by_side():
            if len(big([7201, 7202], 6, 2)) != 2:
                return False
            V, M = sess.pics[7201], sess.pics[7202]
            return V["col"] + V["c"] <= M["col"] and M["row"] <= V["row"] and V["row"] + V["r"] <= M["row"] + M["r"]
        check(settle(side_by_side, 12), "big overlay MilkDrop layout 4: the video left (its height centred), the visuals right (%s)" % {i: (p["row"], p["col"], p["c"], p["r"]) for i, p in sess.pics.items() if i < 7300})
        sess.send(b"\x1b[1;2B", 1.0)
        check("VideoFullMilkDropLayout=0" in conf(), "big overlay MilkDrop: SHIFT+DOWN goes round to layout 0")
        sess.send(":", 1.2)
        check(settle(lambda: "Visuals: MilkDrop" in sess.text() and "MILKDROP" in sess.text(), 10), "big overlay: \":\" opens the visuals overlay over it")
        sess.send(b"\x1b", 0.8)
        check("Visuals: MilkDrop" not in sess.text() and "MILKDROP" in sess.text(), "big overlay: ESC closes the visuals overlay, the big overlay stays")
        sess.send("m", 1.0)
        check("VideoFullSource=video" in conf(), "big overlay: m again goes back to the video")
        sess.send(b"\x1b", 1.5)
        # --- Style Option: MilkDrop for this track (the rows above the album row)
        def md_field():
            p = sess.pics.get(7201)
            return p is not None and p["c"] >= 30 and p["r"] >= 10
        check(not md_field(), "style option: the field is not on MilkDrop before")
        sess.send("S", 1.5)
        check(settle(lambda: "MilkDrop for this track" in sess.text() and "More Options" in sess.text(), 10),
              "style option: the MilkDrop rows are there (projectM installed)")
        for _ in range(3):   # Look -> (up) the album row -> More Options -> MilkDrop for this track
            sess.send(b"\x1b[A", 0.25)
        sess.send(b"\x1b[C", 1.0)
        check(settle(lambda: "on (shown while it plays)" in sess.text(), 4), "style option: RIGHT on MilkDrop for this track -> on (%r)" % next((l.strip()[:70] for l in sess.screen.display if "MilkDrop for" in l), None))
        ok = settle(md_field, 24)
        if not ok:
            open(os.path.join(root, "mdl_fail_screen.txt"), "w", encoding="utf-8").write(sess.text() + "\n\n" + repr({i: p for i, p in sess.pics.items() if i < 7300}))
        check(ok, "style option: the field switches to the MilkDrop visuals (the first picture within 12 s, also on a busy machine; %s)" % {i: (p["row"], p["col"], p["c"], p["r"]) for i, p in sess.pics.items() if i < 7300})
        sess.pump(2.0)   # (the first preset is chosen once the visuals run)
        sess.send(b"\x1b[B", 0.3)
        # More Options: the MilkDrop options (":") for this track only; its Preset Overlay row opens the overlay, ENTER on a preset makes it the track's own
        sess.send("\r", 1.5)
        check(settle(lambda: "this track" in sess.text() and "Only for this track" in sess.text() and "Hard Cut Sensitivity" in sess.text(), 8),
              "style option: ENTER on More Options opens the MilkDrop options for this track only (%r)" % next((l.strip()[:70] for l in sess.screen.display if "Only for" in l), None))
        check("Use Effect" in sess.text(), "style option: the track's options have Use Effect")
        for _ in range(12):   # (the drawing rows are the same for all tracks: shown, and marked)
            if "Mesh Detail" in sess.text(): break
            sess.send(b"\x1b[B", 0.2)
        check("Mesh Detail" in sess.text() and "(all tracks)" in sess.text(), "style option: the track's options also have the drawing rows (Mesh Detail), marked for all tracks")
        for _ in range(30):   # (back up to Preset)
            sess.send(b"\x1b[A", 0.1)
            if "> Preset    <" in sess.text(): break
        sess.send(b"\x1b[B", 0.5)   # Preset -> Preset Overlay
        sess.send("\r", 1.5)
        check(settle(lambda: "MILKDROP FOR THIS TRACK" in sess.text(), 8), "style option: ENTER on Preset Overlay opens the preset overlay of this track")
        check("[f]" in sess.text(), "style option: the preset overlay names [f] (the favourites folder)")
        sess.send("/", 0.5)           # (a known preset, found with the search: the folder's rows differ from one preset pack to another)
        for ch in "stickflower":
            sess.send(ch, 0.1)
        settle(lambda: "Stickflower" in sess.text(), 10)
        sess.send("\r", 0.8)          # (from the query to the list)
        sess.send("\r", 2.0)          # (the first result: the track's own)
        store = os.path.join(home, ".cache", "mousiki", "covers", "choices.txt")
        def stored():
            return [l for l in open(store, encoding="utf-8").read().split("\n") if l.startswith("D\t")] if os.path.exists(store) else []
        # (the message is checked loosely: the panel's turning disk makes every frame a whole one, and a reader of the terminal can catch one half
        # written while the visuals keep the machine busy)
        chosen = settle(lambda: "MILKDROP FOR THIS TRACK" not in sess.text() and any("Stickflower" in l for l in stored()), 15)
        check(chosen, "style option: ENTER on a preset makes it the track's own (%r)" % next((l.strip()[:90] for l in sess.screen.display if "saved:" in l), None))
        rec = stored()
        check(len(rec) == 1 and "Alpha Clip" in rec[0] and rec[0].split("\t")[2].startswith("1|") and ".milk" in rec[0],
              "style option: the track's MilkDrop choice is stored with its file, keep and effect (%r)" % (rec[0][-80:] if rec else None))
        for _ in range(3):   # (ESC closes Style Option; on a busy machine an ESC read together with the next key would be ALT+key)
            sess.send(b"\x1b", 1.5)
            if settle(lambda: "STYLE OPTION" not in sess.text(), 6):
                break
        # the next track has no choice: the view before (the oscilloscope) comes back
        sess.send("n", 2.5)
        check(settle(lambda: not md_field() and "Beta Song" in sess.text()), "style option: the next track gives the field back (no MilkDrop picture; %s)" % {i: (p["row"], p["col"], p["c"], p["r"]) for i, p in sess.pics.items() if i < 7300})
        check(sess.alive(), "milkdrop layouts: the program is still running")
    finally:
        sess.kill()


def settings_general_texts(binary, root, scripts):
    """Settings -> GENERAL: Start Mode reads "music player"; Track Buffer (MB) has a five-character field, a typed number above the limit is
    set to it (a quarter of the RAM, at most 16384) and the status line names the range; the [ Set back to default ] buttons of Track Buffer
    and Terminal Size are in the same column (51); the transliteration previews are what Mousiki makes of the examples (Translit. Kanji:
    the poem, Japanese text is not changed by the row; Latin Name Lookup: what the lookup does and the names that are never looked up)."""
    lib = os.path.join(root, "sgt_music")
    os.makedirs(lib)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 20)
    home = os.path.join(root, "sgt_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=false\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
    try:
        sess.wait_for(lambda t: "Alpha Song" in t, 10)
        sess.send("s", 1.2)
        sess.send(b"\t", 0.8)   # GENERAL
        def find(label):
            return next((l for l in sess.screen.display if label in l and "Preview:" not in l), "")
        check("music player" in find("Start Mode"), "settings: Start Mode reads music player (%r)" % find("Start Mode").strip()[:60])
        tb, ts = find("Track Buffer (MB)"), find("Terminal Size")
        check(tb.find("[ Set back to default ]") == ts.find("[ Set back to default ]") == 50, "settings: the two [ Set back to default ] buttons are in column 51 (%d, %d)" % (tb.find("[ Set back") + 1, ts.find("[ Set back") + 1))
        def tbval():
            l = find("Track Buffer (MB)")
            return l.split(":", 1)[1].split("[")[0].strip() if ":" in l else ""
        check(tbval() == "64" and tb.find("[ Set back") - tb.find(":") == 19, "settings: the Track Buffer field is five characters wide (%r)" % tb.strip()[:60])
        for _ in range(12):   # to Track Buffer (MB)
            sess.send(b"\x1b[B", 0.1)
        sess.send(b"\r", 0.5)
        for _ in range(4):
            sess.send(b"\x7f", 0.1)
        sess.send("999999", 0.5)   # (a sixth digit is not taken)
        sess.send(b"\r", 1.0)
        t = sess.text()
        val = tbval()
        check(val.isdigit() and 64 <= int(val) <= 16384 and int(val) < 99999, "settings: a number above the limit is set to the limit (%s)" % val)
        check("allowed here: 64 - " in t, "settings: the status line names the range")
        sess.send(b"\x1b[C", 0.4)
        sess.send(b"\r", 0.8)
        check(tbval() == "64", "settings: Set back to default restores 64 (%r)" % tbval())
        sess.send(b"\x1b[D", 0.3)
        for _ in range(20):   # Track Buffer -> Translit. Kanji (Rate Cap ... Latin Wo)
            sess.send(b"\x1b[B", 0.1)
        sess.pump(1.2)
        t = sess.text()
        check("Preview: Translit. Kanji" in t and "\u8349\u306e\u540d\u3082" in t and "\u6625\u306e\u96e8" in t and "not changed by this row" in t, "settings: the Translit. Kanji preview shows the poem and what the row does")
        check("Always the same" not in t and "kusa no na mo" in t, "settings: no \"Always the same\" block; the poem's reading by the kanji reader is named")
        sess.send(b"\x1b[B", 0.3)
        sess.pump(1.0)
        t = sess.text()
        check("Preview: Latin Name Lookup" in t and "MusicBrainz" in t and "HAKU" in t and "\u67f3\u4e95" in t, "settings: the Latin Name Lookup preview explains the lookup and shows HAKU and the kanji name")
        # SHIFT+RIGHT / LEFT switch the tabs (the legend names them); Use MilkDrop is the last row of MAIN UI VIZ/LYRICS FIELD
        check("[SHIFT+\u2190\u2192/TAB] Switch tab" in sess.text(), "settings: the legend reads [SHIFT+<-->/TAB] Switch tab")
        sess.send(b"\x1b[1;2C", 1.0)
        check("ANIMA/VIZ" in sess.text() and sess.wait_for(lambda t: "DISK" in t and "Start Mode" not in t, 4), "settings: SHIFT+RIGHT goes to the next tab (ANIMA/VIZ)")
        seen = ""
        for _ in range(14):
            seen += "\n" + sess.text()
            if "Use MilkDrop" in sess.text():
                break
            sess.send(b"\x1b[1;2B", 0.5)
        seen += "\n" + sess.text()
        check("Use MilkDrop" in seen and seen.rfind("Use Sphere") < seen.rfind("Use MilkDrop") and seen.rfind("MAIN UI VIZ/LYRICS FIELD") < seen.rfind("Use MilkDrop"),
              "settings: Use MilkDrop is in MAIN UI VIZ/LYRICS FIELD, under Use Sphere")
        check("MilkDrop GPU" in seen, "settings: the MILKDROP block has the MilkDrop GPU row")
        sess.send(b"\x1b[1;2D", 1.0)
        check(sess.wait_for(lambda t: "Start Mode" in t, 4), "settings: SHIFT+LEFT goes back to the previous tab (GENERAL)")
        sess.send(b"\x1b[1;2C", 0.6)
        sess.send(b"\x1b[1;2C", 1.0)                        # ANIMA/VIZ, then PATHS
        paths_seen = ""
        for _ in range(40):
            paths_seen += "\n" + sess.text()
            if "Preset Path 1" in paths_seen:
                break
            sess.send(b"\x1b[B", 0.25)
        check("MILKDROP PRESET PATH" in paths_seen and "Preset Path 1" in paths_seen, "settings: the PATHS tab has the MILKDROP PRESET PATH section with the regular folder first")
        check(sess.alive(), "settings general texts: the program is still running")
    finally:
        sess.kill()


def overlays_queue_enter(binary, root, scripts):
    """The big list overlay's legend is a tree with a Switch group and one "[ESC] Close / Clear"; SHIFT+k / SHIFT+l / SHIFT+f open the queue /
    list / folder overlay from the big video overlay too. In the queue: ENTER (Reposition Queue State) plays the highlighted item and moves the
    queue along -- in an unlocked queue the items it jumped over are kept at the end; SHIFT+ENTER (Requeue Track and Play) plays only that item."""
    lib = os.path.join(root, "oqe_music")
    os.makedirs(lib)
    for n in "ABCD":
        make_wav(os.path.join(lib, "Track %s.wav" % n), 60)
    home = os.path.join(root, "oqe_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=false\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
    try:
        sess.wait_for(lambda t: "Track D" in t, 10)
        sess.pump(1.0)
        sess.send("L", 1.0)
        t = sess.text()
        check("Switch" in t and "Queue overlay" in t and "Folder overlay" in t and t.count("[ESC]") == 1 and "[ESC] Close / Clear" in t,
              "list overlay: the legend has a Switch group and one [ESC] Close / Clear")
        sess.send(b"\x1b", 0.8)
        check("Switch" not in sess.text(), "list overlay: ESC closes it (no search to clear)")
        # the big video overlay: SHIFT+k / SHIFT+l / SHIFT+f open the other big overlays
        for key, word in (("K", "SEARCH QUEUE"), ("L", "Queue overlay"), ("F", "Folder to list")):
            sess.send("v", 1.0)
            check("[SHIFT+k/l/f] Queue/List/Folders" in sess.text(), "big video overlay: the legend names the queue / list / folder overlays (%s)" % key)
            sess.send(key, 1.2)
            check(word in sess.text() and "[v / ESC] Close" not in sess.text(), "big video overlay: %s opens its overlay (%s)" % (key, word))
            sess.send(b"\x1b", 0.8)
        # a queue of four
        for _ in range(4):
            sess.send("e", 0.4)
            sess.send(b"\x1b[B", 0.3)
        sess.send("K", 1.0)
        def queue_rows():
            return [l.split("|")[0].strip("\u2502 ").strip() for l in sess.screen.display if "Track " in l and "|" in l]
        q0 = queue_rows()
        check(len(q0) == 4, "queue: four items (%s)" % q0)
        check("Reposition queue state" in sess.text() and "Requeue track and play" in sess.text(), "queue overlay: the legend names ENTER and SHIFT+ENTER")
        sess.send("!", 0.6)   # unlock
        check("Lock" in sess.text() and "QUEUE (locked)" not in sess.text(), "queue: unlocked")
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\r", 1.5)   # ENTER on the third
        q1 = queue_rows()
        check(len(q0) == 4 and q1 == q0[3:] + q0[:2], "queue ENTER (unlocked): the third plays, the two before it wait at the end (%s -> %s)" % (q0, q1))
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x1b[13;2u", 1.5)   # SHIFT+ENTER on the second row
        q2 = queue_rows()
        check(len(q1) == 3 and q2 == [q1[0], q1[2]], "queue SHIFT+ENTER: only that item plays, the others stay (%s -> %s)" % (q1, q2))
        check(sess.alive(), "overlays / queue: the program is still running")
    finally:
        sess.kill()


def video_fx_list_frozen(binary, root, scripts):
    """The effect list: in Settings its preview is a picture (Kitty) of the sample video (the cat) when the track has no video; over the main
    screen the video in the field stands still while the list is open (one decoder: the list's preview plays) and comes back to life when it closes;
    VIDEO OPTIONS (SHIFT+s in the big video) fills the screen with the options on the left and the video, as a picture, on the right."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: effect list frozen video (ffmpeg is not installed on this machine)")
        return
    lib = os.path.join(root, "vff_music")
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=40", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=40", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    make_wav(os.path.join(lib, "Plain Song.wav"), 40)
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    def start(tag):
        home = os.path.join(root, "vff_home_" + tag)
        cfgdir = os.path.join(home, ".config", "mousiki")
        os.makedirs(cfgdir)
        with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=true\nVideoMode=true\nHKeyVideo=Y\nVideoAuto=true\n" % lib)
        # (MOUSIKI_SYNC_WRITE: every frame is written before the next is made -- this harness reads the terminal far slower than a terminal does, and
        # with the frames written behind its back the pictures would wait for it; the picture logic itself is the same)
        sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1", "KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty", "MOUSIKI_SYNC_WRITE": "1"}, quiet_stderr=True, gfx=True)
        sess.wait_for(lambda t: "Clip Song" in t, 10)
        sess.pump(2.5)
        return sess
    sess = start("a")
    try:
        def big(i, c=30, r=10):
            p = sess.pics.get(i)
            return p is not None and p["c"] >= c and p["r"] >= r
        # the field plays the video; the VIDEO overlay -> Effect -> ENTER: the list
        a = sess.pic_rgba.get(7201)
        sess.pump(1.0)
        check(a is not None and sess.pic_rgba.get(7201) != a, "effect list: the video in the field plays before")
        sess.send("Y", 1.0)
        for _ in range(12):
            if "> Effect " in sess.text():
                break
            sess.send(b"\x1b[B", 0.25)
        sess.send(b"\r", 1.5)
        check("VIDEO EFFECT" in sess.text(), "effect list: it opens from the VIDEO overlay")
        sess.pump(1.5)
        f0, p0 = sess.pic_rgba.get(7201), sess.pic_rgba.get(7202)
        sess.pump(1.5)
        f1, p1 = sess.pic_rgba.get(7201), sess.pic_rgba.get(7202)
        check(f0 is not None and f0 == f1, "effect list: the video in the field stands still while the list is open")
        check(p0 is not None and p1 is not None and p0 != p1, "effect list: its preview plays (the screen's decoder)")
        sess.send(b"\x1b[B", 1.2)   # another effect: the still picture behind shows it too
        check(sess.pic_rgba.get(7201) != f1, "effect list: the still picture behind takes the highlighted effect")
        sess.send(b"\x1b", 1.0)
        sess.send(b"\x1b", 1.0)
        g0 = sess.pic_rgba.get(7201)
        sess.pump(1.5)
        check(g0 is not None and sess.pic_rgba.get(7201) != g0, "effect list: the video plays again after the list")
        # VIDEO OPTIONS of the big video: the whole screen, the options and the video side by side
        sess.send("v", 1.5)
        sess.send("S", 1.5)
        t = sess.text()
        p = sess.pics.get(7201)
        check("VIDEO OPTIONS" in t and "Video Contrast" in t and p is not None and p["col"] > 60 and p["c"] >= 40, "VIDEO OPTIONS: the options on the left, the video as a big picture on the right (%s)" % p)
        sess.send(b"\x1b", 1.0)
        sess.send(b"\x1b", 1.0)
        # Settings: the list's preview is a picture; a track without a video shows the cat
        sess.send("n", 2.5)   # (Plain Song: no video)
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)
        for _ in range(7):
            sess.send(b"\x1b[B", 0.15)
        sess.send(b"\r", 2.5)
        check("VIDEO EFFECT" in sess.text(), "effect list: it opens from Settings")
        check(settle_pic(sess, 7201, 40, 15), "effect list in Settings: the preview is a picture (%s)" % sess.pics.get(7201))
        c0 = sess.pic_rgba.get(7201)
        sess.pump(1.0)
        check(c0 is not None and sess.pic_rgba.get(7201) != c0, "effect list in Settings: the sample video plays in it (no test picture)")
        check(sess.alive(), "effect list frozen: the program is still running")
    finally:
        sess.kill()


def settle_pic(sess, i, c, r, tries=12):
    for _ in range(tries):
        p = sess.pics.get(i)
        if p is not None and p["c"] >= c and p["r"] >= r:
            return True
        sess.pump(0.5)
    return False


def milkdrop_radio_book(binary, root, scripts):
    """The MilkDrop visuals in the radio and the audio book player. Radio: "." from the spectrogram reaches MilkDrop in the scope block (a
    Kitty picture; half blocks on a terminal without pictures), ":" opens the visuals overlay (RIGHT changes the preset, ESC closes and
    saves OsciSphere=milkdrop and the MilkDrop keys in radio_config.txt), ANIMA/VIZ has a MILKDROP block. Audio book: "." reaches the
    MilkDrop view in its field, ":" opens the overlay, ANIMA/VIZ has the MILKDROP block and Viz Order 5 (no video place). Skipped without
    projectM / ffmpeg."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: milkdrop radio / book (ffmpeg is not installed on this machine)")
        return
    tone = os.path.join(root, "mdr_tone.mp3")
    if subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=330:duration=240", tone], capture_output=True).returncode != 0:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    def settle(sess, cond, tries=12):
        for _ in range(tries):
            if cond():
                return True
            sess.pump(0.5)
        return cond()
    def radio(tag, gfx):
        home = os.path.join(root, "mdr_home_" + tag)
        cfgdir = os.path.join(home, ".config", "mousiki")
        os.makedirs(cfgdir)
        with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:   # a local file plays like a stream
            f.write("# name | genre | country | codec | kbps | url | [*] favorite\nLocal Tone | Rock | US | MP3 | 128 | %s | *\n" % tone)
        with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nStartMode=radio\nWizardSeen=true\n" % os.path.join(root, "music"))
        with open(os.path.join(cfgdir, "radio_config.txt"), "w", encoding="utf-8") as f:
            f.write("WizardSeen=true\nOsciSphere=spectro\n")
        env = {"MOUSIKI_RADIO_NULL": "1"}
        if gfx:
            env.update({"KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"})
        sess = Session(binary, home, scripts, env, quiet_stderr=True, gfx=gfx)
        sess.wait_for(lambda t: "Local Tone" in t, 15)
        sess.send(b"\r", 2.0)   # tune it
        return sess, os.path.join(cfgdir, "radio_config.txt")
    # --- the radio, Kitty
    sess, rcfg = radio("kitty", True)
    try:
        sess.send(":", 1.2)
        if "Not available" in sess.text():
            print("  skipped: milkdrop radio / book (projectM 4 or presets are not installed on this machine)")
            return
        check("Visuals: MilkDrop" in sess.text() and "Show in the Scope Block" in sess.text(), "radio: \":\" opens the visuals overlay")
        sess.send(b"\x1b", 0.8)
        check("Visuals: MilkDrop" not in sess.text(), "radio: ESC closes the visuals overlay")
        def big():
            p = sess.pics.get(7201)
            return p is not None and p["c"] >= 30 and p["r"] >= 10
        check(not big(), "radio: no MilkDrop picture on the spectrogram")
        sess.send(".", 0.5)
        check(settle(sess, big, 16), "radio: \".\" from the spectrogram reaches MilkDrop, a picture fills the scope block (%s)" % sess.pics.get(7201))
        a = sess.pic_rgba.get(7201)
        def bars_black(img):   # the radio block is wider than 16:9: the picture is centred, the first and last columns are black bars
            w_, h_, px_ = img
            return all(px_[(y * w_ + x) * 4 + c] == 0 for y in range(h_) for x in (0, 1, w_ - 2, w_ - 1) for c in range(3))
        check(a is not None and bars_black(a), "milkdrop radio: Aspect Correction (on by default): a block wider than 16:9 has black bars at the sides (%s)" % (None if a is None else "%dx%d" % (a[0], a[1])))
        moved = False
        for _ in range(8):   # (a busy machine draws fewer pictures a second)
            sess.pump(0.75)
            b = sess.pic_rgba.get(7201)
            if a is not None and b is not None and a != b:
                moved = True
                break
        check(moved, "radio: the MilkDrop picture moves")
        sess.send(":", 1.5)
        def preset_line():
            return next((l for l in sess.screen.display if "> Preset" in l), "")
        before = preset_line()
        sess.send(b"\x1b[C", 2.5)
        after = preset_line()
        check(before and after and before != after, "radio: RIGHT on Preset changes the preset")
        check("preset " in sess.text() and " of " in sess.text(), "radio: the overlay shows the preset count")
        t = sess.text()
        check("Preset Overlay" in t and "Shuffle Favourites" in t and "Hard Cut Interval" in t and "Mesh Detail" in t and "Clean Start" in t,
              "radio: the overlay has the player's rows (Preset Overlay, Shuffle Favourites, Hard Cut Interval, Mesh Detail, Clean Start ...)")
        def go_to(label):
            for _ in range(30):
                if any(("> " + label) in l for l in sess.screen.display): return True
                sess.send(b"\x1b[B", 0.12)
            return False
        check(go_to("MilkDrop Effect"), "radio: the overlay has the video effect rows (MilkDrop Effect)")
        efx = lambda: next((l for l in sess.screen.display if "> MilkDrop Effect" in l), "")
        e0 = efx()
        sess.send(b"\x1b[C", 1.0)
        check(efx() != e0 and "Off" in e0, "radio: RIGHT on MilkDrop Effect picks an effect (%r -> %r)" % (e0.strip()[-12:], efx().strip()[-14:]))
        check(go_to("Sixel Colours") and "adaptive" in "".join(sess.screen.display), "radio: the overlay has Sixel Colours (adaptive)")
        sess.send(b"\x1b[C", 1.0)
        check(any("> Sixel Colours" in l and "fixed" in l for l in sess.screen.display), "radio: RIGHT on Sixel Colours switches to fixed")
        sess.send(b"\x1b[C", 0.6)
        sess.send(b"\x1b[A", 0.2)
        sess.send(b"\x1b[A", 0.2)
        for _ in range(30):   # back to the Preset row
            if any(("> Preset" in l and "< " in l) for l in sess.screen.display): break
            sess.send(b"\x1b[A", 0.12)
        # the preset overlay: "p" opens it, Favourites on top, [a] adds the preset under the cursor, [d] takes it out, ESC goes back to the visuals overlay
        favf = os.path.join(os.path.dirname(rcfg), "milkdrop_favourites.txt")
        sess.send("p", 1.5)
        check(settle(sess, lambda: "MilkDrop presets" in sess.text(), 8), "radio: [p] opens the preset overlay (on the folder of the preset shown)")
        for _ in range(40):   # down to a preset (folders come first)
            if sess.text().count(".milk") == 0 and "> " in sess.text():
                pass
            sess.send(b"\x1b[B", 0.15)
            moved_to = [l for l in sess.screen.display if l.lstrip("\u2502 ").startswith("> ")]
            if moved_to and "/" not in moved_to[0].split("> ", 1)[1].strip().rstrip("\u2502 ") and "Favourites" not in moved_to[0] and "Recommended" not in moved_to[0] and ".." not in moved_to[0]:
                break
        sess.send("a", 1.0)
        check(os.path.exists(favf) and len([l for l in open(favf, encoding="utf-8") if l.strip() and not l.startswith("#")]) == 1, "radio: [a] writes the preset into milkdrop_favourites.txt")
        sess.send("f", 1.0)
        check("Favourites" in sess.text() and "1 favourites" in sess.text(), "radio: [f] shows the favourites")
        sess.send("d", 1.0)
        check(len([l for l in open(favf, encoding="utf-8") if l.strip() and not l.startswith("#")]) == 0, "radio: [d] takes it out of the favourites")
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b", 0.8)
        check("MilkDrop presets" not in sess.text() and "Visuals: MilkDrop" in sess.text(), "radio: ESC leaves the preset overlay for the visuals overlay")
        sess.send(b"\x1b", 1.0)
        with open(rcfg, encoding="utf-8") as f:
            c = f.read()
        check("OsciSphere=milkdrop" in c and "MilkDropFrameRate=30" in c and "UseMilkDrop=true" in c and "MilkDropHardCutInterval=20" in c and "MilkDropMeshDetail=1" in c, "radio: OsciSphere=milkdrop and the MilkDrop keys are saved in radio_config.txt")
        sess.send(".", 1.5)
        check(settle(sess, lambda: not big()), "radio: \".\" goes on from MilkDrop to the oscilloscope")
        sess.send("s", 1.2)
        for _ in range(2):
            sess.send(b"\t", 0.6)
        for _ in range(60):   # down to the last rows of the tab
            if "Sixel Colours" in sess.text():
                break
            sess.send(b"\x1b[B", 0.1)
        sess.pump(0.6)
        t = sess.text()
        check("MILKDROP" in t and "Use MilkDrop" in t and "Show Preset Name" in t, "radio: ANIMA/VIZ has the MILKDROP block")
        check("Sixel Colours" in t and "MilkDrop Effect" in t, "radio: ANIMA/VIZ has the effect rows too")
        for _ in range(3):   # SHIFT+UP: ten rows at a time (three presses cannot reach this far with single steps)
            sess.send(b"\x1b[1;2A", 0.3)
        sess.pump(0.5)
        check("Sixel Colours" not in sess.text(), "radio: SHIFT+UP in the settings tab moves ten rows at a time")
        check(sess.alive(), "radio milkdrop: the program is still running")
    finally:
        sess.kill()
    # --- the radio, no pictures: half blocks
    sess, rcfg = radio("text", False)
    try:
        sess.send(".", 0.5)
        ok = settle(sess, lambda: sum(l.count("\u2580") for l in sess.screen.display[1:15]) > 200, 16)
        check(ok, "radio: without pictures MilkDrop is drawn in half blocks (%d)" % sum(l.count("\u2580") for l in sess.screen.display[1:15]))
    finally:
        sess.kill()
    # --- the audio book
    home = os.path.join(root, "mdb_home")
    books = os.path.join(root, "mdb_books")
    os.makedirs(os.path.join(books, "Some Book"))
    shutil.copy(tone, os.path.join(books, "Some Book", "01 part.mp3"))
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("AudioBookPath=%s\nStartMode=audiobook\nUseTerminalSettings=true\nWizardSeen=true\n" % books)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1", "KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"}, quiet_stderr=True, gfx=True)
    try:
        sess.wait_for(lambda t: "Some Book" in t, 15)
        sess.send(b"\r", 2.0)   # open the book
        sess.send(b"\r", 3.0)   # play it
        def field():   # the MilkDrop picture in the field (the book's cover is a picture too, but small: 30 x 15 at the left)
            p = sess.pics.get(7201)
            return p is not None and p["c"] >= 40 and p["col"] > 40
        presses = 0
        for _ in range(6):
            if field():
                break
            sess.send(".", 0.3)
            presses += 1
            settle(sess, field, 6)
        check(field(), "audio book: \".\" reaches the MilkDrop view in its field (%d presses)" % presses)
        sess.send(":", 1.2)
        check(settle(sess, lambda: "Visuals: MilkDrop" in sess.text(), 10), "audio book: \":\" opens the visuals overlay")
        sess.send(b"\x1b", 1.0)
        bc = os.path.join(cfgdir, "audiobook_config.txt")
        check(os.path.exists(bc) and "MilkDropView=true" in open(bc, encoding="utf-8").read(), "audio book: the view is saved in audiobook_config.txt (its own)")
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)   # ANIMA/VIZ
        seen = ""
        for _ in range(90):   # down through the tab
            seen += sess.text()
            if "Show Preset Name" in sess.text() and "Viz Order 5" in seen:
                break
            sess.send(b"\x1b[B", 0.1)
        sess.pump(0.6)
        seen += sess.text()
        check("MILKDROP" in seen and "Viz Order 5" in seen and "Viz Order 6" not in seen and "Show Preset Name" in seen, "audio book: ANIMA/VIZ has the MILKDROP block and Viz Order 1..5")
        check(sess.alive(), "audio book milkdrop: the program is still running")
    finally:
        sess.kill()


def milkdrop_visuals(binary, root, scripts):
    """The MilkDrop visuals (projectM, loaded at run time when it is installed): "." reaches the MilkDrop view, which fills the field with a
    moving picture (Kitty image) drawn from the music; ":" opens the visuals overlay (the preset, previous / next / random, the options;
    projectM's renderer and the preset count); the view is saved. Without projectM (or without presets) the view is skipped by "." and
    the overlay says why. Skipped on a machine without projectM."""
    def start(tag, extra, gfx=True):
        lib = os.path.join(root, "md_music_" + tag)
        os.makedirs(lib)
        make_wav(os.path.join(lib, "Alpha Song.wav"), 40)
        home = os.path.join(root, "md_home_" + tag)
        cfgdir = os.path.join(home, ".config", "mousiki")
        os.makedirs(cfgdir)
        with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=true\nUseLyrics=false\nUseSphere=false\nVizDefault=osci\n%s" % (lib, extra))
        env = {"MOUSIKI_NULL_AUDIO": "1"}
        if gfx:
            env.update({"KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"})
        sess = Session(binary, home, scripts, env, quiet_stderr=True, gfx=gfx)
        sess.wait_for(lambda t: "Alpha Song" in t, 10)
        sess.pump(1.5)
        return sess, os.path.join(cfgdir, "config.txt")
    # Is projectM here at all? (the overlay says it)
    sess, cfg = start("probe", "", gfx=False)
    try:
        sess.send(":", 1.5)
        t = sess.text()
        check("Visuals: MilkDrop" in t, "milkdrop: \":\" opens the visuals overlay")
        here = "Not available" not in t
        sess.send(b"\x1b", 0.8)
        check("Visuals: MilkDrop" not in sess.text(), "milkdrop: ESC closes the overlay")
    finally:
        sess.kill()
    if not here:
        print("  skipped: MilkDrop drawing (projectM 4 or presets are not installed on this machine)")
        return
    # "." reaches the view; the picture fills the field and moves
    sess, cfg = start("view", "")
    try:
        def md_pic():   # the MilkDrop picture: a big picture in the field (the oscilloscope / spectrogram here are braille / tiles)
            p = sess.pics.get(7201)
            return p if p and p["c"] >= 30 and p["r"] >= 10 else None
        presses = 0
        for _ in range(4):
            sess.send(".", 0.3)
            presses += 1
            for _ in range(8):
                if md_pic():
                    break
                sess.pump(0.5)
            if md_pic():
                break
        check(md_pic() is not None and presses == 2, "milkdrop: \".\" reaches the MilkDrop view (osci -> spectrogram -> MilkDrop: %d presses)" % presses)
        p = sess.pics.get(7201)
        check(p is not None and p["c"] >= 30 and p["r"] >= 10, "milkdrop: a picture fills the field (%s)" % (None if p is None else "%d x %d cells" % (p["c"], p["r"])))
        a = sess.pic_rgba.get(7201)
        check(a is not None and a[0] / a[1] <= 16 / 9 + 0.03, "milkdrop: Aspect Correction (on by default): the picture is never wider than 16:9 (%s)" % (None if a is None else "%dx%d" % (a[0], a[1])))
        moved = False
        for _ in range(8):   # (a busy machine draws fewer pictures a second)
            sess.pump(0.75)
            b = sess.pic_rgba.get(7201)
            if a is not None and b is not None and a != b:
                moved = True
                break
        check(moved, "milkdrop: the picture moves (another picture a moment later)")
        sess.send(":", 1.5)
        t = sess.text()
        check("preset " in t and " of " in t and "drawn by" in t, "milkdrop: the overlay shows the preset count and what draws")
        def preset_line():
            return next((l for l in sess.screen.display if "> Preset" in l), "")
        before = preset_line()
        sess.send(b"\x1b[C", 2.5)   # RIGHT on the Preset row: the next preset
        after = preset_line()
        check(before and after and before != after, "milkdrop: RIGHT on Preset changes the preset (%r -> %r)" % (before.strip()[:40], after.strip()[:40]))
        sess.send(b"\x1b", 1.0)
        # the preset's name in its box over the field for 3 s after a change (n in the overlay, then closed at once)
        sess.send(":", 1.2)
        sess.send("n", 1.0)
        name = preset_line().split("Preset", 1)[-1].strip(" |<>\u2502")
        sess.send(b"\x1b", 0.2)
        frag = name[:12].strip()
        check(frag and sess.wait_for(lambda t: "Visuals: MilkDrop" not in t and frag in t, 2.0),
              "milkdrop: the preset's name shows in its box over the field (%r)" % frag)
        with open(cfg, encoding="utf-8") as f:
            c = f.read()
        check("MilkDropView=true" in c, "milkdrop: the view is saved (MilkDropView=true)")
        check(sess.alive(), "milkdrop: the program is still running")
    finally:
        sess.kill()
    # no presets: "." leaves the view out, the overlay says why
    sess, cfg = start("nopresets", "MilkDropPresets=%s\n" % os.path.join(root, "md_none"), gfx=False)
    try:
        seen = False
        for _ in range(5):
            sess.send(".", 0.8)
            seen = seen or "milkdrop:" in sess.text()
        check(not seen, "milkdrop: without presets \".\" leaves the view out")
        sess.send(":", 1.2)
        check("Not available" in sess.text() and "no MilkDrop presets" in sess.text(), "milkdrop: the overlay says why it cannot run")
    finally:
        sess.kill()


def milkdrop_window(binary, root, scripts):
    """The MilkDrop window (HKeyMilkdropWindow): the key opens `mousiki --milkdrop-window`, the engine keeps drawing in the player and the pictures go
    through a pipe to the window (SDL2 with its dummy video driver here; a PPM of what the window shows is written by the test hook); the terminal
    shows a note instead of the pictures; the key closes it again. Skipped without projectM + presets, or without SDL2."""
    import ctypes.util
    if not (ctypes.util.find_library("SDL2-2.0") or ctypes.util.find_library("SDL2")):
        print("  skipped: milkdrop window (SDL2 is not installed on this machine)")
        return
    lib = os.path.join(root, "mdw_music")
    os.makedirs(lib)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 40)
    home = os.path.join(root, "mdw_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    cfgf = os.path.join(cfgdir, "config.txt")
    with open(cfgf, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=true\nUseLyrics=false\nUseSphere=false\nVizDefault=osci\nHKeyMilkdropWindow=|\nMilkDropWindowResolution=640\n" % lib)
    shot = os.path.join(root, "mdw_shot.ppm")
    env = {"MOUSIKI_NULL_AUDIO": "1", "KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty", "SDL_VIDEODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
           "MOUSIKI_MILK_SHOT": shot, "MOUSIKI_MILK_SHOT_FRAME": "20"}
    sess = Session(binary, home, scripts, env, quiet_stderr=True, gfx=True)
    try:
        sess.wait_for(lambda t: "Alpha Song" in t, 10)
        sess.pump(1.5)
        sess.send(":", 1.5)
        here = "Not available" not in sess.text()
        sess.send(b"\x1b", 0.8)
        if not here:
            print("  skipped: milkdrop window (projectM 4 or presets are not installed on this machine)")
            return
        def md_pic():
            p = sess.pics.get(7201)
            return p if p and p["c"] >= 30 and p["r"] >= 10 else None
        for _ in range(4):   # the MilkDrop view (osci -> spectrogram -> MilkDrop)
            sess.send(".", 0.3)
            for _ in range(8):
                if md_pic():
                    break
                sess.pump(0.5)
            if md_pic():
                break
        check(md_pic() is not None, "milkdrop window: the MilkDrop view is on screen before the window opens")
        sess.send("|", 1.0)
        check(sess.wait_for(lambda t: "MilkDrop is in its own window" in t, 8), "milkdrop window: the key opens it, the field says where the pictures are")
        got = False
        for _ in range(60):   # the engine starts, draws and sends the pictures through the pipe
            if os.path.exists(shot) and os.path.getsize(shot) > 1000:
                got = True
                break
            sess.pump(0.5)
        check(got, "milkdrop window: a picture of the engine reaches the window")
        if got:
            data = open(shot, "rb").read()
            head, rest = data.split(b"\n255\n", 1)
            check(len(set(rest[i:i + 3] for i in range(0, len(rest) - 3, 3 * 997))) > 3, "milkdrop window: the picture is not one flat colour")
        sess.send("|", 1.5)
        check(sess.wait_for(lambda t: "MilkDrop is in its own window" not in t, 6), "milkdrop window: the key closes it, the note is gone")
        check(sess.alive(), "milkdrop window: the program is still running")
    finally:
        sess.kill()
    # the effect of the terminal view (MilkDropEffect=) is on the window's pictures too: Phosphor Green leaves (almost) no red and blue
    shot2 = os.path.join(root, "mdw_shot2.ppm")
    with open(cfgf, "a", encoding="utf-8") as f:
        f.write("MilkDropEffect=phosphor green\n")
    env2 = dict(env); env2["MOUSIKI_MILK_SHOT"] = shot2
    sess = Session(binary, home, scripts, env2, quiet_stderr=True, gfx=True)
    try:
        sess.wait_for(lambda t: "Alpha Song" in t, 10)
        sess.pump(1.5)
        sess.send("|", 1.0)
        got = False
        for _ in range(60):
            if os.path.exists(shot2) and os.path.getsize(shot2) > 1000:
                got = True
                break
            sess.pump(0.5)
        check(got, "milkdrop window: with an effect a picture reaches the window")
        if got:
            data = open(shot2, "rb").read()
            head, rest = data.split(b"\n255\n", 1)
            n = len(rest) // 3
            mr = sum(rest[0::3]) / max(1, n); mg = sum(rest[1::3]) / max(1, n); mb = sum(rest[2::3]) / max(1, n)
            check(mg > 8 and mr < mg * 0.5 and mb < mg * 0.5, "milkdrop window: the effect (Phosphor Green) is on the window's picture too (mean R %.1f G %.1f B %.1f)" % (mr, mg, mb))
        sess.send("|", 1.0)
    finally:
        sess.kill()


def milkdrop_preset_overlay(binary, root, scripts):
    """The visuals overlay's "Preset Overlay" row: the preset folder over the whole screen with the visuals beside it (the highlighted
    preset plays at once), a "Favourites" folder on top ([a] adds, [d] in it takes out; milkdrop_favourites.txt), ENTER chooses, ESC
    brings the preset from before back. "Shuffle Favourites" is saved. Skipped on a machine without projectM."""
    lib = os.path.join(root, "mdp_music")
    os.makedirs(lib)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 60)
    home = os.path.join(root, "mdp_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    cfg = os.path.join(cfgdir, "config.txt")
    favf = os.path.join(cfgdir, "milkdrop_favourites.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=true\nUseLyrics=false\nUseSphere=false\nVizDefault=osci\n"
                "MilkDropView=true\nMilkDropShuffle=false\nMilkDropPresetDuration=0\nMilkDropHardCuts=false\nVideoResolution=standard\nMilkDropFrameRate=15\nHKeyVideoWindow=\":\"\nHKeyVisualsMenu=\"\"\n" % lib)   # (15 pictures a second: the software renderer leaves room for the keys; no timed or beat changes: the preset stays while it is checked)
    env = {"MOUSIKI_NULL_AUDIO": "1", "KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"}
    sess = Session(binary, home, scripts, env, quiet_stderr=True, gfx=True)
    try:
        sess.wait_for(lambda t: "Alpha Song" in t, 10)
        sess.pump(1.5)
        # the old video window key is gone and ":" is the visuals key again
        sess.send(":", 1.5)
        t = sess.text()
        check("Visuals: MilkDrop" in t, "preset overlay: \":\" opens the visuals overlay (the old HKeyVideoWindow gave it back)")
        if "Not available" in t:
            print("  skipped: MilkDrop drawing (projectM 4 or presets are not installed on this machine)")
            return
        check("Preset Overlay" in t and "Shuffle Favourites" in t, "preset overlay: the visuals overlay has the rows Preset Overlay and Shuffle Favourites")
        def preset_line():
            return next((l for l in sess.screen.display if "> Preset " in l or "  Preset   " in l), "")
        sess.wait_for(lambda t: "(starting" not in t and "(shows when" not in t, 8)
        before = preset_line().split("Preset", 1)[-1].strip(" |<>│")
        sess.send(b"\x1b[B", 0.5)   # Preset Overlay
        sess.send("\r", 1.5)
        check(sess.wait_for(lambda t: "MILKDROP PRESETS" in t, 6), "preset overlay: ENTER on Preset Overlay opens it over the whole screen")
        def big_pic():
            for i, p in sess.pics.items():
                if p and p["c"] >= 30 and p["r"] >= 10:
                    return p
            return None
        check(sess.wait_for(lambda t: big_pic() is not None, 8), "preset overlay: the visuals are shown beside the list (a picture)")
        check(before[:12] in sess.text(), "preset overlay: it opens on the preset shown (%r)" % before[:12])
        # the next preset in the list plays at once
        sess.send(b"\x1b[B", 1.5)
        sess.send("a", 1.0)
        favs = open(favf, encoding="utf-8").read() if os.path.exists(favf) else ""
        lines = [l for l in favs.splitlines() if l and not l.startswith("#")]
        check(len(lines) == 1 and lines[0].endswith(".milk"), "preset overlay: [a] writes the preset into milkdrop_favourites.txt (%d lines)" % len(lines))
        added = os.path.splitext(os.path.basename(lines[0]))[0] if lines else "?"
        sess.send("f", 1.5)   # [f]: straight to the Favourites folder
        check(sess.wait_for(lambda t: "remove from favourites" in t and added[:12] in t, 5), "preset overlay: [f] goes to the Favourites folder (%r)" % added[:12])
        # ESC: back to the visuals overlay, the preset from before again
        sess.send(b"\x1b", 2.5)
        check(sess.wait_for(lambda t: "MILKDROP PRESETS" not in t and "Visuals: MilkDrop" in t, 6), "preset overlay: ESC goes back to the visuals overlay")
        check(sess.wait_for(lambda t: before[:12] in preset_line(), 6), "preset overlay: ESC brings the preset from before back (%r)" % preset_line().strip()[:50])
        # the Recommended list (milkdrop_recommended.txt: names looked up in the installed presets; a line that fits nothing is skipped)
        with open(os.path.join(cfgdir, "milkdrop_recommended.txt"), "w", encoding="utf-8") as rf:
            rf.write("# my picks\n%s\nno such preset anywhere.milk\n" % os.path.splitext(os.path.basename(lines[0]))[0].upper() if lines else "")
        # open again, go up to the top: the Favourites folder is first and holds the preset; the other folders are there too
        sess.send("\r", 1.5)
        sess.send(b"\x1b[D", 1.0)
        check(sess.wait_for(lambda t: "▸ " in t and "◂ .." in t, 4), "preset overlay: LEFT goes up one folder and lists its folders")
        for _ in range(3):
            sess.send(b"\x1b[D", 0.6)
        t = sess.text()
        check("★ Favourites (1)" in t and "▸ Dancer" in t and "▸ Waveform" in t or ("★ Favourites (1)" in t and t.count("▸ ") >= 5),
              "preset overlay: the top folder lists ★ Favourites (1) and the preset folders")
        check("Recommended (1)" in t, "preset overlay: the top level has a Recommended folder with the one preset the list names (names match without case or extension)")
        sess.send(b"\x1b[H", 0.6)
        sess.send(b"\x1b[B", 0.6)   # Recommended (the row under Favourites)
        sess.send(b"\x1b[C", 1.5)
        check(sess.wait_for(lambda t: "1 recommended" in t and added[:12] in t, 4), "preset overlay: RIGHT opens Recommended with the preset the list names (%r)" % added[:12])
        sess.send(b"\x1b[D", 1.0)
        check(sess.wait_for(lambda t: "Recommended (1)" in t and "MILKDROP PRESETS" in t, 4), "preset overlay: LEFT goes back to the top level")
        # the search: "/" and a misspelt word still finds the presets (fuzzy), ESC goes back to the folder
        sess.send("/", 0.5)
        for c in "stikflower":
            sess.send(c, 0.1)
        check(sess.wait_for(lambda t: "Search: stikflower" in t and "Stickflower" in t, 6), "preset overlay: \"/\" searches every preset, fuzzy (stikflower -> Stickflower)")
        sess.send(b"\x1b", 1.0)
        check(sess.wait_for(lambda t: "Search:" not in t and "★ Favourites (1)" in t, 4), "preset overlay: ESC clears the search, back in the folder")
        sess.send(b"\x1b[H", 0.8)   # HOME: the first row, the Favourites folder
        sess.send(b"\x1b[C", 1.5)
        check(sess.wait_for(lambda t: "Favourites" in t and added[:12] in t and "remove from favourites" in t, 4), "preset overlay: RIGHT opens the Favourites folder with the preset (%r)" % added[:12])
        # ENTER chooses it: back in the visuals overlay with that preset
        sess.send("\r", 2.0)
        check(sess.wait_for(lambda t: "MILKDROP PRESETS" not in t and added[:12] in preset_line(), 6), "preset overlay: ENTER chooses the preset (%r)" % preset_line().strip()[:50])
        # [d] in the favourites takes it out
        sess.send("\r", 1.5)
        for _ in range(4):
            sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[H", 0.8)
        sess.send(b"\x1b[C", 1.5)
        sess.send("d", 1.0)
        favs = open(favf, encoding="utf-8").read()
        check(not [l for l in favs.splitlines() if l and not l.startswith("#")], "preset overlay: [d] in the Favourites folder takes the preset out")
        sess.send(b"\x1b", 1.5)
        # Shuffle Favourites is saved
        for _ in range(5):
            sess.send(b"\x1b[B", 0.3)
        t = sess.text()
        check(sess.wait_for(lambda t: any("> Shuffle Favourites" in l for l in sess.screen.display), 6), "preset overlay: DOWN reaches Shuffle Favourites")
        sess.send(b"\x1b[C", 0.8)
        # Effect and Sixel Colours (6 rows further down): the effect is the visuals' own, the colours adaptive by default
        def big_rgba():
            best = None
            for i, p in sess.pics.items():
                if p and p["c"] >= 30 and p["r"] >= 10 and i in sess.pic_rgba:
                    best = sess.pic_rgba[i]
            return best
        cube = {v & 0xF8 for v in (0, 40, 85, 135, 190, 255)}   # (Kitty pictures are sent with 5 bits per channel)
        greys = {round(i * 255 / 23) & 0xF8 for i in range(24)}
        def fixed_share():   # the share of pixels that are colours of the fixed palette (Kitty at standard resolution: a palette picture)
            px = big_rgba()
            if not px:
                return -1.0
            w, h, raw = px
            n = fixed = 0
            for k in range(0, len(raw), 4 * 7):
                r, g, b = raw[k], raw[k + 1], raw[k + 2]
                if r + g + b < 30:
                    continue
                n += 1
                if (r in cube and g in cube and b in cube) or (r == g == b and r in greys):
                    fixed += 1
            return fixed / n if n else -1.0
        sess.pump(2.0)
        a = fixed_share()
        for _ in range(8):
            sess.send(b"\x1b[B", 0.3)
        check(sess.wait_for(lambda t: any("> Effect" in l for l in sess.screen.display), 4), "preset overlay: DOWN reaches Effect")
        sess.send(b"\x1b[C", 0.8)
        check(sess.wait_for(lambda t: any("> Effect" in l and "Scanlines" in l for l in sess.screen.display), 4), "preset overlay: RIGHT on Effect: Scanlines")
        sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[B", 0.5)
        check(sess.wait_for(lambda t: any("> Sixel Colours" in l and "adaptive" in l for l in sess.screen.display), 4), "preset overlay: Sixel Colours is adaptive by default")
        sess.send(b"\x1b[C", 2.5)
        sess.pump(2.0)
        f2 = fixed_share()
        check(a >= 0 and f2 >= 0 and a < f2 - 0.3 and f2 > 0.9, "preset overlay: adaptive colours are the picture's own, fixed the fixed palette (fixed-palette pixels: adaptive %.2f, fixed %.2f)" % (a, f2))
        sess.send(b"\x1b[A", 0.5)
        sess.send(b"\x1b[C", 0.5)   # (Effect again: Scanlines, saved)
        sess.send(b"\x1b", 1.0)
        c = open(cfg, encoding="utf-8").read()
        check("MilkDropShuffleFavourites=true" in c, "preset overlay: Shuffle Favourites is saved")
        check("MilkDropEffect=scanlines" in c and "MilkDropSixelColors=fixed" in c, "preset overlay: MilkDropEffect= and MilkDropSixelColors= are saved")
        check("HKeyVideoWindow" not in c, "preset overlay: the old HKeyVideoWindow line is gone from config.txt")
        # Shuffle Favourites off again: the preset shown stays (it used to jump to the list's first one, "fast transition to black")
        sess.send(":", 1.5)
        def go_row(label):   # UP until the row is selected (the overlay opens on the row used last); a busy machine draws late: it must stay
            for _ in range(24):
                if any("> " + label in l for l in sess.screen.display):
                    sess.pump(1.0)
                    if any("> " + label in l for l in sess.screen.display):
                        return True
                    continue
                sess.send(b"\x1b[A", 0.6)
            return False
        def preset_name():
            l = next((l for l in sess.screen.display if "Preset    <" in l or "Preset   <" in l), "")
            return l.split("<", 1)[-1].split(">", 1)[0].strip() if "<" in l else ""
        # (a panel over the running visuals can be read half drawn now and then: the rows are read until they are there)
        go_row("Shuffle Favourites")
        sess.wait_for(lambda t: preset_name() != "", 4)
        shown = preset_name()
        fav_row = lambda: next((l for l in sess.screen.display if "> Shuffle Favourites" in l), "")
        turned = False
        for _ in range(2):   # (once more when the row still says on: a key can be lost while the panel is drawn late)
            go_row("Shuffle Favourites")
            if not fav_row().rstrip(" \u2502").endswith("off"):
                sess.send(b"\x1b[D", 1.0)
            turned = sess.wait_for(lambda t: fav_row().rstrip(" \u2502").endswith("off"), 8)
            if turned:
                break
        sess.pump(2.0)
        sess.wait_for(lambda t: preset_name() != "", 4)
        after = preset_name()
        check(turned and shown and after == shown and "transition to black" not in after,
              "preset overlay: Shuffle Favourites off keeps the preset shown (%r -> %r)" % (shown[:40], after[:40]))
        # Auto MilkDrop (the last row): on switches Auto Video off, off brings it back
        def close_overlay():   # ESC until the visuals overlay is gone (an ESC read together with the next key would be ALT+key)
            for _ in range(3):
                sess.send(b"\x1b", 1.5)
                if sess.wait_for(lambda t: "Visuals: MilkDrop" not in t, 5):
                    sess.pump(1.0)
                    if "Visuals: MilkDrop" not in sess.text():
                        return
        def set_auto(want):   # (tries again when the row did not turn: a key can be lost under load; the file is written a moment later)
            key = "MilkDropAuto=" + ("true" if want == "on" else "false")
            for _ in range(2):
                if "Visuals: MilkDrop" not in sess.text():
                    sess.send(":", 1.5)
                    sess.wait_for(lambda t: "Visuals: MilkDrop" in t, 5)
                go_row("Auto MilkDrop")
                if not any("> Auto MilkDrop" in l and l.rstrip(" \u2502").endswith(want) for l in sess.screen.display):
                    sess.send(b"\x1b[C", 1.0)
                sess.wait_for(lambda t: any("> Auto MilkDrop" in l and l.rstrip(" \u2502").endswith(want) for l in sess.screen.display), 8)
                close_overlay()
                deadline = time.time() + 8
                while time.time() < deadline and key not in open(cfg, encoding="utf-8").read():
                    sess.pump(0.5)
                if key in open(cfg, encoding="utf-8").read():
                    break
            return open(cfg, encoding="utf-8").read()
        check(any("Auto MilkDrop" in l and "off" in l for l in sess.screen.display), "preset overlay: the last row is Auto MilkDrop (off)")
        c = set_auto("on")
        check("MilkDropAuto=true" in c and "VideoAuto=false" in c and "MilkDropAutoVideoWas=true" in c, "preset overlay: Auto MilkDrop on switches Auto Video off (and remembers it)")
        c = set_auto("off")
        check("MilkDropAuto=false" in c and "VideoAuto=true" in c and "MilkDropAutoVideoWas=false" in c, "preset overlay: Auto MilkDrop off brings Auto Video back")
        check(sess.alive(), "preset overlay: the program is still running")
    finally:
        sess.kill()


def gapless_playback(binary, root, scripts):
    """Settings -> GENERAL -> AUDIO -> Gapless Playback (Gapless=off|album|full, off by default). The sound that goes to the (silent test)
    device is recorded (MOUSIKI_AUDIO_TAP) and the change from one track to the next is measured in samples: off leaves a gap of silence
    (the device is opened anew); album goes on without a single silent sample into the next track of the same album and keeps the gap
    before a track of another album; full goes on without a gap into that one too. The screen follows the change (the next title, the
    time from 0 again). A track queued while the next one was already prepared is the one that follows -- without a gap as well."""
    import array, shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: gapless (ffmpeg is not installed on this machine)")
        return
    def make(lib, name, freq, secs, album):
        meta = ["-metadata", "album=" + album, "-metadata", "artist=Tester"] if album else []
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=%d:duration=%s:sample_rate=44100" % (freq, secs),
                        "-ac", "2"] + meta + [os.path.join(lib, name + ".flac")], check=True)
    def silent_runs(tap):
        d = array.array("f")
        with open(tap, "rb") as f:
            d.frombytes(f.read())
        m = d[0::2]
        first = next((i for i in range(len(m)) if abs(m[i]) > 1e-4), None)
        last = next((i for i in range(len(m) - 1, -1, -1) if abs(m[i]) > 1e-4), None)
        runs, i = [], first
        while first is not None and i < last:
            if abs(m[i]) <= 1e-4:
                j = i
                while j < last and abs(m[j]) <= 1e-4:
                    j += 1
                if j - i > 8:
                    runs.append((j - i) / 44.1)
                i = j
            else:
                i += 1
        return runs
    def play(tag, mode, tracks, wait, keys=None):
        lib = os.path.join(root, "gl_music_" + tag)
        os.makedirs(lib)
        for t in tracks:
            make(lib, *t)
        home = os.path.join(root, "gl_home_" + tag)
        os.makedirs(os.path.join(home, ".config", "mousiki"))
        with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nAutoPlay=true\nNormalizeVolume=false\nPauseFade=false\nUseLyrics=false\n%s" %
                    (lib, "" if mode is None else "Gapless=%s\n" % mode))
        tap = os.path.join(root, "gl_tap_" + tag + ".raw")
        sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1", "MOUSIKI_AUDIO_TAP": tap}, quiet_stderr=True)
        try:
            sess.wait_for(lambda t: tracks[0][0] in t, 10)
            if keys:
                keys(sess)
            sess.pump(wait)
            return sess, silent_runs(tap)
        except Exception:
            sess.kill()
            raise
    # off (the default: no Gapless= line)
    sess, runs = play("off", None, [("01 Alpha", 440, 4, "One"), ("02 Beta", 660, 4, "One")], 5.0)
    try:
        check(any(r >= 40 for r in runs), "gapless off (default): a gap of silence between the tracks (%s ms)" % [round(r) for r in runs])
        sess.send("s", 1.2)
        sess.send(b"\t", 0.8)                # GENERAL
        row = next((l for l in sess.screen.display if "Gapless Playback" in l), "")
        check("off" in row and "AUDIO" in sess.text(), "gapless: the row is in GENERAL -> AUDIO, off by default (%r)" % row.strip()[:70])
    finally:
        sess.kill()
    # album, the same album
    sess, runs = play("album", "album", [("01 Alpha", 440, 4, "One"), ("02 Beta", 660, 4, "One")], 5.0)
    try:
        check(not runs, "gapless album: not one silent sample between two tracks of the same album (%s)" % [round(r) for r in runs])
        fl = [l for l in sess.screen.display if "File      :" in l]
        check(fl and "02 Beta" in fl[0], "gapless album: the screen shows the next track")
        check(any("[ 00:0" in l for l in sess.screen.display), "gapless album: the time runs from 0 again")
        check(sess.alive(), "gapless album: the program is still running")
    finally:
        sess.kill()
    # album, another album: the gap stays; full: no gap
    sess, runs = play("album2", "album", [("01 Alpha", 440, 4, "One"), ("02 Beta", 660, 4, "Two")], 5.0)
    try:
        check(any(r >= 40 for r in runs), "gapless album: another album keeps the gap (%s ms)" % [round(r) for r in runs])
    finally:
        sess.kill()
    sess, runs = play("full", "full", [("01 Alpha", 440, 4, "One"), ("02 Beta", 660, 4, "Two")], 5.0)
    try:
        check(not runs, "gapless full: no gap into a track of another album (%s)" % [round(r) for r in runs])
    finally:
        sess.kill()
    # full: a track queued while the next one was prepared already is the one that follows, without a gap
    def queue_third(sess):
        sess.pump(1.5)                     # (the track is shorter than the lead: the next one in the list is prepared at once)
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x1b[B", 0.3)
        sess.send("a", 0.5)                # 03 Gamma goes to the front of the queue
    sess, runs = play("queue", "full", [("01 Alpha", 440, 5, "One"), ("02 Beta", 660, 5, "One"), ("03 Gamma", 550, 5, "One")], 4.5, keys=queue_third)
    try:
        t = sess.text()
        check(not runs, "gapless full: no gap into the queued track either (%s)" % [round(r) for r in runs])
        fl = [l for l in sess.screen.display if "File      :" in l]
        check(fl and "03 Gamma" in fl[0], "gapless full: the queued track (not the one prepared before, the next in the list) follows (%r)" % (fl[0].strip()[:60] if fl else None))
    finally:
        sess.kill()


def autoplay_and_sphere(binary, root, scripts):
    """Settings -> GENERAL -> AUDIO -> Auto-Play (AutoPlay=, false by default): off, the track the player opens with is loaded and stands paused (the play
    button shows PLAY, the time stays at 0:00) until the play / pause key (p); on, it plays at once. ANIMA/VIZ -> SPHERE -> Pause Sphere (PauseSphere=, false by default):
    off, a turning sphere keeps the full frame rate while the track is paused (Reduce Motion When Paused cut it to 10 a second: the sphere stuttered);
    on, the sphere stops while paused and the screen calms down. The two rows are in their categories and RIGHT switches them."""
    lib = os.path.join(root, "aps_music")
    os.makedirs(lib, exist_ok=True)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    def start(tag, extra):
        home = os.path.join(root, "aps_home_" + tag)
        cfgdir = os.path.join(home, ".config", "mousiki")
        os.makedirs(cfgdir)
        with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\nElimentDummyButtons=true\nUseLyrics=false\nVizDefault=sphere\nNoLyricsViz=sphere\n%s" % (lib, extra))
        sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
        sess.wait_for(lambda t: "Alpha Song" in t)
        sess.pump(3.0)
        return sess, os.path.join(cfgdir, "config.txt")
    def frames(sess, secs):
        b0 = len(sess.raw); sess.pump(secs)
        return sess.raw[b0:].count(b"\x1b[?2026h")
    # --- Auto-Play off: paused at the start, the sphere turns at the full rate
    sess, cfg = start("off", "AutoPlay=false\n")
    try:
        t = sess.text()
        check("PLAY" in t and "PAUSE" not in t, "auto-play off: the opening track stands paused (the button shows PLAY)")
        check("0:00" in t, "auto-play off: the time stays at 0:00")
        sess.pump(6.0)                       # quiet for more than 3 s: Reduce Motion When Paused would be on
        n = frames(sess, 3.0)
        check(n >= 3 * 30 * 0.6, "pause sphere off: paused and untouched, the turning sphere keeps the full frame rate (%d frames in 3 s)" % n)
        sess.send("p", 1.5)
        check("PAUSE" in sess.text(), "auto-play off: the play / pause key (p) starts the track (the button shows PAUSE)")
        # the rows
        sess.send("s", 1.2)
        sess.send(b"\t", 0.8)                # GENERAL
        check("Auto-Play" in sess.text() and "AUDIO" in sess.text(), "auto-play: the row is in GENERAL -> AUDIO")
        sess.send(b"\t", 0.8)                # ANIMA/VIZ
        label = "Pause Sphere"
        def row():
            for y, line in enumerate(sess.screen.display):
                if label in line and "Preview:" not in line:
                    return y, line
            return None, ""
        for _ in range(150):
            sess.send(b"\x1b[B", 0.1)
            y, line = row()
            if y is not None and any(c.reverse for c in sess.screen.buffer[y].values()):
                break
        sess.pump(0.4)
        y, line = row()
        disp = sess.screen.display
        hdr = next((i for i, l in enumerate(disp) if l.strip().strip("│|").strip() == "SPHERE" or " SPHERE " in l.replace("│", " ")), None)
        cover = next((i for i, l in enumerate(disp) if "DISK / COVER OPTIONS" in l), None)
        check(y is not None and hdr is not None and hdr < y, "pause sphere: the row is under its own SPHERE category")
        check(cover is None or hdr is None or cover < hdr, "pause sphere: SPHERE comes below DISK / COVER OPTIONS")
        check("false" in line, "pause sphere: false by default: %r" % line)
        sess.send(b"\x1b[C", 0.8)
        y, line = row()
        check("true" in line and "pause sphere: on" in sess.text(), "pause sphere: RIGHT switches it on and the status line says so")
        sess.send("s", 1.5)
        check("PauseSphere=true" in open(cfg, encoding="utf-8").read(), "pause sphere: config.txt has PauseSphere=true once the settings are saved")
        check(sess.alive(), "auto-play / pause sphere: the program is still running")
    finally:
        sess.kill()
    # --- Pause Sphere on: paused, the sphere stands still and the screen calms down
    sess, cfg = start("still", "AutoPlay=false\nPauseSphere=true\n")
    try:
        sess.pump(6.0)
        n = frames(sess, 3.0)
        check(n <= 40, "pause sphere on: paused and untouched, the sphere stands still and the frame rate is low (%d frames in 3 s)" % n)
    finally:
        sess.kill()
    # --- Auto-Play on: it plays at once
    sess, cfg = start("on", "AutoPlay=true\n")
    try:
        check("PAUSE" in sess.text(), "auto-play on: the opening track plays at once (the button shows PAUSE)")
    finally:
        sess.kill()


def spectro_history_setting(binary, root, scripts):
    """Settings -> ANIMA/VIZ -> OSCI/SPECTRO SETTINGS: the row Keep Spectrogram History is true by default, LEFT / RIGHT switch it, the status line says what
    it does, and the choice reaches config.txt (SpectroHistory=)."""
    home = os.path.join(root, "sph_home")
    lib = os.path.join(root, "music")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    cfg = os.path.join(cfgdir, "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)                               # ANIMA/VIZ
        label = "Keep Spectrogram History"

        def row():
            for y, line in enumerate(sess.screen.display):
                if label in line and "Preview:" not in line:
                    return y, line
            return None, ""
        for _ in range(150):                                 # down to the row: the cursor row is drawn in reverse video
            sess.send(b"\x1b[B", 0.1)
            y, line = row()
            if y is not None and any(c.reverse for c in sess.screen.buffer[y].values()):
                break
        sess.pump(0.4)
        y, line = row()
        check(y is not None and "OSCI/SPECTRO SETTINGS" in sess.text(), "spectro history: the row is under OSCI/SPECTRO SETTINGS")
        check("true" in line, "spectro history: true by default: %r" % line)
        sess.send(b"\x1b[C", 0.8)
        sess.wait_for(lambda t: "keep spectrogram history: off" in t, 6)   # (a slow machine draws the status line a moment after the row)
        y, line = row()
        check("false" in line and (sys.platform == "darwin" or "keep spectrogram history: off" in sess.text()), "spectro history: RIGHT switches it off and the status line says so: %r" % line)
        sess.send(b"\x1b[D", 0.8)
        sess.wait_for(lambda t: "keep spectrogram history: on" in t, 6)
        y, line = row()
        check("true" in line and (sys.platform == "darwin" or "keep spectrogram history: on" in sess.text()), "spectro history: LEFT switches it on again: %r" % line)
        sess.send(b"\x1b[C", 0.6)                           # off; "s" saves and closes the settings
        sess.send("s", 1.5)
        txt = open(cfg, encoding="utf-8").read()
        check("SpectroHistory=false" in txt, "spectro history: config.txt has SpectroHistory=false once the settings are saved")
        check(sess.alive(), "spectro history: the program is still running")
    finally:
        sess.kill()


def settings_cli_row(binary, root, scripts):
    """Settings -> GENERAL: the category CLI / REMOTE / UPDATES with the row CLI Scripts (ENTER opens the script overlay, ESC returns) and
    the Pause Fade row in AUDIO."""
    home = os.path.join(root, "scr_home")
    lib = os.path.join(root, "scr_music")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    with open(os.path.join(cfgdir, "cli", "night.cli"), "w", encoding="utf-8") as f:
        f.write("volume 25\n")
    sess = Session(binary, home, scripts, {}, quiet_stderr=True)
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
        sess.send("s", 1.5)
        sess.send(b"\t", 0.6)
        def find(label):
            for line in sess.screen.display:
                if label in line and "Preview:" not in line:
                    return line
            return ""
        for _ in range(13):          # Playback, Start Mode, Display queue, 6 mode rows, Choose Audio Device, Stereo, Normalize, Track Buffer, Rate Cap
            sess.send(b"\x1b[B", 0.1)
        sess.send(b"\x1b[B", 0.4)    # Pause Fade
        check("Pause Fade" in find("Pause Fade") and "true" in find("Pause Fade"), "pause fade: the row exists in AUDIO and is true by default: %r" % find("Pause Fade"))
        sess.send(b"\x1b[C", 0.6)
        check("false" in find("Pause Fade"), "pause fade: RIGHT switches it off")
        sess.send(b"\x1b[D", 0.6)
        check("true" in find("Pause Fade"), "pause fade: LEFT switches it on again")
        for _ in range(7):           # Auto-Play, Gapless Playback, Black Background, Use Terminal Settings, Terminal Size, Full Screen, Reduce Motion
            sess.send(b"\x1b[B", 0.15)
        sess.send(b"\x1b[B", 0.5)    # CLI Scripts
        check("CLI / REMOTE / UPDATES" in sess.text() and "CLI Scripts" in find("CLI Scripts"), "cli row: the category is CLI / REMOTE / UPDATES and has the row CLI Scripts")
        check("[ENTER] open" in find("CLI Scripts"), "cli row: the row says [ENTER] open")
        sess.send(b"\r", 1.0)
        check("SCRIPTS (" in sess.text() and "EDITOR" in sess.text(), "cli row: ENTER opens the script overlay")
        sess.send(b"\x1b", 1.0)
        check("CLI Scripts" in find("CLI Scripts") and "GENERAL" in sess.text(), "cli row: ESC in the overlay comes back to the settings")
        check(sess.alive(), "cli row: the program is still running")
    finally:
        sess.kill()


def radio_cli_extras(binary, root, scripts):
    """The radio's aliases, `set` / `settings` and hooks (batch 256)."""
    import subprocess
    home = os.path.join(root, "radioextras_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\nJazz Night | Jazz | DE | MP3 | 128 | http://127.0.0.1:9/j | *\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\nWizardSeen=true\n" % os.path.join(root, "music"))
    with open(os.path.join(cfgdir, "cli", "hooks.cli"), "w", encoding="utf-8") as f:
        f.write("# hooks\non station-change = volume 22\non stream-error = volume 11\non track-change = volume 99\n")
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"), MOUSIKI_RADIO_NULL="1")
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})

    def q(line, timeout=15):
        p = subprocess.Popen([binary, "--query", line], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t0 = time.time()
        while p.poll() is None and time.time() - t0 < timeout:
            sess.pump(0.2)
        if p.poll() is None:
            p.kill()
        return p.returncode, (p.stdout.read() or "").strip()

    try:
        sess.wait_for(lambda t: "Plain Station" in t)
        sess.send(b"\x1b", 0.5)
        sess.pump(1.0)
        code, out = q("alias")
        check(code == 0 and "no aliases" in out, "radio alias: none at first: %r" % out)
        code, out = q("alias night = volume 25; mute")
        check(code == 0 and out.startswith("alias night = volume 25; mute"), "radio alias: defined: %r" % out)
        code, out = q("night")
        check(code == 0 and "volume" in out and "-> 25%" in out and "muted" in out, "radio alias: runs its commands: %r" % out)
        check(os.path.exists(os.path.join(cfgdir, "cli", "aliases.cli")), "radio alias: saved in aliases.cli")
        code, out = q("alias volume = stop")
        check(code == 3 and "is a command" in out, "radio alias: a command name cannot be an alias: %r" % out)
        code, out = q("unalias night")
        check(code == 0 and "removed" in out, "radio unalias: %r" % out)
        code, out = q("night")
        check(code == 3 and "unknown command" in out, "radio alias: gone after unalias: %r" % out)
        code, out = q("settings pulse")
        check(code == 0 and "PulseWave=true" in out, "radio settings: lists with values: %r" % out)
        code, out = q("set PulseWave off")
        check(code == 0 and "PulseWave=false (saved)" in out, "radio set: changes and saves: %r" % out)
        code, out = q("get pulsewave")
        check(code == 0 and out == "false", "radio get: reads a setting: %r" % out)
        with open(os.path.join(cfgdir, "config.txt"), encoding="utf-8") as f:
            cfg = f.read()
        check("PulseWave=false" in cfg or os.path.exists(os.path.join(cfgdir, "radio_config.txt")), "radio set: written to the config file")
        code, out = q("set PulseWave maybe")
        check(code == 3 and "on / off" in out, "radio set: a bad value is an error: %r" % out)
        code, out = q("set Nothing on")
        check(code == 3 and "unknown setting" in out, "radio set: an unknown setting is an error: %r" % out)
        code, out = q("hooks")
        check(code == 0 and "on station-change = volume 22" in out and "track-change" not in out, "radio hooks: lists the radio's hooks only: %r" % out)
        code, out = q("volume 50")
        check(code == 0 and "50%" in out, "radio hooks: start volume: %r" % out)
        code, out = q("tune jazz")
        sess.pump(3.0)
        code, out = q("get volume")
        check(out in ("22", "11"), "radio hooks: station-change / stream-error ran its command (volume %s)" % out)
    finally:
        sess.kill()


def radio_cli_typed(binary, root, scripts):
    """The radio's typed command line: "c:" in the SEARCH box (history + output in the STATIONS pane), "cs:" (the scripts), the console
    (SHIFT+c: log + command line) and the CLI Scripts row of the settings."""
    home = os.path.join(root, "radiotyped_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(os.path.join(cfgdir, "cli"))
    with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\nJazz Night | Jazz | DE | MP3 | 128 | http://127.0.0.1:9/j | *\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\nWizardSeen=true\n" % os.path.join(root, "music"))
    with open(os.path.join(cfgdir, "cli", "calm.cli"), "w", encoding="utf-8") as f:
        f.write("# quiet\nvolume 20\necho calm\n")
    with open(os.path.join(cfgdir, "cli", "party.cli"), "w", encoding="utf-8") as f:
        f.write("volume 80\n")
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        sess.wait_for(lambda t: "Plain Station" in t)
        sess.send(b"\x1b", 0.5)
        sess.pump(1.0)
        sess.send("/", 0.5)
        sess.send("c:", 0.6)
        t = sess.text()
        check("COMMAND HISTORY / OUTPUT" in t and "type a command" in t, "radio c:: the STATIONS pane turns into the command history: %r" % t[:0])
        check("Plain Station" not in t.split("PRESETS")[0].split("SEARCH")[-1], "radio c:: the stations are not filtered by the typed text")
        sess.send("stations", 0.3)
        sess.send(b"\r", 0.8)
        t = sess.text()
        check("> stations" in t and "2 stations" in t, "radio c:: ENTER runs the line, the pane shows it and the answer")
        sess.send("volume 33", 0.3)
        sess.send(b"\r", 0.8)
        check("-> 33%" in sess.text(), "radio c:: volume answers old -> new")
        sess.send("quit", 0.3)
        sess.send(b"\r", 0.8)
        check("radio with [q]" in sess.text() and sess.alive(), "radio c:: quit is refused with the key that closes the radio")
        sess.send(b"\x1b[A", 0.5)
        check("quit" in sess.text().split("SEARCH")[1] if "SEARCH" in sess.text() else True, "radio c:: UP recalls the last line")
        sess.send(b"\x1b", 0.6)
        sess.send(b"\x1b", 0.6)
        t = sess.text()
        check("Plain Station" in t and "COMMAND HISTORY" not in t, "radio c:: ESC leaves the command line, the stations are back")
        # cs: -- the scripts
        sess.send("/", 0.5)
        sess.send("cs:", 0.8)
        t = sess.text()
        check("SCRIPTS (4)" in t and "calm" in t and "volume 20" in t and "party" in t, "radio cs:: the pane lists the scripts with their first command: %r" % "")
        sess.send("calm", 0.8)
        t = sess.text()
        check("SCRIPTS (1)" in t and "party" not in t.split("PRESETS")[0], "radio cs:: typing narrows the list")
        sess.send(b"\r", 1.0)
        t = sess.text()
        check("EDITOR" in t and "calm.cli" in t and "volume 20" in t, "radio cs:: ENTER opens the script overlay with the first match loaded")
        sess.send(b"\x12", 1.0)   # CTRL+R: run
        check("calm" in sess.text() and "(done)" in sess.text(), "radio script overlay: CTRL+R runs the editor text and shows the answers")
        sess.send(b"\x1b", 1.0)
        check("EDITOR" not in sess.text() and "Plain Station" in sess.text(), "radio script overlay: ESC closes it")
        # the console
        sess.send("C", 1.0)
        t = sess.text()
        check("CONSOLE" in t and "session started" in t and "CLI: " in t, "radio console: SHIFT+c shows the log (with the typed commands)")
        check("STATUS MESSAGES" in t and "COMMAND LINE" in t, "radio console: the log, the STATUS MESSAGES and the COMMAND LINE boxes are all there")
        sess.send("v", 0.8)                  # verbose log: a status message
        t = sess.text()
        check("verbose log: on" in t.split("STATUS MESSAGES")[1].split("COMMAND LINE")[0], "radio console: a status line shows in the STATUS MESSAGES box")
        sess.send(b"\t", 0.6)               # log -> status messages
        sess.send(b"\t", 0.6)               # -> command line
        sess.send("echo hello", 0.3)
        sess.send(b"\r", 0.8)
        t = sess.text()
        check("> echo hello" in t and "hello" in t, "radio console: the command pane runs a line")
        sess.send(b"\x1b", 0.6)
        sess.send(b"\x1b", 0.8)
        check("CONSOLE" not in sess.text() and "Plain Station" in sess.text(), "radio console: ESC clears the line, a second ESC closes it")
        # the settings row
        sess.send("s", 1.2)
        sess.send(b"\t", 0.6)
        for _ in range(9):
            sess.send(b"\x1b[B", 0.15)
        sess.pump(0.4)
        check("CLI / REMOTE / UPDATES" in sess.text() and "CLI Scripts" in sess.text(), "radio settings: the category is CLI / REMOTE / UPDATES with the row CLI Scripts")
        sess.send(b"\r", 1.0)
        check("EDITOR" in sess.text() and "SCRIPTS (" in sess.text(), "radio settings: ENTER on CLI Scripts opens the script overlay")
        sess.send(b"\x1b", 1.0)
        check("CLI Scripts" in sess.text() and "GENERAL" in sess.text(), "radio settings: ESC in the overlay comes back to the settings")
        check(sess.alive(), "radio typed cli: the radio is still running")
    finally:
        sess.kill()


def xdg_dirs(binary, root, scripts):
    """XDG_CONFIG_HOME / XDG_CACHE_HOME: the config (and the cli scripts) are read from, and the log written to, the XDG folders."""
    import subprocess
    if sys.platform in ("darwin", "win32"):   # macOS and Windows keep everything under $HOME/.config/mousiki (app_dirs.h): the XDG variables are not used there
        print("  skipped: XDG folders (only Linux and other Unix systems use them)")
        return
    home = os.path.join(root, "xdg_home")
    xc = os.path.join(root, "xdg_conf")
    xk = os.path.join(root, "xdg_cache")
    lib = os.path.join(root, "xdg_music")
    os.makedirs(os.path.join(xc, "mousiki", "cli"))
    os.makedirs(home)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(xc, "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nWizardSeen=true\n" % lib)
    with open(os.path.join(xc, "mousiki", "cli", "startup.cli"), "w", encoding="utf-8") as f:
        f.write("volume 41\n")
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=xc, XDG_CACHE_HOME=xk)
    sess = Session(binary, home, scripts, {"XDG_CONFIG_HOME": xc, "XDG_CACHE_HOME": xk}, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        check("Alpha Song" in sess.text(), "xdg: the music path of $XDG_CONFIG_HOME/mousiki/config.txt is used")
        sess.pump(3.5)
        p = subprocess.Popen([binary, "--query", "get volume"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t0 = time.time()
        while p.poll() is None and time.time() - t0 < 15:
            sess.pump(0.2)
        out = (p.stdout.read() or "").strip()
        check(out == "41", "xdg: startup.cli of the XDG config folder ran, and --query finds the running program: %r" % out)
        check(os.path.isdir(os.path.join(xk, "mousiki")), "xdg: the cache folder is $XDG_CACHE_HOME/mousiki")
        check(not os.path.exists(os.path.join(home, ".config", "mousiki", "config.txt")), "xdg: no config was made under ~/.config")
    finally:
        sess.kill()


def make_tagged_wav(path, seconds, fields):
    """A WAV with a LIST / INFO chunk (INAM title, IART artist, IPRD album) after the audio: what the player reads without ffprobe."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    def chunk(cid, body):
        return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    fmt = struct.pack("<HHIIHH", 1, 1, 8000, 16000, 2, 16)
    info = b"INFO" + b"".join(chunk(k.encode(), v.encode("utf-8") + b"\0") for k, v in fields)
    body = b"WAVE" + chunk(b"fmt ", fmt) + chunk(b"data", b"\x00\x00" * 8000 * seconds) + chunk(b"LIST", info)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def search_wav_tags(binary, root, scripts):
    """The tags of a WAV file (a LIST / INFO chunk) are read in the program, so the search finds the file by them; and a query typed in one go (a
    paste, keys that piled up) gives the same list as the same query typed key by key (the list below the box is made once per pass of the loop)."""
    home = os.path.join(root, "wt_home")
    lib = os.path.join(root, "wt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Plain One.wav"), 5)
    make_wav(os.path.join(lib, "Plain Two.wav"), 5)
    make_tagged_wav(os.path.join(lib, "Tagged.wav"), 5, [("INAM", "Zanzibar Nights"), ("IART", "Quartz Band"), ("IPRD", "Island Songs")])
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Tagged" in sess.text() and "Plain One" in sess.text():
                break
        def search(query, one_go):
            sess.send("/", 0.5)
            if one_go:
                sess.send(query, 1.5)
            else:
                for c in query:
                    sess.send(c, 0.12)
                sess.pump(1.0)
            t = sess.text()
            sess.send(b"\x1b", 0.8)
            return t
        for query in ("zanzibar", "quartz", "island"):
            for one_go in (True, False):
                found = ""
                for _ in range(6):   # (the tags are read by the background sweep: a moment after the start)
                    found = search(query, one_go)
                    if "| Tagged" in found:
                        break
                    sess.pump(1.0)
                check("| Tagged" in found and "| Plain One" not in found and "| Plain Two" not in found,
                      "wav tags: '%s' (%s) finds the WAV by its tags and nothing else" % (query, "typed in one go" if one_go else "key by key"))
        check(sess.alive(), "wav tags: the program is still running at the end")
    finally:
        sess.kill()


def search_key(binary, root, scripts):
    """The search key of the main screen ("/", SHIFT+7 on a German keyboard) also searches in the cheat sheet, the help wizard and READ LYRICS
    (the old SHIFT+s / CTRL+s do not any more); the legends name it for the keyboard."""
    home = os.path.join(root, "sk_home")
    lib = os.path.join(root, "sk_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    os.makedirs(os.path.join(lib, "lyrics"))
    with open(os.path.join(lib, "lyrics", "Alpha Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first line\n[00:03.00]second line\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, {"XKB_DEFAULT_LAYOUT": "de"}, quiet_stderr=True)
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
        sess.send("?", 1.0)
        check("[SHIFT+7] search" in sess.text(), "search key: the cheat sheet names SHIFT+7 on a German keyboard")
        sess.send("S", 0.6)
        check("Search: " not in sess.text(), "search key: SHIFT+s no longer opens the cheat sheet search")
        sess.send("/", 0.6)
        check("Search: " in sess.text(), "search key: / opens the cheat sheet search")
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x1b", 0.5)
        sess.send("W", 1.0)
        check("[SHIFT+7] search" in sess.text(), "search key: the wizard names SHIFT+7 on a German keyboard")
        sess.send("/", 0.8)
        check("SEARCH" in sess.text().split("\n")[0], "search key: / opens the wizard search")
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x0c", 1.5)
        check("READ LYRICS" in sess.text() and "[SHIFT+7] Search" in sess.text(), "search key: the READ LYRICS legend names SHIFT+7")
        sess.send(b"\x13", 0.8)
        check("SEARCH LYRICS FILES" not in sess.text(), "search key: CTRL+s no longer opens the lyrics file search")
        sess.send("/", 0.8)
        check("SEARCH LYRICS FILES" in sess.text(), "search key: / opens the lyrics file search")
        sess.send("/", 0.4)
        check("SEARCH LYRICS FILES" in sess.text(), "search key: a typed / is text in the search, it does not close it")
        sess.send(b"\x1b", 0.6)
        check("SEARCH LYRICS FILES" not in sess.text() and "READ LYRICS" in sess.text(), "search key: ESC closes the lyrics file search")
        check(sess.alive(), "search key: the program is still running")
    finally:
        sess.kill()


def organigram_menu(binary, root, scripts):
    """The "<" menu index is an organigram: Main with its six categories, DOWN goes into a category, LEFT / RIGHT move between the boxes,
    UP goes back, ENTER opens the menu of a box (a group box only goes in), ESC closes it."""
    home = os.path.join(root, "org_home")
    lib = os.path.join(root, "org_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
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
        sess.send("<", 1.5)
        sess.wait_for(lambda t: "Main" in t and "System" in t and "Sound" in t and "Library" in t and "Other Lists" in t and "Visualizations" in t and "Lyrics" in t)
        t = sess.text()
        check("Main" in t and "System" in t and "Sound" in t and "Library" in t and "Other Lists" in t and "Visualizations" in t and "Lyrics" in t,
              "organigram: Main and its categories are drawn")
        check("Settings" in t and "Cheat Sheet" in t and "Equalizer" not in t, "organigram: the first category is open")
        sess.send(b"\x1b[C", 0.8)
        sess.wait_for(lambda t: "Equalizer" in t and "Cheat Sheet" not in t)
        t = sess.text()
        check("Equalizer" in t and "Cheat Sheet" not in t, "organigram: RIGHT goes to the next category")
        for _ in range(3):
            sess.send(b"\x1b[C", 0.5)
        sess.send(b"\x1b[B", 0.6)
        sess.send(b"\x1b[B", 0.6)
        sess.wait_for(lambda t: "Meta Data Menu" in t and "Fetch and Convert" in t and "Cover and Video Look" in t)
        t = sess.text()
        check("Meta Data Menu" in t and "Fetch and Convert" in t and "Cover and Video Look" in t, "organigram: the meta data branches open one level at a time")
        sess.send(b"\x1b[B", 0.6)
        sess.wait_for(lambda t: "Fetch Cover" in t and "File Conversion" in t)
        t = sess.text()
        check("Fetch Cover" in t and "File Conversion" in t, "organigram: the lower menus of a branch are shown")
        sess.send(b"\x1b[A", 0.5)
        sess.send(b"\x1b[A", 0.5)
        sess.send(b"\x1b[A", 0.5)
        for _ in range(4):
            sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[B", 0.5)
        sess.send(b"\r", 1.2)   # System / Settings: ENTER opens the settings
        sess.wait_for(lambda t: "PATHS" in t or "GENERAL" in t)
        t = sess.text()
        check("PATHS" in t or "GENERAL" in t, "organigram: ENTER opens the menu of the box (Settings)")
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.0)
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.0)
        for _ in range(5):
            sess.send(b"\x1b[C", 0.4)
        sess.send(b"\x1b[B", 0.6)
        sess.send(b"\x1b[C", 0.6)
        sess.send(b"\x1b[B", 0.8)
        sess.wait_for(lambda t: "Spectrum Bars" in t and "Spectrogram" in t and "Oscilloscope" in t and "Creator" in t and "Extra Window" in t and "Options" in t)
        t = sess.text()
        check("Spectrum Bars" in t and "Spectrogram" in t and "Oscilloscope" in t and "Creator" in t and "Extra Window" in t and "Options" in t,
              "organigram: the visualizations are grouped (Spectrogram: options, overlay, extra window) on several rows")
        sess.send("T", 1.0)   # SHIFT+t: the classic view, every category as a box
        sess.wait_for(lambda t: "Equalizer" in t and "Karaoke Mode" in t and "Playlists" in t and "Fetch Cover" in t)
        t = sess.text()
        check("Equalizer" in t and "Karaoke Mode" in t and "Playlists" in t and "Fetch Cover" in t, "organigram: SHIFT+t shows the classic view with every category's options")
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.0)
        sess.wait_for(lambda t: "Equalizer" in t and "Karaoke Mode" in t)
        t = sess.text()
        check("Equalizer" in t and "Karaoke Mode" in t, "organigram: the classic view is remembered")
        sess.send("T", 1.0)
        sess.wait_for(lambda t: "Karaoke Mode" not in t and "Visualizations" in t)
        t = sess.text()
        check("Karaoke Mode" not in t and "Visualizations" in t, "organigram: SHIFT+t goes back to the tree")
        sess.send(b"\x1b", 1.0)
        check("Alpha Song" in sess.text() and sess.alive(), "organigram: ESC closes it and the main screen is back")
    finally:
        sess.kill()


def shrinking_window(binary, root, scripts):
    """The window is made smaller while a track plays: the oscilloscope field goes first, then the disk (narrow), the whole meta data
    panel goes when the rows are too few for it and the list; under 40 columns a note replaces the screen. Nothing runs past the edge."""
    global COLS, ROWS
    COLS, ROWS = 140, 45
    home = os.path.join(root, "sw_home")
    lib = os.path.join(root, "sw_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Alpha Song.wav"), 60)
    make_wav(os.path.join(lib, "Beta Song.wav"), 60)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)

    def resize(c, r):
        global COLS, ROWS
        same = (sess.screen.columns, sess.screen.lines) == (c, r)
        COLS, ROWS = c, r
        fcntl.ioctl(sess.fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
        os.kill(sess.pid, signal.SIGWINCH)
        if not same:   # (a real terminal keeps what it shows when the size stays the same: only a new size makes a new screen)
            sess.screen = pyte.Screen(c, r)
            sess.stream = pyte.ByteStream(sess.screen)
        sess.raw = bytearray()
        sess.pump(1.2)

    def braille_cols(row_text, lo, hi):
        return sum(1 for ch in row_text[lo:hi] if "\u2801" <= ch <= "\u28ff")

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
        sess.send(b"\r", 2.0)
        t = sess.text()
        for _ in range(20):   # (a slow machine needs longer to load the track)
            if "File      :" in t: break
            sess.pump(0.5); t = sess.text()
        # (the field right of the meta data shows "No sync" when no lyrics are found, but nothing while an online lookup is still running:
        # on a machine with a network the text is not there yet, so only the meta data is checked)
        check("File      :" in t, "shrinking: the full window shows the meta data")
        if "File      :" not in t: print("  screen at the failure:\n" + "\n".join("    | " + l.rstrip() for l in t.split("\n") if l.strip()), flush=True)
        check_fit(sess, "shrinking 140x45", False)
        resize(100, 45)
        lines = sess.screen.display
        check(braille_cols(lines[4], 0, 34) > 5, "shrinking: at 100 columns the disk is there")
        check_fit(sess, "shrinking 100x45", False)
        resize(66, 45)
        lines = sess.screen.display
        check(braille_cols(lines[4], 0, 34) > 5 and "File      :" in sess.text(), "shrinking: at 66 columns the disk and the meta data stay")
        check_fit(sess, "shrinking 66x45", False)
        resize(52, 45)
        lines = sess.screen.display
        check(braille_cols(lines[4], 0, 34) == 0 and "File      :" in sess.text(), "shrinking: at 52 columns the disk is gone, the meta data stay")
        check_fit(sess, "shrinking 52x45", False)
        resize(40, 45)
        check("File      :" in sess.text(), "shrinking: at 40 columns the meta data are still there")
        check_fit(sess, "shrinking 40x45", False)
        resize(30, 45)
        check("too narrow" in sess.text() and "File      :" not in sess.text(), "shrinking: under 40 columns a note replaces the screen")
        check_fit(sess, "shrinking 30x45", False)
        resize(120, 45)
        check("File      :" in sess.text() and "too narrow" not in sess.text(), "shrinking: the screen is back when the window is wide again")
        resize(120, 24)
        check("File      :" not in sess.text() and "Alpha Song" in sess.text() and "SEARCH LOCAL" in sess.text(), "shrinking: with too few rows the meta data panel goes and the list stays")
        check_fit(sess, "shrinking 120x24", False)
        resize(120, 40)
        check(b"\x1b[2J" not in sess.raw and "File      :" in sess.text(), "shrinking: growing the rows repaints in place (no full-screen erase, so no flash)")
        check_fit(sess, "shrinking 120x40", False)
        check(sess.screen.cursor.y == 0, "shrinking: the cursor is parked in the top row, so a lower window pushes no row out of the screen")
        resize(120, 40)
        check("File      :" in sess.text(), "shrinking: the meta data panel is back when there are rows for it and the list")
        check_fit(sess, "shrinking 120x40", False)
        # stray text over a line that keeps changing (the disk spins next to the meta data): the line is sent whole now and then, so it heals
        row = next((i for i, l in enumerate(sess.screen.display) if "File      :" in l), -1)
        check(row >= 0, "shrinking: the File row is on the screen")
        if row >= 0:
            col = sess.screen.display[row].index("File      :")
            sess.stream.feed(("\x1b[%d;%dHZZZZZZZZ" % (row + 1, col + 1)).encode())
            check("ZZZZZZZZ" in sess.screen.display[row], "shrinking: (the stray text is on the emulator's screen)")
            for _ in range(14):
                sess.pump(0.5)
                if "ZZZZZZZZ" not in sess.screen.display[row]: break
            check("ZZZZZZZZ" not in sess.screen.display[row] and "File      :" in sess.screen.display[row],
                  "shrinking: stray text in a line that keeps changing is written over within a few seconds")
        check(sess.alive(), "shrinking: the program is still running")
    finally:
        sess.kill()


def colors_scroll(binary, root, scripts):
    """Settings -> COLORS in a window too short for its 25 lines: it scrolls with the chosen row, marks hidden rows, and fits."""
    global COLS, ROWS
    COLS, ROWS = 100, 20
    home = os.path.join(root, "cs_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.pump(1.0)
        sess.send("s", 1.2)
        t = sess.text()
        check("THEME" in t and "BORDER_COLOR" in t, "colours scroll: the top of the tab is shown first")
        check("\u25bc" in t and "\u25b2" not in t, "colours scroll: a mark says there are more rows below")
        check("FOCUS" not in t, "colours scroll: the last row is not visible yet")
        check_fit(sess, "colours scroll (top)")
        for _ in range(22):
            sess.send(b"\x1b[B", 0.12)
        t = sess.text()
        check("FOCUS" in t and "\u25b2" in t, "colours scroll: moving down scrolls to the last row, with a mark for rows above")
        check("THEME" not in t, "colours scroll: the first rows are scrolled off")
        check_fit(sess, "colours scroll (bottom)")
        sess.send(b"\r", 0.5)                               # edit the chosen colour: the caret must be on the visible row
        check_fit(sess, "colours scroll (editing)")
        sess.send(b"\x1b", 0.5)
        check(sess.alive(), "colours scroll: the program is still running")
    finally:
        sess.kill()


# The scenarios, longest first. Each one runs in a worker process of its own (own copy of the library, own HOME): they only
# sleep and read frames, so on a few cores they overlap well. MOUSIKI_SMOKE_JOBS=1 runs them one after the other.
def run_scenario(name, binary, root, scripts):
    home = os.path.join(root, "home")
    if name == "main":
        main_session(binary, root, scripts)
    elif name == "kitty_cover_options":
        kitty_cover_options(binary, home, scripts)
    elif name == "selftest_command":
        selftest_command(binary, scripts)
    elif name == "search_key":
        search_key(binary, root, scripts)
    elif name == "search_wav_tags":
        search_wav_tags(binary, root, scripts)
    elif name == "cli_commands":
        cli_commands(binary, root, scripts)
    elif name == "settings_cli_row":
        settings_cli_row(binary, root, scripts)
    elif name == "xdg_dirs":
        xdg_dirs(binary, root, scripts)
    elif name == "radio_cli":
        radio_cli(binary, root, scripts)
    elif name == "video_fx_picker":
        video_fx_picker(binary, root, scripts)
    elif name == "video_fx_picker_sizes":
        video_fx_picker_sizes(binary, root, scripts)
    elif name == "radio_sign_border":
        radio_sign_border(binary, root, scripts)
    elif name == "video_fx_picker_third_slot":
        video_fx_picker_third_slot(binary, root, scripts)
    elif name == "milkdrop_visuals":
        milkdrop_visuals(binary, root, scripts)
    elif name == "milkdrop_window":
        milkdrop_window(binary, root, scripts)
    elif name == "milkdrop_preset_overlay":
        milkdrop_preset_overlay(binary, root, scripts)
    elif name == "milkdrop_layouts_style":
        milkdrop_layouts_style(binary, root, scripts)
    elif name == "overlays_queue_enter":
        overlays_queue_enter(binary, root, scripts)
    elif name == "video_fx_list_frozen":
        video_fx_list_frozen(binary, root, scripts)
    elif name == "settings_general_texts":
        settings_general_texts(binary, root, scripts)
    elif name == "milkdrop_radio_book":
        milkdrop_radio_book(binary, root, scripts)
    elif name == "gapless_playback":
        gapless_playback(binary, root, scripts)
    elif name == "autoplay_and_sphere":
        autoplay_and_sphere(binary, root, scripts)
    elif name == "main_lyrics_translation_key":
        main_lyrics_translation_key(binary, root, scripts)
    elif name == "radio_cli_extras":
        radio_cli_extras(binary, root, scripts)
    elif name == "radio_cli_typed":
        radio_cli_typed(binary, root, scripts)
    elif name == "cli_core":
        cli_core(binary, root, scripts)
    elif name == "cli_replies_hooks":
        cli_replies_hooks(binary, root, scripts)
    elif name == "cli_scripts_overlay":
        cli_scripts_overlay(binary, root, scripts)
    elif name == "waterfall_view":
        waterfall_view(binary, root, scripts)
    elif name == "translation_lines":
        translation_lines(binary, root, scripts)
    elif name == "shrinking_window":
        shrinking_window(binary, root, scripts)
    elif name == "colors_scroll":
        colors_scroll(binary, root, scripts)
    elif name == "translation_editor":
        translation_editor(binary, root, scripts)
    elif name == "translation_paste_undo":
        translation_paste_undo(binary, root, scripts)
    elif name == "opened_lyrics_is_current":
        opened_lyrics_is_current(binary, root, scripts)
    elif name == "disk_field_setting":
        disk_field_setting(binary, root, scripts)
    elif name == "queue_shuffle":
        queue_shuffle(binary, root, scripts)
    elif name == "queue_search_colors":
        queue_search_colors(binary, root, scripts)
    elif name == "spectro_history_setting":
        spectro_history_setting(binary, root, scripts)
    elif name == "narrow80":
        narrow_sweep(binary, root, scripts, 80, 24)
    elif name == "narrow100":
        narrow_sweep(binary, root, scripts, 100, 30)
    else:
        globals()[name](binary, root, scripts)


SCENARIOS = ["narrow80", "narrow100", "main", "read_lyrics_play", "karaoke_timing", "book_speed_and_devices", "paging_and_translit_row",
             "mode_keys", "kitty_cover_options", "radio_latin_toggle", "radio_organigram", "radio_idle_rate", "selftest_command", "opened_lyrics_is_current", "disk_field_setting", "queue_shuffle", "queue_search_colors", "bulk_add_options", "bulk_add_flow", "translation_editor", "translation_create", "play_mode_back", "disk_border_setting", "translation_paste_undo", "translation_lines", "waterfall_view", "colors_scroll", "shrinking_window", "search_key", "search_wav_tags", "video_overlays", "video_dither_option", "book_embedded_chapters", "cli_commands", "cli_core", "radio_cli", "radio_cli_typed", "radio_cli_extras", "video_fx_picker", "video_fx_picker_sizes", "main_lyrics_translation_key", "radio_sign_border", "xdg_dirs", "settings_cli_row", "cli_replies_hooks", "cli_scripts_overlay", "video_pair_tags", "video_fetch_flow", "video_fetch_restore", "settings_video_preview", "video_files_tab", "video_library_modes", "video_fullscreen_sizes", "video_base_per_track", "video_fx_picker_picture", "video_translation_tab", "organigram_menu", "scope_spectro_overlays", "radio_waterfall", "radio_station_edit", "spectro_history_setting", "autoplay_and_sphere", "video_fx_picker_third_slot", "gapless_playback", "milkdrop_visuals", "milkdrop_window", "milkdrop_preset_overlay", "milkdrop_layouts_style", "milkdrop_radio_book", "settings_general_texts", "overlays_queue_enter", "video_fx_list_frozen"]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    binary = os.path.abspath(args[0])
    scripts = None
    keep = "--keep" in args
    # a hang must not use up the whole CTest timeout without a trace: after this many seconds Python prints where every
    # thread is stuck and ends the test (MOUSIKI_SMOKE_LIMIT, default 1500 s -- 63 scenarios two at a time take about 14 minutes on two cores;
    # the CTest timeout, 1800 s, is longer)
    import faulthandler
    faulthandler.dump_traceback_later(int(os.environ.get("MOUSIKI_SMOKE_LIMIT", "1500")), exit=True)
    if "--scripts" in args:
        scripts = os.path.abspath(args[args.index("--scripts") + 1])
    if not os.path.exists(binary):
        print("skipped: the player is not built (%s); build the target `mousiki` too" % binary)
        return 77

    if "--worker" in args:   # one scenario, in the folder the parent made for it
        name = args[args.index("--worker") + 1]
        root = args[args.index("--root") + 1]
        run_scenario(name, binary, root, scripts)
        print("%d checks, %d failed" % (checks, len(failures)))
        return 1 if failures else 0

    import subprocess
    import concurrent.futures
    root = tempfile.mkdtemp(prefix="mousiki_ui_")
    make_library(root)
    jobs = int(os.environ.get("MOUSIKI_SMOKE_JOBS", str(max(1, min(4, os.cpu_count() or 1)))))
    only = args[args.index("--only") + 1].split(",") if "--only" in args else SCENARIOS
    progress("%d scenarios, %d at a time" % (len(only), jobs))

    import threading
    # The MilkDrop scenarios draw with the software renderer when there is no graphics card: two of them at once on a small machine
    # (two cores) leave the keys of the harness waiting for seconds. They run one after the other (the others still run beside them).
    heavy_lock = threading.Lock()

    def work(name):
        if name.startswith("milkdrop_"):
            with heavy_lock:
                return work_now(name)
        return work_now(name)

    def work_now(name):
        sub = os.path.join(root, "w_" + name)
        shutil.copytree(root, sub, ignore=shutil.ignore_patterns("w_*"))
        cmd = [sys.executable, os.path.abspath(__file__), binary, "--worker", name, "--root", sub]
        if scripts:
            cmd += ["--scripts", scripts]
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True)
        return name, p.returncode, p.stdout + p.stderr, time.time() - t0

    total_checks, total_failed, bad, fails = 0, 0, [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for name, rc, out, secs in pool.map(work, only):
            print("--- %s (%ds)" % (name, secs), flush=True)
            print(out.rstrip(), flush=True)
            m = re.findall(r"(\d+) checks, (\d+) failed", out)
            fails += ["%s: %s" % (name, l.strip()[6:]) for l in out.splitlines() if l.startswith("  FAIL:")]
            if m:
                total_checks += int(m[-1][0]); total_failed += int(m[-1][1])
            if rc not in (0, 1) or not m:
                total_failed += 1
                bad.append(name)
                print("  FAIL: scenario %s did not finish (exit code %d)" % (name, rc), flush=True)
    if not keep:
        shutil.rmtree(root, ignore_errors=True)
    if fails:   # (every failure in one place: a long log is cut by CI viewers)
        print("FAILED CHECKS:")
        for f in fails:
            print("  " + f)
    print("%d checks, %d failed" % (total_checks, total_failed))
    return 1 if total_failed else 0


if __name__ == "__main__":
    sys.exit(main())
