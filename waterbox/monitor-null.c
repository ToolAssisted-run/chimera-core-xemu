/* CHIMERA: the headless APU monitor - the mixed EP frames go nowhere (M1).
 * The waterbox driver taps mcpx_apu_monitor_frame's buffer for Chimera audio
 * in M3; until then the sink only has to exist so apu.c links.
 */
#include "qemu/osdep.h"
#include "apu_int.h"

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 0;
    d->monitor.queued_bytes_high = 0;
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    (void)d;
}
