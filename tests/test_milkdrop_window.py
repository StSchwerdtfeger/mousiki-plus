#!/usr/bin/env python3
"""The MilkDrop window process (`mousiki --milkdrop-window`): feeds it a config packet and a picture through stdin like the player does and
checks the picture it shows (SDL's dummy video driver + software renderer, a PPM written by the test hook). Skips (77) without SDL2."""
import os, struct, subprocess, sys, tempfile, ctypes.util

def main():
    exe = sys.argv[1]
    if not (ctypes.util.find_library("SDL2-2.0") or ctypes.util.find_library("SDL2")):
        print("SKIP: SDL2 is not installed"); return 77
    tmp = tempfile.mkdtemp()
    shot = os.path.join(tmp, "shot.ppm")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", MOUSIKI_MILK_SHOT=shot,
               MOUSIKI_MILK_SHOT_FRAME="1", MOUSIKI_MILK_SHOT_EXIT="1", HOME=tmp, XDG_CONFIG_HOME=tmp, XDG_CACHE_HOME=tmp)
    p = subprocess.Popen([exe, "--milkdrop-window"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    def packet(t, payload): return struct.pack("<BI", t, len(payload)) + payload
    cfg = struct.pack("<Ii", 0x4B4C494D, 48000) + bytes([0]) + b"Test Station".ljust(96, b"\0")
    cfg = cfg.ljust(4 + 4 + 4 + 96, b"\0")   # (the struct is padded to 4 bytes)
    w, h = 64, 36
    rgb = bytearray()
    for y in range(h):
        for x in range(w):
            rgb += bytes([255 if x < w // 2 else 0, 0, 255 if x >= w // 2 else 0])   # left half red, right half blue
    try:
        p.stdin.write(packet(1, cfg)); p.stdin.write(packet(3, struct.pack("<II", w, h) + bytes(rgb))); p.stdin.flush()
        for _ in range(3):   # (the window may be slow to start; a few pictures in a row)
            p.stdin.write(packet(3, struct.pack("<II", w, h) + bytes(rgb))); p.stdin.flush()
        p.wait(timeout=30)
    except Exception as e:
        p.kill(); print("FAIL: " + repr(e)); return 1
    if not os.path.exists(shot):
        print("FAIL: the window wrote no picture (exit code %s)" % p.returncode); return 1
    data = open(shot, "rb").read()
    head, rest = data.split(b"\n255\n", 1)
    _, dims = head.split(b"\n", 1)
    ow, oh = map(int, dims.split())
    def px(x, y): i = (y * ow + x) * 3; return tuple(rest[i:i + 3])
    left, right = px(ow // 4, oh // 2), px(3 * ow // 4, oh // 2)
    ok = left[0] > 200 and left[2] < 60 and right[2] > 200 and right[0] < 60
    print(("PASS" if ok else "FAIL") + ": window %dx%d, left %s, right %s" % (ow, oh, left, right))
    return 0 if ok else 1

sys.exit(main())
