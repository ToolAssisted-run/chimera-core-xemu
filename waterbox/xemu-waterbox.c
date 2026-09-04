/* xemu-waterbox.c - the headless xemu driver.
 *
 * Replaces the whole SDL/imgui UI layer (ui/xemu.c and friends) in builds
 * where SDL is absent: supplies main(), the input state xid.c reads, and the
 * few UI hooks core code calls. Copied into extern/xemu/ui/ by
 * waterbox/apply-patches.sh; ui/meson.build compiles it when !sdl.found().
 *
 * Frame model (M1): one frame = one vblank interval of VIRTUAL time. A
 * one-shot QEMU_CLOCK_VIRTUAL timer marks the frame boundary and the loop
 * pumps main_loop_wait until it fires. Under icount the virtual clock is
 * driven by executed instructions, so a frame is a deterministic quantum of
 * emulation, not of host time.
 *
 * CHIMERA_FRAMES=<n> runs n frames and exits 0 (the gate's native leg);
 * unset, it runs the plain qemu main loop forever.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/replay.h"
#include "system/cpus.h"
#include "ui/console.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "io/channel-file.h"
#include "qapi/error.h"
#include "qemu-main.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/config-file.h"

#include "ui/xemu-input.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-snapshots.h"
#include "ui/xemu-widescreen.h"
#include "ui/xemu-net.h"
#include "hw/xbox/nv2a/nv2a.h"

#include <locale.h>
#include <sched.h>

int (*qemu_main)(void);

/* ---- the GPU bridge (chimera-gl) ----------------------------------------
 * Whether a real GPU draws is decided before the machine boots: the sandbox
 * is handed the host's callback (SetGpuBridge, before Init), the native
 * reference brings up its own EGL context when CHIMERA_GPU asks. Either way
 * the choice only flips g_config's renderer; the null renderer remains the
 * deterministic default. */
bool chimera_gl_available(void);
const char *chimera_gl_describe(void);
/* the true presented frame under the GL renderer (pgraph/gl/display.c) */
int nv2a_chimera_read_display(uint8_t *out, int cap_w, int cap_h,
                              int *out_w, int *out_h);
#ifdef CHIMERA_GUEST
bool chimera_gl_try_bridge(uint64_t addr);
#endif

static void chimera_choose_renderer(bool want_gpu)
{
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_NULL;
    if (want_gpu && chimera_gl_available()) {
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
    }
    /* the shader disk cache is a desktop comfort; neither the sandbox nor
     * the reference build wants host-disk state */
    g_config.perf.cache_shaders = false;
}

#ifndef __GLIBC__
/* The waterbox musl has no signals, and no sigsetjmp. Every QEMU call site
 * passes savemask=0, which makes sigsetjmp exactly setjmp - and the alias
 * must be a tail jump, not a C wrapper, or the jmp_buf would capture a
 * frame that is gone by the time anyone longjmps to it.
 */
__asm__(".globl sigsetjmp\n"
        ".type sigsetjmp,@function\n"
        "sigsetjmp:\n"
        "\tjmp setjmp\n");
#endif

/* ---- input: four Duke pads, always plugged, driven by the harness -------
 *
 * xid.c polls xemu_input_get_bound() on every USB interrupt transfer and
 * reads buttons/axis straight out of ControllerState. The four pads live
 * here as plain state the driver writes before each frame; the USB devices
 * themselves are created once after the machine is up, the same way the
 * GUI's xemu_input_bind does it (an internal usb-hub per port, the xid
 * gamepad on its port 1).
 */

#define CHIMERA_PORTS 4
#define CHIMERA_BUTTONS 14 /* CONTROLLER_BUTTON_A..RSTICK, the Duke set */

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);

static ControllerState chimera_pads[CHIMERA_PORTS];
static bool chimera_pads_attached;
/* which ports have a Duke plugged in (settings port1..port4); an empty port
 * has no hub and no pad, exactly an unplugged controller */
static bool chimera_port_present[CHIMERA_PORTS] = { true, false, false, false };

int xemu_input_get_test_mode(void)
{
    return 0;
}

