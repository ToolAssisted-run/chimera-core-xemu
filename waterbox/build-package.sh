#!/bin/sh
# Builds the xemu waterbox core package and installs it into a chimera
# checkout as build/Cores/xemu.chimeraCore.
#
# A package is core.wbx (fixed name) + waterbox.config + default_keybinds.json,
# loaded through chimera's one built-in generic adapter. Unlike every other
# core, the guest is built by xemu's OWN configure/meson (setup-guest.sh);
# core.wbx is the qemu-system-i386 it links.
#
# Usage: ./build-package.sh [-r <chimera root>] [-m <miniBox dir>]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
chimera_root=""
minibox=""
while getopts "r:m:" opt; do
	case "$opt" in
		r) chimera_root="$OPTARG" ;;
		m) minibox="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass -r <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"
# miniBox normally lives inside the chimera checkout; -m is for a checkout
# that keeps it somewhere else (CI puts chimera under a path of its own).
mb="${minibox:-$chimera_root/extern/chimera-common-minibox}"
mb="$(cd "$mb" && pwd)"

# the guest, via xemu's own meson cross build
[ -f "$root/build/qemu-guest/build.ninja" ] || sh "$here/setup-guest.sh" -m "$mb"
ninja -C "$root/build/qemu-guest" qemu-system-i386
cp "$root/build/qemu-guest/qemu-system-i386" "$root/build/core.wbx"
sh "$mb/source/guest/check-wbx.sh" "$root/build/core.wbx"

staging="$root/build/package-staging"
rm -rf "$staging"
mkdir -p "$staging"
cp "$root/build/core.wbx" "$staging/core.wbx"
cp "$here/waterbox.config" "$staging/waterbox.config"
cp "$here/default_keybinds.json" "$staging/default_keybinds.json"
# The core-declared file form for the project wizard (slots, cardinality,
# formats, tooltips) - the frontend renders it, this file decides it.
cp "$here/file_slots.json" "$staging/file_slots.json"
# the terms travel with the binary: this package may be downloaded on its
# own, and the emulator inside it is somebody else's work under somebody
# else's licence (see waterbox/package-licenses.json)
python3 "$mb/source/guest/package-licenses.py" "$root" "$staging"

# ---- version (see chimera docs: commit-as-version, stamped by CD) ----
core_version="${CORE_VERSION:-}"
if [ -z "$core_version" ]; then
	if commit="$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null)"; then
		git -C "$root" diff --quiet HEAD 2>/dev/null || commit="$commit-dirty"
		core_version="$commit+local"
	else
		core_version="unversioned+local"
	fi
fi
python3 - "$staging/waterbox.config" "$core_version" <<'PYVER'
import json, sys
path, version = sys.argv[1], sys.argv[2]
with open(path) as f:
    cfg = json.load(f)
cfg["version"] = version
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PYVER

# ---- provenance: what built this exact package (inputs only) ----
gccver="$(gcc -dumpfullversion)"
musl_version="$(cat "$mb/extern/musl/VERSION" 2>/dev/null || echo unknown)"
binutils_version="$(ld --version | head -1 | grep -o '[0-9][0-9.]*$' || echo unknown)"
os_id="$(. /etc/os-release 2>/dev/null && printf '%s %s' "${ID:-unknown}" "${VERSION_ID:-}" || echo unknown)"
guest_kit="$(git -C "$mb" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
xemu_pin="$(git -C "$root/extern/xemu" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
python3 - "$staging/build.json" <<PYPROV
import json, subprocess
def git(*args, default="unknown"):
    try:
        return subprocess.run(["git", "-C", "$root", *args], capture_output=True,
                              text=True, check=True).stdout.strip()
    except Exception:
        return default
json.dump({
    "version": "$core_version",
    "source": {"commit": git("rev-parse", "HEAD"),
               "origin": git("config", "--get", "remote.origin.url", default=""),
               "dirty": "-dirty" in "$core_version"},
    "toolchain": {"compiler": "gcc $gccver", "binutils": "$binutils_version",
                  "target": "x86_64-linux-musl", "musl": "$musl_version"},
    "guestKit": {"name": "miniBox", "commit": "$guest_kit"},
    "upstream": {"name": "xemu", "pin": "$xemu_pin"},
    "builtOn": "$os_id",
}, open("$staging/build.json", "w"), indent=2, sort_keys=True)
PYPROV

cores_dir="$chimera_root/build/Cores"
mkdir -p "$cores_dir"
zip_path="$cores_dir/xemu.chimeraCore"
rm -f "$zip_path"
# deterministic packaging: sorted entries, fixed timestamp/permissions, pinned
# compression - the package's SHA1 is the core's identity (movies cite it)
python3 - "$staging" "$zip_path" <<'PYEOF'
import hashlib, os, sys, zipfile

staging, zip_path = sys.argv[1], sys.argv[2]
FIXED_DATE = (1980, 1, 1, 0, 0, 0)

def write_package(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, dirs, files in os.walk(staging):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(root, name)
                info = zipfile.ZipInfo(os.path.relpath(full, staging), date_time=FIXED_DATE)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o644 << 16
                with open(full, "rb") as f:
                    z.writestr(info, f.read())

write_package(zip_path)
sha1 = hashlib.sha1(open(zip_path, "rb").read()).hexdigest()
print(f"{zip_path}\n  sha1 {sha1}")
PYEOF
