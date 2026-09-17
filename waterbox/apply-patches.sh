#!/bin/sh
# Applies the numbered patches to the extern/xemu submodule and copies the
# driver sources in. Idempotent: a tree
# that already carries the whole series is left alone, a pristine one has it
# applied, and anything in between is an error that names the files.
#
# THE SERIES IS JUDGED AS A WHOLE (ported from chimera-core-rpcs3, where the old
# way stopped a build). Judging a patch at a time - does it apply, else does it
# reverse - cannot work once two patches touch the same place: on a fully
# patched tree the earlier one neither applies nor reverses. Depending on the
# script that either stopped the build on a tree with nothing wrong with it, or
# printed a warning that then hid real ones; and the cheaper tests some cores
# used instead - "the tree has changes", "one file holds a marker" - cannot see
# a tree where a single file was reverted by hand (git checkout -- file), which
# silently drops that file's share of several patches and builds anyway.
#
# So: what the series SHOULD leave behind is worked out on a scratch copy - the
# touched files as the submodule's HEAD has them, with every patch applied in
# order - and the working tree is compared with that.
# The three copied files are OURS (waterbox/) rather than patches, so the
# driver can be edited without regenerating diffs:
#   xemu-waterbox.c   -> ui/            (headless main + input + stubs)
#   monitor-null.c    -> hw/xbox/mcpx/apu/  (silent APU sink)
#   chimera-latency.c -> block/         (fixed-virtual-latency filter)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
# XEMU_TREE points it at another checkout: how this script is tested without
# touching the tree a build depends on
tree="${XEMU_TREE:-$root/extern/xemu}"

if [ "$(git -C "$tree" rev-parse --show-toplevel 2>/dev/null)" != "$(cd "$tree" 2>/dev/null && pwd -P)" ]; then
	echo "extern/xemu is not checked out; run:" >&2
	echo "  git -C $root submodule update --init --recursive extern/xemu" >&2
	exit 1
fi

# OUR sources, copied in on every run - whichever state the patches were in -
# so an edit to the driver reaches the build without touching the series.
copy_driver()
{
	cp "$here/xemu-waterbox.c" "$tree/ui/xemu-waterbox.c"
	cp "$here/xemu-savedata.c" "$tree/ui/xemu-savedata.c"
	cp "$here/default-eeprom.c" "$tree/ui/default-eeprom.c"
	cp "$here/guest-syscalls.c" "$tree/ui/guest-syscalls.c"
	cp "$here/monitor-null.c" "$tree/hw/xbox/mcpx/apu/monitor-null.c"
	cp "$here/chimera-latency.c" "$tree/block/chimera-latency.c"
	cp "$here/dsp-jit-null.c" "$tree/hw/xbox/mcpx/apu/dsp/dsp-jit-null.c"
	cp "$here/det-pow.c" "$tree/hw/xbox/mcpx/apu/det-pow.c"
	mkdir -p "$tree/hw/xbox/nv2a/pgraph/chimera-gl"
	cp "$here"/gl-shim/gloffscreen.h "$here"/gl-shim/gl-context.c \
	   "$here"/gl-shim/gl-bridge-cshim.cpp \
	   "$here"/generated-gl/gl-bridge-guest.cpp "$here"/generated-gl/gl-bridge-ops.h \
	   "$tree/hw/xbox/nv2a/pgraph/chimera-gl/"
	cp "$root/../chimera/extern/chimera-common-minibox/source/gl/gl-bridge.h" \
	   "$tree/hw/xbox/nv2a/pgraph/chimera-gl/" 2>/dev/null || \
	   cp "$here/gl-shim/gl-bridge.h" "$tree/hw/xbox/nv2a/pgraph/chimera-gl/"
	cp -r "$here/glad" "$tree/hw/xbox/nv2a/pgraph/chimera-gl/"
	mkdir -p "$tree/hw/xbox/mcpx/apu/samplerate"
	cp "$here"/samplerate/* "$tree/hw/xbox/mcpx/apu/samplerate/"
	echo "driver sources copied"
}

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# every file the series touches (a created or deleted file has /dev/null on one side)
cat "$root"/patches/*.patch | sed -n 's#^--- a/##p; s#^+++ b/##p' | sort -u > "$scratch/touched"

# the scratch copy: those files as HEAD has them (one the series creates is absent)
mkdir "$scratch/tree"
while IFS= read -r f; do
	if git -C "$tree" cat-file -e "HEAD:$f" 2>/dev/null; then
		mkdir -p "$scratch/tree/$(dirname "$f")"
		git -C "$tree" show "HEAD:$f" > "$scratch/tree/$f"
		# the mode travels too, or git apply remarks on every executable file
		case "$(git -C "$tree" ls-tree HEAD -- "$f")" in 100755*) chmod +x "$scratch/tree/$f" ;; esac
	fi
done < "$scratch/touched"

# pristine: the working tree still has HEAD's version of every touched file
pristine=1
while IFS= read -r f; do
	if [ -e "$scratch/tree/$f" ]; then
		cmp -s "$scratch/tree/$f" "$tree/$f" || { pristine=0; break; }
	elif [ -e "$tree/$f" ]; then
		pristine=0; break
	fi
done < "$scratch/touched"

# The series is tried on the scratch copy FIRST, whichever state the tree is in:
# a series that does not apply in order must be found out here, and not half
# way through applying it to the real tree.
for p in "$root"/patches/*.patch; do
	(cd "$scratch/tree" && git apply "$p") || {
		echo "the series does not apply to the submodule's HEAD at $(basename "$p") - was the submodule moved without rebasing the patches?" >&2
		exit 1
	}
done


if [ "$pristine" -eq 1 ]; then
	for p in "$root"/patches/*.patch; do
		git -C "$tree" apply "$p"
		echo "applied: $(basename "$p")"
	done
	copy_driver
	exit 0
fi

# not pristine: it must then be EXACTLY what the whole series leaves behind
wrong=0
while IFS= read -r f; do
	if [ -e "$scratch/tree/$f" ]; then
		cmp -s "$scratch/tree/$f" "$tree/$f" 2>/dev/null || { echo "not as the series leaves it: $f" >&2; wrong=1; }
	elif [ -e "$tree/$f" ]; then
		echo "the series deletes this, and it is there: $f" >&2; wrong=1
	fi
done < "$scratch/touched"

if [ "$wrong" -ne 0 ]; then
	echo "extern/xemu is partly patched. To start again from the submodule's HEAD:" >&2
	echo "  git -C extern/xemu reset --hard && git -C extern/xemu clean -fd && waterbox/apply-patches.sh" >&2
	echo "(that discards edits made in the tree - turn them into a patch first)" >&2
	exit 1
fi
echo "already applied: all $(ls "$root"/patches/*.patch | wc -l) patches"
copy_driver