ControllerState *xemu_input_get_bound(int index)
{
    if (!chimera_pads_attached || index < 0 || index >= CHIMERA_PORTS ||
        !chimera_port_present[index]) {
        return NULL;
    }
    return &chimera_pads[index];
}

void xemu_input_update_controller(ControllerState *state)
{
    (void)state; /* nothing to poll: the driver already wrote the state */
}

void xemu_input_update_rumble(ControllerState *state)
{
    (void)state; /* rumble lands in chimera_pads[].rumble_l/r; no motor */
}

static void chimera_attach_gamepads(void)
{
    /* the machine's four controller ports sit on these usb ports */
    static const int port_map[CHIMERA_PORTS] = { 3, 4, 1, 2 };

    /* visible to xid.c before the devices realize: realize paths read the
     * bound state through xemu_input_get_bound() */
    for (int i = 0; i < CHIMERA_PORTS; i++) {
        chimera_pads[i].bound = i;
    }
    chimera_pads_attached = true;

    for (int i = 0; i < CHIMERA_PORTS; i++) {
        char *tmp;

        if (!chimera_port_present[i]) {
            continue; /* an empty port: no hub, no pad, nothing to enumerate */
        }

        QDict *hub_qdict = qdict_new();
        qdict_put_str(hub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", port_map[i]);
        qdict_put_str(hub_qdict, "port", tmp);
        g_free(tmp);
        qdict_put_int(hub_qdict, "ports", 3);
        QemuOpts *hub_opts = qemu_opts_from_qdict(
            qemu_find_opts("device"), hub_qdict, &error_abort);
        DeviceState *hub_dev = qdev_device_add(hub_opts, &error_abort);

        QDict *pad_qdict = qdict_new();
        qdict_put_str(pad_qdict, "driver", DRIVER_DUKE);
        tmp = g_strdup_printf("gamepad_%d", i);
        qdict_put_str(pad_qdict, "id", tmp);
        g_free(tmp);
        qdict_put_int(pad_qdict, "index", i);
        tmp = g_strdup_printf("1.%d.1", port_map[i]);
        qdict_put_str(pad_qdict, "port", tmp);
        g_free(tmp);
        QemuOpts *pad_opts = qemu_opts_from_qdict(
            qemu_find_opts("device"), pad_qdict, &error_abort);
        DeviceState *pad_dev = qdev_device_add(pad_opts, &error_abort);

        qobject_unref(hub_qdict);
        qobject_unref(pad_qdict);
        object_unref(OBJECT(hub_dev));
        object_unref(OBJECT(pad_dev));
        chimera_pads[i].device = hub_dev;
    }
}

/* ---- UI hooks ------------------------------------------------------------ */

void xemu_queue_notification(const char *msg)
{
    fprintf(stderr, "xemu: %s\n", msg);
}

void xemu_queue_error_message(const char *msg)
{
    fprintf(stderr, "xemu error: %s\n", msg);
}

static bool widescreen;

void xemu_set_widescreen(bool enable)
{
    widescreen = enable;
}

bool xemu_get_widescreen(void)
{
    return widescreen;
}

void xemu_net_enable(void)
{
}

void xemu_net_disable(void)
{
}

int xemu_net_is_enabled(void)
{
    return 0;
}

/* ---- snapshot extra data -------------------------------------------------
 * The GUI writes a magic-tagged blob (disc path, title, thumbnail) ahead of
 * the machine state; the format tolerates absence, so headless writes
 * nothing and skips the blob when a GUI-written one is present.
 */

void xemu_snapshots_save_extra_data(QEMUFile *f)
{
    (void)f;
}

bool xemu_snapshots_offset_extra_data(QEMUFile *f)
{
    unsigned int v = qemu_get_be32(f);
    if (v != XEMU_SNAPSHOT_DATA_MAGIC) {
        qemu_file_skip(f, -4);
        return true;
    }
    qemu_get_be32(f); /* version */
    uint32_t size = qemu_get_be32(f);
    void *buf = g_malloc(size);
    qemu_get_buffer(f, buf, size);
    g_free(buf);
    return true;
}

void xemu_snapshots_mark_dirty(void)
{
}

/* ---- audio: the APU monitor accumulator (waterbox/monitor-null.c) ------- */

extern int16_t chimera_audio_buf[];
extern int chimera_audio_count;

/* ---- the frame loop ------------------------------------------------------ */

/* NTSC field rate, 60000/1001 Hz - what a real Xbox's video hardware scans
 * out at. One frame is 1001/60000 s = 16,683,333 1/3 ns, NOT a whole number
 * of nanoseconds, so the boundary walks an exact absolute schedule (every
 * third deadline lands on a whole 50,050,000 ns) instead of accumulating a
 * rounded quantum that would drift off the true rate. PAL arrives with the
 * vsync work. */
#define VBLANK_3FRAMES_NS 50050000LL

/* The warp governor. With icount sleep=off, an idle guest's virtual clock
 * leaps to the next timer deadline instantly - which can overtake a disk
 * request's fixed-latency delivery point while the host I/O is still in
 * flight, making delivery time depend on the host after all. A permanently
 * pending short-period timer bounds every warp step, so virtual time can
 * only creep ahead of the host, never leap.
 *
 * All periodic timers here advance by ABSOLUTE deadline: re-arming off
 * "now" would accumulate callback lateness, which is host timing - the one
 * thing being fenced out.
 */
#define GOVERNOR_NS 10000

static QEMUTimer *governor;
static int64_t governor_next;

static void governor_tick(void *opaque)
{
    (void)opaque;
#ifdef CHIMERA_GUEST
    /* The sandbox's threads are cooperative: the vCPU never syscalls while
     * executing, so the main loop's BH work (disk completions) would starve
     * until the frame boundary. This tick runs at exact virtual instants on
     * the vCPU thread - yielding here rotates every green thread once per
     * governor period, deterministically. */
    sched_yield();
#endif
    governor_next += GOVERNOR_NS;
    timer_mod_ns(governor, governor_next);
}

static bool frame_done;
static bool debug_no_pause; /* CHIMERA_DEBUG_NOPAUSE: boot-bisect aid */

void glo_release_current(void);

static void frame_boundary(void *opaque)
{
    (void)opaque;
    frame_done = true;
    /* GL work migrates across the stop: whatever context this thread holds
     * goes back so the other side may bind it (EGL forbids stealing) */
    glo_release_current();
    if (debug_no_pause) {
        return;
    }
    /* Freeze execution exactly here. Under icount this callback can run on
     * the vCPU thread itself - vm_stop() from there DEFERS the stop and the
     * machine drifts past the boundary at host-dependent instants. So: the
     * vCPU stops itself synchronously at this exact virtual instant, and
     * the main thread completes the runstate change once the frame loop
     * returns (see run_one_frame).
     */
    if (qemu_in_vcpu_thread()) {
        cpu_stop_current();
    } else {
        vm_stop(RUN_STATE_PAUSED);
    }
}

static QEMUTimer *frame_timer;
static int64_t frame_next;
static int64_t frame_base;  /* virtual time the frame schedule starts from */
static int64_t frame_index; /* frames since frame_base */

static void frame_machinery_init(void)
{
    debug_no_pause = getenv("CHIMERA_DEBUG_NOPAUSE") != NULL;
    governor = timer_new_ns(QEMU_CLOCK_VIRTUAL, governor_tick, NULL);
    governor_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (getenv("CHIMERA_DEBUG_NOGOV") != NULL) {
        governor_next = INT64_MAX / 2; /* park it */
    }
    governor_tick(NULL);

    frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, frame_boundary, NULL);
    frame_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    frame_index = 0;
    frame_next = frame_base;
}

