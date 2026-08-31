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

## M2 status: the real boot - kernel up, game loading, gates green

At authentic speed (shift=0) the machine truly boots: MCPX bootrom, 2BL
kernel decrypt (RC4 + SHA-1), kernel at 0x80010000, DVD mount, and Prince
of Persia's Dare engine loading with PCRTC scanning out its framebuffer
(black by design: null renderer until the GPU bridge). ~600 frames in under
a minute of wall clock. `run-gate.sh 600` passes both legs, and with
XBOX_DVD_PATH set the same two legs pass with the disc in (the state grows
to ~16MB as the game fills RAM).

What the real boot flushed out, each found as a hang or a diverging byte:

9.  Warp only for a truly halted guest (patch 0008): cpu_thread_is_idle
    counts a merely STOPPED cpu as idle, and right after vm_start there is
    a window before the vCPU thread clears stopped. icount warped whole
    frame budgets into bias at host-dependent instants. Now every cpu must
    be genuinely HLTed (and without pending work) before the warp timer
    arms.
10. The NV2A PTIMER alarm respin (patch 0008): the alarm distance in ns can
    floor to zero while the virtual clock is frozen during timer
    processing, so the alarm re-arms at the same instant forever - the vCPU
    thread spins at 100% host CPU executing nothing. A nonzero distance now
    rounds up to at least one reg tick.
11. vblank must be DELIVERED: with -display none there is no console loop,
    so the driver calls graphic_hw_update(qemu_console_lookup_by_index(0))
    once per frame (NULL means no console and silently no-ops). The kernel
    idle loop waits on a GPU progress counter that only advances when the
    vsync ISR pokes NV_PGRAPH_INCREMENT.
12. Sleepers must not outlive the stop: vm_stop drains all block requests,
    and a filter sleeper waiting on a frozen virtual clock stalls the drain
    for the whole host timeout. The filter's vm-change-state handler
    releases every sleeper the moment the runstate leaves running - which
    is exactly the delivery model of mechanism 6.

The rest of M2, all gated by `XBOX_DVD_PATH=... run-gate.sh 1200`:

