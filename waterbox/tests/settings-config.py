#!/usr/bin/env python3
# Writes a copy of a miniHawk config with the given core sync settings applied - the same shape the
# settings dialog (and a movie header) stores: a name -> value map under the adapter's type name.
#
# Usage: settings-config.py <source config> <output config> <settings JSON> [<firmware JSON>]
# The optional firmware map is {"<decl id>": "<file path>"} - the same shape the
# Firmware window stores, keyed under this core's name.
import json
import sys

cfg = json.load(open(sys.argv[1]))
cfg.setdefault("CoreSettings", {})["Chimera.Emulation.Common.Waterbox.WaterboxCore"] = {
    "Values": json.loads(sys.argv[3])
}
if len(sys.argv) > 4:
    fw = cfg.setdefault("CoreFirmware", {})
    for decl_id, path in json.loads(sys.argv[4]).items():
        fw["xemu/" + decl_id] = path
# Pick this core explicitly: an install can hold several packages claiming the Xbox system, and --core only LOADS a package, it does not choose it.
cfg.setdefault("DefaultCores", {})["XBOX"] = "xemu"
json.dump(cfg, open(sys.argv[2], "w"), indent=2)
