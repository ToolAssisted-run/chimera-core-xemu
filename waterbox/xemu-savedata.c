/* xemu-savedata.c - save-data EXPORT and RE-USE for the Xbox core.
 *
 * The Xbox keeps its per-title save games on the emulated hard disk, in the
 * FATX filesystem of the data partition (drive E:), under UDATA/<titleId>/.
 * This file is the whole savedata guest ABI group for xemu: the four exports
 * the frontend's "Export Save Data..." reads, plus the import that seeds the
 * disk from a project's savedata slot before seal.
 *
 * It talks to the LIVE disk through the HDD's QEMU BlockBackend (blk_pread /
 * blk_pwrite), so it sees the base image plus every write the running session
 * has made (the chimera-latency COW keeps those in guest memory, captured by
 * savestates). A frame boundary is the only time the host calls in, and the
 * machine is halted there, so the read is a coherent snapshot; the import runs
 * once at the end of Init, before seal, so what it writes lands in the sealed
 * baseline and a savestate carries only what the game changed since.
 *
 * There is no full FATX read/write library in the tree (ui/thirdparty/fatx
 * only formats a blank image), so a compact FATX16/FATX32 reader+writer lives
 * here, wired to the block layer. CHIMERA_GUEST only: the native reference
 * build (which the determinism gate compares against) is left untouched.
 *
 * Copied into extern/xemu/ui/ by waterbox/apply-patches.sh, compiled by
 * ui/meson.build in the no-SDL (waterbox) branch beside xemu-waterbox.c.
 */

#include "qemu/osdep.h"

#ifdef CHIMERA_GUEST

#include "system/block-backend.h"
#include "system/runstate.h"
#include "emulibc.h"
#include "waterbox_slots.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the Xbox HDD's data partition (drive E:) ---------------------------
 * The retail Xbox kernel carries a fixed partition table; the standard data
 * partition begins at LBA 0x55F400 and is 0x990000 sectors long. The disc
 * xemu ships (and the images its users own) is laid out this way - the FATX
 * superblock sits exactly at this offset and the root directory falls where
 * this size puts it. */
#define SD_SECTOR 512ULL
#define XBOX_E_OFFSET (0x55F400ULL * SD_SECTOR)
#define XBOX_E_SIZE (0x990000ULL * SD_SECTOR)

#define FATX_MAGIC 0x58544146u /* 'FATX' little-endian */
#define FATX_FAT_OFFSET 0x1000ULL
#define FATX_FAT_ALIGN 0x1000ULL
#define FATX_DIRENT 64
#define FATX_MAX_NAME 42
#define FATX_ATTR_DIR 0x10
/* 2000-01-01 00:00:00 in FATX packed form: a fixed, deterministic stamp. */
#define FATX_FIXED_TIME 0x00210000u

/* The subtree that is "save data": per-title user save games. */
#define SD_ROOT "UDATA"

typedef struct {
    BlockBackend *hdd;
    uint32_t bytes_per_cluster;
    uint32_t root_cluster;
    uint32_t num_clusters;
    int fat32; /* 4-byte FAT entries, else 2 */
    uint64_t fat_bytes;
    uint64_t data_offset; /* partition-relative byte offset of cluster 1 */
    uint8_t *fat;         /* cached FAT, fat_bytes long */
    int fat_dirty;
    int ok;
} fatx_fs;

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/* The live HDD BlockBackend. The chimera drive is index 0, media=disk, which
 * qemu names ide0-hd0 (the CD is ide0-cd1). Fall back to the first backend
 * that carries a medium, in case the naming ever changes. */
static BlockBackend *sd_find_hdd(void)
{
    BlockBackend *blk = blk_by_name("ide0-hd0");
    if (blk) {
        return blk;
    }
    for (blk = blk_next(NULL); blk; blk = blk_next(blk)) {
        if (blk_bs(blk) && blk_getlength(blk) >= (int64_t)XBOX_E_SIZE) {
            return blk;
        }
    }
    return NULL;
}

static int sd_pread(fatx_fs *fs, uint64_t part_off, void *buf, uint64_t len)
{
    return blk_pread(fs->hdd, (int64_t)(XBOX_E_OFFSET + part_off),
                     (int64_t)len, buf, 0);
}

static int sd_pwrite(fatx_fs *fs, uint64_t part_off, const void *buf,
                     uint64_t len)
{
    return blk_pwrite(fs->hdd, (int64_t)(XBOX_E_OFFSET + part_off),
                      (int64_t)len, buf, 0);
}

