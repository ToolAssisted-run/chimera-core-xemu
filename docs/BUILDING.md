# Building the xemu core

This repository builds one file, `xemu.chimeraCore`: xemu (the original Xbox
emulator, a fork of QEMU) built as a sandboxed guest, together with the
declarations Chimera reads. The guest is built by xemu's own configure and
meson; `core.wbx` inside the package is the `qemu-system-i386` that build
links. The steps below are the ones `.github/workflows/chimera.yml` runs from
a fresh clone on a public Ubuntu runner.

What CI proves for this core is small, and it is stated plainly in the
workflow: both flavours build from a clean checkout, the guest binary passes
the sandbox's own checks, and the package loads in the frontend's contract
tests. Nothing in CI executes an instruction of the machine, because an Xbox
cannot start without a boot ROM and a flash ROM, and neither may be
distributed. Every emulation leg is run by hand (see "Run the gates").

Placeholders used below:

- `<core>` - the checkout of this repository. Commands run from it unless
  stated otherwise.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, the sandbox host and
  the guest toolchain.

## Requirements

Operating system: CI uses the `ubuntu-latest` runner. Cores are built on Linux
only. The package that comes out runs on Linux and on Windows.

System packages, as the workflow installs them:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3 bison flex curl libglib2.0-dev mono-complete libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev
```

The workflow pins no compiler version: the default `gcc` and `g++` are used.

.NET: the workflow uses `actions/setup-dotnet@v4` with `dotnet-version: '8.0'`.
By hand, install the .NET SDK 8.0. Chimera's README gives the command it
expects: `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`.
.NET and `mono-complete` are needed for Chimera's contract tests and for the
frontend gate, not for building the package.

Downloaded and built by the scripts themselves. Each tarball is pinned by
SHA-256 and kept in `build/deps-src`, so it is fetched once:

- `waterbox/setup-native.sh` fetches pixman 0.46.2 and builds it static into
  `build/native-deps`, for the native reference.
- `waterbox/build-deps.sh` (called by `setup-guest.sh`) fetches zlib 1.3.1,
  glib 2.84.4 and pixman 0.46.2 and builds them static for the guest into
  `build/guest-deps`. glib brings pcre2 and libffi as subprojects from its own
  tarball.
- glib 2.84 needs meson 1.4.0 or newer and Ubuntu 24.04 ships 1.3.2. When the
  system meson is too old, `build-deps.sh` makes a private Python venv in
  `build/meson-venv`, installs `meson>=1.4` into it with pip and uses that.
  The system meson is not touched.
- `build-deps.sh` copies the host's Linux UAPI headers (`/usr/include/linux`,
  `asm-generic` and the x86_64 `asm`) into `build/guest-deps/include`.
- xemu's own meson fetches the subprojects it needs as wraps when it is set
  up (`docs/PLAN.md`, "Build facts discovered so far", lists them).
- When miniBox builds its C++ guest toolchain it fetches the GCC source that
  matches the host compiler (about 84 MB) to build libstdc++ for the guest.

So the build needs network access the first time.

Time: the workflow gives the job 120 minutes.

Sources: Chimera is taken at its `main` branch (`CHIMERA_REF` in the
workflow). xemu is whatever commit the `extern/xemu` submodule points at.

## Get the sources

Clone this repository and its submodule. The workflow does it with
`actions/checkout@v6` and `submodules: true`, which is one level:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-xemu.git <core>
cd <core>
git submodule update --init
```

That step does not initialise xemu's own nested submodules.

Get Chimera and the sources it is built from (this includes miniBox):

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
cd <chimera>
git submodule update --init --recursive extern
```

CI checks out Chimera's `main` branch into `chimera-checkout` inside the core
checkout and passes that path to the scripts.

Where the scripts look when you pass nothing:

- `waterbox/build-package.sh` looks for Chimera at `../chimera` beside this
  repository, then at `$HOME/chimera`. `-r <chimera>` names it. miniBox is
  `<chimera>/extern/chimera-common-minibox` unless `-m <miniBox>` says
  otherwise.
- `waterbox/setup-guest.sh` takes `-m <miniBox>` or the `MINIBOX_DIR` variable,
  and falls back to `$HOME/chimera/extern/chimera-common-minibox`.
- `waterbox/run-gate.sh` takes `MINIBOX_DIR`, then tries
  `../chimera/extern/chimera-common-minibox`, then the same under `$HOME`.

## Build miniBox

Two build directories: the host library, and the C++ guest toolchain (musl and
libstdc++ in a guest sysroot).

```sh
mb=<chimera>/extern/chimera-common-minibox
meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

