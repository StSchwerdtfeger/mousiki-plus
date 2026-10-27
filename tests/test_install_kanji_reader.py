#!/usr/bin/env python3
"""Tests of scripts/install_kanji_reader.py without internet: a local web server hands out a tiny fake wheel.

  * right checksum            -> installed, exit 0
  * wrong checksum            -> nothing installed, the target folder is gone again, exit 3, the message names both checksums
  * damaged once, then right  -> the second try succeeds (exit 0)
  * a wheel with a path leaving the folder is refused (exit 1)

usage: test_install_kanji_reader.py <scripts folder>"""
import hashlib
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import zipfile


def make_wheel(evil=False):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("../evil.txt" if evil else "fakepkg/__init__.py", "x = 1\n")
    return buf.getvalue()


class Handler(http.server.BaseHTTPRequestHandler):
    files = {}     # path -> bytes
    damaged = {}   # path -> how many answers are still damaged

    def do_GET(self):
        data = self.files.get(self.path)
        if data is None:
            self.send_error(404)
            return
        if self.damaged.get(self.path, 0) > 0:
            self.damaged[self.path] -= 1
            data = b"\xff" + data[1:]
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


def run_case(scripts, port, name, wheel, sha, damaged=0):
    work = tempfile.mkdtemp(prefix="mousiki_kt_")
    try:
        shutil.copy(os.path.join(scripts, "install_kanji_reader.py"), work)
        Handler.files["/" + name] = wheel
        Handler.damaged["/" + name] = damaged
        with open(os.path.join(work, "kanji_pins.json"), "w") as fh:
            json.dump({"packages": {"fakepkg": {"file": name, "url": "http://127.0.0.1:%d/%s" % (port, name), "sha256": sha}}}, fh)
        target = os.path.join(work, "pylib")
        p = subprocess.run([sys.executable, "-I", os.path.join(work, "install_kanji_reader.py"), target], capture_output=True, text=True, timeout=60)
        return p.returncode, p.stdout + p.stderr, os.path.isdir(target) and bool(os.listdir(target)), os.path.exists(os.path.join(work, "evil.txt"))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    scripts = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    bad = []

    def check(cond, what):
        if not cond:
            bad.append(what)
            print("FAIL:", what)

    good = make_wheel()
    sha = hashlib.sha256(good).hexdigest()
    rc, out, installed, _ = run_case(scripts, port, "fakepkg-1.0-py3-none-any.whl", good, sha)
    check(rc == 0 and installed, "right checksum: installed (exit %d)" % rc)
    rc, out, installed, _ = run_case(scripts, port, "fakepkg-1.0-py3-none-any.whl", good, "0" * 64)
    check(rc == 3, "wrong checksum: exit code 3 (got %d)" % rc)
    check(not installed, "wrong checksum: nothing installed")
    check("0" * 64 in out and sha in out, "wrong checksum: the message names the expected and the received checksum")
    check("Mousiki works without kanji readings" in out, "wrong checksum: the message says what it means")
    rc, out, installed, _ = run_case(scripts, port, "fakepkg-1.0-py3-none-any.whl", good, sha, damaged=1)
    check(rc == 0 and installed, "damaged once: the second try installs (exit %d)" % rc)
    rc, out, installed, _ = run_case(scripts, port, "fakepkg-1.0-py3-none-any.whl", good, sha, damaged=2)
    check(rc == 3 and not installed, "damaged twice: refused with exit 3 (got %d)" % rc)
    evil = make_wheel(evil=True)
    rc, out, installed, leaked = run_case(scripts, port, "evil-1.0-py3-none-any.whl", evil, hashlib.sha256(evil).hexdigest())
    check(rc == 1 and not leaked, "a path leaving the folder is refused (exit %d)" % rc)
    srv.shutdown()
    print("%d problems" % len(bad) if bad else "all ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
