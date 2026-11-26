#!/usr/bin/env python3
"""tools/check_vendored.py: a check that could not be completed never reads as 'nothing behind upstream' (the weekly workflow
would close the open issue on it)."""
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "tools"))
import check_vendored as cv  # noqa: E402


def row(status, name="miniaudio"):
    return {"name": name, "have": "0.11.0", "upstream": "mackron/miniaudio", "latest": "0.11.2" if status != "error" else "", "url": "",
            "status": status, "error": "HTTP Error 403" if status == "error" else "", "source": "files"}


fail = []
good = cv.report([row("current")])
if "Nothing behind upstream." not in good or cv.failed_count([row("current")]) != 0:
    fail.append("all lookups fine: should say nothing is behind")
bad = cv.report([row("current"), row("error", "dr_libs")])
if "Nothing behind upstream." in bad or "incomplete" not in bad or cv.failed_count([row("current"), row("error")]) != 1:
    fail.append("a failed lookup must not read as 'nothing behind'")
both = cv.report([row("outdated"), row("unknown", "x")])
if "behind upstream" not in both or "incomplete" not in both:
    fail.append("outdated and failed together: both are said")
if fail:
    print("FAIL: " + "; ".join(fail))
    sys.exit(1)
print("ok")
