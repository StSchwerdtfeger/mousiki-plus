#!/usr/bin/env python3
"""The generated .inc files must match their sources: src/wizard_text.inc from Mousiki_Help_Wizard.txt (tools/wizard_to_inc.py) and
src/remote_page.inc from remote_app/web/index.html (tools/web_to_inc.py). A changed source whose .inc was not regenerated fails here.
usage: test_generated_files.py [repo root]"""
import os
import subprocess
import sys
import tempfile

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

jobs = [
    ("tools/wizard_to_inc.py", ["Mousiki_Help_Wizard.txt"], "src/wizard_text.inc"),
    ("tools/web_to_inc.py", ["remote_app/web/index.html"], "src/remote_page.inc"),
]
bad = 0
with tempfile.TemporaryDirectory() as tmp:
    for script, inputs, target in jobs:
        out = os.path.join(tmp, os.path.basename(target))
        cmd = [sys.executable, "-I", os.path.join(root, script)] + [os.path.join(root, i) for i in inputs] + [out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: %s did not run: %s" % (script, (r.stderr or r.stdout).strip()))
            bad += 1
            continue
        with open(out, "rb") as a, open(os.path.join(root, target), "rb") as b:
            if a.read().replace(b"\r\n", b"\n") != b.read().replace(b"\r\n", b"\n"):
                print("FAIL: %s is out of date: run  python3 %s" % (target, script))
                bad += 1
if bad:
    sys.exit(1)
print("generated files are up to date")
