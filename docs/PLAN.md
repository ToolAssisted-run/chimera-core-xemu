# xemu as a Chimera core - analysis and plan

xemu is an original Xbox emulator built as a fork of full QEMU (pinned base:
xemu master d73326b6, 2026-08-26, QEMU 10.2.0 - the last release tag v0.8.99
is a year stale and misses a year of nv2a work). Unlike every previous core,
there is no BizHawk precedent and no curated-source-list build: QEMU's meson
does heavy code generation (QAPI, trace events, per-target configs), so this
port drives xemu's OWN configure/meson twice - a native reference build and a
guest build with the waterbox flags injected - and carries its changes as
numbered patches applied to the submodule (the pcsx2 convention).

## Why this is feasible at all

- miniBox has deterministic green threads with futex emulation. QEMU's thread
  population (vCPU, iothread, RCU, nv2a pfifo, MCPX APU frame thread, voice
  workers) runs as guest threads under cooperative deterministic scheduling.
- miniBox supports RWX pages, so the TCG JIT runs inside the arena. The arena
  base is fixed, so generated code is a pure function of guest state, and
  whole-arena savestates carry the translation cache. `-Dtcg_interpreter=true`
  (TCI) is the fallback if the JIT misbehaves.
- QEMU's `icount` mode drives the virtual clock from executed instructions:
  no host time reaches emulation, and the vCPU yields at virtual timer
  deadlines - which is also what makes a spinning vCPU cooperate with green
  threads. icount is mandatory for this port.
- `qemu_main_loop()` is `while (...) main_loop_wait(false)`. Frame boundary =
  run `main_loop_wait` until the vblank fires (xemu's 16.67ms REALTIME timer
  becomes a virtual-clock timer), then return to the host. Same shape as
  flycast-after-the-fix.
- Firmware: xemu wants MCPX boot ROM, flash BIOS (MS kernel), EEPROM (xemu can
  generate one), HDD image. Maps 1:1 onto Chimera's firmware channel; the 8GB
  HDD rides the dosbox-x sparse-overlay mechanism.
- pgraph has three renderers: `gl` (OpenGL 4.0 core - past softpipe's 3.3
  ceiling), `vk` (Vulkan), and `null` (146-line stub). M1 gates use null;
  real video is the GPU bridge (proven on PS2, unproven on Windows).
- Cromwell (the GPL Xbox BIOS) boots the machine without the MS kernel: the
  no-copyrighted-firmware gate, same trick as Opera's dummy BIOS.

## The two structural risks

1. glib >= 2.66 is a hard QEMU dependency, woven through everything. It must
   be built static against the musl guest toolchain (M0). Its meson build
   pulls pcre2/libffi as subprojects, which keeps M0 to three dep builds
   (zlib, glib, pixman).
2. Video requires the GPU bridge (GL 4.0). Until the bridge ships, xemu is
   headless-plus-machine-state only.

## Milestones

- M0: guest-toolchain static builds of zlib, glib (+pcre2/libffi wraps),
  pixman, installed into build/guest-deps; pkg-config resolves them.
- M1: headless boot. i386-softmmu only, TCG, icount, null renderer, UI layer
  replaced by a waterbox driver, SDL/imgui/slirp/vulkan/tools all off.
  Gate: Cromwell boots, N frames, machine state native == sandbox byte for
  byte.
- M2: real MCPX/kernel via firmware channel (user-supplied, never in repo),
  HDD via sparse overlay, xid gamepad input, frame-at-vblank, video via the
  null renderer's VGA scanout if any, else machine-state-only.
- M3: audio (MCPX APU + dsp56300 subproject; voice worker threads pinned to
  a deterministic count).
- M4: video via the GPU bridge (GL 4.0 renderer).
- M5: savestates (arena snapshot; audit for host handles), movies, packaging,
  licences (QEMU is GPL-2.0; the bundle addendum follows the dosbox-x model).

## Build facts discovered so far

- meson 1.5.1 / ninja 1.11.1 / python 3.12 on the box; guest toolchain at
  extern/tools/chimera-common-minibox/build/meson-cpp (guest_cpp=true build,
  libstdc++ sysroot present).
- QEMU subprojects that matter: berkeley-softfloat-3, berkeley-testfloat-3,
  keycodemapdb, tomlplusplus, xxhash, nv2a_vsh_cpu, dsp56300 (M3). All meson
  wraps (network fetch at setup). The roms/* git submodules are other-machine
  blobs xbox never uses - left uninitialised.
- xemu's own UI (ui/xemu*.c, imgui, SDL3) hosts qemu_main on a second thread
  and owns main(); the port replaces that layer entirely.
- Rust in QEMU 10.2 is optional and stays off.
