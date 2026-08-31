#!/bin/sh
# The native determinism leg: two identical headless runs from a pristine HDD
# copy each, machine state dumped after N frames, compared byte for byte.
# This is the precondition for every other gate - if the native build cannot
# agree with itself, native vs sandbox comparison is meaningless.
#
# Usage: waterbox/run-determinism-native.sh [frames]
# Firmware comes from $XBOX_FW_DIR (default ~/xbox-roms/"Xbox BIOS"), and is
# never written to: the HDD is copied per run.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
frames="${1:-60}"
fw="${XBOX_FW_DIR:-$HOME/xbox-roms/Xbox BIOS}"
bin="$root/build/qemu-native/qemu-system-i386"
run="$root/build/run-native"
mkdir -p "$run"

[ -x "$bin" ] || { echo "native build missing: $bin" >&2; exit 1; }

# fixed icount shift and a fixed RTC base: no host clock may reach the machine
QEMU_ARGS="-icount shift=5,sleep=off -rtc base=2000-01-01,clock=vm"

# The EEPROM is an identity xemu mints with real randomness (keys, MAC,
# serial) - it is per-project persistent data, not part of the machine. Mint
# it once and give every run a copy, the way Chimera will.
if [ ! -f "$run/eeprom-master.bin" ]; then
	cp "$fw/Hard Disk/xbox_hdd.qcow2" "$run/hdd-seed.qcow2"
	cat > "$run/xemu-seed.toml" <<EOF
[general]
show_welcome = false

[display]
renderer = 'NULL'

[sys]
mem_limit = '64'

[sys.files]
bootrom_path = '$fw/MCPX Boot ROM/mcpx_1.0.bin'
flashrom_path = '$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin'
eeprom_path = '$run/eeprom-master.bin'
hdd_path = '$run/hdd-seed.qcow2'
EOF
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES=1 \
		"$bin" -config_path "$run/xemu-seed.toml" $QEMU_ARGS \
		>/dev/null 2>&1 || true
	rm -f "$run/hdd-seed.qcow2" "$run/xemu-seed.toml"
	[ -f "$run/eeprom-master.bin" ] || { echo "eeprom mint failed" >&2; exit 1; }
fi

for i in 1 2; do
	rm -f "$run/hdd-$i.qcow2" "$run/eeprom-$i.bin"
	cp "$fw/Hard Disk/xbox_hdd.qcow2" "$run/hdd-$i.qcow2"
	cp "$run/eeprom-master.bin" "$run/eeprom-$i.bin"
	cat > "$run/xemu-$i.toml" <<EOF
[general]
show_welcome = false

[display]
renderer = 'NULL'

[sys]
mem_limit = '64'

[sys.files]
bootrom_path = '$fw/MCPX Boot ROM/mcpx_1.0.bin'
flashrom_path = '$fw/Flash ROM (BIOS)/Complex_4627v1.03.bin'
eeprom_path = '$run/eeprom-$i.bin'
hdd_path = '$run/hdd-$i.qcow2'
EOF
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES="$frames" \
		CHIMERA_STATE_OUT="$run/state-$frames-$i.bin" \
		timeout 240 "$bin" -config_path "$run/xemu-$i.toml" $QEMU_ARGS \
		> "$run/leg-$i.log" 2>&1 || { echo "leg $i: TIMED OUT or crashed"; tail -3 "$run/leg-$i.log"; }
	grep -E "xemu-waterbox" "$run/leg-$i.log" || true
done

if cmp -s "$run/state-$frames-1.bin" "$run/state-$frames-2.bin"; then
	echo "PASS: $frames frames deterministic ($(stat -c%s "$run/state-$frames-1.bin") bytes of state)"
else
	echo "FAIL: $frames frames differ in $(cmp -l "$run/state-$frames-1.bin" "$run/state-$frames-2.bin" | wc -l) bytes" >&2
	exit 1
fi
