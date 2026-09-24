#include "gl_blit.h"

#if defined(__ANDROID__)

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <stdlib.h>

#define GL_BLIT_TAG "flutter_ffi_uvc"
#define GL_BLIT_LOGI(...) \
  __android_log_print(ANDROID_LOG_INFO, GL_BLIT_TAG, "@@@@GL_BLIT/I " __VA_ARGS__)
#define GL_BLIT_LOGW(...) \
  __android_log_print(ANDROID_LOG_WARN, GL_BLIT_TAG, "@@@@GL_BLIT/W " __VA_ARGS__)

#define GL_BLIT_MAX_WINDOWS 2

typedef struct {
  ANativeWindow *window;
  EGLSurface surface;
  /* Set when this window rejects EGL: reported as failure so the caller
   * keeps using the CPU blit for it, without retrying EGL every frame. */
  int unsupported;
} gl_blit_target_t;

struct gl_blit {
  EGLDisplay display;
  EGLContext context;
  EGLConfig config;
  GLuint program;
  GLuint texture;
  GLint attr_pos;
  GLint attr_uv;
  GLint uniform_tex;
  int tex_w;
  int tex_h;
  gl_blit_target_t targets[GL_BLIT_MAX_WINDOWS];
};

static const char kVertexShader[] =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char kFragmentShader[] =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(uTex, vUV);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static EGLSurface make_scratch(gl_blit_t *blit) {
  const EGLint attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  return eglCreatePbufferSurface(blit->display, blit->config, attrs);
}

