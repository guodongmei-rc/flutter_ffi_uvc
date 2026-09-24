#ifndef FLUTTER_FFI_UVC_GL_BLIT_H
#define FLUTTER_FFI_UVC_GL_BLIT_H

#if defined(__ANDROID__)

#include <android/native_window.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPU blit for the video-encoder input surface: uploads a decoded RGBA frame
 * as a GL texture and draws it into the encoder's ANativeWindow with
 * rotation/flip in texture coordinates, replacing the per-pixel CPU blit
 * (blit_rgba_transform + ANativeWindow_lock, ~8MB transformed writes per
 * frame) on the recording path.
 *
 * Only ever used with the encoder surface, never with Flutter's
 * SurfaceTexture (its producer API may already be CPU-bound, which EGL
 * cannot attach to).
 *
 * Threading: all gl_blit_render calls run on the frame callback thread;
 * gl_blit_destroy runs after that thread has exited. */
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
