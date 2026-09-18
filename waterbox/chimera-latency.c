/*
 * Fixed-virtual-latency block filter, for deterministic machines
 *
 * Every request completes exactly CHIMERA_BLK_LATENCY_NS of VIRTUAL time
 * after it was issued. The guest can never observe how long the host's I/O
 * actually took: submission happens at a deterministic virtual instant (the
 * guest issued it), and delivery happens at submission + L, reached only by
 * executing guest instructions. If the host is ever slower than L of
 * virtual time, delivery slips and determinism is lost - that is loudly
 * reported, never silent.
 *
 * This is the standalone cousin of blkreplay: same interception points, but
 * anchored to the virtual clock instead of the record/replay log.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/coroutine.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "system/runstate.h"
#include "qobject/qdict.h"
#include "qapi/error.h"

/* cow=on keeps every write in an in-memory overlay and never touches the
 * underlying file: the image stays pristine on disk, the written state lives
 * in (savestated) memory, and the sandbox needs no writable host file at all.
 * Reads merge the overlay over the base.
 *
 * The overlay is chunked, and a chunk is per-SECTOR: each sector is either
 * still the base's, held as bytes, known to be zero, or a MIRROR - a sector
 * whose bytes are provably a stretch of a mounted image, held as that image
 * and an offset. A game installs itself: Prince of Persia copies 287 MB off
 * the DVD onto the hard disk in its first 2400 frames, and every savestate
 * carried that second copy of the disc (the machine's own memory in use was
 * 37 MB). The filter sees both ends of the copy - the DVD's reads and the
 * disk's writes both pass through it - so it remembers the last reads that
 * came straight off a base image, and a write whose bytes equal what one of
 * them read (compared, never assumed) is recorded as a mirror of that image
 * rather than stored. Any write that matches nothing is held as bytes, as
 * before, so nothing a game can do reads differently; a mirrored sector costs
 * eight bytes instead of 512.
 *
 * Deterministic: the decision depends only on the guest's sequence of reads
 * and writes, which is the machine's, so native and sandbox agree - and no
 * comparison ever waits, because every request already completes at the next
 * frame boundary whatever the host did meanwhile.
 */
#define COW_CHUNK 65536
#define COW_SECTOR 512
#define COW_SECTORS (COW_CHUNK / COW_SECTOR)

enum {
    SRC_BASE = -1, /* never written: the base image's */
    SRC_DATA = -2, /* held in the chunk's own bytes */
    SRC_ZERO = -3, /* written as zeros (write_zeroes) */
    /* >= 0: an offset in `src`'s base image holding exactly these bytes */
};

typedef struct CowChunk {
    guint8 *data;                 /* COW_CHUNK bytes, once a sector is held */
    BlockDriverState *src;        /* the filter the mirrored sectors read from */
    int64_t sector[COW_SECTORS];
} CowChunk;

typedef struct ChimeraLatencyState {
    bool cow;
    GHashTable *overlay; /* chunk index -> CowChunk */
} ChimeraLatencyState;

/* The last reads that came straight off a base image (no overlay in their
 * range), newest last: what a copy's writes are compared against. Shared by
 * every instance, because the copy goes from one device to another. */
#define RECENT_READS 64
#define MIRROR_MIN 2048  /* a write smaller than this is metadata, not a copy */
static struct { BlockDriverState *bs; int64_t offset, bytes; } recent_reads[RECENT_READS];
static unsigned recent_next;
/* where the copy will most likely continue: just past the last match */
static struct { BlockDriverState *bs; int64_t next; } copy_cursor;

static void cow_chunk_free(gpointer p)
{
    CowChunk *c = p;
    g_free(c->data);
    g_free(c);
}

static void remember_read(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    if (bytes < MIRROR_MIN) {
        return;
    }
    recent_reads[recent_next % RECENT_READS].bs = bs;
    recent_reads[recent_next % RECENT_READS].offset = offset;
    recent_reads[recent_next % RECENT_READS].bytes = bytes;
    recent_next++;
}

static void chimera_latency_vm_state(void *opaque, bool running,
                                     RunState state);

