# chimera-core-xemu

The original Xbox as a [Chimera](https://github.com/ToolAssisted-run/chimera)
waterbox core, built from [xemu](https://xemu.app) - itself a fork of QEMU.
The whole machine (a 733MHz Pentium III, the NV2A GPU, the MCPX APU with its
DSPs, four Duke controllers on a real USB bus) runs deterministically inside
the miniBox sandbox: byte-for-byte reproducible boots, savestates that are
arena snapshots, and a GPU bridge that lets xemu's own OpenGL renderer draw
on a real driver when a picture is wanted over portability.

Unlike every other Chimera core, this one drives xemu's OWN configure/meson
(QEMU's code generation makes a curated source list infeasible): a native
reference build and a sandboxed cross build, changed only by the numbered
patches in `patches/` and the driver sources `apply-patches.sh` copies in.

## Building

    waterbox/setup-native.sh && ninja -C build/qemu-native qemu-system-i386
    waterbox/setup-guest.sh  && ninja -C build/qemu-guest  qemu-system-i386
    waterbox/build-package.sh        # -> <chimera>/build/Cores/xemu.chimeraCore

## The gate

Machine firmware is user-supplied and never enters this repository
(`$XBOX_FW_DIR`, default `~/xbox-roms/Xbox BIOS`: the MCPX boot ROM, a flash
BIOS, a hard disk image). The mounted images are never written - all writes
land in an in-memory overlay.

    XBOX_DVD_PATH=<game.iso> XBOX_GPU=1 waterbox/run-gate.sh 1200

Six legs, all byte-for-byte: native A/B, native == sandbox, the audio
stream, savestates reloaded around every frame, held input reaching the
game, and the GPU leg - where, because the Xbox is UMA and rendered
surfaces download back into RAM, "the machines agree" includes the picture.

    XBOX_DVD_PATH=<game.iso> waterbox/tests/run-frontend.sh

boots the package inside the Chimera GUI (headless, Xvfb) and holds the
frontend's machine to the same standard.

Do not run two gates at once: they share `build/gate/` and the comparisons
race.

## What is here

- `waterbox/xemu-waterbox.c` - the driver: headless main, the frame loop
  (one 16666667ns virtual-time slice per frame under icount), inputs,
  video, audio and savestate exports.
- `waterbox/chimera-latency.c` - the block filter: every disk request
  completes at the next frame boundary (deterministic in both thread
  models), writes go to an in-memory COW overlay.
- `waterbox/gl-shim/`, `waterbox/glad/`, `waterbox/generated-gl/` - the GPU
  bridge: gloffscreen rebuilt over glad, the generated guest wrappers, and
  the EGL context the native reference renders through.
- `waterbox/samplerate/`, `waterbox/det-pow.c` - vendored so both builds
  compute identical audio samples.
- `patches/` - numbered, one owner per upstream file; `docs/PLAN.md` tells
  the whole story, mechanism by mechanism.