static uint64_t sd_align(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

/* FAT entry accessors over the cached table. */
static uint32_t fat_get(fatx_fs *fs, uint32_t n)
{
    if (n >= fs->num_clusters) {
        return 0;
    }
    if (fs->fat32) {
        return rd_le32(fs->fat + (uint64_t)n * 4);
    }
    const uint8_t *p = fs->fat + (uint64_t)n * 2;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void fat_set(fatx_fs *fs, uint32_t n, uint32_t v)
{
    if (n >= fs->num_clusters) {
        return;
    }
    if (fs->fat32) {
        wr_le32(fs->fat + (uint64_t)n * 4, v);
    } else {
        uint8_t *p = fs->fat + (uint64_t)n * 2;
        p[0] = v & 0xff;
        p[1] = (v >> 8) & 0xff;
    }
    fs->fat_dirty = 1;
}

static int fat_is_eoc(fatx_fs *fs, uint32_t v)
{
    return fs->fat32 ? (v >= 0xfffffff8u) : (v >= 0xfff8u);
}

static uint32_t fat_eoc(fatx_fs *fs)
{
    return fs->fat32 ? 0xffffffffu : 0xffffu;
}

static uint64_t cluster_offset(fatx_fs *fs, uint32_t n)
{
    return fs->data_offset + (uint64_t)(n - 1) * fs->bytes_per_cluster;
}

/* Open the E: FATX. Returns 1 on a mounted, valid filesystem; 0 otherwise
 * (no disk, or the partition is not FATX - then there is simply no save
 * data, which is a valid answer). */
static int fatx_open(fatx_fs *fs)
{
    memset(fs, 0, sizeof(*fs));
    fs->hdd = sd_find_hdd();
    if (!fs->hdd) {
        return 0;
    }

    uint8_t sb[16];
    if (sd_pread(fs, 0, sb, sizeof(sb)) < 0) {
        return 0;
    }
    if (rd_le32(sb) != FATX_MAGIC) {
        return 0;
    }
    uint32_t spc = rd_le32(sb + 8);
    fs->root_cluster = rd_le32(sb + 12);
    if (spc == 0 || spc > 1024 || fs->root_cluster == 0) {
        return 0;
    }
    fs->bytes_per_cluster = spc * (uint32_t)SD_SECTOR;
    fs->num_clusters = (uint32_t)(XBOX_E_SIZE / fs->bytes_per_cluster);
    fs->fat32 = fs->num_clusters >= 0xfff4u;
    uint32_t esz = fs->fat32 ? 4 : 2;
    fs->fat_bytes = sd_align((uint64_t)fs->num_clusters * esz, FATX_FAT_ALIGN);
    fs->data_offset = FATX_FAT_OFFSET + fs->fat_bytes;

    fs->fat = malloc(fs->fat_bytes);
    if (!fs->fat) {
        return 0;
    }
    if (sd_pread(fs, FATX_FAT_OFFSET, fs->fat, fs->fat_bytes) < 0) {
        free(fs->fat);
        fs->fat = NULL;
        return 0;
    }
    fs->ok = 1;
    return 1;
}

static void fatx_flush(fatx_fs *fs)
{
    if (fs->ok && fs->fat_dirty) {
        sd_pwrite(fs, FATX_FAT_OFFSET, fs->fat, fs->fat_bytes);
        fs->fat_dirty = 0;
    }
}

static void fatx_close(fatx_fs *fs)
{
    fatx_flush(fs);
    free(fs->fat);
    fs->fat = NULL;
    fs->ok = 0;
}

/* Read a whole cluster chain into a freshly malloc'd buffer. *out_len is the
 * chain length in bytes (clusters * bytes_per_cluster). Returns NULL on error
 * or an empty/zero chain. */
static uint8_t *read_chain(fatx_fs *fs, uint32_t first, uint64_t *out_len)
{
    if (first < 2 || first >= fs->num_clusters) {
        return NULL;
    }
    /* count clusters first (bounded by num_clusters, so a corrupt loop ends) */
    uint32_t n = first, count = 0;
    for (uint32_t guard = 0; guard <= fs->num_clusters; guard++) {
        count++;
        uint32_t e = fat_get(fs, n);
        if (fat_is_eoc(fs, e) || e == 0) {
            break;
        }
        if (e < 2 || e >= fs->num_clusters) {
            break;
        }
        n = e;
    }
    uint64_t len = (uint64_t)count * fs->bytes_per_cluster;
    uint8_t *buf = malloc(len);
    if (!buf) {
        return NULL;
    }
    n = first;
    for (uint32_t i = 0; i < count; i++) {
        if (sd_pread(fs, cluster_offset(fs, n),
                     buf + (uint64_t)i * fs->bytes_per_cluster,
                     fs->bytes_per_cluster) < 0) {
            free(buf);
            return NULL;
        }
        uint32_t e = fat_get(fs, n);
        if (fat_is_eoc(fs, e) || e < 2 || e >= fs->num_clusters) {
            break;
        }
        n = e;
    }
    *out_len = len;
    return buf;
}

/* ---- directory scanning -------------------------------------------------- */

typedef struct {
    char name[FATX_MAX_NAME + 1];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
    uint64_t dev_part_off; /* partition-relative offset of this 64-byte entry */
} fatx_entry;

/* Parse one on-disk 64-byte entry. Returns 1 = a live entry, 0 = deleted
 * (skip), -1 = end of directory. */
static int parse_entry(const uint8_t *d, fatx_entry *e)
{
    uint8_t nl = d[0];
    if (nl == 0x00 || nl == 0xff) {
        return -1;
    }
    if (nl == 0xe5) {
        return 0;
    }
    if (nl > FATX_MAX_NAME) {
        return 0;
    }
    memcpy(e->name, d + 2, nl);
    e->name[nl] = '\0';
    e->attr = d[1];
    e->first_cluster = rd_le32(d + 44);
    e->size = rd_le32(d + 48);
    return 1;
}

/* Look up `name` in the directory whose chain starts at dir_cluster. On a hit
 * fills *out (including dev_part_off, so the caller can rewrite it) and
 * returns 1; 0 = not found. */
static int dir_find(fatx_fs *fs, uint32_t dir_cluster, const char *name,
                    fatx_entry *out)
{
    uint32_t n = dir_cluster;
    for (uint32_t guard = 0; guard <= fs->num_clusters; guard++) {
        if (n < 1 || n >= fs->num_clusters) { /* cluster 1 is the root dir */
            return 0;
        }
        uint64_t base = cluster_offset(fs, n);
        uint8_t *cl = malloc(fs->bytes_per_cluster);
        if (!cl) {
            return 0;
        }
        if (sd_pread(fs, base, cl, fs->bytes_per_cluster) < 0) {
            free(cl);
            return 0;
        }
        for (uint32_t off = 0; off + FATX_DIRENT <= fs->bytes_per_cluster;
             off += FATX_DIRENT) {
            fatx_entry e;
            int r = parse_entry(cl + off, &e);
            if (r < 0) {
                free(cl);
                return 0; /* end of directory */
            }
            if (r == 0) {
                continue;
            }
            if (strcmp(e.name, name) == 0) {
                e.dev_part_off = base + off;
                *out = e;
                free(cl);
                return 1;
            }
        }
        free(cl);
        uint32_t nx = fat_get(fs, n);
        if (fat_is_eoc(fs, nx) || nx < 2 || nx >= fs->num_clusters) {
            return 0;
        }
        n = nx;
    }
    return 0;
}

/* ---- EXPORT: walk UDATA/, snapshot (path, size, bytes) ------------------- */

typedef struct {
    char *path;
    uint8_t *buf;
    int64_t size;
} sd_file;

static sd_file *g_files;
static int32_t g_count;

static void export_free(void)
{
    for (int32_t i = 0; i < g_count; i++) {
        free(g_files[i].path);
        free(g_files[i].buf);
    }
    free(g_files);
    g_files = NULL;
    g_count = 0;
}

static void export_add(const char *path, const uint8_t *data, int64_t size)
{
    sd_file *grown = realloc(g_files, sizeof(sd_file) * (g_count + 1));
    if (!grown) {
        return;
    }
    g_files = grown;
    g_files[g_count].path = strdup(path);
    g_files[g_count].size = size;
    g_files[g_count].buf = malloc(size > 0 ? (size_t)size : 1);
    if (g_files[g_count].path && g_files[g_count].buf && size > 0) {
        memcpy(g_files[g_count].buf, data, (size_t)size);
    }
    g_count++;
}

/* Recurse a directory, emitting every regular file under `prefix`. */
static void export_walk(fatx_fs *fs, uint32_t dir_cluster, const char *prefix)
{
    uint64_t dlen = 0;
    uint8_t *dir = read_chain(fs, dir_cluster, &dlen);
    if (!dir) {
        return;
    }
    for (uint64_t off = 0; off + FATX_DIRENT <= dlen; off += FATX_DIRENT) {
        fatx_entry e;
        int r = parse_entry(dir + off, &e);
        if (r < 0) {
            break;
        }
        if (r == 0) {
            continue;
        }
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", prefix, e.name);
        if (e.attr & FATX_ATTR_DIR) {
            if (e.first_cluster >= 2 && e.first_cluster < fs->num_clusters) {
                export_walk(fs, e.first_cluster, child);
            }
        } else {
            uint8_t *fdata = NULL;
            uint64_t clen = 0;
            if (e.size > 0 && e.first_cluster >= 2) {
                fdata = read_chain(fs, e.first_cluster, &clen);
            }
            uint32_t got = (fdata && e.size <= clen) ? e.size : 0;
            export_add(child, fdata, got);
            free(fdata);
        }
    }
    free(dir);
}

static void export_rebuild(void)
{
    export_free();
    fatx_fs fs;
    if (!fatx_open(&fs)) {
        return;
    }
    fatx_entry root;
    if (dir_find(&fs, fs.root_cluster, SD_ROOT, &root) &&
        (root.attr & FATX_ATTR_DIR) && root.first_cluster >= 2) {
        export_walk(&fs, root.first_cluster, SD_ROOT);
    }
    fatx_close(&fs);
}

/* The HDD's block backend runs behind the chimera-latency filter, which only
 * completes a request immediately when the machine is stopped; while it runs,
 * delivery waits for the next frame boundary, which never comes from inside a
 * host call. So both export and import fence their block I/O between a
 * vm_stop and a restore of the prior run state - the export runs at a frame
 * boundary (already stopped, a no-op) and the import runs in Init (where the
 * machine is autostarted-but-idle, so stopping it changes nothing the machine
 * can observe and keeps the baseline deterministic). */
ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
    bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }
    export_rebuild();
    if (was_running) {
        vm_start();
    }
    return g_count;
}

