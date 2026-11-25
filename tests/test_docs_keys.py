#!/usr/bin/env python3
"""Every key of the example config.txt and radio_config.txt must be named in the user manual and in the help wizard.
A key may be named on its own, as a family (Prefix*) or, for the hotkeys, as the bare action name Xyz (the "Default values" chapters list them that way).
usage: test_docs_keys.py [repo root]"""
import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def read(*p):
    with open(os.path.join(root, *p), encoding="utf-8") as f:
        return f.read()


docs = {"manual": read("Mousiki_User_Manual", "Mousiki_User_Guide.md"), "wizard": read("Mousiki_Help_Wizard.txt")}


def named(text, key):
    if key in text:
        return True
    if any(key.startswith(w) for w in re.findall(r"([A-Za-z][A-Za-z0-9_]{3,})\*", text)):   # a family named as Prefix* (ColorOsci*, Box*)
        return True
    return key.startswith("HKey") and re.search(r"\b" + re.escape(key[4:]) + r"\b", text) is not None


missing = []
for cfg in ("config.txt", "radio_config.txt"):
    keys = [m.group(1) for m in (re.match(r"^([A-Za-z][A-Za-z0-9_]*)=", l) for l in read(cfg).splitlines()) if m]
    for k in keys:
        for name, text in docs.items():
            if not named(text, k):
                missing.append(f"{cfg}: {k} is not in the {name}")
if missing:
    print("\n".join(missing))
    print(f"{len(missing)} key(s) of the example configs are not documented")
    sys.exit(1)
print("every key of config.txt and radio_config.txt is in the manual and the wizard")
