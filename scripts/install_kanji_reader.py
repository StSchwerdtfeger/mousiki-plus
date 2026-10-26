#!/usr/bin/env python3
"""Downloads pykakasi (and what it needs) from PyPI into a folder, for the optional kanji readings of Mousiki's Latin letters display.

usage: install_kanji_reader.py <target folder>        (the installer calls it with <install folder>\\pylib)

Why a script and not `pip install`: the Python that comes with the Windows installer is the "embeddable" build, which has no pip.
Only wheels from pypi.org are fetched, each checked against the SHA-256 that PyPI publishes, and unpacked (a wheel is a zip file).
pykakasi is licensed GPL-3.0-or-later and is NOT part of Mousiki: it is downloaded to your computer on request and run as a separate
program by kanji_reading.py. Exit code 0 = installed, 1 = failed (the message says why; Mousiki then simply goes without readings).
"""
import hashlib
import json
import os
import re
import sys
import sysconfig
import urllib.request
import zipfile

WANT = ["pykakasi"]
UA = {"User-Agent": "mousiki-kanji-installer"}


def norm(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return r.read()


def platform_tag():
    return sysconfig.get_platform().replace("-", "_").replace(".", "_").lower()   # win_amd64, win32, linux_x86_64 ...


def wheel_ok(fname):
    """Pure-Python wheels, or one built for this Python and platform."""
    m = re.match(r"^.+?-.+?(?:-\d[^-]*)?-([^-]+)-([^-]+)-([^-]+)\.whl$", fname)
    if not m:
        return False
    py, abi, plat = m.groups()
    if abi == "none" and plat == "any":
        return True
    cp = "cp%d%d" % sys.version_info[:2]
    mine = platform_tag()
    plats = plat.split(".")
    arch_ok = any(p == mine or (mine.startswith("linux_") and p.startswith(("manylinux", "musllinux")) and mine.split("_", 1)[1] in p)
                  or (mine == "macosx" and p.startswith("macosx")) for p in plats)
    return arch_ok and (py == cp or abi == "abi3" and py.startswith("cp3"))


def pick(name):
    info = json.loads(fetch("https://pypi.org/pypi/%s/json" % name))
    files = [f for f in info["urls"] if f["packagetype"] == "bdist_wheel" and wheel_ok(f["filename"]) and not f.get("yanked")]
    if not files:
        raise RuntimeError("no suitable wheel of %s for this Python" % name)
    files.sort(key=lambda f: (f["filename"].endswith("none-any.whl"), f["filename"]))   # a binary wheel for this machine first
    requires = []
    for r in info["info"].get("requires_dist") or []:
        if "extra ==" in r:
            continue
        mm = re.match(r"^([A-Za-z0-9_.-]+)", r)
        if mm and ("python_version" not in r or "<" not in r):
            requires.append(mm.group(1))
    return files[0], requires


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    target = os.path.abspath(sys.argv[1])
    os.makedirs(target, exist_ok=True)
    done, queue = set(), list(WANT)
    try:
        while queue:
            name = norm(queue.pop(0))
            if name in done:
                continue
            done.add(name)
            f, requires = pick(name)
            print("downloading %s" % f["filename"])
            data = fetch(f["url"])
            if hashlib.sha256(data).hexdigest() != f["digests"]["sha256"]:
                raise RuntimeError("%s: the checksum does not match what PyPI publishes" % f["filename"])
            tmp = os.path.join(target, f["filename"])
            with open(tmp, "wb") as fh:
                fh.write(data)
            with zipfile.ZipFile(tmp) as z:
                for n in z.namelist():   # (no entry may leave the target folder)
                    if os.path.isabs(n) or ".." in n.replace("\\", "/").split("/"):
                        raise RuntimeError("%s: unsafe path in the archive" % f["filename"])
                z.extractall(target)
            os.remove(tmp)
            queue.extend(requires)
    except Exception as e:   # noqa: BLE001 -- any failure just means "no readings"
        print("kanji reader not installed: %s" % e)
        return 1
    print("pykakasi installed into %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