ECL_EXPORT const char *GetSaveDataFileName(int32_t index)
{
    return (index >= 0 && index < g_count) ? g_files[index].path : NULL;
}

ECL_EXPORT int64_t GetSaveDataFileSize(int32_t index)
{
    return (index >= 0 && index < g_count) ? g_files[index].size : 0;
}

ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t index)
{
    return (index >= 0 && index < g_count) ? g_files[index].buf : NULL;
}

/* ---- IMPORT: seed the FATX from the project's savedata slot --------------- */

/* Allocate one free data cluster, marked end-of-chain. 0 = disk full. */
static uint32_t alloc_cluster(fatx_fs *fs)
{
    for (uint32_t n = 2; n < fs->num_clusters; n++) {
        if (fat_get(fs, n) == 0) {
            fat_set(fs, n, fat_eoc(fs));
            return n;
        }
    }
    return 0;
}

static void free_chain(fatx_fs *fs, uint32_t first)
{
    uint32_t n = first;
    for (uint32_t guard = 0; guard <= fs->num_clusters; guard++) {
        if (n < 2 || n >= fs->num_clusters) {
            return;
        }
        uint32_t nx = fat_get(fs, n);
        fat_set(fs, n, 0);
        if (fat_is_eoc(fs, nx) || nx < 2 || nx >= fs->num_clusters) {
            return;
        }
        n = nx;
    }
}

