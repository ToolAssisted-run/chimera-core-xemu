/* CHIMERA: the offscreen-GL abstraction the pgraph GL renderer compiles
 * against, rebuilt over glad instead of SDL + epoxy. Same five entry points
 * as upstream's thirdparty/gloffscreen; what stands behind them differs per
 * build:
 *
 *   guest  - there is no context and no driver in the sandbox. The glad
 *            function pointers are filled with generated wrappers that hand
 *            every call to the host through the one callback a guest may
 *            make (the GPU bridge). glo_context_create merely checks the
 *            bridge is installed.
 *   native - a real EGL surfaceless context (the same shape the frontend's
 *            host half uses), so the reference build and the sandbox render
 *            through the same driver and can be compared byte for byte.
 */
#ifndef GLOFFSCREEN_H_
#define GLOFFSCREEN_H_

#include <stdbool.h>
#include <glad/gl.h>

struct _GloContext;
typedef struct _GloContext GloContext;

void glo_set_current(GloContext *context);
/* re-bind the native EGL context if the calling thread changed (no-op in the
 * sandbox: green threads share one host thread and the host owns the
 * context) */
void glo_ensure_current(void);
/* release whatever context the calling thread holds (the frame boundary's
 * handoff point: GL work migrates between the vCPU and main threads there) */
void glo_release_current(void);
bool glo_check_extension(const char *ext_name);
GloContext *glo_context_create(void);
void glo_context_destroy(GloContext *context);

void glo_readpixels(GLenum gl_format, GLenum gl_type,
                    unsigned int bytes_per_pixel, unsigned int stride,
                    unsigned int width, unsigned int height, bool vflip,
                    void *data);

/* Not part of upstream gloffscreen: what the driver asks before choosing a
 * renderer, and what it prints when a GPU is on offer. */
bool chimera_gl_available(void);
const char *chimera_gl_describe(void);

/* Forget every binding the virtual contexts remember. Called when the GL
 * objects are about to be rebuilt in a different real context: the names those
 * bindings hold were handed out by a context that is gone, so binding them
 * again would name nothing. Memory only, no GL. */
void chimera_glo_reset_bindings(void);

/* opaque context handoff for the inline pfifo service (pfifo.c) */
void *chimera_glo_current(void);
void *chimera_glo_render(void);
void chimera_glo_rebind(void *ctx);

#endif /* GLOFFSCREEN_H_ */
