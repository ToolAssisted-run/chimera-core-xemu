#!/bin/sh
# The M1 gate: three legs, all byte-for-byte on the full machine-state
# stream, booting the real firmware headless with the null renderer.
#   1. native run A vs native run B      (the machine agrees with itself)
#   2. native vs sandbox (core.wbx)      (the waterbox changes nothing)
#
# Usage: waterbox/run-gate.sh [frames]   (default 60)
# Firmware from $XBOX_FW_DIR (default ~/xbox-roms/"Xbox BIOS"), never
# written: the block filter's cow mode keeps all writes in memory.
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

QEMU_ARGS="-icount shift=5,sleep=off -rtc base=2000-01-01,clock=vm"

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

fail=0

for i in A B; do
	XEMU_BASE_PATH="$run" CHIMERA_FRAMES="$frames" \
		CHIMERA_STATE_OUT="$run/state-nat-$i.bin" \
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
	--frames "$frames" --state-out "$run/state-wbx.bin" \
	> "$run/leg-wbx.log" 2>&1 || { echo "sandbox leg died"; tail -3 "$run/leg-wbx.log"; fail=1; }
if cmp -s "$run/state-nat-A.bin" "$run/state-wbx.bin"; then
	echo "PASS: native == sandbox at $frames frames ($(stat -c%s "$run/state-wbx.bin") bytes of state)"
else
	echo "FAIL: native and sandbox states differ"; fail=1
fi

exit $fail
