#!/bin/sh
# The M1 gate: three legs, all byte-for-byte on the full machine-state
# stream, booting the real firmware headless with the null renderer.
#   1. native run A vs native run B      (the machine agrees with itself)
#   2. native vs sandbox (core.wbx)      (the waterbox changes nothing)
#
# Usage: waterbox/run-gate.sh [frames]   (default 60)
# Firmware from $XBOX_FW_DIR (default ~/xbox-roms/"Xbox BIOS"), never
# written: the block filter's cow mode keeps all writes in memory.
# Set XBOX_DVD_PATH to an iso to run the same legs with a disc inserted.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
frames="${1:-60}"
fw="${XBOX_FW_DIR:-$HOME/xbox-roms/Xbox BIOS}"
nat="$root/build/qemu-native/qemu-system-i386"
wbx="$root/build/qemu-guest/qemu-system-i386"
runwbx="$root/build/run-wbx"
run="$root/build/gate"
mkdir -p "$run"

[ -x "$nat" ] || { echo "native build missing (waterbox/setup-native.sh + ninja)" >&2; exit 1; }
[ -x "$wbx" ] || { echo "guest build missing (waterbox/setup-guest.sh + ninja qemu-system-i386)" >&2; exit 1; }

# MINIBOX_DIR, or the sibling checkout: a path built from $HOME works on the
# machine that wrote it and nowhere else - a runner's $HOME is not a developer's.
mb="${MINIBOX_DIR:-}"
[ -n "$mb" ] || for c in "$root/../chimera/extern/chimera-common-minibox" "$HOME/chimera/extern/chimera-common-minibox"; do
	[ -d "$c" ] && { mb="$c"; break; }
done
[ -n "$mb" ] && [ -d "$mb" ] || { echo "miniBox not found; set MINIBOX_DIR" >&2; exit 1; }
mb="$(cd "$mb" && pwd)"
mbh="$mb/build/meson-cpp/source/host"
# Built when missing OR older than its sources: a run-wbx left over from
# before a change to gl-host.c would put the gate's gpu leg behind a
# dispatcher nobody had fixed yet, and it would pass.
if [ ! -x "$runwbx" ] || [ "$here/run-wbx.c" -nt "$runwbx" ] || [ "$here/gl-host.c" -nt "$runwbx" ]; then
	gcc -O2 -DCHIMERA_GL_BRIDGE -o "$runwbx" \
		"$here/run-wbx.c" "$here/gl-host.c" "$here/glad/src/gl.c" \
		-I "$mb/source/host" -I "$mb/source/gl" \
		-I "$here/generated-gl-host" -I "$here/glad/include" \
		"$mbh/libminiboxhost.so" -Wl,-rpath,"$mbh" -lEGL
fi

# No -icount here on purpose: both flavors now default to the cpuSpeed setting's
# value (500 MIPS, shift=1), the guest through Init and the native reference by
# injecting the same default when its command line does not say otherwise. The
# gate exists to compare them, so it must not hand one of them a different
# machine. Set CHIMERA_ICOUNT_SHIFT to move both at once.
QEMU_ARGS="-rtc base=2000-01-01,clock=vm"

# Content minted ONCE and reused by every leg and every run of this script:
# a fresh EEPROM has real randomness baked into it by xemu itself, and it is
# per-project persistent data, not a leg's output. It lives in its own
# directory, separate from every leg directory below, so that wiping a leg's
# directory - which every leg below now does, unconditionally, before it
# runs - can never take the EEPROM out with it.
shared="$run/shared"
mkdir -p "$shared"

# Every leg below gets its own directory, wiped and recreated the moment the
# leg starts, and never written to by any other leg. This is the fix for
# docs/gates.md mode B: a single shared, never-cleared build/gate meant a leg
# whose runs had just died could still find yesterday's (or another leg's)
# output sitting under the same names, compare that leftover pair, and PASS.
# With a private directory wiped on entry, a leg that dies writes nothing an
# adjacent check can mistake for a result.
leg_dir() {
	d="$run/$1"
	rm -rf "$d"
	mkdir -p "$d"
	printf '%s' "$d"
}

