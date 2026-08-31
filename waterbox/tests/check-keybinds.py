#!/usr/bin/env python3
# Asserts that the bindings a core package ships became the frontend's defaults for its controller.
#
# Usage: check-keybinds.py <config written by EmuHawk> <package default_keybinds.json> <controller>
import json
import sys

cfg = json.load(open(sys.argv[1]))
shipped = json.load(open(sys.argv[2]))
name = sys.argv[3]

want = shipped["AllTrollers"][name]
got = cfg.get("AllTrollers", {}).get(name)
if got is None:
    sys.exit(f"the frontend has no bindings at all for '{name}'")
differing = {k: (v, got.get(k)) for k, v in want.items() if got.get(k) != v}
if differing:
    sys.exit(f"bindings differ from the package's: {differing}")

wantAnalog = shipped.get("AllTrollersAnalog", {}).get(name)
nAnalog = 0
if wantAnalog is not None:
    gotAnalog = cfg.get("AllTrollersAnalog", {}).get(name)
    if gotAnalog is None:
        sys.exit(f"the frontend has no analog bindings for '{name}'")
    for axis, bind in wantAnalog.items():
        gb = gotAnalog.get(axis)
        if gb is None:
            sys.exit(f"analog bind for '{axis}' was not adopted")
        for field, v in bind.items():
            got = gb.get(field)
            # A number makes the round trip through the frontend as a 32-bit
            # float, so a deadzone shipped as 0.1 comes back as 0.100000001.
            # That is the same number, and a gate that called it a difference
            # would be testing the width of a float rather than whether the
            # package's bindings were adopted.
            if isinstance(v, float) and isinstance(got, (int, float)):
                if abs(float(got) - v) > 1e-6:
                    sys.exit(f"analog bind '{axis}.{field}' differs: shipped {v!r}, config {got!r}")
            elif got != v:
                sys.exit(f"analog bind '{axis}.{field}' differs: shipped {v!r}, config {got!r}")
    nAnalog = len(wantAnalog)

wantAF = shipped.get("AllTrollersAutoFire", {}).get(name)
if wantAF is not None:
    gotAF = cfg.get("AllTrollersAutoFire", {}).get(name)
    if gotAF != wantAF:
        sys.exit(f"autofire bindings differ from the package's: shipped {wantAF}, config {gotAF}")
print(f"{name}: {len(want)} bindings and {nAnalog} analog bindings adopted from the package")
