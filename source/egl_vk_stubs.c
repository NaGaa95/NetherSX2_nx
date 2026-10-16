/* egl_vk_stubs.c -- the EGL/GLES symbols the VULKAN build has nothing else to
 * link against.
 *
 * The Vulkan (NVK) build does NOT link switch-mesa's libEGL/libGLESv2: they and
 * NVK both bundle mesa util/nir/compiler object code and cannot co-link. But
 * libemucore.so has undefined egl* dynamic imports and our EGL layer
 * (hooks/egl.c) still compiles and calls the raw egl functions, so every one of
 * them has to exist for the link to resolve.
 *
 * The nxvk portlib package (berlint-hub/nxvk, libnvk.a + libnvk_support.a)
 * exports no egl or gl symbol at all -- nm --defined-only over both archives
 * finds 14155 globals, none of them egl* -- whereas the older 23-archive
 * vulkan/lib layout used to carry NVK's rust_switch_stubs, which supplied most
 * of them. This file therefore covers the whole set now, not just a handful.
 *
 * On the Vulkan renderer path the core drives GSDeviceVK and never actually
 * calls these; they only need to exist and fail benignly. Compiled to nothing
 * in the OpenGL build (real switch-mesa symbols linked instead) and in the
 * native Deko3D build (hooks/egl.c is excluded there by #ifndef USE_DEKO3D).
 *
 * Signatures are the Khronos EGL 1.5 ones (EGL-Registry api/EGL/egl.h), which
 * is what <EGL/egl.h> declares; keep them in sync if that header ever changes.
 */
#if defined(USE_VULKAN) && !defined(USE_UNIFIED_MESA)

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stddef.h> /* NULL -- egl.h ho na Switch zarucene nedodava */

/* Already queried by the core before any context exists. */
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
  (void)d; (void)s; (void)a; if (v) *v = 0; return EGL_FALSE; }
EGLContext eglGetCurrentContext(void) { return EGL_NO_CONTEXT; }
EGLSurface eglGetCurrentSurface(EGLint rw) { (void)rw; return EGL_NO_SURFACE; }
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)""; }

/* The rest of the EGL surface hooks/egl.c forwards to. */
EGLBoolean eglBindAPI(EGLenum api) { (void)api;
  return EGL_FALSE; }
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) { (void)dpy; (void)attrib_list; (void)configs; (void)config_size; (void)num_config;
  return EGL_FALSE; }
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list) { (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  return EGL_NO_CONTEXT; }
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) { (void)dpy; (void)config; (void)attrib_list;
  return EGL_NO_SURFACE; }
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list) { (void)dpy; (void)config; (void)win; (void)attrib_list;
  return EGL_NO_SURFACE; }
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) { (void)dpy; (void)ctx;
  return EGL_FALSE; }
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) { (void)dpy; (void)surface;
  return EGL_FALSE; }
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) { (void)dpy; (void)config; (void)attribute; (void)value;
  return EGL_FALSE; }
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) { (void)display_id;
  return EGL_NO_DISPLAY; }
EGLint eglGetError(void) {
  return EGL_SUCCESS; }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) { (void)procname;
  return NULL; }
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) { (void)dpy; (void)major; (void)minor;
  return EGL_FALSE; }
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) { (void)dpy; (void)draw; (void)read; (void)ctx;
  return EGL_FALSE; }
const char * eglQueryString(EGLDisplay dpy, EGLint name) { (void)dpy; (void)name;
  return ""; }
EGLBoolean eglReleaseThread(void) {
  return EGL_FALSE; }
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) { (void)dpy; (void)surface;
  return EGL_FALSE; }
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) { (void)dpy; (void)interval;
  return EGL_FALSE; }
EGLBoolean eglTerminate(EGLDisplay dpy) { (void)dpy;
  return EGL_FALSE; }

#endif // USE_VULKAN