static int chimera_latency_open(BlockDriverState *bs, QDict *options,
                                int flags, Error **errp)
{
    ChimeraLatencyState *st = bs->opaque;

    static bool vm_state_handler_registered;
    if (!vm_state_handler_registered) {
        vm_state_handler_registered = true;
        qemu_add_vm_change_state_handler(chimera_latency_vm_state, NULL);
    }

    st->cow = false;
    const char *cow = qdict_get_try_str(options, "cow");
    if (cow) {
        st->cow = strcmp(cow, "on") == 0;
        qdict_del(options, "cow");
    }
    if (st->cow) {
        st->overlay = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, cow_chunk_free);
        /* writes never reach the file, so open it read-only */
        flags &= ~BDRV_O_RDWR;
    }

    int ret = bdrv_open_file_child(NULL, options, "image", bs, errp);
    if (ret < 0) {
        return ret;
    }
    bs->supported_write_flags = BDRV_REQ_WRITE_UNCHANGED;
    bs->supported_zero_flags = BDRV_REQ_WRITE_UNCHANGED;
    return 0;
}

/* fetch-or-create the overlay chunk holding `chunk_idx`: every sector still
 * the base's, and no bytes of its own yet */
static CowChunk *cow_chunk_for_write(BlockDriverState *bs, int64_t chunk_idx)
{
    ChimeraLatencyState *st = bs->opaque;
    CowChunk *c = g_hash_table_lookup(st->overlay, &chunk_idx);
    if (c) {
        return c;
    }
    c = g_new0(CowChunk, 1);
    for (int i = 0; i < COW_SECTORS; i++) {
        c->sector[i] = SRC_BASE;
    }
    int64_t *key = g_new(int64_t, 1);
    *key = chunk_idx;
    g_hash_table_insert(st->overlay, key, c);
    return c;
}

/* whether any overlay chunk lies in [offset, offset + bytes) */
static bool cow_touched(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    ChimeraLatencyState *st = bs->opaque;
    for (int64_t c = offset / COW_CHUNK; c * COW_CHUNK < offset + bytes; c++) {
        if (g_hash_table_lookup(st->overlay, &c)) {
            return true;
        }
    }
    return false;
}

/* one sector of a chunk as the machine would read it, into `out` */
static int coroutine_fn GRAPH_RDLOCK
cow_sector_read(BlockDriverState *bs, CowChunk *c, int64_t chunk_idx, int s,
                guint8 *out)
{
    int64_t at = chunk_idx * COW_CHUNK + (int64_t)s * COW_SECTOR;
    switch (c->sector[s]) {
    case SRC_DATA:
        memcpy(out, c->data + s * COW_SECTOR, COW_SECTOR);
        return 0;
    case SRC_ZERO:
        memset(out, 0, COW_SECTOR);
        return 0;
    case SRC_BASE: {
        int64_t len = bdrv_co_getlength(bs->file->bs);
        memset(out, 0, COW_SECTOR);
        if (at < len) {
            return bdrv_co_pread(bs->file, at, MIN((int64_t)COW_SECTOR, len - at), out, 0);
        }
        return 0;
    }
    default:
        return bdrv_co_pread(c->src->file, c->sector[s], COW_SECTOR, out, 0);
    }
}

/* the sector becomes bytes of the chunk's own, holding what it held */
static int coroutine_fn GRAPH_RDLOCK
cow_sector_materialise(BlockDriverState *bs, CowChunk *c, int64_t chunk_idx, int s)
{
    if (c->sector[s] == SRC_DATA) {
        return 0;
    }
    if (!c->data) {
        c->data = g_malloc0(COW_CHUNK);
    }
    int ret = cow_sector_read(bs, c, chunk_idx, s, c->data + s * COW_SECTOR);
    if (ret < 0) {
        return ret;
    }
    c->sector[s] = SRC_DATA;
    return 0;
}

/* copy any overlaid sectors over a buffer just read from the base: bytes
 * from the chunk, zeros, or the mirrored image - the last in runs, since a
 * copied file is contiguous in both places */