- Input: four Duke pads are always plugged (driver replicates the GUI's
  usb-hub + usb-xbox-gamepad qdev creation; xid.c polls the pads straight
  out of the driver's ControllerState array). FrameAdvance's packed word
  carries 4 x 14 buttons port-major; SetButton/SetAxis carry the rest.
  A booted game polls from ~frame 974 (vclock 16.2s); the DASHBOARD never
  starts the OHCI controller at all, so the input leg needs the DVD: hold
  START from frame 1000, machine must differ from the plain run and
  native must equal sandbox under the press. The xid in_state is NOT in
  the migration stream - only input the guest software actually consumed
  can leave a trace.
- Video: GetVideoBgra/GetVideoWidth/GetVideoHeight read the console
  DisplaySurface that the per-frame graphic_hw_update makes the VGA core
  render out of VRAM (PCRTC start, CRTC mode, nv2a's 15/16/32 bpp hook),
  pixman-converted to BGRA. Null renderer keeps 3D black until the GPU
  bridge; the mode logic and scanout path are the real ones.
- Savestates: the miniBox arena snapshot simply works - run-wbx --rerecord
  saves and reloads the state around EVERY frame and the machine is
  byte-identical to the plain run (the gate's savestate leg, 60 frames).

## M3 status: audio - the machine makes sound, gated byte for byte

GetAudio/GetAudioSampleCount export interleaved s16 stereo pairs at 48kHz;
waterbox/monitor-null.c accumulates the APU monitor's EP frames (256 pairs
each) and the driver drains the accumulator per video frame. The dashboard
boot jingle comes out as real samples (peak ~10000, seven seconds with the
fade, then menu ambience). The gate's audio leg requires native A == B ==
sandbox on the whole sample stream and refuses pure silence past 600
frames. run-wbx --audio-out / native CHIMERA_AUDIO_OUT dump the stream.

What it took, beyond M1's virtual-clock APU (patch 0004 + 0002):

13. The vframe tick was 8x too slow: se_frame mixes a 32-sample VP
    sub-frame and eight of them make one 256-sample EP frame - upstream's
    thread throttles only when ep_frame_div hits a multiple of 8, so the
    virtual timer must tick every EP_FRAME_US/8 (666.6us), not every
    EP_FRAME_US. This also puts NV_PAPU_XGSCNT (ep_frame_div * 32, the
    guest-visible 48kHz timebase) at the right rate.
14. pause_requested stayed true forever: realize sets it, and only the SDL
    thread's resume path cleared it - headless, voice_work_dispatch
    silently dropped its queue every frame, which is why every voice
    "played" but produced nothing. wait_for_idle/resume now track the flag
    (they are called from the vm-state-change handler), so voice work runs
    exactly while the machine runs.
15. The locked-voice wait would livelock: voice_work_dispatch runs in
    virtual-timer context on the vCPU thread with the BQL held, and
    upstream's 1ms cond_timedwait waits for the guest to unlock a voice -
    the guest that cannot run because we hold its thread. A voice locked
    at the tick instant means the guest is mid-update: the VP frame is
    skipped deterministically (lock state is machine state).

16. 94 samples in 24 seconds came out one LSB apart between native and
    sandbox while machine state stayed byte-identical (the monitor tap
    never feeds back). The real culprit, found by printing MXCSR at the
    divergence: xemu's TCG SSE helpers execute the GUEST's ldmxcsr on the
    HOST, so when the game programs round-toward-zero, the emulator's own
    float math inherits it - on whichever host thread runs guest code.
    Natively the APU work sat on a thread the guest never touched; in the
    sandbox the green threads share one host FPU, and the audio math
    started rounding the way the game likes at the exact virtual instant
    the game set its FPU up. apu_vframe_tick now pins the default MXCSR
    (0x1f80) for the mixer and hands the guest's value back afterwards.
    Two related hardenings landed on the way to finding it, and stay:
    hw/xbox/mcpx/apu/det-pow.c gives both builds the SAME powf/pow
    (musl's, verified 0 ULP vs glibc powf over 2M inputs - glibc/musl
    genuinely differ by a ULP on double pow), and libsamplerate 0.2.2 is
    vendored into the tree (samplerate/, SINC_FASTEST only) so the
    resampler is one TU compiled by both builds instead of two library
    builds with two compilers' ideas of the same arithmetic.

Boot lore that fell out of the debugging: the game never hung at all - the
"stall" at frame ~490 is the dashboard's boot animation ending, and Prince
of Persia's intro FMV plays through the PVIDEO overlay (live overlay regs,
steady 2-3MB/s DVD streaming), which neither pgraph flips nor the VGA
scanout show. Overlay composition arrives with the GPU bridge (M4).

## M4 status: the GPU bridge - a real driver draws, and the machine agrees

The whole of xemu's NV2A GL renderer (pgraph/gl + glsl, GL 4.0 core) now
compiles in every build, over glad instead of SDL + epoxy. The five-entry
gloffscreen abstraction is rebuilt in pgraph/chimera-gl/: in the sandbox
the glad function pointers are filled with generated bridge wrappers
(waterbox/generated-gl, 115 of miniBox's 194-entry master list - 21 names
appended for this core) and every call crosses to the host through the one
callback a guest may make; the native reference brings up its own EGL
surfaceless context, so both render through the same driver and the gate
can compare their machines byte for byte.

That comparison MEANS more here than on PCSX2 or flycast: the Xbox is UMA,
and rendered surfaces are downloaded back into machine RAM (the same
glo_readpixels path upstream uses). GPU output feeds machine state by
design. The gpu gate leg (XBOX_GPU=1) holds anyway on one host and one
driver: native A == native B == sandbox at 600 frames, 40.7MB of state
with the boot animation's rendered frames inside it - and the picture is
the real one, the animated Xbox logo, out of both builds. Across machines
or drivers a GPU-drawn run is not deterministic and must say so (the
frontend's ce_session_deterministic story, M5).

What the port took:
- epoxy is gone: gloffscreen.h re-typed over glad; glo_check_extension via
  glGetStringi; the renderer sources compile unmodified but for two
  variable shadows (blit.c) that upstream's laxer warnings never saw.
- Contexts: upstream's shared render/display context pair is real EGL
  shared contexts natively. EGL forbids binding a context current on
  another thread, so the driver RELEASES whatever the thread holds at
  every frame boundary (vCPU lets go before the stop, main before the
  start) - the BQL serializes all GL, the boundary is where work migrates.
  glo_ensure_current() rebinds the render context in pfifo_drain and the
  display update. In the sandbox all of this is a no-op: one host thread,
  the host owns the context.
- The pfifo-thread waits: every "set pending, kick, wait" in
  gl/{renderer,surface,display}.c gets pfifo_service_pending(d) first - an
  inline run of the renderer's process_pending with the render context
  temporarily bound, after which the event_wait falls straight through.
  Same disease, same cure as M1's synchronous drain.
- nv2a_context_init was the UI's job; the driver calls it (only the CHOSEN
  renderer gets early_context_init - the GL renderer is always compiled
  now, and its context creation must not run in a null-rendered gate).
- The GL shader disk cache is a desktop comfort: a sandbox has no home
  directory. g_config.perf.cache_shaders is forced off in both builds and
  init respects it (upstream created the folder and spawned the reload
  thread unconditionally).
- run-wbx grew the host half: pcsx2's gl-host.c verbatim (core-agnostic by
  design) + the full-list generated dispatcher + glad, handed over as
  SetGpuBridge BEFORE Init, CHIMERA_GPU=1 to ask.

The display pipeline is the export now: under GL, GetVideoBgra calls
nv2a_chimera_read_display (display.c) - the same sync the GUI's scanout
runs composes the surface AND the PVIDEO overlay into gl_display_buffer in
the display context, and the result reads back BGRA. Prince of Persia's
intro FMV, invisible to every earlier export, shows. The native binary
dumps the same picture via CHIMERA_VIDEO_OUT. When no surface scans out
(or no GPU), the VGA view of RAM stays the truth.

