/* Standalone driver for the waterboxed xemu core: loads core.wbx under the
 * miniBox host, mounts the firmware under the frontend's fixed names, runs N
 * frames and writes the machine-state stream - the same bytes the native
 * reference writes via CHIMERA_STATE_OUT, so the two runs diff directly.
 *
 * usage: run-wbx <core.wbx> --mcpx F --bios F --eeprom F --hdd F
 *                [--dvd F] [--frames N] [--state-out F]
 */
#include "minibox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s)
{
    return (intptr_t)fread(d, 1, s, ((freader *)ud)->f);
}

typedef int (*intfn)(void);
typedef int64_t (*i64fn)(void);
typedef void (*framefn)(uint64_t);
typedef const char *(*strfn)(void);
typedef uint8_t *(*bytesfn)(void);

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
    long frames = 60;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mcpx") && i + 1 < argc) mcpx = argv[++i];
        else if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) eeprom = argv[++i];
        else if (!strcmp(argv[i], "--hdd") && i + 1 < argc) hdd = argv[++i];
        else if (!strcmp(argv[i], "--dvd") && i + 1 < argc) dvd = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--state-out") && i + 1 < argc) stateOut = argv[++i];
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

    for (long i = 0; i < frames; i++) {
        FrameAdvance(0);
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

    wbx_deactivate_host(h, &r);
    return 0;
}