static int coroutine_fn GRAPH_RDLOCK
cow_apply_overlay(BlockDriverState *bs, int64_t offset, int64_t bytes, guint8 *buf)
{
    ChimeraLatencyState *st = bs->opaque;
    for (int64_t ci = offset / COW_CHUNK; ci * COW_CHUNK < offset + bytes; ci++) {
        CowChunk *c = g_hash_table_lookup(st->overlay, &ci);
        if (!c) {
            continue;
        }
        int64_t cs = ci * COW_CHUNK;
        int64_t from = MAX(offset, cs);
        int64_t to = MIN(offset + bytes, cs + (int64_t)COW_CHUNK);
        /* a partial sector at either edge: through a sector buffer */
        for (int64_t at = from; at < to; ) {
            int s = (int)((at - cs) / COW_SECTOR);
            int64_t sec_start = cs + (int64_t)s * COW_SECTOR;
            int64_t sec_end = sec_start + COW_SECTOR;
            int64_t n = MIN(to, sec_end) - at;
            if (c->sector[s] == SRC_BASE) {
                at += n;
                continue;
            }
            if (at != sec_start || n != COW_SECTOR) {
                guint8 sec[COW_SECTOR];
                int ret = cow_sector_read(bs, c, ci, s, sec);
                if (ret < 0) {
                    return ret;
                }
                memcpy(buf + (at - offset), sec + (at - sec_start), n);
                at += n;
                continue;
            }
            if (c->sector[s] >= 0) {
                /* a run of mirrored sectors that follow each other in the
                 * source too: one read */
                int64_t src = c->sector[s];
                int64_t run = COW_SECTOR;
                int k = s + 1;
                while (at + run + COW_SECTOR <= to && k < COW_SECTORS
                       && c->sector[k] == src + run) {
                    run += COW_SECTOR;
                    k++;
                }
                int ret = bdrv_co_pread(c->src->file, src, run, buf + (at - offset), 0);
                if (ret < 0) {
                    return ret;
                }
                at += run;
                continue;
            }
            if (c->sector[s] == SRC_ZERO) {
                memset(buf + (at - offset), 0, COW_SECTOR);
            } else {
                memcpy(buf + (at - offset), c->data + s * COW_SECTOR, COW_SECTOR);
            }
            at += COW_SECTOR;
        }
    }
    return 0;
}

/* Where, if anywhere, a base image holds exactly these bytes: the copy's
 * cursor first (the next piece of a file being copied), then the start of
 * each recent read, newest first. Compared byte for byte. */
static BlockDriverState * coroutine_fn GRAPH_RDLOCK
cow_find_source(const guint8 *buf, int64_t bytes, int64_t *src_offset)
{
    guint8 *tmp = g_malloc(bytes);
    BlockDriverState *found = NULL;
    for (int i = -1; i < RECENT_READS && !found; i++) {
        BlockDriverState *src;
        int64_t at;
        if (i < 0) {
            if (!copy_cursor.bs) {
                continue;
            }
            src = copy_cursor.bs;
            at = copy_cursor.next;
        } else {
            unsigned k = (recent_next + RECENT_READS - 1 - (unsigned)i) % RECENT_READS;
            src = recent_reads[k].bs;
            if (!src || recent_reads[k].bytes < bytes) {
                continue;
            }
            at = recent_reads[k].offset;
        }
        int64_t len = bdrv_co_getlength(src->file->bs);
        if (at < 0 || at + bytes > len) {
            continue;
        }
        if (bdrv_co_pread(src->file, at, bytes, tmp, 0) < 0) {
            continue;
        }
        if (memcmp(tmp, buf, bytes) == 0) {
            found = src;
            *src_offset = at;
        }
    }
    g_free(tmp);
    return found;
}

/* the bytes of one write into the overlay, as a mirror where a source was
 * found and as bytes where not */
static int coroutine_fn GRAPH_RDLOCK
cow_write(BlockDriverState *bs, int64_t offset, int64_t bytes, const guint8 *buf,
          BlockDriverState *src, int64_t src_offset)
{
    for (int64_t done = 0; done < bytes; ) {
        int64_t pos = offset + done;
        int64_t ci = pos / COW_CHUNK;
        CowChunk *c = cow_chunk_for_write(bs, ci);
        int64_t cs = ci * COW_CHUNK;
        int s = (int)((pos - cs) / COW_SECTOR);
        int64_t sec_start = cs + (int64_t)s * COW_SECTOR;
        int64_t n = MIN(bytes - done, sec_start + COW_SECTOR - pos);
        const bool whole = pos == sec_start && n == COW_SECTOR;
        /* a chunk mirrors one image; a second one's sectors are held as bytes */
        const bool mirror = whole && src && (c->src == NULL || c->src == src);
        if (mirror) {
            c->src = src;
            c->sector[s] = src_offset + done;
        } else {
            int ret = cow_sector_materialise(bs, c, ci, s);
            if (ret < 0) {
                return ret;
            }
            memcpy(c->data + (pos - cs), buf + done, n);
        }
        done += n;
    }
    return 0;
}