# xemu's native reference treats a missing BootROM/BIOS/HDD as non-fatal: it
# queues an error message (extern/xemu system/vl.c) and boots without them,
# so the process still exits 0 having "run to completion" on a machine that
# never had firmware. The sandbox mounts each file itself and refuses to
# start without it, dying loudly (see waterbox/xemu-waterbox.c). Hold native
# to the same bar here so a missing-content run cannot pass as a real native
# leg just because its exit code was 0.
nat_log_ok() {
	! grep -q "^xemu error: Failed to open" "$1"
}

# bridge_answered FILE...: did the GPU bridge have a case for every opcode the
# guest sent it? gl-host.c's default arm logs and returns 0, and 0 is a
# perfectly plausible answer to nearly every question the bridge carries - so a
# guest that was answered and a guest that was shrugged at look the same, and
# two flavours that were both shrugged at compare EQUAL.
#
# That is not a worry, it is a measurement: GL_OP_CONTEXT_ID (chimera issue
# #43, the opcode that lets the renderer notice its GL objects belong to a
# context that is gone) had no case in gl-host.c for as long as the opcode
# existed, and this gate was green over it. Absent was indistinguishable from
# working (~/chimera/docs/gates.md, mode C). So no gpu leg may go green over
# that line: every one runs this first, on each flavour's stderr that went
# through gl-host.c, and the message names the opcodes.
bridge_gap=""
bridge_answered() {
	bridge_gap=""
	for f in "$@"; do
		[ -f "$f" ] || continue
		grep -q 'has no case' "$f" || continue
		bridge_gap="the GPU bridge had no case for $(grep -o 'opcode [0-9]*' "$f" | sort -u | tr '\n' ',' | sed 's/,$//; s/,/, /g') and answered 0 ($(basename "$f"))"
		return 1
	done
	return 0
}

if [ ! -f "$shared/eeprom-master.bin" ]; then
	cat > "$run/xemu-seed.toml" <<EOF
[general]
show_welcome = false
[display]
renderer = 'NULL'
[audio]
use_dsp_jit = false
[sys]
mem_limit = '64'
[sys.files]
bootrom_path = '$fw/MCPX Boot ROM/mcpx_1.0.bin'
flashrom_path = '$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin'
eeprom_path = '$shared/eeprom-master.bin'
hdd_path = '$fw/Hard Disk/xbox_hdd.qcow2'
EOF
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES=1 timeout 240 "$nat" \
		-config_path "$run/xemu-seed.toml" $QEMU_ARGS >/dev/null 2>&1 || true
	[ -f "$shared/eeprom-master.bin" ] || { echo "eeprom mint failed" >&2; exit 1; }
fi

cat > "$run/xemu.toml" <<EOF
[general]
show_welcome = false
[display]
renderer = 'NULL'
[audio]
use_dsp_jit = false
[sys]
mem_limit = '64'
[sys.files]
bootrom_path = '$fw/MCPX Boot ROM/mcpx_1.0.bin'
flashrom_path = '$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin'
eeprom_path = '$shared/eeprom-master.bin'
hdd_path = '$fw/Hard Disk/xbox_hdd.qcow2'
EOF
if [ -n "${XBOX_DVD_PATH:-}" ]; then
	printf "dvd_path = '%s'\n" "$XBOX_DVD_PATH" >> "$run/xemu.toml"
fi

fail=0

# The base leg: native determinism, native == sandbox, and (below) the audio
# comparison all read the very same set of runs, so they share one directory.
base=$(leg_dir base)

for i in A B; do
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES="$frames" \
		CHIMERA_STATE_OUT="$base/state-nat-$i.bin" \
		CHIMERA_AUDIO_OUT="$base/audio-nat-$i.s16" \
		timeout 590 "$nat" -config_path "$run/xemu.toml" $QEMU_ARGS \
		> "$base/leg-nat-$i.log" 2>&1 || { echo "native leg $i died"; tail -3 "$base/leg-nat-$i.log"; fail=1; }
	nat_log_ok "$base/leg-nat-$i.log" || { echo "native leg $i ran without its firmware (see log)"; fail=1; }
