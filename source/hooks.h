/* hooks.h -- EGL hooks and core patches
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HOOKS_H__
#define __HOOKS_H__

#include <EGL/egl.h>
#include <GLES2/gl2.h>

void patch_game(void);

typedef enum {
  CORE_VER_UNKNOWN = 0,
  CORE_VER_V22N_4248,
  CORE_VER_V22N_3668
} CoreVersion;
extern CoreVersion g_core_version;
int core_is_3668(void);
int core_shutdown_achievements(void);
int core_shutdown_mtgs(void);

void egl_gl_ownership_park(void);
void egl_gl_ownership_release(void);
int  egl_gl_thread_holds_context(void);

// EGL hooks (hooks/egl.c) -- registered in imports.c in place of the real eglXxx
EGLDisplay eglGetDisplayHook(EGLNativeDisplayType display_id);
EGLBoolean eglInitializeHook(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglBindAPIHook(EGLenum api);
const char *eglQueryStringHook(EGLDisplay dpy, EGLint name);
EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list);
EGLSurface eglCreateWindowSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                      EGLNativeWindowType win, const EGLint *attrib_list);
EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config,
                                       const EGLint *attrib_list);
EGLBoolean eglDestroySurfaceHook(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglDestroyContextHook(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
EGLBoolean eglSwapBuffersHook(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval);
const GLubyte *glGetStringHook(GLenum name);
void *eglGetProcAddressHook(const char *name);

#endif
