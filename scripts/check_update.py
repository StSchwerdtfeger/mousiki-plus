"""Asks GitHub which release of Mousiki Plus is the newest one and prints {"ok": true, "tag": "v5.2.0", "url": "..."}.
With the argument yt-dlp/yt-dlp it asks for the newest yt-dlp release instead ({"ok": true, "tag": "2026.10.26", ...}).

Called by Mousiki at start-up (at most once a day; switch it off with UpdateCheck=false in config.txt). Only the public
releases page of the project is read; nothing about you or your music is sent. Uses only Python's standard library."""
import json
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# the projects this script may ask about (the argument is never put into the address unchecked)
REPOS = ("StSchwerdtfeger/mousiki-plus", "yt-dlp/yt-dlp")
repo = sys.argv[1] if len(sys.argv) > 1 else REPOS[0]
API = "https://api.github.com/repos/%s/releases/latest" % (repo if repo in REPOS else REPOS[0])


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def main():
    try:
        import lrc   # its http_get_json: system certificates, User-Agent, timeout
    except Exception:
        emit({"ok": False, "error": "MODULE_MISSING"})
    try:
        data = lrc.http_get_json(API, {}, timeout=8)
    except Exception as e:
        emit({"ok": False, "error": "NETWORK", "detail": str(e)[:200]})
    tag = str(data.get("tag_name", "")).strip()
    if not tag:
        emit({"ok": False, "error": "NO_RELEASE"})
    emit({"ok": True, "tag": tag, "url": str(data.get("html_url", ""))})


main()
