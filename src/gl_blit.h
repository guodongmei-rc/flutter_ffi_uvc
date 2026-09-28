#ifndef FLUTTER_FFI_UVC_GL_BLIT_H
#define FLUTTER_FFI_UVC_GL_BLIT_H

#if defined(__ANDROID__)

#include <android/native_window.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPU blit: uploads a decoded RGBA frame as a GL texture and draws it into
 * an ANativeWindow with rotation/flip in texture coordinates, replacing the
 * per-pixel CPU blit (blit_rgba_transform + ANativeWindow_lock, ~8MB
 * transformed writes per frame at 1080p). A window that rejects EGL is
 * marked unsupported and the caller falls back to the CPU blit for it.
 *
 * Two independent instances exist — one for the preview surface, one for
 * the recording (encoder input) surface — but BOTH are driven from the
 * single unified render thread: only one thread ever touches EGL, and
 * encoder backpressure (a blocking eglSwapBuffers) stalls neither the
 * frame callback thread nor the preview's renderer instance.
 *
 * Threading: each instance is single-threaded — gl_blit_create,
 * gl_blit_render and gl_blit_destroy of one instance must all run on the
 * same thread (EGL contexts are thread-bound). */
typedef struct gl_blit gl_blit_t;

gl_blit_t *gl_blit_create(void);
void gl_blit_destroy(gl_blit_t *blit);

/* Renders one frame into window. Returns 1 on success, 0 on failure. Once a
 * window has failed EGL setup it is marked unsupported and every later call
 * fails immediately (caller keeps using the CPU blit for it). */
int gl_blit_render(
    gl_blit_t *blit,
    ANativeWindow *window,
    const uint8_t *rgba,
    int src_w,
    int src_h,
    int rot,
    int flip_h,
    int flip_v);

#ifdef __cplusplus
}
#endif

#endif  // __ANDROID__
#endif  // FLUTTER_FFI_UVC_GL_BLIT_H