/* with cow on, the child is strictly read-only: writes live in the overlay,
 * so no write permission may propagate down - the image may well be a
 * read-only mount */
static void chimera_latency_child_perm(BlockDriverState *bs, BdrvChild *c,
                                       BdrvChildRole role,
                                       BlockReopenQueue *reopen_queue,
                                       uint64_t perm, uint64_t shared,
                                       uint64_t *nperm, uint64_t *nshared)
{
    ChimeraLatencyState *st = bs->opaque;
    bdrv_default_perms(bs, c, role, reopen_queue, perm, shared, nperm, nshared);
    if (st->cow) {
        *nperm &= ~(BLK_PERM_WRITE | BLK_PERM_RESIZE |
                    BLK_PERM_WRITE_UNCHANGED);
        *nshared = BLK_PERM_ALL;
    }
}

static int64_t coroutine_fn GRAPH_RDLOCK
chimera_latency_co_getlength(BlockDriverState *bs)
{
    return bdrv_co_getlength(bs->file->bs);
}

/* Delivery model: every request completes at the NEXT FRAME BOUNDARY (the
 * vm_stop the driver performs each frame). That instant is a pure virtual
 * time, identical in the native reference and the sandbox regardless of
 * their thread models - host-preemptive on one side, cooperative green
 * threads on the other - so the disk can never carry host scheduling into
 * the machine. The 0..16.7ms quantized latency is also about what a real
 * drive does.
 *
 * Requests issued while the machine is stopped (device realize probing the
 * geometry, savevm, and the boundary itself) complete immediately: the
 * machine observes nothing while stopped.
 */
typedef struct ChimeraSleeper {
    Coroutine *co;
    bool waiting;
    bool done;
    bool inserted;
    QLIST_ENTRY(ChimeraSleeper) next;
} ChimeraSleeper;

static QLIST_HEAD(, ChimeraSleeper) chimera_sleepers =
    QLIST_HEAD_INITIALIZER(chimera_sleepers);

static void chimera_latency_vm_state(void *opaque, bool running,
                                     RunState state)
{
    (void)opaque;
    (void)state;
    if (!running) {
        ChimeraSleeper *s, *tmp;
        QLIST_FOREACH_SAFE(s, &chimera_sleepers, next, tmp) {
            if (!s->done) {
                s->done = true;
                if (s->waiting) {
                    qemu_coroutine_enter(s->co);
                }
                /* not yet waiting: still inside its host I/O; it will see
                 * done in pace_end and return without sleeping */
            }
        }
    }
}

static void coroutine_fn chimera_pace_begin(ChimeraSleeper *s)
{
    memset(s, 0, sizeof *s);
    s->co = qemu_coroutine_self();
    if (!runstate_is_running()) {
        s->done = true;
        return;
    }
    QLIST_INSERT_HEAD(&chimera_sleepers, s, next);
    s->inserted = true;
}