/* CHIMERA_FRAME_PROFILE=1 splits the MAIN thread's share of a frame. The vCPU
 * runs in its own thread, so wall clock here measures the frame machinery
 * rather than the emulated processor: graphic_hw_update is nv2a's scanout and
 * the vblank it raises, and the pump is main_loop_wait servicing timers and
 * the APU. Native only - the sandbox freezes the host clock. */
#ifndef CHIMERA_GUEST
static int frame_profile = -1;
static double prof_gfx, prof_pump;
static long prof_frames;
static double prof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

static void run_one_frame(void)
{
    /* One vblank per frame, delivered at the boundary while the machine is
     * still paused: nv2a's gfx_update raises NV_PCRTC_INTR_0_VBLANK, and
     * the guest sees the interrupt the moment the next frame starts. With
     * -display none there is no refresh timer, so this is THE vblank
     * source - which is exactly what a frame-stepped machine wants.
     * (Look the console up explicitly: with no display attached there is
     * no active console for the NULL shorthand to find.) */
#ifndef CHIMERA_GUEST
    if (frame_profile < 0)
        frame_profile = getenv("CHIMERA_FRAME_PROFILE") != NULL;
    const double t0 = frame_profile ? prof_now() : 0;
#endif
    graphic_hw_update(qemu_console_lookup_by_index(0));
#ifndef CHIMERA_GUEST
    if (frame_profile)
        prof_gfx += prof_now() - t0;
#endif

    frame_done = false;
    frame_index++;
    frame_next = frame_base + (frame_index * VBLANK_3FRAMES_NS) / 3;
    timer_mod_ns(frame_timer, frame_next);
    glo_release_current(); /* the vCPU thread takes the render context back */
    if (!runstate_is_running()) {
        vm_start();
    }
#ifndef CHIMERA_GUEST
    const double t1 = frame_profile ? prof_now() : 0;
#endif
    while (!frame_done) {
        main_loop_wait(false);
    }
#ifndef CHIMERA_GUEST
    if (frame_profile) {
        prof_pump += prof_now() - t1;
        if (++prof_frames % 100 == 0)
            fprintf(stderr, "[frame] %ld frames: gfx %.1f ms/f, pump %.1f ms/f\n",
                    prof_frames, prof_gfx * 1000.0 / prof_frames,
                    prof_pump * 1000.0 / prof_frames);
    }
#endif
    if (!debug_no_pause && runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED); /* the vCPU already stopped at the boundary */
    }
}

