#!/bin/bash
# The frontend half of the gate: load the xemu package in Chimera's window
# (run headless: no display is needed), boot the Xbox for a fixed number of
# frames with nothing pressed, and require the machine's RAM to be
# byte-identical to the sandbox reference (run-wbx, which the core gate already
# proved equal to the native build). Then prove the package's keybinds are the
# machine's, and - when a GPU bridge is on offer - that the frontend's machine
# with a GPU matches the reference with the same driver and the screen is not
# black.
#
# It needs the same content the core gate needs: the MCPX boot ROM, a flash
# BIOS and a hard disk image ($XBOX_FW_DIR, laid out as the core gate expects) -
# and, unlike the core gate, a DISC ($XBOX_DVD_PATH), because the frontend
# starts a machine from a rom and a bios-only Xbox has none to hand it (see the
# note beside that check). Without any of them it reports SKIP.
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

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/xemu.chimeraCore"
runwbx="$root/build/run-wbx"
wbx="$root/build/qemu-guest/qemu-system-i386"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
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

# A DISC IS REQUIRED HERE, unlike the core gate, and this is the one place it
# is not the content that is the reason. Chimera starts a machine when it is
# given a ROM: --core registers a package and nothing more (MainForm.cs, "A rom
# needs a core, and choosing one is never implicit"). An Xbox with no disc
# still boots - to the dashboard - but there is no file to hand over, so the
# frontend loads the package, starts nothing, and the witness Lua finds itself
# looking at NullCore. Every leg below then failed with "no OK meta", which
# named neither the cause nor the cure, and CI does not run this script, so
# nobody saw it. Until Chimera can be asked on the command line to boot a
# machine that needs no rom, this is a SKIP and says so.
if [ -z "$disc" ]; then
	report "boot:frontend" SKIP "set XBOX_DVD_PATH: Chimera boots a machine from a ROM and a bios-only Xbox has none to give (see the note above)"
	report "gpu:frontend" SKIP "same"
	report "keybinds" SKIP "the bindings are adopted when the package LOADS, which needs a booted machine: same"
	echo
	echo "$ok ok, $failed failed, $skipped skipped"
	exit 0
fi

work="$here/work"
mkdir -p "$work"

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

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
	local project
	project="$(game_project "$tag" "$cfg" "$package" "$romarg")" || { cp "$work/projects/$tag.make.log" "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# the sandbox reference: same machine, driven by run-wbx (the core gate
# already holds run-wbx == the native build byte-for-byte)
reference_ram() {
	local tag="$1" gpu="$2"
	local env=""
	[ "$gpu" = 1 ] && env="CHIMERA_GPU=1"
	# run-wbx carries its own libminiboxhost via rpath; the chimera
	# a LD_LIBRARY_PATH from the caller would shadow it with the frontend's
	# differently-configured build, which faults
	env -u LD_LIBRARY_PATH $env timeout 900 "$runwbx" "$wbx" \
		--mcpx "$mcpx" --bios "$bios" \
		--eeprom /dev/null --hdd "$hdd" \
		${disc:+--dvd "$disc"} \
		--frames "$frames" --ram-out "$work/ref.$tag.ram.bin" --ram-bytes "$SLICE" \
		> "$work/ref.$tag.log" 2>&1
}

# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings (and the firmware)
# settings_config was given for the config it runs with.
declare -A settings_of firmware_of
settings_config() { python3 "$here/settings-config.py" "$1" "$2" "$3"; settings_of["$1"]="$2"; firmware_of["$1"]="${3:-}"; }
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/projects" settings="${settings_of[$2]:-}" firmware="${firmware_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=()
	[ -n "$firmware" ] && mapfile -t fw < <(python3 -c 'import json,sys
for k, v in json.loads(sys.argv[1]).items(): print("--firmware"); print(k + "=" + v)' "$firmware")
	# one copy of each game, every run's project beside it; no disc, no file
	local copy=""
	mkdir -p "$dir"
	if [ -n "$game" ]; then
		copy="$dir/$(basename "$game")"
		[ -f "$copy" ] && [ "$(stat -c %s "$copy")" = "$(stat -c %s "$game")" ] || cp "$game" "$copy"
	fi
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" ${copy:+"$copy"} \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/$1.make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

# The witness Lua writes its own diagnosis into the metadata as detail=, so a
# failing leg can say what went wrong instead of pointing at a log. Without
# this, "wrong core: NullCore" - which is the whole answer - read as "no OK
# meta (see tests/work/null.log)", and the log did not contain it.
why() { # <tag>
	local d
	d="$(sed -n 's/^detail=//p' "$work/$1.meta.txt" 2>/dev/null | head -1)"
	if [ -n "$d" ]; then printf '%s' "$d"
	elif [ -f "$work/$1.meta.txt" ]; then printf 'the run reported %s' "$(sed -n 's/^status=//p' "$work/$1.meta.txt" | head -1)"
	else printf 'the frontend wrote no metadata at all (a dialog, a stall or a crash)'
	fi
}

# --- 1. the deterministic machine: null renderer, RAM == the reference -----
settings_config "$work/settings.null.json" '{"renderer": "null"}' "$firmware_json"
if ! reference_ram "null" 0; then
	report "boot:frontend" FAIL "reference runner error (see tests/work/ref.null.log)"
elif ! run_frontend "null" "$work/settings.null.json" "$frames" "" "${disc:+$disc}"; then
	report "boot:frontend" FAIL "$(why null) (see tests/work/null.log)"
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
settings_config "$work/settings.gpu.json" '{}' "$firmware_json"
if ! reference_ram "gpu" 1; then
	report "gpu:frontend" FAIL "reference runner error (see tests/work/ref.gpu.log)"
elif ! grep -q "gpu bridge:" "$work/ref.gpu.log"; then
	report "gpu:frontend" SKIP "no GL context on this machine; the null leg above is the whole story"
elif ! run_frontend "gpu" "$work/settings.gpu.json" "$frames" "$work/gpu.png" "${disc:+$disc}"; then
	report "gpu:frontend" FAIL "$(why gpu) (see tests/work/gpu.log)"
elif ! cmp -s "$work/ref.gpu.ram.bin" "$work/gpu.ram.bin"; then
	report "gpu:frontend" FAIL "System RAM differs from the GPU reference (driver mismatch?)"
elif [ ! -s "$work/gpu.png" ]; then
	report "gpu:frontend" FAIL "no screenshot written"
else
	report "gpu:frontend" PASS "$frames frames with the GPU, RAM identical, screenshot at tests/work/gpu.png"
fi

# --- 3. the bindings the package ships are the machine's when nobody changed them
# (the window keeps only the keys a person changed; the settings of the null
# run have none). The null run's project is the machine to ask.
printf 'out=%s/keys.controls.json\nmeta=%s/keys.meta.txt\n' "$work" "$work" > "$work/job.keys.txt"
rm -f "$work/keys.controls.json" "$work/keys.meta.txt"
( cd "$chimera_root" && MINIHAWK_JOB="$work/job.keys.txt" timeout 900 "$chimera_exe" --headless \
	"--config=$work/settings.null.json" "--project=$work/projects/null.chimeraProject" \
	"--lua=$here/dump-controls.lua" ) > "$work/keys.log" 2>&1
if [ ! -f "$work/keys.meta.txt" ] || ! grep -q "^status=OK" "$work/keys.meta.txt"; then
	report "keybinds" FAIL "the bindings were not written (see tests/work/keys.log)"
elif out="$(python3 "$here/check-keybinds.py" "$work/keys.controls.json" "$wb/default_keybinds.json" "Xbox Controller" 2>&1)"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