CI keeps both directories between runs with `actions/cache@v4` and skips each
`meson setup` when the directory's `build.ninja` is already there.

## Build the core

### Patches and driver sources

`patches/` holds the numbered patch series against `extern/xemu`.
`waterbox/apply-patches.sh` applies it, and `setup-native.sh` and
`setup-guest.sh` both run it first. The series is judged as a whole:

- a pristine submodule gets every patch, in order;
- a tree that already carries the whole series is left alone
  (`already applied: all N patches`);
- anything in between is an error that names the files and prints the command
  that starts again from the submodule's HEAD;
- a series that does not apply to the submodule's HEAD is an error before the
  tree is touched;
- a submodule that is not checked out is an error that says so.

The same script also copies this repository's own driver sources from
`waterbox/` into the xemu tree, on every run and whatever state the patches
were in: `xemu-waterbox.c`, `xemu-savedata.c`, `default-eeprom.c`,
`guest-syscalls.c`, `monitor-null.c`, `chimera-latency.c`, `dsp-jit-null.c`,
`det-pow.c`, the GL shim with `glad` and the generated GL wrappers, and
`samplerate/`. The build compiles the copies. For `gl-bridge.h` it takes
miniBox's copy when a Chimera checkout sits at `../chimera`, and the one in
`waterbox/gl-shim/` otherwise.

### The native reference

xemu built headless with the host toolchain. The gate compares the sandboxed
core against it. A package does not contain it. CI builds it first:

```sh
sh waterbox/setup-native.sh
ninja -C build/qemu-native qemu-system-i386
```

`setup-native.sh` takes no options. It applies the patches, builds pixman when
`build/native-deps` does not have it, and runs xemu's configure for
`i386-softmmu` only, with SDL, GTK, VNC, networking, KVM and Xen off. The
result is `build/qemu-native/qemu-system-i386`.

### The guest

```sh
sh waterbox/setup-guest.sh -m "$mb"
ninja -C build/qemu-guest qemu-system-i386
```

`setup-guest.sh` writes the meson cross file `build/guest-cross.ini` and the
toolchain wrappers (`build/guest-cc`, `build/guest-cxx`, `build/guest-bin/`),
runs `build-deps.sh` when the guest's zlib, glib or pixman is missing, applies
the patches, and runs xemu's configure for a static cross build. The files it
writes hold machine-local paths; they live under the ignored `build/`. The
result is `build/qemu-guest/qemu-system-i386`, which is the core.

## Build the package

```
waterbox/build-package.sh [-r <chimera root>] [-m <miniBox dir>]
```

There is no `-o` option. The script:

1. runs `setup-guest.sh -m <miniBox>` when `build/qemu-guest/build.ninja` is
   missing;
2. runs `ninja -C build/qemu-guest qemu-system-i386` and copies the result to
   `build/core.wbx`;
3. runs miniBox's `check-wbx.sh` on it;
4. stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json`, the licence texts named by
   `waterbox/package-licenses.json` and a `build.json` that records what built
   the package;
5. zips them with sorted entries and fixed timestamps into
   `<chimera>/build/Cores/xemu.chimeraCore` and prints the file's SHA-1.

Version stamp, written into the packaged `waterbox.config`:

- CI sets `CORE_VERSION` to the commit:
  `CORE_VERSION=<commit> ./waterbox/build-package.sh -m "$mb" -r <chimera>`.
- Without `CORE_VERSION` the script stamps `<commit>+local`, or
  `<commit>-dirty+local` when the tree has changes. The build patches
  `extern/xemu` in place, which counts as a change, so a hand build normally
  says `-dirty`.
- `versionDate` is the commit's date in UTC, never the build's.

A hand-built package is for testing. Chimera's publishing script refuses a
version that carries `+local` or `-dirty`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets into Chimera because somebody puts the file in its `Cores` folder.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`,
  and `build-package.sh -r <chimera>` has already written the package there.