static void run_frames(long frames)
{
    frame_machinery_init();
    bool trace = getenv("CHIMERA_TRACE_FRAMES") != NULL;

    /* CHIMERA_PRESS=port:mask:from:to holds a button mask on one pad for a
     * frame range - the native half of the input gate leg. The sandbox half
     * is the same pattern fed through FrameAdvance's packed word. */
    int press_port = -1, press_from = 0, press_to = 0;
    unsigned press_mask = 0;
    const char *press = getenv("CHIMERA_PRESS");
    if (press) {
        if (sscanf(press, "%d:%x:%d:%d", &press_port, &press_mask,
                   &press_from, &press_to) != 4) {
            press_port = -1;
        }
    }

    /* CHIMERA_AUDIO_OUT collects every frame's monitor samples into one raw
     * s16le stereo file - the native half of the audio gate leg */
    FILE *audio_out = NULL;
    const char *audio_path = getenv("CHIMERA_AUDIO_OUT");
    if (audio_path) {
        audio_out = fopen(audio_path, "wb");
    }

    for (long i = 0; i < frames; i++) {
        if (press_port >= 0 && press_port < CHIMERA_PORTS) {
            chimera_pads[press_port].buttons =
                (i >= press_from && i < press_to) ? press_mask : 0;
        }
        chimera_audio_count = 0;
        run_one_frame();
        if (audio_out) {
            fwrite(chimera_audio_buf, 4, chimera_audio_count, audio_out);
        }
        if (trace) {
            fprintf(stderr, "frame %ld: late %" PRId64 " ns\n", i,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - frame_next);
        }
    }
    if (audio_out) {
        fclose(audio_out);
    }

    /* CHIMERA_VIDEO_OUT: the last frame's presented picture, raw BGRA with
     * a 12-byte header (w, h, 0) - the native half of a picture compare */
    const char *video_path = getenv("CHIMERA_VIDEO_OUT");
    if (video_path && g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL) {
        static uint8_t pix[1920 * 1080 * 4];
        int w = 0, h = 0;
        if (nv2a_chimera_read_display(pix, 1920, 1080, &w, &h)) {
            FILE *f = fopen(video_path, "wb");
            if (f) {
                uint32_t hdr[3] = { (uint32_t)w, (uint32_t)h, 0 };
                fwrite(hdr, 4, 3, f);
                fwrite(pix, 4, (size_t)w * h, f);
                fclose(f);
            }
        }
    }
}