done
# -s before cmp, on every leg below as well: two EMPTY state files compare
# equal, so a machine that stopped serialising its state would have read as
# "native deterministic" and then as "native == sandbox" - two passes on
# nothing at all.
if [ ! -s "$base/state-nat-A.bin" ] || [ ! -s "$base/state-nat-B.bin" ]; then
	echo "FAIL: a native run wrote no machine state"; fail=1
elif cmp -s "$base/state-nat-A.bin" "$base/state-nat-B.bin"; then
	echo "PASS: native deterministic at $frames frames"
else
	echo "FAIL: native runs differ"; fail=1
fi

timeout 590 "$runwbx" "$wbx" \
	--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
	--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
	--eeprom "$shared/eeprom-master.bin" \
	--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
	${XBOX_DVD_PATH:+--dvd "$XBOX_DVD_PATH"} \
	--frames "$frames" --state-out "$base/state-wbx.bin" \
	--audio-out "$base/audio-wbx.s16" \
	> "$base/leg-wbx.log" 2>&1 || { echo "sandbox leg died"; tail -3 "$base/leg-wbx.log"; fail=1; }
if [ ! -s "$base/state-wbx.bin" ]; then
	echo "FAIL: the sandbox run wrote no machine state"; fail=1
elif cmp -s "$base/state-nat-A.bin" "$base/state-wbx.bin"; then
	echo "PASS: native == sandbox at $frames frames ($(stat -c%s "$base/state-wbx.bin") bytes of state)"
else
	echo "FAIL: native and sandbox states differ"; fail=1
fi

# The audio leg: the APU monitor's sample stream is machine output; the two
# native runs and the sandbox must produce the same bytes, and a boot that
# reached the dashboard jingle must produce actual sound.
if [ ! -s "$base/audio-nat-A.s16" ] || [ ! -s "$base/audio-wbx.s16" ]; then
	# an APU that stopped handing its monitor's samples over produces two empty
	# files, and two empty files are byte-identical: the leg below would have
	# congratulated a core that made no sound at all, which is exactly how
	# flycast shipped silent for its whole life
	echo "FAIL: a run produced no audio samples at all"; fail=1
elif ! cmp -s "$base/audio-nat-A.s16" "$base/audio-nat-B.s16"; then
	echo "FAIL: native audio streams differ"; fail=1
elif ! cmp -s "$base/audio-nat-A.s16" "$base/audio-wbx.s16"; then
	echo "FAIL: native and sandbox audio differ"; fail=1
elif [ "$frames" -ge 600 ] && ! LC_ALL=C grep -qm1 "[^\\x00]" "$base/audio-nat-A.s16"; then
	echo "FAIL: audio is pure silence"; fail=1
else
	echo "PASS: audio leg - $(stat -c%s "$base/audio-wbx.s16") bytes, native == sandbox"
	# the samples are equal and there are some, but WHETHER THEY ARE SOUND is
	# only asked past 600 frames: before the dashboard jingle the machine is
	# silent by design, so the question has no honest answer yet. Said out loud,
	# because the default run is 60 and a silent line reads as a green one.
	[ "$frames" -ge 600 ] || echo "SKIP: audio is sound - needs 600+ frames to reach the dashboard jingle (waterbox/run-gate.sh 600); at $frames silence is correct"
fi

# The savestate leg: 60 sandbox frames with the arena saved and reloaded
# around every one of them - a loaded state must continue exactly like the
# run it came from, so the end state must match a plain 60-frame run.
save=$(leg_dir savestate)
for mode in plain rr; do
	[ "$mode" = rr ] && extra="--rerecord" || extra=""
	timeout 590 "$runwbx" "$wbx" \
		--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
		--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
		--eeprom "$shared/eeprom-master.bin" \
		--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
		$extra --frames 60 --state-out "$save/state-wbx-$mode-60.bin" \
		> "$save/leg-wbx-$mode-60.log" 2>&1 || { echo "sandbox $mode-60 leg died"; tail -3 "$save/leg-wbx-$mode-60.log"; fail=1; }
