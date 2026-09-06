/* CHIMERA: the generated guest half (gl-bridge-guest.cpp) is C++ and its
 * installer has C++ linkage; the gloffscreen shim is C. This is the whole
 * bridge between the two. */
#include <glad/gl.h>
#include <cstdint>

#include "gl-bridge.h"
#include "gl-bridge-ops.h"

bool chimera_gl_install(chimera_gl_bridge_fn bridge);

/* Kept so the renderer can ask which real context the calls are landing on
 * (GL_OP_CONTEXT_ID); the generated wrappers keep their own copy for GL. */
static chimera_gl_bridge_fn g_bridge_cshim;

extern "C" bool chimera_gl_install_c(uint64_t addr)
{
    g_bridge_cshim = (chimera_gl_bridge_fn)(uintptr_t)addr;
    return chimera_gl_install((chimera_gl_bridge_fn)(uintptr_t)addr);
}

/* Which real context the calls are landing on; 0 when it cannot be told (no
 * bridge, or a host older than the question). See pgraph_gl_check_context. */
extern "C" uint64_t chimera_gl_context_id(void)
{
    return g_bridge_cshim ? g_bridge_cshim(GL_OP_CONTEXT_ID, 0, 0, 0, 0, 0) : 0;
}