/* Write a 64-byte directory entry for (name, attr, first_cluster, size) into
 * the directory chain that starts at dir_cluster, reusing a free/deleted slot
 * or extending the directory with a fresh cluster. Returns 1 on success. */
static int dir_write_entry(fatx_fs *fs, uint32_t dir_cluster, const char *name,
                           uint8_t attr, uint32_t first_cluster, uint32_t size)
{
    size_t nl = strlen(name);
    if (nl == 0 || nl > FATX_MAX_NAME) {
        return 0;
    }
    uint8_t ent[FATX_DIRENT];
    memset(ent, 0xff, sizeof(ent));
    ent[0] = (uint8_t)nl;
    ent[1] = attr;
    memcpy(ent + 2, name, nl);
    wr_le32(ent + 44, first_cluster);
    wr_le32(ent + 48, size);
    wr_le32(ent + 52, FATX_FIXED_TIME);
    wr_le32(ent + 56, FATX_FIXED_TIME);
    wr_le32(ent + 60, FATX_FIXED_TIME);

    uint32_t n = dir_cluster, last = dir_cluster;
    for (uint32_t guard = 0; guard <= fs->num_clusters; guard++) {
        if (n < 1 || n >= fs->num_clusters) { /* cluster 1 is the root dir */
            break;
        }
        last = n;
        uint64_t base = cluster_offset(fs, n);
        uint8_t *cl = malloc(fs->bytes_per_cluster);
        if (!cl) {
            return 0;
        }
        if (sd_pread(fs, base, cl, fs->bytes_per_cluster) < 0) {
            free(cl);
            return 0;
        }
        for (uint32_t off = 0; off + FATX_DIRENT <= fs->bytes_per_cluster;
             off += FATX_DIRENT) {
            uint8_t l = cl[off];
            if (l == 0x00 || l == 0xff || l == 0xe5) {
                free(cl);
                return sd_pwrite(fs, base + off, ent, FATX_DIRENT) >= 0;
            }
        }
        free(cl);
        uint32_t nx = fat_get(fs, n);
        if (fat_is_eoc(fs, nx) || nx < 2 || nx >= fs->num_clusters) {
            break;
        }
        n = nx;
    }

    /* directory full - extend it by one cluster */
    uint32_t nc = alloc_cluster(fs);
    if (nc == 0) {
        return 0;
    }
    fat_set(fs, last, nc);
    uint8_t *empty = malloc(fs->bytes_per_cluster);
    if (!empty) {
        return 0;
    }
    memset(empty, 0xff, fs->bytes_per_cluster);
    memcpy(empty, ent, FATX_DIRENT);
    int rc = sd_pwrite(fs, cluster_offset(fs, nc), empty, fs->bytes_per_cluster);
    free(empty);
    return rc >= 0;
}