done
if [ ! -s "$save/state-wbx-plain-60.bin" ] || [ ! -s "$save/state-wbx-rr-60.bin" ]; then
	echo "FAIL: a 60-frame sandbox run wrote no machine state"; fail=1
elif cmp -s "$save/state-wbx-plain-60.bin" "$save/state-wbx-rr-60.bin"; then
	echo "PASS: savestate leg - save+load around every frame changes nothing"
else
	echo "FAIL: savestate round-trip diverges"; fail=1
fi

# The input leg: hold START on pad 1 from frame 1000 on. A booted game polls
# the pad from ~frame 974 (the dashboard never starts USB at all), so this
# needs the DVD and at least 1200 frames; the press must leave a different
# machine than the plain run, and native and sandbox must agree on it.
if [ -n "${XBOX_DVD_PATH:-}" ] && [ "$frames" -ge 1200 ]; then
	input=$(leg_dir input)
	press="0:200:1000:$frames"
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES="$frames" CHIMERA_PRESS="$press" \
		CHIMERA_STATE_OUT="$input/state-nat-press.bin" \
		timeout 590 "$nat" -config_path "$run/xemu.toml" $QEMU_ARGS \
		> "$input/leg-nat-press.log" 2>&1 || { echo "native press leg died"; tail -3 "$input/leg-nat-press.log"; fail=1; }
	nat_log_ok "$input/leg-nat-press.log" || { echo "native press leg ran without its firmware (see log)"; fail=1; }
	timeout 590 "$runwbx" "$wbx" \
		--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
		--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
		--eeprom "$shared/eeprom-master.bin" \
		--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
		--dvd "$XBOX_DVD_PATH" \
		--press "$press" \
		--frames "$frames" --state-out "$input/state-wbx-press.bin" \
		> "$input/leg-wbx-press.log" 2>&1 || { echo "sandbox press leg died"; tail -3 "$input/leg-wbx-press.log"; fail=1; }
	# state-nat-A.bin is the base leg's baseline (this run, not a leftover -
	# the base leg above always runs first and always wipes its own
	# directory), used here to prove the press changed the machine at all.
	if [ ! -s "$base/state-nat-A.bin" ] || [ ! -s "$input/state-nat-press.bin" ] || [ ! -s "$input/state-wbx-press.bin" ]; then
		echo "FAIL: a press run (or its baseline) wrote no machine state"; fail=1
	elif cmp -s "$input/state-nat-press.bin" "$base/state-nat-A.bin"; then
		echo "FAIL: the press left no trace in the machine"; fail=1
	elif cmp -s "$input/state-nat-press.bin" "$input/state-wbx-press.bin"; then
		echo "PASS: input leg - the press reached the machine, native == sandbox"
	else
		echo "FAIL: native and sandbox disagree under input"; fail=1
	fi
else
	echo "SKIP: input leg - needs XBOX_DVD_PATH and 1200+ frames (a booted game first polls the pad around frame 974; the dashboard never starts USB at all)"
fi

