/* egl.c -- Switch Mesa compatibility hooks
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

#define FAKE_PBUFFER_MAGIC 0x50425546u /* "PBUF" */

typedef struct FakePbuffer {
  uint32_t magic;
  EGLint width;
  EGLint height;
} FakePbuffer;

volatile int egl_swap_count = 0;
EGLDisplay last_dpy = EGL_NO_DISPLAY;
EGLSurface main_window_surface = EGL_NO_SURFACE;
EGLContext main_real_context = EGL_NO_CONTEXT;

static __thread int tls_holds_context = 0;
static pthread_t gl_owner;
static int gl_owner_valid = 0;
static pthread_mutex_t gl_owner_mutex = PTHREAD_MUTEX_INITIALIZER;

extern int pthr_is_render_thread(void);

static void release_gl_ownership(void) {
  if (!tls_holds_context)
    return;
  if (last_dpy != EGL_NO_DISPLAY)
    eglMakeCurrent(last_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  pthread_mutex_lock(&gl_owner_mutex);
  if (gl_owner_valid && pthread_equal(gl_owner, pthread_self()))
    gl_owner_valid = 0;
  pthread_mutex_unlock(&gl_owner_mutex);
  tls_holds_context = 0;
}

void egl_gl_ownership_park(void) {
  if (pthr_is_render_thread())
    return;
  release_gl_ownership();
}

void egl_gl_ownership_release(void) {
  release_gl_ownership();
}

int egl_gl_thread_holds_context(void) {
  return tls_holds_context;
}

void egl_gl_shutdown(void) {
  // The core owns its surfaces and contexts, so its registered destructors must
  // run first. Once every core thread has stopped, terminate the display itself
  // so Mesa does not retain EGL/GL state while hbloader chain-loads the launcher.
  release_gl_ownership();

  pthread_mutex_lock(&gl_owner_mutex);
  const EGLDisplay dpy = last_dpy;
  pthread_mutex_unlock(&gl_owner_mutex);

  if (dpy == EGL_NO_DISPLAY)
    return;

  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglTerminate(dpy);
  eglReleaseThread();

  pthread_mutex_lock(&gl_owner_mutex);
  gl_owner_valid = 0;
  pthread_mutex_unlock(&gl_owner_mutex);
  tls_holds_context = 0;
  main_real_context = EGL_NO_CONTEXT;
  main_window_surface = EGL_NO_SURFACE;
  last_dpy = EGL_NO_DISPLAY;
}

static EGLint attrib_value(const EGLint *attribs, EGLint key, EGLint fallback) {
  if (!attribs)
    return fallback;
  for (int i = 0; attribs[i] != EGL_NONE; i += 2)
    if (attribs[i] == key)
      return attribs[i + 1];
  return fallback;
}

static FakePbuffer *fake_pbuffer_from_surface(EGLSurface surface) {
  if (surface == EGL_NO_SURFACE)
    return NULL;
  FakePbuffer *p = (FakePbuffer *)surface;
  return (p->magic == FAKE_PBUFFER_MAGIC) ? p : NULL;
}

EGLDisplay eglGetDisplayHook(EGLNativeDisplayType display_id) {
  EGLDisplay dpy = eglGetDisplay(display_id);

  return dpy;
}

EGLBoolean eglInitializeHook(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  EGLBoolean r = eglInitialize(dpy, major, minor);

  return r;
}

EGLBoolean eglBindAPIHook(EGLenum api) {
  EGLBoolean r = eglBindAPI(api);

  return r;
}

const char *eglQueryStringHook(EGLDisplay dpy, EGLint name) {
  const char *s = eglQueryString(dpy, name);

  return s;
}

EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config) {
  // Switch Mesa has no pbuffer config and the core requires ES3.
  EGLint attribs[64];
  int n = 0;
  int saw_renderable = 0;
  if (attrib_list) {
    while (attrib_list[n] != EGL_NONE && n < 60) {
      attribs[n] = attrib_list[n];
      attribs[n + 1] = attrib_list[n + 1];
      if (attribs[n] == EGL_SURFACE_TYPE && (attribs[n + 1] & EGL_PBUFFER_BIT)) {

        attribs[n + 1] &= ~EGL_PBUFFER_BIT;
      }
      if (attribs[n] == EGL_RENDERABLE_TYPE) {
        attribs[n + 1] |= EGL_OPENGL_ES3_BIT;
        saw_renderable = 1;
      }
      n += 2;
    }
  }
  if (!saw_renderable && n < 60) {
    attribs[n++] = EGL_RENDERABLE_TYPE;
    attribs[n++] = EGL_OPENGL_ES3_BIT;
  }
  attribs[n] = EGL_NONE;

  EGLBoolean r = eglChooseConfig(dpy, attribs, configs, config_size, num_config);

  if (r == EGL_TRUE && num_config && *num_config > 0)
    return r;

  static const EGLint relaxed[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  static const EGLint minimal[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_NONE
  };
  const EGLint *fallbacks[] = { relaxed, minimal };
  for (unsigned i = 0; i < 2; i++) {
    r = eglChooseConfig(dpy, fallbacks[i], configs, config_size, num_config);

    if (r == EGL_TRUE && num_config && *num_config > 0)
      return r;
  }
  return r;
}

EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list) {
  EGLContext ctx = eglCreateContext(dpy, config, share_context, attrib_list);
  if (ctx != EGL_NO_CONTEXT)
    main_real_context = ctx;

  return ctx;
}

