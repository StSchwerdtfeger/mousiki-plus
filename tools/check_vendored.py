#!/usr/bin/env python3
"""Compares the libraries shipped as source in third_party/ (third_party/VENDORED.json) with the newest upstream release.

  python tools/check_vendored.py                 prints a report (Markdown)
  python tools/check_vendored.py --out report.md also writes it to a file
  python tools/check_vendored.py --json          machine-readable result
  python tools/check_vendored.py --update-readme rewrites the dependency table in README.md (versions + newest upstream)

Exit code 0 always (it only reports); the number of outdated libraries is written to $GITHUB_OUTPUT as `outdated=N` so the
weekly workflow can open an issue. Uses only the standard library; GITHUB_TOKEN (if set) raises the API rate limit."""
import argparse
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vendored_versions  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
UA = "mousiki-vendored-watch (+https://github.com/StSchwerdtfeger/mousiki-plus)"


def get(url, accept="application/json"):
    headers = {"User-Agent": UA, "Accept": accept}
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = "Bearer " + token
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
        return r.read().decode("utf-8", errors="replace")


def parse_version(text):
    """'v0.11.25' / '3.1.2' / 'release-1.6.1' -> (0, 11, 25); () when there is no number."""
    m = re.search(r"(\d+(?:\.\d+)*)", text or "")
    return tuple(int(x) for x in m.group(1).split(".")) if m else ()


def newer(latest, current):
    a, b = parse_version(latest), parse_version(current)
    if not a or not b:
        return False
    n = max(len(a), len(b))
    return a + (0,) * (n - len(a)) > b + (0,) * (n - len(b))


def latest_release(repo):
    """The newest published release tag; repositories without releases: the highest tag."""
    try:
        data = json.loads(get("https://api.github.com/repos/%s/releases/latest" % repo))
        tag = data.get("tag_name", "")
        if tag:
            return tag, data.get("html_url", "https://github.com/%s/releases" % repo)
    except Exception:
        pass
    tags = json.loads(get("https://api.github.com/repos/%s/tags?per_page=100" % repo))
    best = max((t["name"] for t in tags if parse_version(t["name"])), key=parse_version, default="")
    return best, "https://github.com/%s/tags" % repo


def latest_stb(filename):
    """stb has no releases: the version is in the first lines of the header on the main branch."""
    text = get("https://raw.githubusercontent.com/nothings/stb/master/" + filename, accept="text/plain")
    m = re.search(r"-\s*v(\d+\.\d+)", "\n".join(text.splitlines()[:5]))
    return (m.group(1) if m else ""), "https://github.com/nothings/stb/blob/master/" + filename


def check():
    libs = vendored_versions.load()
    rows = []
    for lib in libs:
        row = {"name": lib["name"], "have": lib["version"], "upstream": lib["upstream"], "latest": "", "url": "", "status": "", "error": "",
               "source": lib["version_source"]}
        try:
            if lib.get("check") == "stb_header":
                row["latest"], row["url"] = latest_stb(lib.get("stb_file", ""))
            else:
                row["latest"], row["url"] = latest_release(lib["upstream"])
            if not row["latest"]:
                row["status"] = "unknown"
            elif lib["version"] == "unknown":
                row["status"] = "info"
            elif newer(row["latest"], lib["version"]):
                row["status"] = "outdated"
            else:
                row["status"] = "current"
        except Exception as e:
            row["status"] = "error"
            row["error"] = str(e)[:120]
        rows.append(row)
    return rows


def report(rows):
    out = ["| Library | In Mousiki | Newest upstream | |", "|---|---|---|---|"]
    mark = {"outdated": "**update available**", "current": "up to date", "info": "version of the copy not recorded", "unknown": "?", "error": "check failed"}
    for r in rows:
        latest = "[%s](%s)" % (r["latest"], r["url"]) if r["latest"] else "-"
        out.append("| %s ([%s](https://github.com/%s)) | %s | %s | %s |" % (r["name"], r["upstream"], r["upstream"], r["have"], latest,
                                                                          mark[r["status"]] + (" (%s)" % r["error"] if r["error"] else "")))
    n = sum(1 for r in rows if r["status"] == "outdated")
    manual = [r["name"] for r in rows if r["source"] != "files"]
    out.append("")
    if manual:
        out.append("No version number in the files of: %s. Only its newest upstream release is shown; compare by hand (see `third_party/README.md`).\n"
                   % ", ".join(manual))
    if n:
        out.append("%d librar%s behind upstream. What to do: read the upstream change log (security fixes first), replace the files of the "
                   "library in `third_party/` with the new release, build, run the tests, commit. Nothing else has to be edited: the version is read "
                   "from the files (see `third_party/README.md`)." % (n, "y is" if n == 1 else "ies are"))
    else:
        out.append("Nothing behind upstream.")
    return "\n".join(out) + "\n"


README_START = "<!-- vendored-table:start (generated by tools/check_vendored.py --update-readme; do not edit by hand) -->"
README_END = "<!-- vendored-table:end -->"


def readme_table(rows):
    """The 'Dependencies and security' table of the root README: the vendored libraries (version read from their files, newest
    upstream release looked up now) plus the hand-written rows of VENDORED.json for what is not vendored."""
    import datetime
    meta = {l["name"]: l for l in json.loads((ROOT / "third_party" / "VENDORED.json").read_text(encoding="utf-8"))["libraries"]}
    extra = json.loads((ROOT / "third_party" / "VENDORED.json").read_text(encoding="utf-8")).get("readme_extra_rows", [])
    today = datetime.date.today().isoformat()
    out = ["| Component | Used for | Version here | Newest upstream (checked %s) |" % today, "|---|---|---|---|"]
    for r in rows:
        m = meta[r["name"]]
        have = r["have"] if r["have"] != "unknown" else "vendored copy (no version marker)"
        if r["status"] in ("error", "unknown"):
            newest = "?"
        elif r["status"] == "outdated":
            newest = "**%s** (update available)" % r["latest"].lstrip("v")
        else:
            newest = r["latest"].lstrip("v")
        out.append("| %s | %s | %s | %s |" % (m.get("readme_name", r["name"]), m.get("readme_use", m.get("use", "")), have, newest))
    for e in extra:
        out.append("| " + " | ".join(e) + " |")
    return "\n".join(out)


def update_readme(rows):
    path = ROOT / "README.md"
    text = path.read_text(encoding="utf-8")
    a, b = text.find(README_START), text.find(README_END)
    if a < 0 or b < a:
        sys.stderr.write("README.md has no vendored-table markers\n")
        return 1
    if any(r["status"] == "error" for r in rows):
        sys.stderr.write("the upstream lookup failed for some libraries (no network?): README.md not changed\n")
        return 1
    path.write_text(text[:a] + README_START + "\n" + readme_table(rows) + "\n" + text[b:], encoding="utf-8")
    print("README.md: dependency table updated")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--update-readme", action="store_true", help="rewrite the dependency table in README.md (needs the network)")
    ap.add_argument("--out")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    rows = check()
    if a.update_readme:
        return update_readme(rows)
    text = json.dumps(rows, indent=2) if a.json else report(rows)
    sys.stdout.write(text)
    if a.out:
        Path(a.out).write_text(report(rows), encoding="utf-8")
    gh = os.environ.get("GITHUB_OUTPUT")
    if gh:
        with open(gh, "a", encoding="utf-8") as f:
            f.write("outdated=%d\n" % sum(1 for r in rows if r["status"] == "outdated"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