# The GPU leg (XBOX_GPU=1): the same machine with the GL renderer drawing
# through a real driver - EGL surfaceless in the native build, the bridge in
# the sandbox. On the Xbox the GPU's output feeds back into RAM (UMA), so
# machine state now CONTAINS the picture: the compare only holds on one host
# with one driver, which is exactly what this leg pins down. Runs at 600
# frames; the boot animation is fully rendered by then.
if [ -n "${XBOX_GPU:-}" ]; then
	gpu=$(leg_dir gpu)
	for i in A B; do
		XEMU_BASE_PATH="$run" CHIMERA_FRAMES=600 CHIMERA_GPU=1 \
			CHIMERA_STATE_OUT="$gpu/state-gpu-nat-$i.bin" \
			timeout 590 "$nat" -config_path "$run/xemu.toml" $QEMU_ARGS \
			> "$gpu/leg-gpu-nat-$i.log" 2>&1 || { echo "gpu native leg $i died"; tail -3 "$gpu/leg-gpu-nat-$i.log"; fail=1; }
		nat_log_ok "$gpu/leg-gpu-nat-$i.log" || { echo "gpu native leg $i ran without its firmware (see log)"; fail=1; }
	done
	CHIMERA_GPU=1 timeout 590 "$runwbx" "$wbx" \
		--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
		--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
		--eeprom "$shared/eeprom-master.bin" \
		--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
		${XBOX_DVD_PATH:+--dvd "$XBOX_DVD_PATH"} \
		--frames 600 --state-out "$gpu/state-gpu-wbx.bin" \
		> "$gpu/leg-gpu-wbx.log" 2>&1 || { echo "gpu sandbox leg died"; tail -3 "$gpu/leg-gpu-wbx.log"; fail=1; }
	# state-nat-A.bin is the base leg's baseline (this run, not a leftover),
	# used here to prove the GPU actually left a trace in machine state.
	# The sandbox run is the one that went through gl-host.c (the native
	# binary drives EGL itself), and it is held to bridge_answered FIRST: the
	# machine state compared below does not carry the bridge's answers, so
	# two states agreed while opcode 4 went unanswered 920,292 times a run.
	if ! bridge_answered "$gpu/leg-gpu-wbx.log"; then
		echo "FAIL: gpu leg - $bridge_gap"; fail=1
	elif [ ! -s "$gpu/state-gpu-nat-A.bin" ] || [ ! -s "$gpu/state-gpu-wbx.bin" ] || [ ! -s "$base/state-nat-A.bin" ]; then
		echo "FAIL: gpu leg - a run (or the base leg's baseline) wrote no machine state"; fail=1
	elif ! cmp -s "$gpu/state-gpu-nat-A.bin" "$gpu/state-gpu-nat-B.bin"; then
		echo "FAIL: gpu leg - native runs differ"; fail=1
	elif ! cmp -s "$gpu/state-gpu-nat-A.bin" "$gpu/state-gpu-wbx.bin"; then
		echo "FAIL: gpu leg - native and sandbox differ"; fail=1
	elif cmp -s "$gpu/state-gpu-nat-A.bin" "$base/state-nat-A.bin"; then
		echo "FAIL: gpu leg - the GPU left no trace (did it draw at all?)"; fail=1
	else
		echo "PASS: gpu leg - the GPU drew, native == sandbox on this driver, and every opcode the guest sent had a case"
	fi
else
	echo "SKIP: gpu leg - set XBOX_GPU=1 to render through a real driver (EGL surfaceless natively, the bridge in the sandbox) and hold the two byte-equal at 600 frames"
fi

# The gl:rebuild-at-zero leg (chimera issue 126): the greenzone's FRAME-0
# ANCHOR is a state like no other, and it has to restore like any other.
#
# It is taken right after Init and before the first frame advance - so it is
# the only state in a session made while the machine had never stopped at a
# frame boundary, and the only one made before the GL renderer ever looked at
# which context its objects came from. TAStudio reaches a frame by loading the
# state BEFORE it and emulating one forward, so frames 0 and 1 both load that
# anchor: whatever is wrong with it reaches a person playing their movie from
# the beginning.
#
# Two things could be. StateLoaded's tb_flush asserts on
# "!runstate_is_running()", and the anchor is the one state that comes back
# saying RUNNING - so loading it ABORTED the core. And the GL check reads a
# stored context id of 0 as "nothing to rebuild", which would be wrong for that
# same state - the bug PCSX2 had; this core happens to write the id during Init
# at nv2a_reset's pfifo drain, so it never had that hole (docs/PLAN.md).
#
# What it measures: the core must survive a restore to the anchor, and the
# renderer must rebuild after it - a rebuild is hundreds of bridge calls and
# says so on stderr, while an idle frame of this machine is one call. A restore
# to frame 2, an ordinary state, must do the same: it is the DIFFERENCE between
# the two that a bug of this family shows up as.
#
# WHAT IT DOES NOT STAND IN FOR (docs/gates.md, E): llvmpipe is not a driver,
# and a rebuild that RUNS is not a picture that is right. It needs a disc and
# the firmware, so on a public runner it SKIPs - docs/PLAN.md, "What CI runs".
crunroot=""
for c in "${CHIMERA_ROOT:-}" "$root/../chimera" "$HOME/chimera"; do
	if [ -n "$c" ] && [ -x "$c/build/meson-linux/chimera-run" ] &&
		[ -f "$c/build/Cores/xemu.chimeraCore" ]; then
		crunroot="$c"
		break
	fi
