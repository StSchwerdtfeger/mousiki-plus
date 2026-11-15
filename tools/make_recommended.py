#!/usr/bin/env python3
"""
make_recommended.py -- writes milkdrop_recommended.txt, the list of recommended MilkDrop presets, from your favourites.

Mark the presets you like with [a] in the preset overlay (":" -> Preset Overlay); Mousiki keeps them, one full path a line, in
milkdrop_favourites.txt in its config folder (Linux ~/.config/mousiki, macOS ~/Library/Application Support/mousiki,
Windows %APPDATA%\\mousiki). This script turns that into a list of paths *inside the preset folder* (no copies of the presets):

    python3 tools/make_recommended.py --presets <preset folder> [--favourites FILE] [--out milkdrop_recommended.txt]

Commit the result next to README.md. The installer scripts (tools/milkdrop-unix.sh, tools/milkdrop-windows.ps1, so also packaging/
release.py and setup) put it beside the presets as recommended.txt; Mousiki shows it as the "Recommended" folder of the preset
overlay and looks every line up in the presets that are installed. (A user's own list: milkdrop_recommended.txt in the config folder,
the same format.) A favourite outside the preset folder is written as its file name only.
"""
import argparse, os, sys
from pathlib import Path

HEADER = """# Recommended MilkDrop presets -- shown as the "Recommended" folder at the top of Mousiki's preset overlay (":" -> Preset Overlay).
# One preset a line: its path inside the preset collection ("Fractal/some preset.milk") or just its file name (the extension and the
# case may be left out). Mousiki looks every line up in the presets that are installed; a line that fits nothing is skipped.
# Lines starting with # are comments. tools/make_recommended.py writes this file from your favourites.
"""

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
    ap.add_argument("--presets", type=Path, required=True, help="the preset folder the favourites come from")
    ap.add_argument("--out", type=Path, default=Path("milkdrop_recommended.txt"), help="the list to write (default: milkdrop_recommended.txt here)")
    a = ap.parse_args()
    fav = a.favourites or default_favourites()
    if not fav.is_file():
        sys.exit("no favourites file at %s" % fav)
    root = a.presets.resolve()
    lines, seen = [], set()
    for raw in fav.read_text(encoding="utf-8", errors="replace").splitlines():
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        p = Path(raw)
        try:
            entry = p.resolve().relative_to(root).as_posix()
        except ValueError:
            entry = p.name
            print("outside the preset folder, written by name only:", raw)
        if entry.lower() not in seen:
            seen.add(entry.lower())
            lines.append(entry)
    a.out.write_text(HEADER + "\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
    print("%d presets written to %s" % (len(lines), a.out))

if __name__ == "__main__":
    main()
