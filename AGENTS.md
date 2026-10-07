# AGENTS.md - xemu core for Chimera

This repository turns xemu (the original Xbox emulator, a fork of QEMU) into a
core for Chimera, a frontend for tool-assisted speedruns. Upstream is the
`extern/xemu` submodule, built by its own configure and meson and changed only
by the numbered patches in `patches/` and the driver sources that
`waterbox/apply-patches.sh` copies in. The product is one file,
`xemu.chimeraCore`, which Chimera loads and runs inside its sandbox (miniBox)
on Linux and on Windows. Detail for every step below is in `docs/BUILDING.md`.

## Layout

- `extern/xemu` - upstream xemu, a submodule.
- `patches/` - the numbered patch series against `extern/xemu`.
- `waterbox/apply-patches.sh` - applies the series (judged as a whole) and
  copies the driver sources into the xemu tree.
- `waterbox/setup-native.sh` - configures the native reference.
- `waterbox/setup-guest.sh` - cross file, toolchain wrappers, the guest's
  dependencies (`build-deps.sh`: zlib, glib, pixman), then the guest configure.
- `waterbox/build-package.sh` - builds the guest and writes the package.
- `waterbox/xemu-waterbox.c` - the driver: headless main, frame loop, input,
  video, audio and savestate exports. `xemu-savedata.c`, `default-eeprom.c`,
  `guest-syscalls.c`, `chimera-latency.c`, `monitor-null.c`, `dsp-jit-null.c`
  and `det-pow.c` are copied into the xemu tree with it.
- `waterbox/gl-shim/`, `glad/`, `generated-gl/`, `generated-gl-host/`,
  `gl-host.c` - the GPU bridge, guest side and host side.
- `waterbox/samplerate/` - vendored, so both builds compute the same audio.
  `waterbox/run-wbx.c` - the host driver the gates run the core with.
- `waterbox/run-gate.sh`, `waterbox/tests/run-frontend.sh` - the core gate
  and the frontend gate.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json` - what the package declares.
- `docs/PLAN.md` - milestones, decisions, and what CI runs.

## Set up the build environment

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake pkg-config python3 bison flex curl libglib2.0-dev mono-complete libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev
# the contract tests need the .NET SDK 8.0; the frontend gate also needs xvfb

git submodule update --init                      # in this repository
# Chimera and miniBox (~/chimera is the scripts' fallback)
git clone https://github.com/ToolAssisted-run/chimera.git ~/chimera
git -C ~/chimera submodule update --init --recursive extern
mb=~/chimera/extern/chimera-common-minibox
meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The first build downloads pinned tarballs (pixman, zlib, glib), a newer meson
into a private venv when the system's is older than 1.4, xemu's meson
subprojects, and the GCC source for miniBox's libstdc++.

## Build

```sh
# from the root of this repository
mb=~/chimera/extern/chimera-common-minibox
sh waterbox/setup-native.sh                      # the native reference
ninja -C build/qemu-native qemu-system-i386
sh waterbox/setup-guest.sh -m "$mb"              # the guest
ninja -C build/qemu-guest qemu-system-i386
./waterbox/build-package.sh -m "$mb" -r ~/chimera
```

`build-package.sh` alone is the shortest path to a package: it configures the
guest when `build/qemu-guest` is not configured, builds, checks and packs it.

After editing a driver source under `waterbox/`, copy it in before building:

```sh
sh waterbox/apply-patches.sh
ninja -C build/qemu-guest qemu-system-i386
```

## Install the core into Chimera

`build-package.sh -r <chimera>` writes `<chimera>/build/Cores/xemu.chimeraCore`:
the cores folder of a Chimera source checkout, so nothing else is needed. For
a release bundle, copy the file into the `Cores` folder beside `Chimera.exe`
(or the folder chosen in File > Core Manager > Change folder...). Chimera
downloads nothing; File > Core Manager lists the folder, Refresh List rescans.
A hand build stamps `<commit>+local` (usually `-dirty` too): testing only.

## Test before you commit

What CI checks, and what must always be green, after both builds above (the
contract tests need Chimera's natives built, see `docs/BUILDING.md`):

```sh
sh "$mb/source/guest/check-wbx.sh" build/qemu-guest/qemu-system-i386
./waterbox/build-package.sh -m "$mb" -r ~/chimera
(cd ~/chimera && CHIMERA_CORES_DIR="$PWD/build/Cores" dotnet test \
  source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj -c Release --nologo \
  --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests")
```

That is all CI proves: both flavours build, the guest is sandbox-clean, the
package loads. No emulation leg runs in CI. A change to the machine, the
savestates, the GPU bridge or the patches needs the gate, and the gate needs
the user's firmware (layout in `docs/BUILDING.md`):

```sh
XBOX_FW_DIR=<firmware dir> XBOX_DVD_PATH=<disc image> XBOX_GPU=1 \
  MINIBOX_DIR="$mb" waterbox/run-gate.sh 1200
XBOX_FW_DIR=<firmware dir> XBOX_DVD_PATH=<disc image> \
  waterbox/tests/run-frontend.sh --chimera-root ~/chimera
```

Every line the gate prints must be PASS, or a SKIP you can explain. Without
the firmware on this machine, say in the commit that the gate was not run. Do
not run two gates at once: they share `build/gate/`.

## Rules of this repository

- Never commit inside `extern/xemu`. A change to xemu is a numbered patch in
  `patches/`, applied by `waterbox/apply-patches.sh`.
- The driver sources live in `waterbox/` and are copied into the xemu tree by
  `apply-patches.sh`. Edit them in `waterbox/`, never the copies, and run the
  script before building.
- The patched tree must be exactly what the whole series leaves behind:
  `apply-patches.sh` refuses anything else.
- Determinism is the product. The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate must
  round-trip. The gate checks it; a change that breaks it is a bug.
- A new leg needs a negative control: show it fails when the thing it checks
  is broken, and say so in the commit.
- Never commit firmware, disc images or anything extracted from them (see
  `.gitignore`). Never add network access.
- Do not reorder the buttons or axes in `waterbox/waterbox.config`: the order
  is the wire format.
- Shell scripts stay executable (git mode 100755). Documentation prose is
  plain ASCII.
- Commit messages: `type(scope): a full sentence saying what is now true`.
  Types in use: `feat`, `fix`, `test`, `perf`, `docs`, `ci`. The body says
  what was wrong, what was measured and what the gate said. Issues are filed
  in the chimera repository; a fixing commit cites `ToolAssisted-run/chimera#N`.
- A decision or a finding worth keeping gets a dated section in `docs/PLAN.md`.
- Do not edit `.github/workflows` unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every build step, option and known failure.
- `docs/PLAN.md` - the reasoning; "What CI runs, and what it does not" lists
  the legs, the firmware layout and who runs the gate.
- `.github/workflows/chimera.yml` - the authoritative recipe.
- In the Chimera checkout: `docs/porting-a-core.md`, `docs/gates.md` (how a
  gate goes green on a broken thing) and `docs/core-manager.md`.