/* after qemu_init: it returns holding the BQL and the replay lock; the main
 * loop wants to take them itself (system/main.c does this same dance) */
static void release_and_retake_locks(void)
{
    bql_unlock();
    replay_mutex_unlock();
    replay_mutex_lock();
    bql_lock();
}

#ifdef CHIMERA_GUEST
/* ==== the miniBox core ====================================================
 *
 * The host mounts the firmware as files named "mcpx", "bios", "eeprom" and
 * "hdd", then calls Init() once and FrameAdvance() per frame. There is no
 * config file: g_config is populated here, from wbx settings where one
 * exists. GetStateSize/GetStateData expose the migration stream for the
 * native==sandbox gate.
 */
#include "emulibc.h"
#include "waterbox_settings.h"
#include "qobject/qdict.h"
#include "io/channel-buffer.h"

static char g_loadError[256];
static void chimera_find_ram(void);

int main(void)
{
    return 0; /* work happens in the exports */
}

ECL_EXPORT void SetGpuBridge(uint64_t addr)
{
    if (chimera_gl_try_bridge(addr)) {
        fprintf(stderr, "chimera gl: %s\n", chimera_gl_describe());
    } else {
        fprintf(stderr, "chimera: the GPU bridge was offered and refused\n");
    }
}

ECL_EXPORT const char *GetLoadError(void)
{
    return g_loadError;
}

ECL_EXPORT int Init(void)
{
    setlocale(LC_NUMERIC, "C");

    xemu_settings_set_path("/xemu.toml"); /* absent: defaults */
    if (!xemu_settings_load()) {
        snprintf(g_loadError, sizeof g_loadError, "settings: %s",
                 xemu_settings_get_error_message());
        return 0;
    }

    g_config.general.show_welcome = false;
    chimera_choose_renderer(chimera_gl_available());
    g_config.audio.use_dsp_jit = false;
    g_config.sys.mem_limit = (int)wbx_setting_double("memLimit128", 0)
        ? CONFIG_SYS_MEM_LIMIT_128 : CONFIG_SYS_MEM_LIMIT_64;
    for (int i = 0; i < CHIMERA_PORTS; i++) {
        char key[8], val[16];
        snprintf(key, sizeof key, "port%d", i + 1);
        snprintf(val, sizeof val, "%s", i == 0 ? "duke" : "none");
        wbx_setting_str(key, val, sizeof val);
        chimera_port_present[i] = strcmp(val, "duke") == 0;
    }
    xemu_settings_set_string(&g_config.sys.files.bootrom_path, "mcpx");
    xemu_settings_set_string(&g_config.sys.files.flashrom_path, "bios");
    xemu_settings_set_string(&g_config.sys.files.eeprom_path, "eeprom");
    xemu_settings_set_string(&g_config.sys.files.hdd_path, "hdd");
    FILE *dvdf = fopen("dvd", "rb");
    if (dvdf != NULL) {
        /* a project with an open tray mounts a zero-byte disc; the drive
         * wants either a real image or nothing at all */
        fseek(dvdf, 0, SEEK_END);
        long dvd_size = ftell(dvdf);
        fclose(dvdf);
        if (dvd_size > 0) {
            xemu_settings_set_string(&g_config.sys.files.dvd_path, "dvd");
        }
    }

    /* How fast the Xbox's processor is, as the `cpuSpeed` setting.
     *
     * icount is what makes a frame a fixed quantum of EMULATION rather than of
     * host time: the sandbox freezes the host clock, so the virtual clock has
     * no other source and the frame timer would never fire without it. Its
     * shift says how many nanoseconds of virtual time one instruction costs,
     * as a power of two - so it IS the modelled instruction rate, and it
     * decides how much work a frame contains.
     *
     * The Xbox's CPU is a 733 MHz Pentium III. shift=0 spends 1 ns per
     * instruction, i.e. 1000 MIPS, which that chip would need an IPC above
     * 1.36 to reach and does not; at a realistic IPC it retires something like
     * 440-590 million. So 500 is the closer model as well as the cheaper one,
     * and is the default. Measured cost of a steady-state frame on Prince of
     * Persia (frames 60-120, boot excluded): 1000 -> 88.7 ms, 500 -> 52.2 ms,
     * 250 -> 25.5 ms, 125 -> 18.7 ms, against a 16.7 ms budget.
     *
     * Neither value is exactly a 733 MHz Pentium III: the shift is a power of
     * two, so 1000 and 500 are the choices and the question is only which is
     * less wrong. CHIMERA_ICOUNT_SHIFT overrides it for measurement. */
    static const char *const cpu_speeds[] = { "500", "1000", "250", "125" };
    static const char *const cpu_shifts[] = { "1",   "0",    "2",   "3"   };
    char speed[16];
    strncpy(speed, cpu_speeds[0], sizeof(speed) - 1);
    speed[sizeof(speed) - 1] = '\0';
    wbx_setting_str("cpuSpeed", speed, sizeof(speed));
    const char *shift = cpu_shifts[0];
    for (size_t i = 0; i < sizeof(cpu_speeds) / sizeof(cpu_speeds[0]); i++)
        if (strcmp(speed, cpu_speeds[i]) == 0)
            shift = cpu_shifts[i];
    const char *override = getenv("CHIMERA_ICOUNT_SHIFT");
    if (override != NULL && *override != '\0')
        shift = override;

    static char icount_opt[64];
    snprintf(icount_opt, sizeof(icount_opt), "shift=%s,sleep=off", shift);

    char *argv[] = {
        (char *)"core",
        (char *)"-icount", icount_opt,
        (char *)"-rtc", (char *)"base=2000-01-01,clock=vm",
        NULL
    };
    nv2a_context_init(); /* the chosen renderer creates its contexts */
    qemu_init(5, argv);
    release_and_retake_locks();
    chimera_attach_gamepads();
    chimera_find_ram();
    frame_machinery_init();
    return 1;
}

