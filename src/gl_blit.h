#ifndef FLUTTER_FFI_UVC_GL_BLIT_H
#define FLUTTER_FFI_UVC_GL_BLIT_H

#if defined(__ANDROID__)

#include <android/native_window.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPU blit path: uploads a decoded RGBA frame as a GL texture once and draws
 * it to every attached ANativeWindow (Flutter preview surface, video encoder
 * input surface) with rotation/flip done in texture coordinates. This
 * replaces the per-pixel CPU blit (blit_rgba_transform) which costs ~8MB of
 * transformed writes per surface per frame and blocks on ANativeWindow_lock.
 *
 * Threading: gl_blit_render must be called from a single thread (the frame
 * callback thread). gl_blit_destroy may run on any thread once rendering has
 * stopped (the render thread has exited). */
typedef struct gl_blit gl_blit_t;

gl_blit_t *gl_blit_create(void);
void gl_blit_destroy(gl_blit_t *blit);

/* Renders one frame to up to 4 windows. Returns a bitmask: bit i set means
 * window i failed (caller falls back or counts stats). */
int gl_blit_render(
    gl_blit_t *blit,
    const uint8_t *rgba,
    int src_w,
    int src_h,
    int rot,
    int flip_h,
    int flip_v,
    ANativeWindow *const *windows,
    int n_windows);

#ifdef __cplusplus
}
#endif

#endif  // __ANDROID__
#endif  // FLUTTER_FFI_UVC_GL_BLIT_H
