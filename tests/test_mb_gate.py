#!/usr/bin/env python3
"""scripts/mb_gate.py: separate processes share one MusicBrainz request clock (>= gap seconds between any two requests).
usage: test_mb_gate.py [scripts folder]"""
import os
import subprocess
import sys
import tempfile
import time

scripts = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
GAP = 0.4
child = ("import sys, time; sys.path.insert(0, %r); import mb_gate\n"
         "for _ in range(3):\n    mb_gate.wait(%r, sys.argv[1]); print('%%.3f' %% time.time(), flush=True)\n" % (scripts, GAP))
with tempfile.TemporaryDirectory() as tmp:
    stamp = os.path.join(tmp, "stamp")
    procs = [subprocess.Popen([sys.executable, "-I", "-c", child, stamp], stdout=subprocess.PIPE, text=True) for _ in range(3)]
    times = sorted(float(t) for p in procs for t in p.communicate()[0].split())
    gaps = [b - a for a, b in zip(times, times[1:])]
    if len(times) != 9 or min(gaps) < GAP - 0.05:
        print("FAIL: 3 processes x 3 requests, smallest gap %.3f s (want >= %.2f)" % (min(gaps) if gaps else -1, GAP))
        sys.exit(1)
    # an unusable lock file must not stop anybody: falls back to waiting inside the process
    t0 = time.time()
    r = subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, %r); import mb_gate; mb_gate.wait(%r, %r); mb_gate.wait(%r, %r)" % (scripts, GAP, tmp, GAP, tmp)], timeout=20)
    if r.returncode != 0 or time.time() - t0 < GAP - 0.05:
        print("FAIL: fallback when the stamp file cannot be opened")
        sys.exit(1)
print("ok: %d requests, smallest gap %.2f s" % (len(times), min(gaps)))