static void coroutine_fn chimera_pace_end(ChimeraSleeper *s)
{
    if (!s->done) {
        s->waiting = true;
        qemu_coroutine_yield(); /* entered by the boundary vm_stop */
    }
    if (s->inserted) {
        QLIST_REMOVE(s, next);
    }
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                          QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    ChimeraLatencyState *st = bs->opaque;
    if (getenv("CHIMERA_DEBUG_BLK")) {
        fprintf(stderr, "[blk] read %s off=%lld n=%lld vclock=%lld\n",
                bs->filename, (long long)offset, (long long)bytes,
                (long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    ChimeraSleeper sl;
    chimera_pace_begin(&sl);
    int ret;
    if (st->cow) {
        guint8 *buf = g_malloc(bytes);
        ret = bdrv_co_pread(bs->file, offset, bytes, buf, flags);
        if (ret >= 0) {
            if (cow_touched(bs, offset, bytes)) {
                ret = cow_apply_overlay(bs, offset, bytes, buf);
            } else {
                /* straight off the image: what a copy's writes may match */
                remember_read(bs, offset, bytes);
            }
        }
        if (ret >= 0) {
            qemu_iovec_from_buf(qiov, 0, buf, bytes);
        }
        g_free(buf);
    } else {
        ret = bdrv_co_preadv(bs->file, offset, bytes, qiov, flags);
    }
    chimera_pace_end(&sl);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                           QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    ChimeraLatencyState *st = bs->opaque;
    ChimeraSleeper sl;
    chimera_pace_begin(&sl);
    int ret = 0;
    if (st->cow) {
        guint8 *buf = g_malloc(bytes);
        qemu_iovec_to_buf(qiov, 0, buf, bytes);
        BlockDriverState *src = NULL;
        int64_t src_offset = 0;
        if (bytes >= MIRROR_MIN) {
            src = cow_find_source(buf, bytes, &src_offset);
        }
        if (src) {
            copy_cursor.bs = src;
            copy_cursor.next = src_offset + bytes;
        }
        ret = cow_write(bs, offset, bytes, buf, src, src_offset);
        g_free(buf);
    } else {
        ret = bdrv_co_pwritev(bs->file, offset, bytes, qiov, flags);
    }
    chimera_pace_end(&sl);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pwrite_zeroes(BlockDriverState *bs, int64_t offset,
                                 int64_t bytes, BdrvRequestFlags flags)
{
    ChimeraLatencyState *st = bs->opaque;
    ChimeraSleeper sl;
    chimera_pace_begin(&sl);
    int ret = 0;
    if (st->cow) {
        for (int64_t done = 0; done < bytes && ret >= 0; ) {
            int64_t pos = offset + done;
            int64_t ci = pos / COW_CHUNK;
            CowChunk *c = cow_chunk_for_write(bs, ci);
            int64_t cs = ci * COW_CHUNK;
            int s = (int)((pos - cs) / COW_SECTOR);
            int64_t sec_start = cs + (int64_t)s * COW_SECTOR;
            int64_t n = MIN(bytes - done, sec_start + COW_SECTOR - pos);
            if (pos == sec_start && n == COW_SECTOR) {
                c->sector[s] = SRC_ZERO;
            } else {
                ret = cow_sector_materialise(bs, c, ci, s);
                if (ret >= 0) {
                    memset(c->data + (pos - cs), 0, n);
                }
            }
            done += n;
        }
    } else {
        ret = bdrv_co_pwrite_zeroes(bs->file, offset, bytes, flags);
    }
    chimera_pace_end(&sl);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pdiscard(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    ChimeraLatencyState *st = bs->opaque;
    ChimeraSleeper sl;
    chimera_pace_begin(&sl);
    int ret = 0;
    if (!st->cow) {
        ret = bdrv_co_pdiscard(bs->file, offset, bytes);
    }
    chimera_pace_end(&sl);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_flush(BlockDriverState *bs)
{
    ChimeraLatencyState *st = bs->opaque;
    ChimeraSleeper sl;
    chimera_pace_begin(&sl);
    int ret = 0;
    if (!st->cow) {
        ret = bdrv_co_flush(bs->file->bs);
    }
    chimera_pace_end(&sl);
    return ret;
}

static BlockDriver bdrv_chimera_latency = {
    .format_name            = "chimera-latency",
    .instance_size          = sizeof(ChimeraLatencyState),
    .is_filter              = true,

    .bdrv_open              = chimera_latency_open,
    .bdrv_child_perm        = chimera_latency_child_perm,
    .bdrv_co_getlength      = chimera_latency_co_getlength,

    .bdrv_co_preadv         = chimera_latency_co_preadv,
    .bdrv_co_pwritev        = chimera_latency_co_pwritev,
    .bdrv_co_pwrite_zeroes  = chimera_latency_co_pwrite_zeroes,
    .bdrv_co_pdiscard       = chimera_latency_co_pdiscard,
    .bdrv_co_flush          = chimera_latency_co_flush,
};

static void bdrv_chimera_latency_init(void)
{
    bdrv_register(&bdrv_chimera_latency);
}

block_init(bdrv_chimera_latency_init);
