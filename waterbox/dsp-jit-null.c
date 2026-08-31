/* CHIMERA: the DSP JIT ships as a prebuilt glibc Rust archive, which cannot
 * enter the waterbox. The C interpreter is the engine (use_dsp_jit=false);
 * reaching this stub means the config asked for the impossible.
 */
#include "qemu/osdep.h"
#include "dsp_internal.h"

void dsp_jit_init(DSPState *dsp)
{
    (void)dsp;
    g_assert_not_reached();
}

const DSPOps jit_dsp_ops = { 0 };
