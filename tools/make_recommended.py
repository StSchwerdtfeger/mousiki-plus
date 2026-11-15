#!/usr/bin/env python3
"""
make_recommended.py -- copies your MilkDrop favourites into a "Recommended" folder of the preset collection.

Mark the presets you like with [a] in the preset overlay (":" -> Preset Overlay); Mousiki keeps them, one full path a line, in
milkdrop_favourites.txt in its config folder (Linux ~/.config/mousiki, macOS ~/Library/Application Support/mousiki or the XDG
folder, Windows %APPDATA%\\mousiki).
This script copies those preset files into <preset folder>/Recommended, which the preset overlay then shows as a normal folder.
Packages ship that folder with the presets: put the folder into milkdrop/presets/ before the installer is built.

    python3 tools/make_recommended.py [--favourites FILE] --presets PRESET_FOLDER [--name Recommended] [--dry-run]

Files keep their names (a name that already exists with other content gets a number). Nothing is deleted or moved.
"""
import argparse, filecmp, os, shutil, sys
from pathlib import Path

def default_favourites():
    cands = []
    xdg = os.environ.get("XDG_CONFIG_HOME")
    cands.append(Path(xdg) / "mousiki" if xdg else Path.home() / ".config" / "mousiki")
    cands.append(Path.home() / "Library" / "Application Support" / "mousiki")
    if os.environ.get("APPDATA"):
        cands.append(Path(os.environ["APPDATA"]) / "mousiki")
    for c in cands:
        if (c / "milkdrop_favourites.txt").is_file():
            return c / "milkdrop_favourites.txt"
    return cands[0] / "milkdrop_favourites.txt"

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--favourites", type=Path, default=None, help="the favourites file (default: milkdrop_favourites.txt in Mousiki's config folder)")
    ap.add_argument("--presets", type=Path, required=True, help="the preset folder the Recommended folder goes into")
    ap.add_argument("--name", default="Recommended", help="name of the folder (default: Recommended)")
    ap.add_argument("--dry-run", action="store_true", help="only list what would be copied")
    a = ap.parse_args()
    fav = a.favourites or default_favourites()
    if not fav.is_file():
        sys.exit("no favourites file at %s" % fav)
    dest = a.presets / a.name
    done = skipped = 0
    for line in fav.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        src = Path(line)
        if not src.is_file():
            print("missing:", src)
            skipped += 1
            continue
        target = dest / src.name
        n = 2
        while target.exists() and not filecmp.cmp(src, target, shallow=False):
            target = dest / ("%s (%d)%s" % (src.stem, n, src.suffix))
            n += 1
        if target.exists():
            continue   # the same file is there already
        print(("would copy " if a.dry_run else "copy ") + str(src) + "  ->  " + str(target))
        if not a.dry_run:
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)
        done += 1
    print("%d copied, %d missing" % (done, skipped))

if __name__ == "__main__":
    main()
