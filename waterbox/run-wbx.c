/* Standalone driver for the waterboxed xemu core: loads core.wbx under the
 * miniBox host, mounts the firmware under the frontend's fixed names, runs N
 * frames and writes the machine-state stream - the same bytes the native
 * reference writes via CHIMERA_STATE_OUT, so the two runs diff directly.
 *
 * usage: run-wbx <core.wbx> --mcpx F --bios F --eeprom F --hdd F
 *                [--dvd F] [--frames N] [--state-out F]
 *                [--press port:mask:from:to]
 *
 * --press holds a button mask on one pad for a frame range, mirroring the
 * native binary's CHIMERA_PRESS env - the two halves of the input gate leg.
 */
#include "minibox.h"

#ifdef CHIMERA_GL_BRIDGE
/* the host half of the GPU bridge (gl-host.c): a real context, and the
 * dispatcher the guest's wrappers call into */
int chimera_gl_host_init(char *err, int errlen);
const char *chimera_gl_host_description(void);
uintptr_t chimera_gl_host_dispatch(uintptr_t op, uintptr_t a, uintptr_t b,
                                   uintptr_t c, uintptr_t d, uintptr_t e);
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s)
{
    return (intptr_t)fread(d, 1, s, ((freader *)ud)->f);
}

/* --rerecord: the miniBox arena snapshot, saved and reloaded around every
 * frame. A loaded state must continue exactly like the run it came from. */
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
    membuf *m = (membuf *)ud;
    if (m->len + n > m->cap) {
        m->cap = (m->len + n) * 2 + 64;
        m->b = realloc(m->b, m->cap);
    }
    memcpy(m->b + m->len, d, n);
    m->len += n;
    return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
    membuf *m = (membuf *)ud;
    uintptr_t avail = m->len - m->pos;
    if (n > avail) n = avail;
    memcpy(d, m->b + m->pos, n);
    m->pos += n;
    return (intptr_t)n;
}

typedef int (*intfn)(void);
typedef int64_t (*i64fn)(void);
typedef void (*framefn)(uint64_t);
typedef const char *(*strfn)(void);
typedef uint8_t *(*bytesfn)(void);
typedef uint32_t *(*pixfn)(void);

static uintptr_t proc(mb_host *h, const char *n)
{
    mb_return r;
    wbx_get_proc_addr(h, n, &r);
    if (r.error_message[0]) {
        fprintf(stderr, "get_proc %s: %s\n", n, r.error_message);
        exit(1);
    }
    return (uintptr_t)r.data;
}

