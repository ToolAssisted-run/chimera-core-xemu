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
#include "qapi/error.h"

#define CHIMERA_BLK_LATENCY_NS 2000000 /* 2ms of virtual time */

static int chimera_latency_open(BlockDriverState *bs, QDict *options,
                                int flags, Error **errp)
{
    int ret = bdrv_open_file_child(NULL, options, "image", bs, errp);
    if (ret < 0) {
        return ret;
    }
    bs->supported_write_flags = BDRV_REQ_WRITE_UNCHANGED;
    bs->supported_zero_flags = BDRV_REQ_WRITE_UNCHANGED;
    return 0;
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
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = bdrv_co_preadv(bs->file, offset, bytes, qiov, flags);
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                           QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = bdrv_co_pwritev(bs->file, offset, bytes, qiov, flags);
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pwrite_zeroes(BlockDriverState *bs, int64_t offset,
                                 int64_t bytes, BdrvRequestFlags flags)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = bdrv_co_pwrite_zeroes(bs->file, offset, bytes, flags);
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_pdiscard(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = bdrv_co_pdiscard(bs->file, offset, bytes);
    chimera_latency_pace(t0);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chimera_latency_co_flush(BlockDriverState *bs)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int ret = bdrv_co_flush(bs->file->bs);
    chimera_latency_pace(t0);
    return ret;
}

static BlockDriver bdrv_chimera_latency = {
    .format_name            = "chimera-latency",
    .instance_size          = 0,
    .is_filter              = true,

    .bdrv_open              = chimera_latency_open,
    .bdrv_child_perm        = bdrv_default_perms,
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