/* Input rides two channels and a frame takes their union: the packed word
 * covers all four Duke pads (4 x 14 = 56 bits, port-major in enum bit
 * order), SetButton/SetAxis cover anything else - the analog sticks and
 * triggers have no place in a packed word at all. */
static uint16_t g_setButtons[CHIMERA_PORTS];

/* Which of the declared controls this machine has: a control belongs to a
 * port, and a port without a Duke has none of them. Asked once at boot; the
 * answer holds for the machine's whole life. */
ECL_EXPORT int IsButtonActive(int index)
{
    int port = index / CHIMERA_BUTTONS;
    return index >= 0 && port < CHIMERA_PORTS && chimera_port_present[port];
}

ECL_EXPORT int IsAxisActive(int index)
{
    int port = index / 6; /* six axes per Duke, declaration order */
    return index >= 0 && port < CHIMERA_PORTS && chimera_port_present[port];
}

ECL_EXPORT void SetButton(int index, int value)
{
    int port = index / CHIMERA_BUTTONS, bit = index % CHIMERA_BUTTONS;
    if (index < 0 || port >= CHIMERA_PORTS) {
        return;
    }
    if (value) {
        g_setButtons[port] |= 1u << bit;
    } else {
        g_setButtons[port] &= ~(1u << bit);
    }
}

ECL_EXPORT void SetAxis(int index, int value)
{
    int port = index / CONTROLLER_AXIS__COUNT;
    int axis = index % CONTROLLER_AXIS__COUNT;
    if (index < 0 || port >= CHIMERA_PORTS) {
        return;
    }
    chimera_pads[port].axis[axis] = (int16_t)value;
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
    for (int p = 0; p < CHIMERA_PORTS; p++) {
        uint16_t bits = (packed >> (p * CHIMERA_BUTTONS)) &
                        ((1u << CHIMERA_BUTTONS) - 1);
        chimera_pads[p].buttons = bits | g_setButtons[p];
    }
    chimera_audio_count = 0;
    run_one_frame();
}

ECL_EXPORT int16_t *GetAudio(void)
{
    return chimera_audio_buf;
}

ECL_EXPORT int GetAudioSampleCount(void)
{
    return chimera_audio_count;
}