EGLSurface eglCreateWindowSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                      EGLNativeWindowType win, const EGLint *attrib_list) {
  EGLSurface s = eglCreateWindowSurface(dpy, config, win, attrib_list);
  if (s != EGL_NO_SURFACE) {
    last_dpy = dpy;
    main_window_surface = s;
  }

  return s;
}

EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list) {
  EGLSurface s = eglCreatePbufferSurface(dpy, config, attrib_list);
  if (s != EGL_NO_SURFACE) {

    return s;
  }
  eglGetError();

  // The core uses the stand-in only for surfaceless operation.
  FakePbuffer *fake = (FakePbuffer *)calloc(1, sizeof(*fake));
  if (!fake) {

    return EGL_NO_SURFACE;
  }
  fake->magic = FAKE_PBUFFER_MAGIC;
  fake->width = attrib_value(attrib_list, EGL_WIDTH, 1);
  fake->height = attrib_value(attrib_list, EGL_HEIGHT, 1);
  if (fake->width <= 0) fake->width = 1;
  if (fake->height <= 0) fake->height = 1;

  return (EGLSurface)fake;
}

EGLBoolean eglDestroySurfaceHook(EGLDisplay dpy, EGLSurface surface) {
  FakePbuffer *fake = fake_pbuffer_from_surface(surface);
  if (fake) {
    free(fake);
    return EGL_TRUE;
  }
  if (surface == main_window_surface)
    main_window_surface = EGL_NO_SURFACE;
  EGLBoolean r = eglDestroySurface(dpy, surface);
  return r;
}

EGLBoolean eglDestroyContextHook(EGLDisplay dpy, EGLContext ctx) {
  if (ctx == main_real_context)
    main_real_context = EGL_NO_CONTEXT;
  EGLBoolean r = eglDestroyContext(dpy, ctx);
  return r;
}

EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  if (ctx == EGL_NO_CONTEXT || ctx == NULL) {
    EGLBoolean r = eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    last_dpy = dpy;
    pthread_mutex_lock(&gl_owner_mutex);
    if (gl_owner_valid && pthread_equal(gl_owner, pthread_self()))
      gl_owner_valid = 0;
    pthread_mutex_unlock(&gl_owner_mutex);
    tls_holds_context = 0;
    return r;
  }

  // Fake pbuffers use the window surface or a surfaceless context.
  FakePbuffer *fake_draw = fake_pbuffer_from_surface(draw);
  FakePbuffer *fake_read = fake_pbuffer_from_surface(read);
  EGLSurface real_draw = draw;
  EGLSurface real_read = read;
  if (fake_draw || fake_read) {
    real_draw = (main_window_surface != EGL_NO_SURFACE) ? main_window_surface : EGL_NO_SURFACE;
    real_read = real_draw;
  } else if (draw != EGL_NO_SURFACE) {
    main_window_surface = draw;
    main_real_context = ctx;
  }

  last_dpy = dpy;
  EGLBoolean r = eglMakeCurrent(dpy, real_draw, real_read, ctx);
  if (r == EGL_TRUE) {
    pthread_mutex_lock(&gl_owner_mutex);
    gl_owner = pthread_self();
    gl_owner_valid = 1;
    pthread_mutex_unlock(&gl_owner_mutex);
    tls_holds_context = 1;
  }
  return r;
}

EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  FakePbuffer *fake = fake_pbuffer_from_surface(surface);
  if (fake) {
    EGLint v = 0;
    switch (attribute) {
      case EGL_WIDTH:  v = fake->width;  break;
      case EGL_HEIGHT: v = fake->height; break;
      default:         v = 0;            break;
    }
    if (value) *value = v;

    return EGL_TRUE;
  }

  EGLBoolean r = eglQuerySurface(dpy, surface, attribute, value);
  // Mesa can report 0x0 before the first buffer is acquired.
  if (r == EGL_TRUE && value && *value == 0) {
    if (attribute == EGL_WIDTH) {

      *value = screen_width;
      return r;
    }
    if (attribute == EGL_HEIGHT) {

      *value = screen_height;
      return r;
    }
  }

  return r;
}

EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval) {
  EGLBoolean r = eglSwapInterval(dpy, interval);

  return r;
}

EGLBoolean eglSwapBuffersHook(EGLDisplay dpy, EGLSurface surface) {
  ++egl_swap_count;
  if (fake_pbuffer_from_surface(surface))
    return EGL_TRUE;

  return eglSwapBuffers(dpy, surface);
}

void *eglGetProcAddressHook(const char *name) {
  void *p = (void *)eglGetProcAddress(name);
  if (name && p && !strcmp(name, "glGetString"))
    p = (void *)glGetStringHook;

  return p;
}

const GLubyte *glGetStringHook(GLenum name) {
  const GLubyte *s = glGetString(name);
  if (s) {
    return s;
  }

  switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"NVIDIA";
    case GL_RENDERER: return (const GLubyte *)"NVIDIA Tegra";
    case GL_VERSION:  return (const GLubyte *)"OpenGL ES 3.2";
    case 0x8B8C:      return (const GLubyte *)"OpenGL ES GLSL ES 3.20"; // GL_SHADING_LANGUAGE_VERSION
    case GL_EXTENSIONS:
      return (const GLubyte *)"GL_OES_surfaceless_context "
                              "GL_OES_depth24 GL_OES_packed_depth_stencil";
    default:          return (const GLubyte *)"";
  }
}
