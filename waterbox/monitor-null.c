/* CHIMERA: the headless APU monitor. The DSP mixes EP frames into
 * monitor.frame_buf exactly as upstream; instead of an SDL device the
 * finished frames land in an accumulator the waterbox driver drains once
 * per video frame (GetAudio/GetAudioSampleCount). The samples are machine
 * output - a pure function of machine state - so the audio gate leg can
 * compare them byte for byte between native and sandbox.
 */
#include "qemu/osdep.h"
#include "apu_int.h"

/* ~3.2 EP frames (256 stereo pairs each) arrive per 60Hz video frame; the
 * ring never needs more than a handful, sized generously for PAL and for
 * frames the frontend skips draining. */
#define CHIMERA_AUDIO_CAP 16384

int16_t chimera_audio_buf[CHIMERA_AUDIO_CAP * 2];
int chimera_audio_count; /* stereo pairs accumulated since the last drain */

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
    /* the EP produces one 256-pair frame every 8th sub-frame (upstream's
     * cadence, kept exactly) */
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    int pairs = ARRAY_SIZE(d->monitor.frame_buf);
    if (chimera_audio_count + pairs > CHIMERA_AUDIO_CAP) {
        chimera_audio_count = 0; /* undrained: drop the stale backlog */
    }
    memcpy(&chimera_audio_buf[chimera_audio_count * 2], d->monitor.frame_buf,
           sizeof(d->monitor.frame_buf));
    chimera_audio_count += pairs;

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