Real game rendering flushed out one more determinism bug: pfifo_kick armed
its drain timer at "now", and with GL the RAM-access surface hooks kick
from the MIDDLE of a translation block, where reading the virtual clock is
illegal under icount ("Bad icount read", a crash ~40s into PoP). The
deadline is 0 now - always expired, no clock read, fires at the same
deterministic processing points.

Savestates under an active GL renderer round-trip exactly (the gpu
rerecord check: arena saved and reloaded around every frame, end state
byte-identical) - the host context outlives the load, so the arena's GL
object ids stay matched. A CROSS-SESSION load (movie playback from a cold
start) would find those objects gone; that is M5's problem, along with
Windows and real hardware for the bridge generally.

## M5 status: a real core package, loaded by the real frontend

`waterbox/build-package.sh` builds `xemu.chimeraCore` into a chimera
checkout's `build/Cores/` - core.wbx (check-wbx clean: no TLS, no %fs),
waterbox.config, the Duke keybinds, the wizard's file slots, and the
licences resolved by miniBox's packager (GPL-2.0-or-later overall). The
frontend discovers packages from that directory; there is no registration
step beyond the package itself. Chimera-side: XBOX joined SystemNames, and
extern/cores/xemu is a submodule.

The declarations worth knowing:
- vsync is EXACTLY the driver's quantum: 1000000000/16666667 - both places
  derive from VBLANK_NS.
- The button list is in packed-bit order (xemu's CONTROLLER_BUTTON enum)
  and the axes in SetAxis index order; reordering either breaks the wire.
- Firmware ids are the driver's mount names: mcpx, bios, hdd - and eeprom,
  which is OPTIONAL: without one the machine uses a frozen built-in
  identity (waterbox/default-eeprom.c, minted once by xemu's own generator
  and embedded), proven byte-identical to mounting the same bytes. One
  identity for everyone is what movies want.
- An Xbox project's disc slot is min 0: an empty tray boots the dashboard
  from the hard disk, and the driver ignores the zero-byte "dvd" mount an
  empty slot produces.
- One memory domain, "System RAM" - the real xbox.ram block (pc.ram is a
  decoy), 64MB, writable, for Lua and RAM watch.

`waterbox/tests/run-frontend.sh` is the frontend half of the gate: boot
the package inside Chimera (Mono, headless, Xvfb), null renderer, and
require the frontend machine's System RAM slice to be byte-identical to
run-wbx's (which the core gate holds equal to the native build); then the
same with the GPU on both sides; then the package's keybinds adopted as
the frontend's defaults.

Still open beyond Windows: cross-session savestate load under an active
GL renderer (the arena's GL object ids dangle in a fresh host context -
same-session loads are proven exact; a movie resumed from a cold start
with a GPU needs a renderer rebuild on load), and persistent save export
(the HDD overlay lives in guest memory, so saves persist through
savestates and movies but are not yet exported as files between
sessions).

## The native determinism story (still true, prerequisite)

`waterbox/run-determinism-native.sh N` boots the real firmware (MCPX 1.0 +
Complex 4627 + xbox_hdd.qcow2, user-supplied, never in this repo) headless
for N frames twice from pristine copies and compares the full migration
stream byte for byte. PASS at 60 and 300 frames (5 virtual seconds,
~8MB of state).

What it took - each of these was found by an actual diverging byte, in
order, and lives in patches/ + waterbox/:

1. Frame boundary: one 16.667ms QEMU_CLOCK_VIRTUAL slice per frame under
   `-icount shift=0,sleep=off -rtc base=2000-01-01,clock=vm`. icount sleep
   MUST be off: sleep=on warps by measured host time. shift=0 (1ns per
   instruction, a 733MHz-class machine) is not a luxury: the 2BL's SHA-1
   over the kernel and the byte-wise decompression legitimately burn
   billions of instructions, and at shift=5 the boot takes half an hour of
   virtual frames.
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
6. Disk I/O completed at host time; block/chimera-latency.c (copied in,
   wired by patch 0003) makes every request complete at the NEXT FRAME
   BOUNDARY - the vm_stop the driver performs each frame. That instant is
   pure virtual time and, crucially, is reached identically by the native
   build (host-preemptive threads) and the sandbox (cooperative green
   threads), so the interrupt's position in the instruction stream cannot
   depend on either host storage or thread scheduling. A fixed mid-frame
   virtual deadline was tried first and worked natively, but in the sandbox
   the vCPU green thread never syscalls mid-slice, the thread-pool BHs
   starve to the boundary anyway, and the two sides diverge. Requests
   issued while the vm is stopped (realize-time geometry probe, savevm, the
   boundary itself) complete immediately. The 0..16.7ms quantized latency
   is also about what a real drive does.
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
