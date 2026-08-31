/* CHIMERA: what stands behind gloffscreen.h in a build with no SDL.
 *
 * guest (CHIMERA_GUEST): there is no driver in the sandbox. The generated
 * bridge wrappers (gl-bridge-guest.cpp) fill glad's function pointers and
 * every call crosses to the host through the sandbox's one callback. A
 * "context" is the installed bridge; current-ness is the host's business.
 *
 * native: a real EGL surfaceless context with a pbuffer, the same shape the
 * frontend's bridge host uses - so the reference build and the sandbox render
 * through the same driver on the same machine, which is what lets the gate
 * compare their machines byte for byte. Renderer work reaches GL from more
 * than one thread (the vCPU thread runs virtual-timer work, the main thread
 * runs the frame loop), always serialized by the BQL; the context follows the
 * calling thread in glo_ensure_current().
 */
#include "gloffscreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CHIMERA_GUEST

struct _GloContext { int created; };

#include <stdint.h>
#include "gl-bridge.h" /* miniBox source/gl: the shared contract */

bool chimera_gl_install_c(uint64_t addr); /* gl-bridge-cshim.cpp */

static bool g_bridged;

bool chimera_gl_try_bridge(uint64_t addr)
{
    if (!addr) {
        return false;
    }
    g_bridged = chimera_gl_install_c(addr);
    return g_bridged;
}

bool chimera_gl_available(void)
{
    return g_bridged;
}

/* The host owns the one real context; in the sandbox a GloContext is only
 * a token so display.c's render/display switching still sequences, and
 * chimera_glo_current round-trips for the inline service helper. */
static GloContext guest_contexts[4];
static int guest_context_count;
static GloContext *guest_current;

GloContext *glo_context_create(void)
{
    if (!g_bridged) {
        fprintf(stderr, "chimera gl: context asked for with no bridge\n");
        abort();
    }
    if (guest_context_count >= 4) {
        abort();
    }
    guest_contexts[guest_context_count].created = 1;
    return &guest_contexts[guest_context_count++];
}

void glo_set_current(GloContext *context)
{
    guest_current = context;
}

void glo_ensure_current(void)
{
}

void *chimera_glo_current(void)
{
    return guest_current;
}

void chimera_glo_rebind(void *ctx)
{
    guest_current = ctx;
}

void glo_release_current(void)
{
}

extern GloContext *g_nv2a_context_render;

void *chimera_glo_render(void)
{
    return g_nv2a_context_render;
}

void glo_context_destroy(GloContext *context)
{
    (void)context;
}

#else /* native reference */

#include <glad/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pthread.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

/* The renderer wants what upstream has: a render context and a display
 * context sharing objects, each current on whichever thread does that kind
 * of work. EGL forbids binding a context that is current on another thread,
 * so the driver RELEASES the render context at every frame boundary (the
 * vCPU thread lets go before the machine stops, the main thread lets go
 * before it starts) - the BQL guarantees no two threads touch GL at once,
 * and the boundary is where the work migrates. */
#define MAX_CONTEXTS 4

struct _GloContext {
    int created;
    EGLContext egl;
    pthread_t holder;
    bool held;
};

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLConfig s_config;
static struct _GloContext s_contexts[MAX_CONTEXTS];
static int s_context_count;
static __thread GloContext *s_thread_current;
static bool s_ready;

static GLADapiproc loader(const char *name)
{
    return (GLADapiproc)eglGetProcAddress(name);
}

static int create_display(void)
{
    s_display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                      EGL_DEFAULT_DISPLAY, NULL);
    if (s_display == EGL_NO_DISPLAY) {
        return -1;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(s_display, &major, &minor)) {
        return -1;
    }
    static const EGLint config_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLint count = 0;
    if (!eglChooseConfig(s_display, config_attr, &s_config, 1, &count) ||
        count < 1) {
        return -1;
    }
    /* framebuffer 0 has to exist or every draw is discarded as incomplete */
    static const EGLint pbuffer_attr[] = { EGL_WIDTH, 1024, EGL_HEIGHT, 1024,
                                           EGL_NONE };
    eglBindAPI(EGL_OPENGL_API);
    s_surface = eglCreatePbufferSurface(s_display, s_config, pbuffer_attr);
    if (s_surface == EGL_NO_SURFACE) {
        return -1;
    }
    return 0;
}

static EGLContext create_egl_context(EGLContext share)
{
    /* the renderer is written against GL 4.0 core, same as upstream asks
     * SDL for */
    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    return eglCreateContext(s_display, s_config, share, ctx_attr);
}

bool chimera_gl_available(void)
{
    static int tried;
    if (!tried) {
        tried = 1;
        if (create_display() != 0) {
            return false;
        }
        /* a bootstrap context, to load entry points from the driver */
        EGLContext boot = create_egl_context(EGL_NO_CONTEXT);
        if (boot == EGL_NO_CONTEXT ||
            !eglMakeCurrent(s_display, s_surface, s_surface, boot)) {
            return false;
        }
        if (gladLoadGL(loader) == 0) {
            return false;
        }
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        s_contexts[0].created = 1;
        s_contexts[0].egl = boot;
        s_context_count = 1;
        s_ready = true;
    }
    return s_ready;
}

