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

#define CHIMERA_BLK_LATENCY_NS 2000000 /* 2ms of virtual time */

/* cow=on keeps every write in an in-memory overlay of fixed-size chunks and
 * never touches the underlying file: the image stays pristine on disk, the
 * written state lives in (savestated) memory, and the sandbox needs no
 * writable host file at all. Reads merge the overlay over the base.
 */
#define COW_CHUNK 65536

typedef struct ChimeraLatencyState {
    bool cow;
    GHashTable *overlay; /* chunk index -> guint8[COW_CHUNK] */
} ChimeraLatencyState;

static int chimera_latency_open(BlockDriverState *bs, QDict *options,
                                int flags, Error **errp)
{
    ChimeraLatencyState *st = bs->opaque;

    st->cow = false;
    const char *cow = qdict_get_try_str(options, "cow");
    if (cow) {
        st->cow = strcmp(cow, "on") == 0;
        qdict_del(options, "cow");
    }
    if (st->cow) {
        st->overlay = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);
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

/* fetch-or-create the overlay chunk holding `chunk_idx`, filling it from the
 * base image on first touch (RMW) */
static guint8 * coroutine_fn GRAPH_RDLOCK
cow_chunk_for_write(BlockDriverState *bs, int64_t chunk_idx)
{
    ChimeraLatencyState *st = bs->opaque;
    guint8 *data = g_hash_table_lookup(st->overlay, &chunk_idx);
    if (data) {
        return data;
    }
    data = g_malloc0(COW_CHUNK);
    int64_t base = chunk_idx * COW_CHUNK;
    int64_t len = bdrv_co_getlength(bs->file->bs);
    if (base < len) {
        int64_t n = MIN((int64_t)COW_CHUNK, len - base);
        bdrv_co_pread(bs->file, base, n, data, 0);
    }
    int64_t *key = g_new(int64_t, 1);
    *key = chunk_idx;
    g_hash_table_insert(st->overlay, key, data);
    return data;
}

/* copy any overlaid ranges over a buffer just read from the base */
static void cow_apply_overlay(BlockDriverState *bs, int64_t offset,
                              int64_t bytes, guint8 *buf)
{
    ChimeraLatencyState *st = bs->opaque;
    for (int64_t c = offset / COW_CHUNK; c * COW_CHUNK < offset + bytes; c++) {
        guint8 *data = g_hash_table_lookup(st->overlay, &c);
        if (!data) {
            continue;
        }
        int64_t cs = c * COW_CHUNK;
        int64_t from = MAX(offset, cs);
        int64_t to = MIN(offset + bytes, cs + (int64_t)COW_CHUNK);
        memcpy(buf + (from - offset), data + (from - cs), to - from);
    }
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

/* Sleep the calling coroutine until submission time + L on the virtual
 * clock. Runs AFTER the underlying request completed, so the data is
 * there; the guest just does not hear about it early.
 */
static void coroutine_fn chimera_latency_pace(int64_t t_submit)
{
    /* Requests issued while the machine is stopped (device realize probing
     * the geometry, savevm) must complete immediately: the virtual clock is
     * not advancing, so the sleep would never end - and a stopped machine
     * observes nothing, so immediate delivery is deterministic anyway. */
    if (!runstate_is_running()) {
        return;
    }
    int64_t deadline = t_submit + CHIMERA_BLK_LATENCY_NS;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < deadline) {
        qemu_co_sleep_ns(QEMU_CLOCK_VIRTUAL, deadline - now);
    } else {
        fprintf(stderr, "chimera-latency: host I/O outlived the %dns virtual "
                "budget by %" PRId64 "ns - this run is not deterministic\n",
                CHIMERA_BLK_LATENCY_NS, now - deadline);
    }
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                          QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    ChimeraLatencyState *st = bs->opaque;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret;
    if (st->cow) {
        guint8 *buf = g_malloc(bytes);
        ret = bdrv_co_pread(bs->file, offset, bytes, buf, flags);
        if (ret >= 0) {
            cow_apply_overlay(bs, offset, bytes, buf);
            qemu_iovec_from_buf(qiov, 0, buf, bytes);
        }
        g_free(buf);
    } else {
        ret = bdrv_co_preadv(bs->file, offset, bytes, qiov, flags);
    }
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                           QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    ChimeraLatencyState *st = bs->opaque;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = 0;
    if (st->cow) {
        guint8 *buf = g_malloc(bytes);
        qemu_iovec_to_buf(qiov, 0, buf, bytes);
        for (int64_t done = 0; done < bytes; ) {
            int64_t pos = offset + done;
            guint8 *chunk = cow_chunk_for_write(bs, pos / COW_CHUNK);
            int64_t in = pos % COW_CHUNK;
            int64_t n = MIN(bytes - done, (int64_t)COW_CHUNK - in);
            memcpy(chunk + in, buf + done, n);
            done += n;
        }
        g_free(buf);
    } else {
        ret = bdrv_co_pwritev(bs->file, offset, bytes, qiov, flags);
    }
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pwrite_zeroes(BlockDriverState *bs, int64_t offset,
                                 int64_t bytes, BdrvRequestFlags flags)
{
    ChimeraLatencyState *st = bs->opaque;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = 0;
    if (st->cow) {
        for (int64_t done = 0; done < bytes; ) {
            int64_t pos = offset + done;
            guint8 *chunk = cow_chunk_for_write(bs, pos / COW_CHUNK);
            int64_t in = pos % COW_CHUNK;
            int64_t n = MIN(bytes - done, (int64_t)COW_CHUNK - in);
            memset(chunk + in, 0, n);
            done += n;
        }
    } else {
        ret = bdrv_co_pwrite_zeroes(bs->file, offset, bytes, flags);
    }
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pdiscard(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    ChimeraLatencyState *st = bs->opaque;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = 0;
    if (!st->cow) {
        ret = bdrv_co_pdiscard(bs->file, offset, bytes);
    }
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_flush(BlockDriverState *bs)
{
    ChimeraLatencyState *st = bs->opaque;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = 0;
    if (!st->cow) {
        ret = bdrv_co_flush(bs->file->bs);
    }
    chimera_latency_pace(t0);
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