done
if [ -z "$crunroot" ] || [ -z "${XBOX_DVD_PATH:-}" ]; then
	echo "SKIP: gl:rebuild-at-zero leg - needs XBOX_DVD_PATH, plus chimera-run and an installed xemu.chimeraCore (set CHIMERA_ROOT). It is the only leg here that goes through the engine, which is what mints a context id and takes a greenzone anchor"
else
	gz=$(leg_dir glzero)
	crunbin="$crunroot/build/meson-linux/chimera-run"
	cpkg="$crunroot/build/Cores/xemu.chimeraCore"
	printf '[Input]\nLogKey:#\n' > "$gz/none.txt"
	glrun() { # <movie> <out> <extra args...>
		glmovie="$1"; glout="$2"; shift 2
		CHIMERA_GL_TRACE=1 CHIMERA_GL_STATEAUDIT=1 timeout 900 "$crunbin" "$cpkg" \
			"$XBOX_DVD_PATH" "$glmovie" --frames 120 --gpu \
			--firmware "mcpx=$fw/MCPX Boot ROM/mcpx_1.0.bin" \
			--firmware "bios=$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
			--firmware "hdd=$fw/Hard Disk/xbox_hdd.qcow2" "$@" > "$glout" 2>&1 || true
	}
	# the calls on the first traced frame after the restore
	afterRestore() {
		awk '/ce-gl-audit\] restore/ { seen = 1 }
		     seen && match($0, /\[ce-gl\] frame [0-9]+: [0-9]+ calls/) {
			s = substr($0, RSTART, RLENGTH); split(s, f, " "); print f[4]; exit }' "$1"
	}
	glrun "$gz/none.txt" "$gz/record.log" --record "$gz/movie.txt"
	if [ ! -s "$gz/movie.txt" ]; then
		echo "FAIL: gl:rebuild-at-zero leg - could not record a movie to rewind through (see $gz/record.log)"; fail=1
	else
		glrun "$gz/movie.txt" "$gz/rewind0.log" --greenzone 4096 --rewind-loop 0,1
		glrun "$gz/movie.txt" "$gz/rewind2.log" --greenzone 4096 --rewind-loop 2,1
		zero="$(afterRestore "$gz/rewind0.log")"
		two="$(afterRestore "$gz/rewind2.log")"
		if grep -q "^chimera gl: no context" "$gz/rewind0.log"; then
			echo "SKIP: gl:rebuild-at-zero leg - this machine gives the bridge no GL context"
		elif grep -q "Assertion failed" "$gz/rewind0.log"; then
			echo "FAIL: gl:rebuild-at-zero leg - restoring the frame-0 anchor killed the machine: $(grep -m1 'Assertion failed' "$gz/rewind0.log")"; fail=1
		elif [ -z "$zero" ] || [ -z "$two" ]; then
			echo "FAIL: gl:rebuild-at-zero leg - no restore was traced (see $gz/rewind0.log and $gz/rewind2.log)"; fail=1
		elif ! grep -q "; rebuilding" "$gz/rewind0.log"; then
			echo "FAIL: gl:rebuild-at-zero leg - restoring the frame-0 anchor made $zero GL calls and the renderer never rebuilt, against $two restoring frame 2"; fail=1
		elif ! grep -q "; rebuilding" "$gz/rewind2.log"; then
			echo "FAIL: gl:rebuild-at-zero leg - restoring frame 2 made $two GL calls and the renderer never rebuilt"; fail=1
		elif [ "$zero" -lt 100 ] || [ "$two" -lt 100 ]; then
			echo "FAIL: gl:rebuild-at-zero leg - a rebuild was announced but only $zero / $two calls crossed the bridge after the restore"; fail=1
		else
			echo "PASS: gl:rebuild-at-zero leg - the frame-0 anchor restores and rebuilds ($zero calls), as does frame 2 ($two)"
		fi
	fi
fi

exit $fail