- In a release bundle, copy `xemu.chimeraCore` into the `Cores` folder beside
  `Chimera.exe`, or into the folder chosen in File > Core Manager >
  Change folder... The same file works on Linux and on Windows.
- File > Core Manager lists what is in the folder. Refresh List rescans it.

Published builds are on this repository's Releases page: a rolling `dev`
release on every green push to `main`, and a dated `nightly-YYYY-MM-DD` release
from the scheduled run when `main` moved since the last one. Their asset is
named `xemu-<version>.chimeraCore`.

## Run the gates

### What CI runs

```sh
sh "$mb/source/guest/check-wbx.sh" build/qemu-guest/qemu-system-i386
```

Proves the guest binary is one the sandbox accepts: no thread-local storage,
no forbidden imports, the layout the host maps. It needs no firmware.

CI then prints one SKIP line for each emulation leg and the line
`0 of 10 emulation legs ran here`. It does not run `waterbox/run-gate.sh` or
`waterbox/tests/run-frontend.sh`.

The contract tests are the other thing CI runs. They are Chimera's own tests,
run against the package just built, and they need Chimera's natives because
they open the package through the engine:

```sh
cd <chimera>
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
CHIMERA_CORES_DIR="$PWD/build/Cores" dotnet test source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj \
  -c Release --nologo \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

They prove the package is readable, is built for an ABI this frontend runs,
becomes a working factory, binds only buttons its controller declares, and
stamps a version. No disc, no firmware and no emulation are involved.

### The core gate (by hand, with firmware)

```sh
XBOX_FW_DIR=<firmware dir> MINIBOX_DIR="$mb" waterbox/run-gate.sh [frames]
```

The frame count defaults to 60. The gate needs both builds
(`build/qemu-native/qemu-system-i386` and `build/qemu-guest/qemu-system-i386`)
and miniBox's `build/meson-cpp`. It builds its own host driver,
`build/run-wbx`, and rebuilds it when its sources are newer. Output goes to
`build/gate/`, one directory per leg. It exits non-zero when a leg failed.

The firmware is read from `$XBOX_FW_DIR` (default `~/xbox-roms/Xbox BIOS`),
laid out exactly like this. The names are literal:

```
$XBOX_FW_DIR/
  MCPX Boot ROM/mcpx_1.0.bin
  Flash ROM (BIOS)/Complex_4627v1.03.bin
  Hard Disk/xbox_hdd.qcow2
```

The files are never written: all writes land in an in-memory overlay. There
is no SKIP for missing firmware. Without it the gate fails, which is why CI
does not run it.

Legs, all byte for byte:

- native deterministic - two native runs leave the same machine state.
- native == sandbox - the sandboxed core leaves that same state.
- audio - the two native runs and the sandbox produce the same samples. That
  the samples are sound and not silence is asked only at 600 frames or more.
- savestate - a save and load around every frame changes nothing.
- input - a held button reaches the machine, native == sandbox. Needs
  `XBOX_DVD_PATH=<disc image>` and 1200 frames or more; SKIP otherwise.
- gpu - the GL renderer draws through a real driver, native == sandbox on
  this driver. Needs `XBOX_GPU=1`; SKIP otherwise.
- gpu:internalResolution - the setting reaches the renderer and a scaled
  render is a different machine. The picture's size is asked of the boot
  animation (260 frames), which is the same whatever disc is in the drive.
  Needs `XBOX_GPU=1` and `XBOX_DVD_PATH`.
- gl:rebuild-at-zero - a restore to the frame-0 state survives and the
  renderer rebuilds. Needs `XBOX_DVD_PATH`, plus `chimera-run` and an
  installed package in a Chimera checkout (`CHIMERA_ROOT`, or `../chimera`, or
  `$HOME/chimera`).
- gl:picture-after-load - six frames of the boot animation drawn right after
  a state load are the pictures they were, and with the engine leaving the
  core untold (`CHIMERA_NO_STATE_SAVING=1`) the first is not. Needs the same
  as gl:rebuild-at-zero, and a Chimera whose `chimera-run` has
  `--settle-probe` (4c029b1 or later).

The full local run that `docs/PLAN.md` gives:

```sh
XBOX_FW_DIR="$HOME/xbox-roms/Xbox BIOS" XBOX_DVD_PATH=/path/to/game.iso XBOX_GPU=1 \
  MINIBOX_DIR=~/chimera/extern/chimera-common-minibox waterbox/run-gate.sh 1200
