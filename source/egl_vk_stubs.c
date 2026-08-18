/* egl_vk_stubs.c -- the few EGL/GLES symbols NVK does NOT already stub, for the
 * VULKAN build only.
 *
 * The Vulkan (NVK) build does NOT link switch-mesa's libEGL/libGLESv2 (they and
 * NVK both bundle mesa util/nir/compiler object code and can't co-link). But
 * libemucore.so has 19 undefined egl* dynamic imports and our EGL layer
 * (hooks/egl.c) still compiles and calls the raw egl and gl functions. NVK's own
 * rust_switch_stubs already provides most egl* symbols (eglGetDisplay,
 * eglSwapBuffers, ...); this file adds only the handful it does not, so the link
 * resolves. On the Vulkan renderer path the core drives GSDeviceVK and never
 * actually calls these -- they just need to exist and fail benignly. Compiled to
 * nothing in the OpenGL build (real switch-mesa symbols linked instead).
 */
#if defined(USE_VULKAN) && !defined(USE_UNIFIED_MESA)

#include <EGL/egl.h>
#include <GLES2/gl2.h>

EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
  (void)d; (void)s; (void)a; if (v) *v = 0; return EGL_FALSE; }
EGLContext eglGetCurrentContext(void) { return EGL_NO_CONTEXT; }
EGLSurface eglGetCurrentSurface(EGLint rw) { (void)rw; return EGL_NO_SURFACE; }
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)""; }

#endif // USE_VULKAN
