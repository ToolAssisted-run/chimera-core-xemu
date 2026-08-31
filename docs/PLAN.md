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

## M1 status: COMPLETE - native == sandbox byte for byte (2026-08-31)

`waterbox/run-gate.sh N` runs the full ritual: two native runs must agree,
and the waterboxed core.wbx must produce the byte-identical machine-state
stream. PASS at 60 and 300 frames of real-firmware boot.

The sandbox leg took, beyond the native work below:
- The TLS wall (the PPSSPP lesson at QEMU scale): __thread is fs-relative
  and fs still points at HOST TLS in the box. 26 TLS symbols. Fixes:
  coroutine-tls.h rewritten over pthread keys under CHIMERA_GUEST (11 syms,
  incl. rcu_reader and the coroutine core), current_cpu behind a pthread-key
  accessor macro, `-D__thread=` sweeps the rest into plain globals (safe
  one-at-a-time under green threads), pixman rebuilt with -DPIXMAN_NO_TLS.
  `readelf -sW core.wbx | awk '$4=="TLS"'` MUST stay empty.
- QEMU's coroutines: the sigaltstack backend needs real signal delivery;
  forced --with-coroutine=ucontext and gave the guest a 70-line
  makecontext/swapcontext (waterbox/guest-ucontext.c) - QEMU only ever
  enters each coroutine once through it, all later switching is setjmp.
- waterbox/guest-syscalls.c: fake eventfd/pipe/signalfd fds in guest memory,
  ppoll/poll that yield to the green threads, preadv/pwrite emulation,
  open() minus the O_CLOEXEC-induced internal fcntl, no-op sigaction/prctl,
  ENOSYS epoll/memfd (QEMU falls back), fixed sysinfo + __lsysinfo,
  deterministic getrandom.
- The block filter grew cow=on: writes into an in-memory chunk overlay, the
  child opened read-only (a mounted file), write perms masked in
  .bdrv_child_perm. Identical arrangement native and sandbox, so the disk
  timing matches exactly.
- Exports: Init/FrameAdvance/GetStateSize/GetStateData (+GetLoadError);
  main() returns 0, work happens in exports; the state stash is copied out
  before qemu_fclose (the buffer channel frees its bytes on close).

## The native determinism story (still true, prerequisite)

`waterbox/run-determinism-native.sh N` boots the real firmware (MCPX 1.0 +
Complex 4627 + xbox_hdd.qcow2, user-supplied, never in this repo) headless
for N frames twice from pristine copies and compares the full migration
stream byte for byte. PASS at 60 and 300 frames (5 virtual seconds,
~8MB of state).

What it took - each of these was found by an actual diverging byte, in
order, and lives in patches/ + waterbox/:

1. Frame boundary: one 16.667ms QEMU_CLOCK_VIRTUAL slice per frame under
   `-icount shift=5,sleep=off -rtc base=2000-01-01,clock=vm`. icount sleep
   MUST be off: sleep=on warps by measured host time.
2. cpu_ticks_offset/cpu_clock_offset are host-clock deltas saved into the
   state; on XBOX nothing consumes them (the TSC is virtual-clock derived
   upstream), so pre_save canonicalises them (patch 0006).
3. EEPROM: xemu mints it with real randomness. It is per-project persistent
   data; the gate mints once and copies per leg.
4. The APU frame thread paced EP frames against the host clock and the SDL
   audio queue; headless it is a QEMU_CLOCK_VIRTUAL timer at EP_FRAME_US
   (patch 0004). Voice workers pinned to 1.
5. The nv2a pfifo thread consumed pushbuffers at host speed; headless a
   kick arms a drain timer at virtual-now and the drain runs from the main
   loop (patch 0005). Draining synchronously inside the kicker's MMIO stack
   deadlocks: pgraph work can longjmp back to the CPU loop and leak locks,
   and pgraph_write holds pfifo.lock + pg->lock. The thread's bql_lock
   dances become conditional on bql_locked().
6. Disk I/O completed at host time; block/chimera-latency.c (copied in, wired
   by patch 0003) makes every request complete exactly 2ms of VIRTUAL time
   after submission - the interrupt's position in the instruction stream
   can no longer depend on the host's storage. Requests issued while the vm
   is stopped (realize-time geometry probe, savevm) complete immediately.
7. The warp governor (driver): with sleep=off an idle guest's clock leaps
   to the next deadline, overtaking in-flight I/O in host-dependent ways; a
   permanently pending 10us virtual timer bounds every leap.
8. All periodic timers advance by ABSOLUTE deadline (re-arming off "now"
   accumulates callback lateness = host timing), and the frame boundary
   callback calls vm_stop(RUN_STATE_PAUSED) while it still holds the BQL -
   the rr thread computes instruction budgets under the BQL, so not one
   instruction runs past the boundary.

Debug lore: scripts/analyze-migration.py decodes the state stream; diffing
two runs' JSON names the diverging device instantly. `-Ddebug_mutex=true`
records file:line of every mutex's last taker. Watch for orphaned qemus
holding qcow2 write locks after killed runs - they make later runs hang at
startup, which looks exactly like a new deadlock.

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