/* ---- video: the console surface, which VGA scans out of xbox.ram --------
 *
 * Every frame's graphic_hw_update makes the VGA core render the machine's
 * framebuffer (PCRTC start, CRTC-programmed mode, 15/16/32 bpp) into the
 * console's DisplaySurface - even with no display attached. Under the null
 * renderer the GPU never writes VRAM, so 3D stays black; the mechanism and
 * the mode logic are still the real ones, and CPU-drawn pictures appear.
 * pixman converts whatever format the surface took to BGRA.
 */
#include "ui/surface.h"

#define CHIMERA_MAX_W 1920
#define CHIMERA_MAX_H 1080
static uint32_t g_video[CHIMERA_MAX_W * CHIMERA_MAX_H];
static int g_videoWidth = 640, g_videoHeight = 480;

ECL_EXPORT uint32_t *GetVideoBgra(void)
{
    /* With a GPU drawing, the machine's presented frame is the display
     * pipeline's output - the scanout surface composed with the PVIDEO
     * overlay, exactly what a television saw. Without one (or before any
     * surface scans out), the VGA view of RAM below is the truth. */
    if (g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL) {
        int w = 0, h = 0;
        if (nv2a_chimera_read_display((uint8_t *)g_video, CHIMERA_MAX_W,
                                      CHIMERA_MAX_H, &w, &h)) {
            g_videoWidth = w;
            g_videoHeight = h;
            return g_video;
        }
    }

    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *surf = con ? qemu_console_surface(con) : NULL;
    if (surf == NULL || surface_is_placeholder(surf)) {
        return g_video;
    }
    int w = surface_width(surf), h = surface_height(surf);
    w = (w > CHIMERA_MAX_W) ? CHIMERA_MAX_W : w;
    h = (h > CHIMERA_MAX_H) ? CHIMERA_MAX_H : h;
    pixman_image_t *dst = pixman_image_create_bits_no_clear(
        PIXMAN_x8r8g8b8, w, h, g_video, w * 4);
    pixman_image_composite(PIXMAN_OP_SRC, surf->image, NULL, dst,
                           0, 0, 0, 0, 0, 0, w, h);
    pixman_image_unref(dst);
    g_videoWidth = w;
    g_videoHeight = h;
    return g_video;
}

ECL_EXPORT int GetVideoWidth(void)
{
    return g_videoWidth;
}

ECL_EXPORT int GetVideoHeight(void)
{
    return g_videoHeight;
}

/* ---- memory domains: the machine's RAM, by its real name ----------------
 * The Xbox is UMA: "xbox.ram" is system RAM, video RAM and the GPU's
 * working memory all at once (pc.ram exists but is a decoy). One domain,
 * looked up after the machine exists.
 */
#include "exec/cpu-common.h"

static uint8_t *g_ramPtr;
static int64_t g_ramSize;

static void chimera_find_ram(void)
{
    RAMBlock *rb = qemu_ram_block_by_name("xbox.ram");
    if (rb != NULL) {
        g_ramPtr = qemu_ram_get_host_addr(rb);
        g_ramSize = (int64_t)qemu_ram_get_used_length(rb);
    }
}

ECL_EXPORT int GetMemoryDomainCount(void)
{
    return g_ramPtr != NULL ? 1 : 0;
}

ECL_EXPORT const char *GetMemoryDomainName(int i)
{
    return i == 0 ? "System RAM" : NULL;
}

ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
    return i == 0 ? g_ramPtr : NULL;
}

ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
    return i == 0 ? g_ramSize : 0;
}

ECL_EXPORT int GetMemoryDomainWritable(int i)
{
    return i == 0 ? 1 : 0;
}

/* the gate artifact: the whole-machine migration stream. The buffer channel
 * frees its bytes on close, so they are stashed before the file goes. */
static uint8_t *g_stateData;
static int64_t g_stateSize;

ECL_EXPORT int64_t GetStateSize(void)
{
    Error *err = NULL;
    g_free(g_stateData);
    g_stateData = NULL;
    g_stateSize = -1;

    vm_stop(RUN_STATE_SAVE_VM);
    QIOChannelBuffer *buf = qio_channel_buffer_new(8 << 20);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(buf));
    int ret = qemu_savevm_state(f, &err);
    qemu_fflush(f);
    if (ret == 0) {
        g_stateSize = (int64_t)buf->usage;
        g_stateData = g_memdup2(buf->data, buf->usage);
    } else if (err) {
        error_report_err(err);
    }
    qemu_fclose(f);
    object_unref(OBJECT(buf));
    return g_stateSize;
}