/* Find `name` under dir_cluster; if it is a directory, return its first
 * cluster. If absent, create an empty directory and its entry. 0 = failure. */
static uint32_t dir_get_or_make(fatx_fs *fs, uint32_t dir_cluster,
                                const char *name)
{
    fatx_entry e;
    if (dir_find(fs, dir_cluster, name, &e)) {
        if (!(e.attr & FATX_ATTR_DIR)) {
            return 0; /* a file where a directory is needed */
        }
        return e.first_cluster;
    }
    uint32_t nc = alloc_cluster(fs);
    if (nc == 0) {
        return 0;
    }
    uint8_t *empty = malloc(fs->bytes_per_cluster);
    if (!empty) {
        return 0;
    }
    memset(empty, 0xff, fs->bytes_per_cluster);
    int rc = sd_pwrite(fs, cluster_offset(fs, nc), empty, fs->bytes_per_cluster);
    free(empty);
    if (rc < 0) {
        return 0;
    }
    if (!dir_write_entry(fs, dir_cluster, name, FATX_ATTR_DIR, nc, 0)) {
        return 0;
    }
    return nc;
}

/* Write a regular file `name` with `data`/`size` into dir_cluster, replacing
 * any existing file of that name. Returns 1 on success. */
static int file_write(fatx_fs *fs, uint32_t dir_cluster, const char *name,
                      const uint8_t *data, uint64_t size)
{
    fatx_entry existing;
    int have = dir_find(fs, dir_cluster, name, &existing);
    if (have && (existing.attr & FATX_ATTR_DIR)) {
        return 0; /* a directory already owns this name */
    }
    if (have && existing.first_cluster >= 2) {
        free_chain(fs, existing.first_cluster);
    }

    uint32_t first = 0, prev = 0;
    uint64_t written = 0;
    while (written < size) {
        uint32_t nc = alloc_cluster(fs);
        if (nc == 0) {
            return 0; /* disk full */
        }
        if (first == 0) {
            first = nc;
        } else {
            fat_set(fs, prev, nc);
        }
        prev = nc;

        uint8_t *cl = malloc(fs->bytes_per_cluster);
        if (!cl) {
            return 0;
        }
        memset(cl, 0, fs->bytes_per_cluster);
        uint64_t chunk = size - written;
        if (chunk > fs->bytes_per_cluster) {
            chunk = fs->bytes_per_cluster;
        }
        memcpy(cl, data + written, chunk);
        int rc = sd_pwrite(fs, cluster_offset(fs, nc), cl, fs->bytes_per_cluster);
        free(cl);
        if (rc < 0) {
            return 0;
        }
        written += chunk;
    }

    if (have) {
        /* rewrite the existing entry in place with the new chain and size */
        uint8_t ent[FATX_DIRENT];
        if (sd_pread(fs, existing.dev_part_off, ent, FATX_DIRENT) < 0) {
            return 0;
        }
        wr_le32(ent + 44, first);
        wr_le32(ent + 48, (uint32_t)size);
        return sd_pwrite(fs, existing.dev_part_off, ent, FATX_DIRENT) >= 0;
    }
    return dir_write_entry(fs, dir_cluster, name, 0, first, (uint32_t)size);
}

