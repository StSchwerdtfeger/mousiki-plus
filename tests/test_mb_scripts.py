#!/usr/bin/env python3
"""Every MusicBrainz request of the helper scripts goes through mb_gate.wait() (one request per second for all processes together); the
Cover Art Archive does not. Offline: the network is a fake.
usage: test_mb_scripts.py [scripts folder]"""
import io
import os
import sys
import urllib.request

scripts = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
sys.path.insert(0, scripts)
import mb_gate  # noqa: E402
import fetch_cover  # noqa: E402
import fetch_meta  # noqa: E402
import lookup_artist_name  # noqa: E402

calls = []
mb_gate.wait = lambda *a, **k: calls.append(1)


class Fake(io.BytesIO):
    headers = {"Content-Type": "application/json"}

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


urllib.request.urlopen = lambda req, timeout=None: Fake(b'{"releases": [], "artists": []}')
fail = []


def expect(name, run, want):
    before = len(calls)
    try:
        run()
    except Exception:
        pass   # (what the fake answer makes of the rest does not matter here)
    got = len(calls) - before
    if got != want:
        fail.append("%s: %d waits, wanted %d" % (name, got, want))


expect("fetch_cover: MusicBrainz", lambda: fetch_cover.get("https://musicbrainz.org/ws/2/release/?query=x&fmt=json"), 1)
expect("fetch_cover: Cover Art Archive", lambda: fetch_cover.get("https://coverartarchive.org/release/abc"), 0)
expect("fetch_meta: recording", lambda: fetch_meta.mb_releases("00000000-0000-0000-0000-000000000000"), 1)
expect("lookup_artist_name", lambda: lookup_artist_name.get("https://musicbrainz.org/ws/2/artist/?query=x&fmt=json"), 1)
if fail:
    print("FAIL: " + "; ".join(fail))
    sys.exit(1)
print("ok")
