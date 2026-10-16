#!/usr/bin/env python3
"""Downloads the images that README.md / Changes_from_the_Original.md still load from GitHub ("user-attachments") into
readme_images/ and points the links at the local files, so every image of the README lives in the repository.

Run it once from the repository root (internet access needed), check the result, then commit readme_images/ together
with the two .md files:

    python3 tools/fetch_readme_images.py            # download + rewrite the links
    python3 tools/fetch_readme_images.py --dry-run  # only list what would be downloaded

It also moves preview.gif and mousiki_xy_mode_osci.gif from the repository root into readme_images/ (the README already
points there). Images that are already local are left alone; running it twice does nothing the second time.
"""
import os
import re
import shutil
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(ROOT, "readme_images")
FILES = ["README.md", "Changes_from_the_Original.md"]
# The pictures of the README's introduction get readable names; the README already uses these names.
KNOWN = {
    "95b5b4b0-a69b-42e5-a68b-09ac7c786314": "intro_main_ui",
    "3a04fa3e-a59b-423c-89be-d6dd284e06e8": "intro_radio_ui",
}
EXT = {"image/png": ".png", "image/jpeg": ".jpg", "image/gif": ".gif", "image/webp": ".webp", "image/svg+xml": ".svg"}
URL = re.compile(r"https://github\.com/user-attachments/assets/([0-9a-f-]{36})")


def fetch(asset_id):
    req = urllib.request.Request("https://github.com/user-attachments/assets/" + asset_id, headers={"User-Agent": "mousiki-readme"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read(), r.headers.get_content_type()


def main():
    dry = "--dry-run" in sys.argv
    os.makedirs(FOLDER, exist_ok=True)
    for gif in ("preview.gif", "mousiki_xy_mode_osci.gif"):
        src, dst = os.path.join(ROOT, gif), os.path.join(FOLDER, gif)
        if os.path.exists(src) and not os.path.exists(dst):
            print("move", gif, "-> readme_images/")
            if not dry:
                shutil.move(src, dst)
    # the intro pictures named in the README: download them even if the README already points at readme_images/
    for asset_id, name in KNOWN.items():
        if any(os.path.exists(os.path.join(FOLDER, name + e)) for e in EXT.values()):
            continue
        print("get", name)
        if not dry:
            data, ctype = fetch(asset_id)
            ext = EXT.get(ctype, ".png")
            with open(os.path.join(FOLDER, name + ext), "wb") as f:
                f.write(data)
            if ext != ".png":   # the README says .png: fix the extension there
                for md in FILES:
                    p = os.path.join(ROOT, md)
                    if os.path.exists(p):
                        t = open(p, encoding="utf-8").read().replace("readme_images/" + name + ".png", "readme_images/" + name + ext)
                        open(p, "w", encoding="utf-8").write(t)
    counter = 0
    for md in FILES:
        path = os.path.join(ROOT, md)
        if not os.path.exists(path):
            continue
        text = open(path, encoding="utf-8").read()
        prefix = "changes" if md.startswith("Changes") else "readme"
        for asset_id in dict.fromkeys(URL.findall(text)):
            counter += 1
            name = KNOWN.get(asset_id, "%s_%02d" % (prefix, counter))
            print("get", asset_id, "->", name)
            if dry:
                continue
            data, ctype = fetch(asset_id)
            fname = name + EXT.get(ctype, ".png")
            with open(os.path.join(FOLDER, fname), "wb") as f:
                f.write(data)
            text = text.replace("https://github.com/user-attachments/assets/" + asset_id, "readme_images/" + fname)
        if not dry:
            open(path, "w", encoding="utf-8").write(text)
    print("done" + (" (dry run)" if dry else ": commit readme_images/ and the .md files"))


if __name__ == "__main__":
    main()
