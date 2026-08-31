#!/bin/sh
# Configures the NATIVE reference build (build/qemu-native): headless xemu,
# i386-softmmu only, TCG, null renderer, no SDL/GL/vulkan/network. Applies
# the patches first. pixman is built static into build/native-deps because
# the machine needs it and the host may not have it.
#
# Usage: ./waterbox/setup-native.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

sh "$here/apply-patches.sh"

ndeps="$root/build/native-deps"
src="$root/build/deps-src"
if ! ls "$ndeps"/lib*/pkgconfig/pixman-1.pc "$ndeps"/lib/*/pkgconfig/pixman-1.pc >/dev/null 2>&1; then
	mkdir -p "$src"
	if [ ! -f "$src/pixman-0.46.2.tar.gz" ]; then
		curl -sL -o "$src/pixman-0.46.2.tar.gz" "https://cairographics.org/releases/pixman-0.46.2.tar.gz"
	fi
	echo "3e0de5ba6e356916946a3d958192f15505dcab85134771bfeab4ce4e29bbd733  $src/pixman-0.46.2.tar.gz" | sha256sum -c - >/dev/null
	[ -d "$src/pixman-0.46.2" ] || tar -C "$src" -xzf "$src/pixman-0.46.2.tar.gz"
	rm -rf "$src/pixman-native"
	cp -r "$src/pixman-0.46.2" "$src/pixman-native"
	rm -rf "$src/pixman-native/build"
	meson setup "$src/pixman-native/build" "$src/pixman-native" --prefix "$ndeps" \
		--default-library static --buildtype release \
		-Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled >/dev/null
	ninja -C "$src/pixman-native/build" >/dev/null
	meson install -C "$src/pixman-native/build" >/dev/null
	echo "pixman (native): installed"
fi
pcdir="$(dirname "$(ls "$ndeps"/lib*/pkgconfig/pixman-1.pc "$ndeps"/lib/*/pkgconfig/pixman-1.pc 2>/dev/null | head -1)")"

mkdir -p "$root/build/qemu-native"
cd "$root/build/qemu-native"
PKG_CONFIG_PATH="$pcdir" "$root/extern/xemu/configure" \
	--target-list=i386-softmmu \
	--extra-cflags='-DXBOX=1' \
	--enable-pixman \
	--disable-sdl --disable-opengl --disable-gtk --disable-vnc \
	--disable-slirp --disable-docs --disable-tools --disable-guest-agent \
	--audio-drv-list= --disable-kvm --disable-xen
echo "native build configured: ninja -C build/qemu-native"
