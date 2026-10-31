#!/usr/bin/env python3
"""Downloads pykakasi (and what it needs) from PyPI into a folder, for the optional kanji readings of Mousiki's Latin letters display.

usage: install_kanji_reader.py <target folder>        (the installer calls it with <install folder>\\pylib)

Why a script and not `pip install`: the Python that comes with the Windows installer is the "embeddable" build, which has no pip.
The files are the exact ones listed in kanji_pins.json (next to this script; made by `release.py pin`): pure-Python wheels, so
the same four files for every platform. Each is checked against its pinned SHA-256 and any other file is refused. Without that
file the newest wheels are taken from pypi.org instead, each checked only against the hash PyPI publishes (a warning says so).
A wheel is a zip file; it is simply unpacked.
pykakasi is licensed GPL-3.0-or-later and is NOT part of Mousiki: it is downloaded to your computer on request and run as a separate
program by kanji_reading.py. Exit code 0 = installed, 1 = failed (the message says why; Mousiki then simply goes without readings), 3 = a checksum did not match
(a damaged download is tried twice; nothing is installed then).
"""
import hashlib
import json
import os
import re
import shutil
import sys
import sysconfig
import urllib.request
import zipfile

WANT = ["pykakasi", "janome"]   # pykakasi reads the kanji; janome (Apache-2.0, with its own dictionary) cuts Japanese into words, for the spaces
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


def load_pins():
    """{package: {url, sha256, file}} from kanji_pins.json next to this script, or None."""
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "kanji_pins.json"), encoding="utf-8") as fh:
            pk = json.load(fh).get("packages")
        return pk if pk else None
    except (OSError, ValueError):
        return None


def unpack(data, name, target):
    tmp = os.path.join(target, name)
    with open(tmp, "wb") as fh:
        fh.write(data)
    with zipfile.ZipFile(tmp) as z:
        for n in z.namelist():   # (no entry may leave the target folder)
            if os.path.isabs(n) or ".." in n.replace("\\", "/").split("/"):
                raise RuntimeError("%s: unsafe path in the archive" % name)
        z.extractall(target)
    os.remove(tmp)


class ChecksumError(RuntimeError):
    """A download that is not the file that was pinned (or not the one PyPI publishes)."""


def fetch_checked(url, want, label, kind="pinned", tries=2):
    """Downloads `url` and compares its SHA-256 with `want`. A damaged download is tried once more; a second mismatch
    stops everything with a message that says what was expected, what came, and what to do."""
    got = ""
    for attempt in range(tries):
        data = fetch(url)
        got = hashlib.sha256(data).hexdigest()
        if got == want.lower():
            return data
        if attempt + 1 < tries:
            print("%s: the checksum does not match (download damaged?) -- trying once more" % label)
    raise ChecksumError(
        "%s: the SHA-256 checksum does not match.\n"
        "  expected (%s): %s\n"
        "  received:        %s\n"
        "  address:         %s\n"
        "  Nothing was installed. A damaged download usually fixes itself: try again later. If it happens again, the file on the\n"
        "  server is not the one this version of Mousiki was built with: do not install it by hand, update Mousiki instead\n"
        "  (or report it at github.com/StSchwerdtfeger/mousiki-plus/issues). Mousiki works without kanji readings.\n"
        "  Check: mousiki --selftest-fetch --only pins --verify-pins" % (label, kind, want.lower(), got, url))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    target = os.path.abspath(sys.argv[1])
    existed = os.path.isdir(target)
    os.makedirs(target, exist_ok=True)
    pins = load_pins()
    if pins:   # the pinned files, nothing else: all are downloaded and checked first, only then is anything unpacked
        try:
            files = []
            for name, pin in sorted(pins.items()):
                print("downloading %s (pinned)" % pin["file"])
                files.append((pin["file"], fetch_checked(pin["url"], pin["sha256"], pin["file"])))
            for fname, data in files:
                unpack(data, fname, target)
        except ChecksumError as e:
            print("kanji reader NOT installed -- checksum failure\n%s" % e)
            if not existed:
                shutil.rmtree(target, ignore_errors=True)
            return 3
        except Exception as e:   # noqa: BLE001 -- any failure just means "no readings"
            print("kanji reader not installed: %s" % e)
            if not existed:
                shutil.rmtree(target, ignore_errors=True)
            return 1
        print("pykakasi installed into %s" % target)
        return 0
    print("warning: kanji_pins.json is missing -- the newest files from pypi.org are used (checked against the hash PyPI publishes only)")
    done, queue = set(), list(WANT)
    try:
        while queue:
            name = norm(queue.pop(0))
            if name in done:
                continue
            done.add(name)
            f, requires = pick(name)
            print("downloading %s" % f["filename"])
            data = fetch_checked(f["url"], f["digests"]["sha256"], f["filename"], kind="published by PyPI")
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
    except ChecksumError as e:
        print("kanji reader NOT installed -- checksum failure\n%s" % e)
        return 3
    except Exception as e:   # noqa: BLE001 -- any failure just means "no readings"
        print("kanji reader not installed: %s" % e)
        return 1
    print("pykakasi installed into %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
