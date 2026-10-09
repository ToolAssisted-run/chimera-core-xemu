/* CHIMERA: the generated guest half (gl-bridge-guest.cpp) is C++ and its
 * installer has C++ linkage; the gloffscreen shim is C. This is the whole
 * bridge between the two. */
#include <glad/gl.h>
#include <cstdint>

#include "gl-bridge.h"
#include "gl-bridge-ops.h"

bool chimera_gl_install(chimera_gl_bridge_fn bridge);

extern "C" bool chimera_gl_install_c(uint64_t addr)
{
    return chimera_gl_install((chimera_gl_bridge_fn)(uintptr_t)addr);
}

/* chimera_gl_context_id - which real context the calls are landing on, 0 when
 * it cannot be told; see pgraph_gl_check_context - used to be answered here,
 * from a copy of the bridge pointer kept for it. The generated guest half has
 * answered it itself since miniBox's generator learned the question, and this
 * core's copy of that half predated it; regenerated (2026-10-09, for the GL
 * strings), it is the one definition. */