gl_blit_t *gl_blit_create(void) {
  gl_blit_t *blit = calloc(1, sizeof(*blit));
  if (blit == NULL) {
    return NULL;
  }

  blit->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (blit->display == EGL_NO_DISPLAY || eglInitialize(blit->display, NULL, NULL) != EGL_TRUE) {
    GL_BLIT_LOGW("eglInitialize failed");
    free(blit);
    return NULL;
  }

  const EGLint config_attrs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLint num_configs = 0;
  if (eglChooseConfig(blit->display, config_attrs, &blit->config, 1, &num_configs) != EGL_TRUE ||
      num_configs < 1) {
    GL_BLIT_LOGW("eglChooseConfig failed");
    eglTerminate(blit->display);
    free(blit);
    return NULL;
  }

  const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  blit->context = eglCreateContext(blit->display, blit->config, EGL_NO_CONTEXT, context_attrs);
  if (blit->context == EGL_NO_CONTEXT) {
    GL_BLIT_LOGW("eglCreateContext failed");
    eglTerminate(blit->display);
    free(blit);
    return NULL;
  }

  /* GL calls need a current context; bind a scratch pbuffer during setup. */
  EGLSurface scratch = make_scratch(blit);
  if (scratch == EGL_NO_SURFACE ||
      eglMakeCurrent(blit->display, scratch, scratch, blit->context) != EGL_TRUE) {
    GL_BLIT_LOGW("failed to bind scratch context");
    gl_blit_destroy(blit);
    return NULL;
  }

  GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
  GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
  blit->program = glCreateProgram();
  if (vertex == 0 || fragment == 0 || blit->program == 0) {
    gl_blit_destroy(blit);
    return NULL;
  }
  glAttachShader(blit->program, vertex);
  glAttachShader(blit->program, fragment);
  glLinkProgram(blit->program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(blit->program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GL_BLIT_LOGW("program link failed");
    gl_blit_destroy(blit);
    return NULL;
  }
  blit->attr_pos = glGetAttribLocation(blit->program, "aPos");
  blit->attr_uv = glGetAttribLocation(blit->program, "aUV");
  blit->uniform_tex = glGetUniformLocation(blit->program, "uTex");

  glGenTextures(1, &blit->texture);
  glBindTexture(GL_TEXTURE_2D, blit->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  eglMakeCurrent(blit->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(blit->display, scratch);

  GL_BLIT_LOGI("GL blit renderer created");
  return blit;
}

void gl_blit_destroy(gl_blit_t *blit) {
  if (blit == NULL) {
    return;
  }
  if (blit->display != EGL_NO_DISPLAY) {
    EGLSurface scratch = make_scratch(blit);
    if (scratch != EGL_NO_SURFACE &&
        eglMakeCurrent(blit->display, scratch, scratch, blit->context) == EGL_TRUE) {
      if (blit->texture != 0) {
        glDeleteTextures(1, &blit->texture);
      }
      if (blit->program != 0) {
        glDeleteProgram(blit->program);
      }
      eglMakeCurrent(blit->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroySurface(blit->display, scratch);
    }
    for (int i = 0; i < GL_BLIT_MAX_WINDOWS; i++) {
      if (blit->targets[i].surface != NULL && blit->targets[i].surface != EGL_NO_SURFACE) {
        eglDestroySurface(blit->display, blit->targets[i].surface);
      }
    }
    if (blit->context != EGL_NO_CONTEXT) {
      eglDestroyContext(blit->display, blit->context);
    }
    eglTerminate(blit->display);
  }
  free(blit);
}

/* Maps a destination pixel (dr, dc) through flips + rotation to source UV,
 * mirroring blit_rgba_transform's CPU mapping. */
static void dest_to_uv(
    int dr, int dc, int out_w, int out_h, int src_w, int src_h,
    int rot, int fh, int fv, float *u, float *v) {
  const int eff_dr = fv ? (out_h - 1 - dr) : dr;
  const int eff_dc = fh ? (out_w - 1 - dc) : dc;
  int sr, sc;
  switch (rot) {
    case 90:  sr = (src_h - 1) - eff_dc; sc = eff_dr;               break;
    case 180: sr = (src_h - 1) - eff_dr; sc = (src_w - 1) - eff_dc; break;
    case 270: sr = eff_dc;               sc = (src_w - 1) - eff_dr; break;
    default:  sr = eff_dr;               sc = eff_dc;               break;
  }
  *u = (float)sc / (float)src_w;
  *v = (float)sr / (float)src_h;
}

int gl_blit_render(
    gl_blit_t *blit,
    ANativeWindow *window,
    const uint8_t *rgba,
    int src_w,
    int src_h,
    int rot,
    int flip_h,
    int flip_v) {
  if (blit == NULL || window == NULL || rgba == NULL || src_w <= 0 || src_h <= 0) {
    return 0;
  }

  gl_blit_target_t *target = NULL;
  gl_blit_target_t *free_slot = NULL;
  for (int i = 0; i < GL_BLIT_MAX_WINDOWS; i++) {
    if (blit->targets[i].window == window) {
      target = &blit->targets[i];
      break;
    }
    if (blit->targets[i].window == NULL && free_slot == NULL) {
      free_slot = &blit->targets[i];
    }
  }
  if (target == NULL) {
    if (free_slot == NULL) {
      return 0;
    }
    target = free_slot;
    target->window = window;
    target->surface = EGL_NO_SURFACE;
  }
  if (target->unsupported) {
    return 0;
  }

  if (target->surface == EGL_NO_SURFACE || target->surface == NULL) {
    /* Note: no ANativeWindow_setBuffersGeometry here — on some vendor builds
     * it binds the CPU producer API to the window, which then blocks
     * eglCreateWindowSurface. Buffer size is owned by the Kotlin side via
     * SurfaceTexture.setDefaultBufferSize / the codec configuration. */
    target->surface = eglCreateWindowSurface(blit->display, blit->config, window, NULL);
    if (target->surface == EGL_NO_SURFACE) {
      GL_BLIT_LOGW("eglCreateWindowSurface failed; window stays on CPU blit");
      target->unsupported = 1;
      return 0;
    }
  }

  if (eglMakeCurrent(blit->display, target->surface, target->surface, blit->context) != EGL_TRUE) {
    eglDestroySurface(blit->display, target->surface);
    target->surface = EGL_NO_SURFACE;
    target->unsupported = 1;
    return 0;
  }

  const int out_w = (rot == 90 || rot == 270) ? src_h : src_w;
  const int out_h = (rot == 90 || rot == 270) ? src_w : src_h;

  float uvs[8];
  dest_to_uv(0, 0, out_w, out_h, src_w, src_h, rot, flip_h, flip_v, &uvs[0], &uvs[1]);
  dest_to_uv(0, out_w - 1, out_w, out_h, src_w, src_h, rot, flip_h, flip_v, &uvs[2], &uvs[3]);
  dest_to_uv(out_h - 1, 0, out_w, out_h, src_w, src_h, rot, flip_h, flip_v, &uvs[4], &uvs[5]);
  dest_to_uv(out_h - 1, out_w - 1, out_w, out_h, src_w, src_h, rot, flip_h, flip_v, &uvs[6], &uvs[7]);
  static const float kPositions[8] = {
      -1.0f, 1.0f,
      1.0f, 1.0f,
      -1.0f, -1.0f,
      1.0f, -1.0f,
  };

  glBindTexture(GL_TEXTURE_2D, blit->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (blit->tex_w != src_w || blit->tex_h != src_h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, src_w, src_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    blit->tex_w = src_w;
    blit->tex_h = src_h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  }

  /* Letterbox: keep the output aspect ratio inside whatever buffer size the
   * window currently has (rotation changes out_w/out_h independently of the
   * buffer geometry). */
  int buf_w = ANativeWindow_getWidth(window);
  int buf_h = ANativeWindow_getHeight(window);
  if (buf_w <= 0) buf_w = out_w;
  if (buf_h <= 0) buf_h = out_h;
  int vp_x = 0, vp_y = 0, vp_w = buf_w, vp_h = buf_h;
  const float buf_aspect = (float)buf_w / (float)buf_h;
  const float out_aspect = (float)out_w / (float)out_h;
  if (buf_aspect > out_aspect) {
    vp_w = (int)(buf_h * out_aspect);
    vp_x = (buf_w - vp_w) / 2;
  } else if (out_aspect > buf_aspect) {
    vp_h = (int)(buf_w / out_aspect);
    vp_y = (buf_h - vp_h) / 2;
  }
  glViewport(vp_x, vp_y, vp_w, vp_h);

  glUseProgram(blit->program);
  glUniform1i(blit->uniform_tex, 0);
  glVertexAttribPointer(blit->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, kPositions);
  glEnableVertexAttribArray(blit->attr_pos);
  glVertexAttribPointer(blit->attr_uv, 2, GL_FLOAT, GL_FALSE, 0, uvs);
  glEnableVertexAttribArray(blit->attr_uv);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  if (eglSwapBuffers(blit->display, target->surface) != EGL_TRUE) {
    GL_BLIT_LOGW("eglSwapBuffers failed err=0x%x", eglGetError());
    eglDestroySurface(blit->display, target->surface);
    target->surface = EGL_NO_SURFACE;
    return 0;
  }
  return 1;
}

#endif  // __ANDROID__
