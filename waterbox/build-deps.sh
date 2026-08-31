#!/bin/sh
# M0: builds QEMU's hard dependencies static against the guest toolchain and
# installs them into build/guest-deps. Three builds: zlib (configure-based),
# glib (meson; pulls pcre2 + libffi as subproject wraps from its own tarball),
# pixman (meson). Tarballs are SHA256-pinned; downloads are skipped when the
# pinned file is already present.
#
# Prereq: waterbox/setup-guest.sh has written build/guest-cross.ini.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
cross="$root/build/guest-cross.ini"
src="$root/build/deps-src"
deps="$root/build/guest-deps"
[ -f "$cross" ] || { echo "run waterbox/setup-guest.sh first" >&2; exit 1; }
mkdir -p "$src" "$deps"

ZLIB=zlib-1.3.1
GLIB=glib-2.84.4
PIXMAN=pixman-0.46.2
ZLIB_URL="https://github.com/madler/zlib/releases/download/v1.3.1/$ZLIB.tar.gz"
GLIB_URL="https://download.gnome.org/sources/glib/2.84/$GLIB.tar.xz"
PIXMAN_URL="https://cairographics.org/releases/$PIXMAN.tar.gz"
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
GLIB_SHA=8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90
PIXMAN_SHA=3e0de5ba6e356916946a3d958192f15505dcab85134771bfeab4ce4e29bbd733

fetch() { # fetch <file> <url> <sha256>
	f="$src/$1"
	[ -f "$f" ] || curl -sL -o "$f" "$2"
	echo "$3  $f" | sha256sum -c - >/dev/null || { echo "SHA256 mismatch: $1" >&2; exit 1; }
}

fetch "$ZLIB.tar.gz" "$ZLIB_URL" "$ZLIB_SHA"
fetch "$GLIB.tar.xz" "$GLIB_URL" "$GLIB_SHA"
fetch "$PIXMAN.tar.gz" "$PIXMAN_URL" "$PIXMAN_SHA"

# the guest compile flags for the non-meson build (zlib): pull them out of the
# cross file so there is exactly one place that defines them
cflags="$(sed -n "s/^c_args = \[\(.*\)\]$/\1/p" "$cross" | sed "s/', '/ /g; s/'//g; s/,//g")"

# ---- zlib ------------------------------------------------------------------
if [ ! -f "$deps/lib/libz.a" ]; then
	rm -rf "$src/$ZLIB" && tar -C "$src" -xzf "$src/$ZLIB.tar.gz"
	( cd "$src/$ZLIB" && CC=gcc CFLAGS="$cflags" ./configure --static --prefix="$deps" >/dev/null \
		&& make -j"$(nproc)" libz.a >/dev/null && make install >/dev/null )
	echo "zlib: installed"
else
	echo "zlib: present"
fi

# ---- glib ------------------------------------------------------------------
if ! ls "$deps"/lib*/libglib-2.0.a >/dev/null 2>&1; then
	rm -rf "$src/$GLIB" && tar -C "$src" -xJf "$src/$GLIB.tar.xz"
	meson setup "$src/$GLIB/build" "$src/$GLIB" --cross-file "$cross" \
		--prefix "$deps" --default-library static --buildtype release \
		-Dtests=false -Dinstalled_tests=false -Dnls=disabled -Dselinux=disabled \
		-Dlibmount=disabled -Dman-pages=disabled -Ddocumentation=false \
		-Dintrospection=disabled -Dsysprof=disabled -Dglib_debug=disabled \
		-Dlibelf=disabled -Ddtrace=disabled -Dsystemtap=disabled -Dxattr=false
	ninja -C "$src/$GLIB/build" >/dev/null
	meson install -C "$src/$GLIB/build" >/dev/null
	echo "glib: installed"
else
	echo "glib: present"
fi

# ---- pixman ----------------------------------------------------------------
if ! ls "$deps"/lib*/libpixman-1.a >/dev/null 2>&1; then
	rm -rf "$src/$PIXMAN" && tar -C "$src" -xzf "$src/$PIXMAN.tar.gz"
	meson setup "$src/$PIXMAN/build" "$src/$PIXMAN" --cross-file "$cross" \
		--prefix "$deps" --default-library static --buildtype release \
		-Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
		-Dopenmp=disabled
	ninja -C "$src/$PIXMAN/build" >/dev/null
	meson install -C "$src/$PIXMAN/build" >/dev/null
	echo "pixman: installed"
else
	echo "pixman: present"
fi


# ---- Linux UAPI headers ----------------------------------------------------
# QEMU includes <linux/futex.h> and friends; the musl sysroot carries none.
# The host's UAPI headers are kernel-licence (Linux-syscall-note) and are the
# standard companion to musl.
if [ ! -d "$deps/include/linux" ]; then
	cp -r /usr/include/linux "$deps/include/linux"
	cp -r /usr/include/asm-generic "$deps/include/asm-generic"
	cp -r /usr/include/x86_64-linux-gnu/asm "$deps/include/asm"
	# QEMU bundles newer copies of some of these (iommufd, kvm, vfio...);
	# ours must not shadow them - -I beats -isystem regardless of order
	for f in "$root/extern/xemu/linux-headers/linux/"*.h; do
		rm -f "$deps/include/linux/$(basename "$f")"
	done
	echo "linux uapi headers: installed"
else
	echo "linux uapi headers: present"
fi

echo "guest deps ready under $deps"