GloContext *glo_context_create(void)
{
    if (!chimera_gl_available()) {
        fprintf(stderr, "chimera gl: no EGL context\n");
        abort();
    }
    /* the first created context is the bootstrap one; later ones share
     * objects with it, exactly like upstream's shared SDL contexts */
    for (int i = 0; i < s_context_count; i++) {
        if (!s_contexts[i].created) {
            abort();
        }
    }
    if (s_context_count == 1 && !s_contexts[0].held) {
        /* hand out the bootstrap context first */
        GloContext *c = &s_contexts[0];
        glo_set_current(c);
        return c;
    }
    if (s_context_count >= MAX_CONTEXTS) {
        abort();
    }
    GloContext *c = &s_contexts[s_context_count++];
    c->egl = create_egl_context(s_contexts[0].egl);
    if (c->egl == EGL_NO_CONTEXT) {
        fprintf(stderr, "chimera gl: shared context creation failed\n");
        abort();
    }
    c->created = 1;
    glo_set_current(c);
    return c;
}

void glo_set_current(GloContext *context)
{
    if (!s_ready) {
        return;
    }
    GloContext *cur = s_thread_current;
    if (cur == context) {
        return;
    }
    if (context == NULL) {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (cur != NULL) {
            cur->held = false;
        }
        s_thread_current = NULL;
        return;
    }
    if (context->held && !pthread_equal(context->holder, pthread_self())) {
        /* the release protocol failed somewhere; say so and fall through so
         * the EGL error is at least attributable */
        fprintf(stderr, "chimera gl: context %p still held by another "
                "thread\n", (void *)context);
    }
    if (!eglMakeCurrent(s_display, s_surface, s_surface, context->egl)) {
        fprintf(stderr, "chimera gl: eglMakeCurrent failed (0x%x)\n",
                eglGetError());
        return;
    }
    if (cur != NULL) {
        cur->held = false;
    }
    context->held = true;
    context->holder = pthread_self();
    s_thread_current = context;
}

/* The render context is what pfifo/pgraph work runs against; bind it if this
 * thread has nothing. (The display path binds its own context explicitly.) */
extern GloContext *g_nv2a_context_render;

void glo_ensure_current(void)
{
    if (!s_ready || s_thread_current != NULL) {
        return;
    }
    if (g_nv2a_context_render != NULL) {
        glo_set_current(g_nv2a_context_render);
    }
}

/* The frame boundary's release: whatever this thread holds goes back, so the
 * other thread may take the render context when the work migrates. */
void glo_release_current(void)
{
    if (s_ready && s_thread_current != NULL) {
        glo_set_current(NULL);
    }
}

void *chimera_glo_current(void)
{
    return s_thread_current;
}

void *chimera_glo_render(void)
{
    return g_nv2a_context_render;
}

void chimera_glo_rebind(void *ctx)
{
    glo_set_current((GloContext *)ctx);
}

void glo_context_destroy(GloContext *context)
{
    (void)context;
}

#endif /* CHIMERA_GUEST */

/* ---- shared: pure GL, identical in both builds ------------------------- */

const char *chimera_gl_describe(void)
{
    /* glGetString crosses the bridge into a guest buffer in the sandbox and
     * asks the driver directly in the native build - either way this is what
     * the DRIVER said, which is the point of printing it. */
    static char buf[256];
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    snprintf(buf, sizeof buf, "%s on %s", version ? version : "?",
             renderer ? renderer : "?");
    return buf;
}

bool glo_check_extension(const char *ext_name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (ext != NULL && strcmp(ext, ext_name) == 0) {
            return true;
        }
    }
    return false;
}

void glo_readpixels(GLenum gl_format, GLenum gl_type,
                    unsigned int bytes_per_pixel, unsigned int stride,
                    unsigned int width, unsigned int height, bool vflip,
                    void *data)
{
    /* upstream's implementation, over glad */
    int rl, pa;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rl);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pa);
    glPixelStorei(GL_PACK_ROW_LENGTH, stride / bytes_per_pixel);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glReadPixels(0, 0, width, height, gl_format, gl_type, data);

    if (vflip) {
        GLubyte *b = (GLubyte *)data;
        GLubyte *c = &((GLubyte *)data)[stride * (height - 1)];
        GLubyte *tmp = (GLubyte *)malloc(width * bytes_per_pixel);
        for (unsigned int irow = 0; irow < height / 2; irow++) {
            memcpy(tmp, b, width * bytes_per_pixel);
            memcpy(b, c, width * bytes_per_pixel);
            memcpy(c, tmp, width * bytes_per_pixel);
            b += stride;
            c -= stride;
        }
        free(tmp);
    }

    glPixelStorei(GL_PACK_ROW_LENGTH, rl);
    glPixelStorei(GL_PACK_ALIGNMENT, pa);
}
