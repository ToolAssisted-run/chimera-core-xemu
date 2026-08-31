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