static void mount_path(mb_host *h, const char *name, const char *path)
{
    mb_return r;
    wbx_mount_file_path(h, name, path, &r);
    if (r.error_message[0]) {
        fprintf(stderr, "mount %s (%s): %s\n", name, path, r.error_message);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *wbx = 0, *mcpx = 0, *bios = 0, *eeprom = 0, *hdd = 0, *dvd = 0;
    const char *stateOut = 0;
    const char *videoOut = 0;
    const char *ramOut = 0;
    long ramBytes = 1048576;
    const char *audioOut = 0;
    long frames = 60;
    int press_port = -1, press_from = 0, press_to = 0;
    unsigned press_mask = 0;
    int rerecord = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mcpx") && i + 1 < argc) mcpx = argv[++i];
        else if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) eeprom = argv[++i];
        else if (!strcmp(argv[i], "--hdd") && i + 1 < argc) hdd = argv[++i];
        else if (!strcmp(argv[i], "--dvd") && i + 1 < argc) dvd = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--state-out") && i + 1 < argc) stateOut = argv[++i];
        else if (!strcmp(argv[i], "--video-out") && i + 1 < argc) videoOut = argv[++i];
        else if (!strcmp(argv[i], "--ram-out") && i + 1 < argc) ramOut = argv[++i];
        else if (!strcmp(argv[i], "--ram-bytes") && i + 1 < argc) ramBytes = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--audio-out") && i + 1 < argc) audioOut = argv[++i];
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d:%x:%d:%d", &press_port, &press_mask,
                       &press_from, &press_to) != 4) {
                fprintf(stderr, "bad --press %s\n", argv[i]); return 2;
            }
        }
        else if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
        else if (!wbx) wbx = argv[i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (!wbx || !mcpx || !bios || !eeprom || !hdd) {
        fprintf(stderr, "usage: run-wbx <core.wbx> --mcpx F --bios F "
                "--eeprom F --hdd F [--dvd F] [--frames N] [--state-out F]\n");
        return 2;
    }

    FILE *wf = fopen(wbx, "rb");
    if (!wf) { fprintf(stderr, "cannot open %s\n", wbx); return 1; }

    /* the ELF is ~700MB of span with QEMU's appetite on top: a fat sbrk for
     * glib, a big mmap arena for the TCG buffer, guest RAM and the COW
     * overlay */
    mb_memory_layout_template layout = {
        512u << 20,  /* sbrk */
        16u << 20,   /* sealed */
        128u << 20,  /* invis */
        256u << 20,  /* plain */
        2048u << 20, /* mmap */
    };
    freader fr = { wf };
    mb_return r;
    wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
    fclose(wf);
    if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
    mb_host *h = (mb_host *)r.data;

    mount_path(h, "mcpx", mcpx);
    mount_path(h, "bios", bios);
    mount_path(h, "eeprom", eeprom);
    mount_path(h, "hdd", hdd);
    if (dvd) {
        mount_path(h, "dvd", dvd);
    }

    wbx_activate_host(h, &r);

    /* The GPU, when asked for. Handed over BEFORE Init - Init is where the
     * renderer is chosen. A machine with no usable driver just draws with
     * the null renderer, deterministically. */
#ifdef CHIMERA_GL_BRIDGE
    {
        const char *want = getenv("CHIMERA_GPU");
        if (want && strcmp(want, "0") != 0) {
            char glerr[256] = "";
            if (chimera_gl_host_init(glerr, sizeof glerr) != 0) {
                fprintf(stderr, "gpu bridge: no context (%s)\n", glerr);
            } else {
                typedef void (*setfn_u64)(uint64_t);
                setfn_u64 set_bridge = (setfn_u64)proc(h, "SetGpuBridge");
                wbx_get_callback_addr(h, (mb_external_callback)chimera_gl_host_dispatch, 0, &r);
                if (!r.data || !set_bridge) {
                    fprintf(stderr, "gpu bridge: could not register the callback\n");
                } else {
                    fprintf(stderr, "gpu bridge: %s\n", chimera_gl_host_description());
                    set_bridge((uint64_t)r.data);
                }
            }
        }
    }
#endif

    intfn Init = (intfn)proc(h, "Init");
    if (Init() != 1) {
        strfn GetLoadError = (strfn)proc(h, "GetLoadError");
        fprintf(stderr, "Init failed: %s\n", GetLoadError());
        return 1;
    }

    framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
    i64fn GetStateSize = (i64fn)proc(h, "GetStateSize");
    bytesfn GetStateData = (bytesfn)proc(h, "GetStateData");

    wbx_deactivate_host(h, &r);
    wbx_seal(h, &r);
    if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
    wbx_activate_host(h, &r);

    bytesfn GetAudio = (bytesfn)proc(h, "GetAudio");
    intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");
    FILE *af = NULL;
    if (audioOut) {
        af = fopen(audioOut, "wb");
        if (!af) { fprintf(stderr, "cannot write %s\n", audioOut); return 1; }
    }

    membuf st = { 0 };
    for (long i = 0; i < frames; i++) {
        if (rerecord) {
            st.len = 0;
            wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
            if (r.error_message[0]) { fprintf(stderr, "save_state: %s\n", r.error_message); return 1; }
            st.pos = 0;
            wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
            if (r.error_message[0]) { fprintf(stderr, "load_state: %s\n", r.error_message); return 1; }
        }
        uint64_t packed = 0;
        if (press_port >= 0 && i >= press_from && i < press_to) {
            packed = (uint64_t)press_mask << (press_port * 14);
        }
        FrameAdvance(packed);
        if (af) {
            fwrite(GetAudio(), 4, (size_t)GetAudioSampleCount(), af);
        }
    }
    if (af) {
        fclose(af);
        fprintf(stderr, "run-wbx: audio written to %s\n", audioOut);
    }
    fprintf(stderr, "run-wbx: ran %ld frames\n", frames);

    if (stateOut) {
        int64_t size = GetStateSize();
        if (size < 0) { fprintf(stderr, "state dump failed\n"); return 1; }
        uint8_t *data = GetStateData();
        FILE *f = fopen(stateOut, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", stateOut); return 1; }
        fwrite(data, 1, (size_t)size, f);
        fclose(f);
        fprintf(stderr, "run-wbx: state written to %s (%lld bytes)\n",
                stateOut, (long long)size);
    }

    if (videoOut) {
        pixfn GetVideoBgra = (pixfn)proc(h, "GetVideoBgra");
        intfn GetVideoWidth = (intfn)proc(h, "GetVideoWidth");
        intfn GetVideoHeight = (intfn)proc(h, "GetVideoHeight");
        uint32_t *pix = GetVideoBgra();
        int w = GetVideoWidth(), ht = GetVideoHeight();
        FILE *f = fopen(videoOut, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", videoOut); return 1; }
        fwrite(pix, 4, (size_t)w * ht, f);
        fclose(f);
        fprintf(stderr, "run-wbx: video %dx%d written to %s\n", w, ht, videoOut);
    }

    if (ramOut) {
        typedef uint8_t *(*ptrfn_i)(int);
        typedef int64_t (*i64fn_i)(int);
        ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
        i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");
        uint8_t *ram = GetMemoryDomainPtr(0);
        int64_t size = GetMemoryDomainSize(0);
        if (ram && size > 0) {
            if (ramBytes > size) ramBytes = size;
            FILE *f = fopen(ramOut, "wb");
            if (!f) { fprintf(stderr, "cannot write %s\n", ramOut); return 1; }
            fwrite(ram, 1, (size_t)ramBytes, f);
            fclose(f);
            fprintf(stderr, "run-wbx: %ld bytes of System RAM (of %lld) written to %s\n",
                    ramBytes, (long long)size, ramOut);
        }
    }

    wbx_deactivate_host(h, &r);
    return 0;
}