ECL_EXPORT uint8_t *GetStateData(void)
{
    return g_stateData;
}

#else /* !CHIMERA_GUEST: the native reference binary */

int main(int argc, char **argv)
{
    setlocale(LC_NUMERIC, "C");

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i < argc - 1 && argv[i + 1]) {
                xemu_settings_set_path(argv[i + 1]);
                argv[i + 1] = NULL;
            }
            break;
        }
    }

    if (!xemu_settings_load()) {
        fprintf(stderr, "%s", xemu_settings_get_error_message());
        return 1;
    }

    {
        const char *want = getenv("CHIMERA_GPU");
        bool gpu = want != NULL && strcmp(want, "0") != 0;
        chimera_choose_renderer(gpu);
        if (gpu && g_config.display.renderer != CONFIG_DISPLAY_RENDERER_OPENGL) {
            fprintf(stderr, "chimera gl: no context; using the null renderer\n");
        }
    }

    nv2a_context_init(); /* the chosen renderer creates its contexts */
    if (g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL) {
        fprintf(stderr, "chimera gl: %s\n", chimera_gl_describe());
    }

    /* The guest build gets -icount from the cpuSpeed setting (see Init); this
     * one is a plain qemu and takes its command line, so unless a caller says
     * otherwise it gets the SAME default. Two reasons that matters: the gate
     * compares this binary against the sandboxed core and they have to be the
     * same machine, and without any -icount at all the frame timer never fires
     * and this binary hangs forever rather than failing - which is a trap worth
     * closing, because it looks exactly like a slow emulator. */
    int have_icount = 0;
    for (int i = 1; i < argc; i++)
        if (argv[i] != NULL && strcmp(argv[i], "-icount") == 0)
            have_icount = 1;
    char *injected[2];
    int qargc = argc;
    char **qargv = argv;
    static char icount_opt[64];
    if (!have_icount) {
        const char *shift = getenv("CHIMERA_ICOUNT_SHIFT");
        snprintf(icount_opt, sizeof(icount_opt), "shift=%s,sleep=off",
                 (shift != NULL && *shift != '\0') ? shift : "1");
        static char *newargv[64];
        int n = 0;
        for (int i = 0; i < argc && n < 60; i++)
            newargv[n++] = argv[i];
        injected[0] = (char *)"-icount";
        injected[1] = icount_opt;
        newargv[n++] = injected[0];
        newargv[n++] = injected[1];
        newargv[n] = NULL;
        qargc = n;
        qargv = newargv;
    }
    qemu_init(qargc, qargv);
    release_and_retake_locks();
    chimera_attach_gamepads();

    const char *frames_env = getenv("CHIMERA_FRAMES");
    if (frames_env) {
        run_frames(strtol(frames_env, NULL, 10));
        fprintf(stderr, "xemu-waterbox: ran %s frames, virtual clock %" PRId64
                " ns\n", frames_env,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

        /* the gate artifact: the whole-machine migration stream, byte for
         * byte comparable between two runs and between native and sandbox */
        const char *state_out = getenv("CHIMERA_STATE_OUT");
        if (state_out) {
            Error *err = NULL;
            vm_stop(RUN_STATE_SAVE_VM);
            QIOChannel *ioc = QIO_CHANNEL(qio_channel_file_new_path(
                state_out, O_WRONLY | O_CREAT | O_TRUNC, 0644, &err));
            if (!ioc) {
                error_report_err(err);
                exit(1);
            }
            QEMUFile *f = qemu_file_new_output(ioc);
            int ret = qemu_savevm_state(f, &err);
            qemu_fclose(f);
            object_unref(OBJECT(ioc));
            if (ret < 0) {
                error_report_err(err);
                exit(1);
            }
            fprintf(stderr, "xemu-waterbox: state written to %s\n", state_out);
        }
        exit(0);
    }

    int status = qemu_main_loop();
    qemu_cleanup(status);
    bql_unlock();
    replay_mutex_unlock();
    return status;
}

#endif /* !CHIMERA_GUEST */
