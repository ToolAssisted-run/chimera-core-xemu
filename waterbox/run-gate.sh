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

mbh="$HOME/chimera/extern/tools/chimera-common-minibox/build/meson-cpp/source/host"
[ -x "$runwbx" ] || gcc -O2 -o "$runwbx" "$here/run-wbx.c" \
	-I "$HOME/chimera/extern/tools/chimera-common-minibox/source/host" \
	"$mbh/libminiboxhost.so" -Wl,-rpath,"$mbh"

QEMU_ARGS="-icount shift=0,sleep=off -rtc base=2000-01-01,clock=vm"

# one minted EEPROM for every leg (it is per-project persistent data)
if [ ! -f "$run/eeprom-master.bin" ]; then
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
eeprom_path = '$run/eeprom-master.bin'
hdd_path = '$fw/Hard Disk/xbox_hdd.qcow2'
EOF
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES=1 timeout 240 "$nat" \
		-config_path "$run/xemu-seed.toml" $QEMU_ARGS >/dev/null 2>&1 || true
	[ -f "$run/eeprom-master.bin" ] || { echo "eeprom mint failed" >&2; exit 1; }
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
eeprom_path = '$run/eeprom-master.bin'
hdd_path = '$fw/Hard Disk/xbox_hdd.qcow2'
EOF
if [ -n "${XBOX_DVD_PATH:-}" ]; then
	printf "dvd_path = '%s'\n" "$XBOX_DVD_PATH" >> "$run/xemu.toml"
fi

fail=0

for i in A B; do
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES="$frames" \
		CHIMERA_STATE_OUT="$run/state-nat-$i.bin" \
		CHIMERA_AUDIO_OUT="$run/audio-nat-$i.s16" \
		timeout 590 "$nat" -config_path "$run/xemu.toml" $QEMU_ARGS \
		> "$run/leg-nat-$i.log" 2>&1 || { echo "native leg $i died"; tail -3 "$run/leg-nat-$i.log"; fail=1; }
done
if cmp -s "$run/state-nat-A.bin" "$run/state-nat-B.bin"; then
	echo "PASS: native deterministic at $frames frames"
else
	echo "FAIL: native runs differ"; fail=1
fi

timeout 590 "$runwbx" "$wbx" \
	--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
	--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
	--eeprom "$run/eeprom-master.bin" \
	--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
	${XBOX_DVD_PATH:+--dvd "$XBOX_DVD_PATH"} \
	--frames "$frames" --state-out "$run/state-wbx.bin" \
	--audio-out "$run/audio-wbx.s16" \
	> "$run/leg-wbx.log" 2>&1 || { echo "sandbox leg died"; tail -3 "$run/leg-wbx.log"; fail=1; }
if cmp -s "$run/state-nat-A.bin" "$run/state-wbx.bin"; then
	echo "PASS: native == sandbox at $frames frames ($(stat -c%s "$run/state-wbx.bin") bytes of state)"
else
	echo "FAIL: native and sandbox states differ"; fail=1
fi

# The audio leg: the APU monitor's sample stream is machine output; the two
# native runs and the sandbox must produce the same bytes, and a boot that
# reached the dashboard jingle must produce actual sound.
if ! cmp -s "$run/audio-nat-A.s16" "$run/audio-nat-B.s16"; then
	echo "FAIL: native audio streams differ"; fail=1
elif ! cmp -s "$run/audio-nat-A.s16" "$run/audio-wbx.s16"; then
	echo "FAIL: native and sandbox audio differ"; fail=1
elif [ "$frames" -ge 600 ] && ! LC_ALL=C grep -qm1 "[^\\x00]" "$run/audio-nat-A.s16"; then
	echo "FAIL: audio is pure silence"; fail=1
else
	echo "PASS: audio leg - $(stat -c%s "$run/audio-wbx.s16") bytes, native == sandbox"
fi

# The savestate leg: 60 sandbox frames with the arena saved and reloaded
# around every one of them - a loaded state must continue exactly like the
# run it came from, so the end state must match a plain 60-frame run.
for mode in plain rr; do
	[ "$mode" = rr ] && extra="--rerecord" || extra=""
	timeout 590 "$runwbx" "$wbx" \
		--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
		--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
		--eeprom "$run/eeprom-master.bin" \
		--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
		$extra --frames 60 --state-out "$run/state-wbx-$mode-60.bin" \
		> "$run/leg-wbx-$mode-60.log" 2>&1 || { echo "sandbox $mode-60 leg died"; tail -3 "$run/leg-wbx-$mode-60.log"; fail=1; }
done
if cmp -s "$run/state-wbx-plain-60.bin" "$run/state-wbx-rr-60.bin"; then
	echo "PASS: savestate leg - save+load around every frame changes nothing"
else
	echo "FAIL: savestate round-trip diverges"; fail=1
fi

# The input leg: hold START on pad 1 from frame 1000 on. A booted game polls
# the pad from ~frame 974 (the dashboard never starts USB at all), so this
# needs the DVD and at least 1200 frames; the press must leave a different
# machine than the plain run, and native and sandbox must agree on it.
if [ -n "${XBOX_DVD_PATH:-}" ] && [ "$frames" -ge 1200 ]; then
	press="0:200:1000:$frames"
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES="$frames" CHIMERA_PRESS="$press" \
		CHIMERA_STATE_OUT="$run/state-nat-press.bin" \
		timeout 590 "$nat" -config_path "$run/xemu.toml" $QEMU_ARGS \
		> "$run/leg-nat-press.log" 2>&1 || { echo "native press leg died"; tail -3 "$run/leg-nat-press.log"; fail=1; }
	timeout 590 "$runwbx" "$wbx" \
		--mcpx "$fw/MCPX Boot ROM/mcpx_1.0.bin" \
		--bios "$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin" \
		--eeprom "$run/eeprom-master.bin" \
		--hdd "$fw/Hard Disk/xbox_hdd.qcow2" \
		--dvd "$XBOX_DVD_PATH" \
		--press "$press" \
		--frames "$frames" --state-out "$run/state-wbx-press.bin" \
		> "$run/leg-wbx-press.log" 2>&1 || { echo "sandbox press leg died"; tail -3 "$run/leg-wbx-press.log"; fail=1; }
	if cmp -s "$run/state-nat-press.bin" "$run/state-nat-A.bin"; then
		echo "FAIL: the press left no trace in the machine"; fail=1
	elif cmp -s "$run/state-nat-press.bin" "$run/state-wbx-press.bin"; then
		echo "PASS: input leg - the press reached the machine, native == sandbox"
	else
		echo "FAIL: native and sandbox disagree under input"; fail=1
	fi
fi

exit $fail