```

`waterbox/run-determinism-native.sh [frames]` is the native determinism leg on
its own: two native runs compared byte for byte. It needs the same firmware.

### The frontend gate (by hand, with firmware and a disc)

Runs the package inside Chimera, headless under Mono on a private Xvfb
display (install `xvfb`). It needs Chimera's natives and solution built:

```sh
cd <chimera>
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
cd <core>
XBOX_FW_DIR=<firmware dir> XBOX_DVD_PATH=<disc image> waterbox/tests/run-frontend.sh --chimera-root <chimera>
```

It also accepts `--frames N` (default 600). It needs
`<chimera>/build/Chimera.exe`, the installed package and `build/run-wbx`,
which the core gate builds. Three legs: `boot:frontend` (the machine's RAM in
the frontend equals the sandbox reference's), `gpu:frontend` (the same with
the GPU, and a screenshot is written) and `keybinds` (the package's bindings
became the frontend's). Without the firmware, or without a disc, all three
print SKIP and the script exits 0. It ends with `N ok, M failed, K skipped`.

Do not run two gates at once.

`docs/PLAN.md`, "What CI runs, and what it does not", says who runs these
legs and when.

## Files the core needs at run time

None of these is in the repository or in the package. The user provides them.

Firmware, declared in `waterbox/waterbox.config`:

- MCPX Boot ROM (`mcpx_1.0.bin`, 512 bytes) - the first code the machine runs.
  Nothing boots without it.
- Flash ROM (BIOS) (`complex_4627.bin` is the declared name, 1 MiB) - the
  machine's flash image: a retail dump or a homebrew BIOS. 256 KiB and 512 KiB
  images are also accepted.
- Hard Disk Image (`xbox_hdd.qcow2`) - the hard disk as a qcow2 or raw image,
  holding the dashboard and the save partitions. It is never written.
- EEPROM (`eeprom.bin`, 256 bytes) - optional. Without one the core uses a
  built-in identity that is the same for everyone.

Project files, declared in `waterbox/file_slots.json`:

- Disc (optional, at most one): the game disc as an xiso image (`.iso`,
  `.xiso`). With no disc the machine boots to its dashboard from the hard
  disk image.
- Save data (any number): per-title save files (`.xbx`, `.bin`, `.sav`,
  `.dat`) under `UDATA/<titleId>/<save>/`, as Emulator > Export Save Data...
  writes them.

## Troubleshooting

- `extern/xemu is not checked out`: initialise the submodule
  (`git submodule update --init`).
- `miniBox C++ guest toolchain missing at ...`: build `<miniBox>/build/meson-cpp`
  with `-Dguest_cpp=true` (see "Build miniBox").
- `SHA256 mismatch: <file>`: a downloaded tarball in `build/deps-src` is not
  the pinned one. Remove it and run the script again.
- `glib-2.0 not found` from xemu's configure: the guest dependencies are not
  in `build/guest-deps`. `setup-guest.sh` builds them through `build-deps.sh`;
  run it again and read its output.
- An edit to a driver source under `waterbox/` does not reach the build: the
  build compiles the copy inside `extern/xemu`, and neither `ninja` nor
  `build-package.sh` refreshes it. Run `sh waterbox/apply-patches.sh`, then
  build.
- `extern/xemu is partly patched`: the tree is neither pristine nor exactly
  what the series leaves. The script prints the command that resets the
  submodule and applies the series again. That command discards edits made in
  the tree; turn them into a patch first.
- `the series does not apply to the submodule's HEAD at <patch>`: the
  submodule was moved without rebasing the patches.
- `miniBox not found; set MINIBOX_DIR` from the gate: set it, or keep Chimera
  at `../chimera` or `$HOME/chimera`.
- `eeprom mint failed`, or `native leg ... ran without its firmware`: the
  firmware is not where `$XBOX_FW_DIR` says, or not laid out as shown above.
- The frontend gate prints SKIP for everything: it needs the firmware and,
  unlike the core gate, a disc. Chimera starts a machine from a ROM, and an
  Xbox with no disc has none to hand it.
- `run-wbx not built`: run the core gate first; it builds `build/run-wbx`.
