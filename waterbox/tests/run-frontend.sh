#!/bin/bash
# The frontend half of the gate: load the xemu package in Chimera (under Mono,
# on a private Xvfb display), boot the Xbox for a fixed number of frames with
# nothing pressed, and require the machine's RAM to be byte-identical to the
# sandbox reference (run-wbx, which the core gate already proved equal to the
# native build). Then prove the package's keybinds become the frontend's
# defaults, and - when a GPU bridge is on offer - that the frontend's machine
# with a GPU matches the reference with the same driver and the screen is not
# black.
#
# It needs the same content the core gate needs: the MCPX boot ROM, a flash
# BIOS and a hard disk image ($XBOX_FW_DIR, laid out as the core gate expects),
# and optionally a disc ($XBOX_DVD_PATH). Without them it reports SKIP.
#
# Usage: ./run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=600
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/xemu.chimeraCore"
runwbx="$root/build/run-wbx"
wbx="$root/build/qemu-guest/qemu-system-i386"
[ -f "$emu_exe" ] || { echo "Chimera not built: $emu_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$runwbx" ] || { echo "run-wbx not built (waterbox/run-gate.sh builds it)" >&2; exit 1; }

ok=0
failed=0
skipped=0
report() {
	printf "%-28s %-9s %s\n" "$1" "$2" "$3"
	case "$2" in
		PASS) ok=$((ok + 1)) ;;
		SKIP) skipped=$((skipped + 1)) ;;
		*) failed=$((failed + 1)) ;;
	esac
}
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

fw="${XBOX_FW_DIR:-$HOME/xbox-roms/Xbox BIOS}"
mcpx="$fw/MCPX Boot ROM/mcpx_1.0.bin"
bios="$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin"
hdd="$fw/Hard Disk/xbox_hdd.qcow2"
disc="${XBOX_DVD_PATH:-}"

if [ ! -f "$mcpx" ] || [ ! -f "$bios" ] || [ ! -f "$hdd" ]; then
	report "boot:frontend" SKIP "needs mcpx, bios and hdd under \$XBOX_FW_DIR"
	report "gpu:frontend" SKIP "same"
	report "keybinds" SKIP "would prove the package's bindings become the frontend's"
	echo
	echo "$ok ok, $failed failed, $skipped skipped"
	exit 0
fi

work="$here/work"
mkdir -p "$work"

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

SLICE=1048576

# The frontend resolves firmware through its store, keyed by core name and
# declaration id - the same map the Firmware window writes. The eeprom is
# deliberately NOT given: the built-in default identity must carry the boot.
firmware_json="$(python3 -c "import json,sys; print(json.dumps({'mcpx': sys.argv[1], 'bios': sys.argv[2], 'hdd': sys.argv[3]}))" "$mcpx" "$bios" "$hdd")"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}" romarg="${5:-}"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
		echo "bytes=$SLICE"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" ${romarg:+"$romarg"} ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# the sandbox reference: same machine, driven by run-wbx (the core gate
# already holds run-wbx == the native build byte-for-byte)
reference_ram() {
	local tag="$1" gpu="$2"
	local env=""
	[ "$gpu" = 1 ] && env="CHIMERA_GPU=1"
	# run-wbx carries its own libminiboxhost via rpath; the chimera
	# LD_LIBRARY_PATH exported above would shadow it with the frontend's
	# differently-configured build, which faults
	env -u LD_LIBRARY_PATH $env timeout 900 "$runwbx" "$wbx" \
		--mcpx "$mcpx" --bios "$bios" \
		--eeprom /dev/null --hdd "$hdd" \
		${disc:+--dvd "$disc"} \
		--frames "$frames" --ram-out "$work/ref.$tag.ram.bin" --ram-bytes "$SLICE" \
		> "$work/ref.$tag.log" 2>&1
}

settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2" "$3"; }

# --- 1. the deterministic machine: null renderer, RAM == the reference -----
settings_config "$work/config.null.ini" '{"renderer": "null"}' "$firmware_json"
if ! reference_ram "null" 0; then
	report "boot:frontend" FAIL "reference runner error (see tests/work/ref.null.log)"
elif ! run_frontend "null" "$work/config.null.ini" "$frames" "" "${disc:+$disc}"; then
	report "boot:frontend" FAIL "no OK meta (see tests/work/null.log)"
elif ! cmp -s "$work/ref.null.ram.bin" "$work/null.ram.bin"; then
	report "boot:frontend" FAIL "System RAM differs from the sandbox reference"
elif [ "$(sed -n 's/^ramsize=//p' "$work/null.meta.txt")" != "67108864" ]; then
	report "boot:frontend" FAIL "System RAM is $(sed -n 's/^ramsize=//p' "$work/null.meta.txt") bytes, want 67108864"
else
	report "boot:frontend" PASS "$frames frames, System RAM identical to the sandbox reference"
fi

# --- 2. the GPU: default renderer (opengl-hw), same RAM, and a real picture -
# On an Xbox the GPU's output feeds back into RAM, so this equality only
# holds because the frontend's bridge and run-wbx use the same driver.
settings_config "$work/config.gpu.ini" '{}' "$firmware_json"
if ! reference_ram "gpu" 1; then
	report "gpu:frontend" FAIL "reference runner error (see tests/work/ref.gpu.log)"
elif ! grep -q "gpu bridge:" "$work/ref.gpu.log"; then
	report "gpu:frontend" SKIP "no GL context on this machine; the null leg above is the whole story"
elif ! run_frontend "gpu" "$work/config.gpu.ini" "$frames" "$work/gpu.png" "${disc:+$disc}"; then
	report "gpu:frontend" FAIL "no OK meta (see tests/work/gpu.log)"
elif ! cmp -s "$work/ref.gpu.ram.bin" "$work/gpu.ram.bin"; then
	report "gpu:frontend" FAIL "System RAM differs from the GPU reference (driver mismatch?)"
elif [ ! -s "$work/gpu.png" ]; then
	report "gpu:frontend" FAIL "no screenshot written"
else
	report "gpu:frontend" PASS "$frames frames with the GPU, RAM identical, screenshot at tests/work/gpu.png"
fi

# --- 3. the package's bindings became the frontend's defaults ---------------
# Adoption happens when the package loads, and the frontend writes the
# adopted bindings back into the config it RAN with - so the config of a
# session that loaded the core is the one to ask, not the bootstrap's.
if out="$(python3 "$here/check-keybinds.py" "$work/config.null.ini" "$wb/default_keybinds.json" "Xbox Controller" 2>&1)"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
