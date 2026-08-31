#!/bin/sh
# Applies the numbered patches to the extern/xemu submodule and copies the
# driver sources in. Idempotent: a tree that already carries the changes is
# left alone; anything else is an error worth seeing.
#
# The three copied files are OURS (waterbox/) rather than patches, so the
# driver can be edited without regenerating diffs:
#   xemu-waterbox.c   -> ui/            (headless main + input + stubs)
#   monitor-null.c    -> hw/xbox/mcpx/apu/  (silent APU sink)
#   chimera-latency.c -> block/         (fixed-virtual-latency filter)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
xemu="$root/extern/xemu"

for p in "$root"/patches/*.patch; do
	if git -C "$xemu" apply --check "$p" 2>/dev/null; then
		git -C "$xemu" apply "$p"
		echo "applied: $(basename "$p")"
	elif git -C "$xemu" apply --reverse --check "$p" 2>/dev/null; then
		echo "already applied: $(basename "$p")"
	else
		echo "NEITHER applies nor reverses: $(basename "$p")" >&2
		exit 1
	fi
done

cp "$here/xemu-waterbox.c" "$xemu/ui/xemu-waterbox.c"
cp "$here/guest-syscalls.c" "$xemu/ui/guest-syscalls.c"
cp "$here/monitor-null.c" "$xemu/hw/xbox/mcpx/apu/monitor-null.c"
cp "$here/chimera-latency.c" "$xemu/block/chimera-latency.c"
cp "$here/dsp-jit-null.c" "$xemu/hw/xbox/mcpx/apu/dsp/dsp-jit-null.c"
echo "driver sources copied"
