#!/usr/bin/env python3
"""
make_projectm_source.py -- builds the "corresponding source" package of the projectM build Mousiki ships (LGPL-2.1, section 6).

The MilkDrop visuals use projectM, built UNCHANGED from the pinned commits below by tools/milkdrop-windows.ps1 and
tools/milkdrop-unix.sh. This script downloads exactly those two source archives, checks every one against its pinned checksum (the
same sums the build scripts check: over the files inside), and packs them with a short README into

    projectm-source-<first 8 characters of the projectM commit>.zip

which belongs next to the installers on every GitHub release (the workflow .github/workflows/milkdrop-source.yml does it).

    python3 tools/make_projectm_source.py [--out DIR]

Keep the pins in step with tools/milkdrop-unix.sh, tools/milkdrop-windows.ps1 and THIRD_PARTY_LICENSES.txt.
"""
import argparse, hashlib, io, sys, urllib.request, zipfile
from pathlib import Path

PINS = [
    # (name, commit, the "manifest" checksum the build scripts check, licence)
    ("projectm", "eae22de40144ee8ded92f3c04b93a4c2f82adcc0", "72181d0f93696f3a6e0a2f5b800c649f2ba9c30d6a16f5af0391cb6ddd04ef75", "LGPL-2.1"),
    ("projectm-eval", "22fb0cfd8f2dfbcd2b68f2443e7f44e19b32c09a", "b3b2e8a6f5548c1fe3d61bc1a996807a2f035b77400e94b91271401e3a179385", "MIT"),
]

README = """projectM source code for Mousiki Plus
=====================================

Mousiki Plus shows its MilkDrop visuals with projectM 4.2 (https://github.com/projectM-visualizer/projectm, LGPL-2.1).
The libraries that come with Mousiki (projectM-4 and projectM-4-playlist: .dll, .so, .dylib) are built UNCHANGED from the
source in this package. This is the corresponding source code (LGPL-2.1, section 6).

{files}
Build it yourself (any other build of projectM 4.2 can replace the shipped libraries: Mousiki loads them at run time and is
not linked against them):
  - Linux / macOS:  tools/milkdrop-unix.sh      (in the Mousiki source; builds exactly these two archives)
  - Windows:        tools/milkdrop-windows.ps1  (the same)
  projectm-eval goes where its git submodule would be (projectm/vendor/projectm-eval); cmake -DENABLE_PLAYLIST=ON.

The licence texts are inside the archives (projectm: COPYING / LICENSE.txt; projectm-eval: LICENSE.md) and, for the shipped
packages, in app/milkdrop/ (LICENSE-projectM.txt, LICENSE-projectm-eval.md, LICENSE-hlslparser.txt).
Mousiki Plus: https://github.com/StSchwerdtfeger/mousiki-plus   (see THIRD_PARTY_LICENSES.txt)
"""

def manifest(data):
    """The checksum of tools/milkdrop-unix.sh (zip_manifest) and tools/milkdrop-windows.ps1: one SHA-256 over '<sha256 of the
    file> <path inside the archive's top folder>' for every file, ordered by path. The archive's own bytes are not used (GitHub
    may pack the same files differently); any changed file changes it."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        fs = {}
        for info in z.infolist():
            n = info.filename
            if n.endswith('/') or n.find('/') < 0:
                continue
            fs[n[n.find('/') + 1:]] = info
        lines = []
        for n in sorted(fs, key=lambda t: t.encode('utf-16-be')):
            lines.append('%s %s\n' % (hashlib.sha256(z.read(fs[n])).hexdigest(), n))
    return hashlib.sha256(''.join(lines).encode('utf-8')).hexdigest()

def fetch(name, commit, want):
    url = "https://github.com/projectM-visualizer/%s/archive/%s.zip" % (name, commit)
    print("downloading", url, flush=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "mousiki-source-offer"}), timeout=120) as r:
        data = r.read()
    try:
        got = manifest(data)
    except zipfile.BadZipFile:
        got = "(not a zip)"
    if got != want:
        sys.exit("%s: files checksum is %s, expected %s -- the archive on the server changed; find out why and update the pin "
                 "(tools/milkdrop-unix.sh, tools/milkdrop-windows.ps1, here, THIRD_PARTY_LICENSES.txt)" % (name, got, want))
    return data

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".", help="where the zip goes (default: here)")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    dest = out / ("projectm-source-%s.zip" % PINS[0][1][:8])
    lines = []
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_STORED) as z:
        for name, commit, want, lic in PINS:
            data = fetch(name, commit, want)
            z.writestr("%s-%s.zip" % (name, commit), data)
            lines.append("  %s-%s.zip   %s   commit %s   files checksum %s" % (name, commit, lic, commit, want))
        z.writestr("README.txt", README.format(files="Files:\n" + "\n".join(lines) + "\n"))
    print("wrote", dest, "(%d bytes)" % dest.stat().st_size)

if __name__ == "__main__":
    main()
