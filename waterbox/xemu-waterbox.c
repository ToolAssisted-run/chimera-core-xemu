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
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "io/channel-file.h"
#include "qapi/error.h"
#include "qemu-main.h"

#include "ui/xemu-input.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-snapshots.h"
#include "ui/xemu-widescreen.h"
#include "ui/xemu-net.h"

#include <locale.h>

int (*qemu_main)(void);

/* ---- input: four ports the harness pokes, no controllers bound yet ------ */

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);

int xemu_input_get_test_mode(void)
{
    return 0;
}

ControllerState *xemu_input_get_bound(int index)
{
    (void)index;
    return NULL;
}

void xemu_input_update_controller(ControllerState *state)
{
    (void)state;
}

void xemu_input_update_rumble(ControllerState *state)
{
    (void)state;
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

/* ---- the frame loop ------------------------------------------------------ */

#define VBLANK_NS 16666667 /* NTSC field; PAL arrives with the vsync work */

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
    governor_next += GOVERNOR_NS;
    timer_mod_ns(governor, governor_next);
}

static bool frame_done;

static void frame_boundary(void *opaque)
{
    (void)opaque;
    frame_done = true;
    /* Freeze the machine exactly here. The rr thread computes the vCPU's
     * next instruction budget under the BQL, which this callback holds - so
     * pausing before returning means not one instruction runs past the
     * boundary. Without this, the vCPU races toward the next deadline while
     * the driver reacts, and the stop instant becomes host timing.
     */
    vm_stop(RUN_STATE_PAUSED);
}

static void run_frames(long frames)
{
    governor = timer_new_ns(QEMU_CLOCK_VIRTUAL, governor_tick, NULL);
    governor_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    governor_tick(NULL);

    QEMUTimer *t = timer_new_ns(QEMU_CLOCK_VIRTUAL, frame_boundary, NULL);
    int64_t next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool trace = getenv("CHIMERA_TRACE_FRAMES") != NULL;
    for (long i = 0; i < frames; i++) {
        frame_done = false;
        next += VBLANK_NS;
        timer_mod_ns(t, next);
        if (!runstate_is_running()) {
            vm_start();
        }
        while (!frame_done) {
            main_loop_wait(false);
        }
        if (trace) {
            fprintf(stderr, "frame %ld: late %" PRId64 " ns\n", i,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - next);
        }
    }
    timer_free(t);
}

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

    qemu_init(argc, argv);

    /* qemu_init returns holding the BQL and the replay lock; the main loop
     * wants to take them itself (system/main.c does this same dance) */
    bql_unlock();
    replay_mutex_unlock();
    replay_mutex_lock();
    bql_lock();

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
