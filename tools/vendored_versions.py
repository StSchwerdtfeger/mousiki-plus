"""Reads third_party/VENDORED.json and works out the version of every library from its own files (shared by
check_vendored.py and make_sbom.py). A library whose files carry no version number keeps the 'version' written in the JSON
(or 'unknown')."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def detect(lib):
    """The version found in the library's files, or '' when the files don't say."""
    spec = lib.get("detect")
    if not spec:
        return ""
    try:
        text = (ROOT / spec["file"]).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    parts = []
    for rx in spec["regexes"]:
        m = re.search(rx, text)
        if not m:
            return ""
        parts.append(m.group(1))
    return ".".join(parts) if len(parts) > 1 else parts[0]


def load():
    """The libraries of VENDORED.json with 'version' filled in and 'version_source' = 'files' | 'recorded' | 'none'."""
    libs = json.loads((ROOT / "third_party" / "VENDORED.json").read_text(encoding="utf-8"))["libraries"]
    for lib in libs:
        found = detect(lib)
        if found:
            lib["version"], lib["version_source"] = found, "files"
        elif lib.get("version", "unknown") != "unknown":
            lib["version_source"] = "recorded"
        else:
            lib["version"], lib["version_source"] = "unknown", "none"
    return libs