/* Create every directory along `relpath` and write its final component as a
 * file. `relpath` is '/'-separated, already validated. Returns 1 on success. */
static int fatx_write_path(fatx_fs *fs, const char *relpath,
                           const uint8_t *data, uint64_t size)
{
    char comps[1024];
    strncpy(comps, relpath, sizeof(comps) - 1);
    comps[sizeof(comps) - 1] = '\0';

    uint32_t cur = fs->root_cluster;
    char *save = NULL;
    char *tok = strtok_r(comps, "/", &save);
    while (tok) {
        char *next = strtok_r(NULL, "/", &save);
        if (strlen(tok) > FATX_MAX_NAME) {
            return 0;
        }
        if (next == NULL) {
            return file_write(fs, cur, tok, data, size);
        }
        cur = dir_get_or_make(fs, cur, tok);
        if (cur == 0) {
            return 0;
        }
        tok = next;
    }
    return 0; /* empty path */
}

/* A savedata name the machine will actually read: relative, under UDATA/, no
 * "..", no leading '/', no backslash. Mirrors flycast's refusal so a project
 * carrying save data the machine cannot use fails loudly rather than silently
 * dropping the user's progress. */
static int sd_name_ok(const char *name)
{
    if (strncmp(name, SD_ROOT "/", strlen(SD_ROOT) + 1) != 0) {
        return 0;
    }
    if (name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) {
        return 0;
    }
    return 1;
}

/* Declared for xemu-waterbox.c's Init(), which calls it before seal. */
int chimera_savedata_import(char *err_out, size_t err_len);

/* Called at the end of Init, before seal. Reads each file the project's
 * "savedata" slot carries and writes it into the E: FATX at the same relative
 * path. Sets *err_out (a >=256-byte buffer) and returns 0 on refusal/failure;
 * returns 1 when nothing was brought or everything landed. */
static int savedata_import_body(char *err_out, size_t err_len)
{
    int32_t saves = wbx_slot_count("savedata");
    if (saves <= 0) {
        return 1;
    }

    fatx_fs fs;
    int mounted = fatx_open(&fs);

    char name[1024];
    for (int32_t i = 0; i < saves; i++) {
        if (wbx_slot_name("savedata", i, name, sizeof(name)) == NULL) {
            continue;
        }
        if (!sd_name_ok(name)) {
            snprintf(err_out, err_len,
                     "this machine keeps its saves under %s/<titleId>/... on the "
                     "hard disk; it cannot place save data called \"%s\". Put the "
                     "files back under the paths Export Save Data wrote.",
                     SD_ROOT, name);
            if (mounted) {
                fatx_close(&fs);
            }
            return 0;
        }
        if (!mounted) {
            snprintf(err_out, err_len,
                     "the project carries Xbox save data but this machine has no "
                     "formatted hard disk to place it on.");
            return 0;
        }

        FILE *f = fopen(name, "rb");
        if (!f) {
            continue; /* slot listed it but no bytes arrived; skip */
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(sz > 0 ? (size_t)sz : 1);
        if (!buf) {
            fclose(f);
            fatx_close(&fs);
            snprintf(err_out, err_len, "out of memory seeding save \"%s\"", name);
            return 0;
        }
        size_t got = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
        fclose(f);

        int ok = fatx_write_path(&fs, name, buf, got);
        free(buf);
        if (!ok) {
            snprintf(err_out, err_len,
                     "could not write save \"%s\" into the hard disk's filesystem",
                     name);
            fatx_close(&fs);
            return 0;
        }
    }

    if (mounted) {
        fatx_close(&fs);
    }
    return 1;
}

int chimera_savedata_import(char *err_out, size_t err_len)
{
    if (wbx_slot_count("savedata") <= 0) {
        return 1; /* nothing to seed, and no reason to touch the machine */
    }
    bool was_running = runstate_is_running();
    if (was_running) {
        vm_stop(RUN_STATE_PAUSED);
    }
    int ok = savedata_import_body(err_out, err_len);
    if (was_running) {
        vm_start();
    }
    return ok;
}

#endif /* CHIMERA_GUEST */
