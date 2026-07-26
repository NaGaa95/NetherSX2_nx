/* imports.c -- libemucore.so import resolution
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <unistd.h>
#include <locale.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <fenv.h>
#include <setjmp.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <switch.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "pthr.h"
#include "hooks.h"
#include "aaudio.h"
#include "dl_emu.h"
#include "hooks/vk.h"   // Vulkan (NVK) bridge decls -- empty unless -DUSE_VULKAN

// crt/newlib-provided symbols forwarded by address
extern uintptr_t __stack_chk_fail;
int *__errno(void);

// BSD/bionic legacy `_ctype_`: the imported OBJECT is itself a pointer to a
// table where [0] is the EOF guard and [c+1] holds the mask for char c. Keep the
// pointer as a distinct data symbol; exporting the table address directly makes
// the core load its first eight classification bytes as a pointer.
unsigned char g_ctype_table[1 + 256];
unsigned char *g_ctype_ptr = g_ctype_table;

// stdout/stderr are DATA imports (FILE* variables); point them into fake_sF so
// the stdio shims recognise them. fake_sF is uint8_t[3][0x100] in libc_shim.c.
FILE *stdout_ptr = (FILE *)&fake_sF[1][0];
FILE *stderr_ptr = (FILE *)&fake_sF[2][0];

// ---------------------------------------------------------------------------
// small local shims (the long tail; the heavy lifting is in libc_shim/pthr/egl)
// ---------------------------------------------------------------------------

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}

static int __android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio; (void)tag; (void)text;
  return 0;
}
static int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio; (void)tag; (void)fmt; (void)va;
  return 0;
}

// sleeping/yielding is a GL-handover point (single-context: cheap)
static int nanosleep_park(const struct timespec *req, struct timespec *rem) {
  egl_gl_ownership_park();
  int r = nanosleep(req, rem);
  return r;
}

// pread isn't in libnx/newlib: emulate via save/seek/read/restore.
static ssize_t pread_compat(int fd, void *buf, size_t n, off_t off) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  ssize_t r = read(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

// newlib/libnx gaps -> benign stubs / libnx-backed
static long getpagesize_fake(void) { return 0x1000; }
static int lockf_fake(int fd, int cmd, off_t len) { (void)fd; (void)cmd; (void)len; return 0; }
static int sigemptyset_fake(void *set) { if (set) memset(set, 0, sizeof(uint64_t)); return 0; }
// arc4random via libnx CSRNG (newlib's pulls getentropy/_getentropy_r, absent here)
static uint32_t arc4random_fake(void) { uint32_t v; randomGet(&v, sizeof(v)); return v; }
static void arc4random_buf_fake(void *buf, size_t n) { randomGet(buf, n); }

// fortified stdio/io: forward to the unchecked shim (we trust the core's sizes)
static size_t __fread_chk_fake(void *p, size_t pn, size_t sz, size_t n, FILE *f) {
  (void)pn; return fread_fake(p, sz, n, f);
}
static size_t __fwrite_chk_fake(const void *p, size_t pn, size_t sz, size_t n, FILE *f) {
  (void)pn; return fwrite_fake(p, sz, n, f);
}
static ssize_t __pread_chk_fake(int fd, void *buf, size_t count, off_t off, size_t bn) {
  (void)bn; return pread_compat(fd, buf, count, off);
}
static ssize_t __write_chk_fake(int fd, const void *buf, size_t count, size_t bn) {
  (void)bn; return write(fd, buf, count);
}

// locale ctype (the core only needs ASCII semantics)
static int tolower_l_fake(int c, locale_t l) { (void)l; return tolower(c); }
static int toupper_l_fake(int c, locale_t l) { (void)l; return toupper(c); }

// ioctl: ashmem fds get the ashmem ioctls (SET_NAME/SET_SIZE/GET_SIZE etc); the
// VTLB memory setup bails if SET_SIZE fails. Everything else -> failure.
static int ioctl_fake(int fd, unsigned long req, ...) {
  int ret;
  if (ashmem_ioctl(fd, req, &ret) == 0) return ret;
  errno = ENOTTY; return -1;
}

// --- PS2 network adapter (DEV9 "Sockets" backend) --------------------------
// The core's SMAP/Sockets stack stays dormant unless the launcher's Network
// toggle set DEV9/Eth/EthEnable (see main.c). g_net_ready is raised there once
// the libnx bsd service is up; until then every entry point fails cleanly --
// identical to the old offline stub -- so a networking-disabled boot is
// unchanged. socket fds are small ints and never collide with the ashmem base
// (0x40000000) used by the VTLB, so they share the fd namespace safely.
volatile int g_net_ready = 0;

#define NET_GUARD do { if (!g_net_ready) { errno = ENETDOWN; return -1; } } while (0)

static int     net_socket(int a,int b,int c){ NET_GUARD; return socket(a,b,c); }
static int     net_bind(int f,const struct sockaddr*a,socklen_t l){ NET_GUARD; return bind(f,a,l); }
static int     net_connect(int f,const struct sockaddr*a,socklen_t l){ NET_GUARD; return connect(f,a,l); }
static ssize_t net_sendto(int f,const void*b,size_t n,int fl,const struct sockaddr*a,socklen_t l){ NET_GUARD; return sendto(f,b,n,fl,a,l); }
static ssize_t net_recvfrom(int f,void*b,size_t n,int fl,struct sockaddr*a,socklen_t*l){ NET_GUARD; return recvfrom(f,b,n,fl,a,l); }
static ssize_t net_recvmsg(int f,struct msghdr*m,int fl){ NET_GUARD; return recvmsg(f,m,fl); }
static int     net_select(int n,fd_set*r,fd_set*w,fd_set*e,struct timeval*t){ if(!g_net_ready){ errno=ENETDOWN; return -1; } return select(n,r,w,e,t); }
static int     net_setsockopt(int f,int lv,int o,const void*v,socklen_t l){ NET_GUARD; return setsockopt(f,lv,o,v,l); }
static int     net_getsockopt(int f,int lv,int o,void*v,socklen_t*l){ NET_GUARD; return getsockopt(f,lv,o,v,l); }
static int     net_getsockname(int f,struct sockaddr*a,socklen_t*l){ NET_GUARD; return getsockname(f,a,l); }
static int     net_shutdown(int f,int h){ NET_GUARD; return shutdown(f,h); }
static int     net_getaddrinfo(const char*n,const char*s,const struct addrinfo*h,struct addrinfo**r){ if(!g_net_ready) return EAI_FAIL; return getaddrinfo(n,s,h,r); }

// libnx exports no getifaddrs; the Sockets relay only uses it to enumerate host
// adapters and tolerates an empty list, so hand back "no interfaces".
struct ifaddrs;
static int  net_getifaddrs(struct ifaddrs** ifap){ if(ifap) *ifap = NULL; return 0; }
static void net_freeifaddrs(struct ifaddrs* ifa){ (void)ifa; }

// The core is FORTIFY-built, so FD_SET/FD_ISSET expand to these _chk helpers;
// route them to a plain byte-addressed fd bitmap (matches the little-endian
// fd_set layout libnx's select() reads). __cmsg_nxthdr returns NULL to end any
// ancillary-data walk (the Sockets backend uses none).
static void fdset_set_chk(int fd, fd_set* s, size_t n){
  if (fd >= 0 && (size_t)(fd>>3) < n) ((unsigned char*)s)[fd>>3] |= (unsigned char)(1u << (fd & 7));
}
static int fdset_isset_chk(int fd, fd_set* s, size_t n){
  return (fd >= 0 && (size_t)(fd>>3) < n) ? ((((unsigned char*)s)[fd>>3] >> (fd & 7)) & 1) : 0;
}
static void *cmsg_nxthdr_stub(void *mhdr, void *cmsg){ (void)mhdr; (void)cmsg; return NULL; }

// Bionic uses bit 0 for TIMER_ABSTIME; newlib uses a different value.
static int clock_nanosleep_fake(clockid_t clk, int flags, const struct timespec *req,
                                struct timespec *rem) {
  struct timespec d = *req;
  if (flags & 1) { // Android TIMER_ABSTIME == 1: req is an absolute deadline
    struct timespec now;
    clock_gettime_fake(clk, &now);
    d.tv_sec = req->tv_sec - now.tv_sec;
    d.tv_nsec = req->tv_nsec - now.tv_nsec;
    if (d.tv_nsec < 0) { d.tv_sec--; d.tv_nsec += 1000000000L; }
    if (d.tv_sec < 0) return 0; // deadline already passed
  }
  if (d.tv_sec > 0 || d.tv_nsec > 33000000L) { // cap at ~2 frames (60fps)
    d.tv_sec = 0; d.tv_nsec = 33000000L;
  }
  return nanosleep(&d, rem);
}


// Linux-only syscalls newlib/libnx doesn't provide -> benign stubs.
static int prctl_fake(int option, ...) { (void)option; return 0; }
static int fallocate_fake(int fd, int mode, off_t off, off_t len) {
  (void)mode; (void)off; // best-effort: just ensure the file is at least len
  return ftruncate(fd, off + len);
}
static int sched_setaffinity_fake(int pid, size_t sz, const void *mask) {
  (void)pid; (void)sz; (void)mask; return 0; // core pins its own threads via libnx
}

static void abort_fake(void) {
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(134);
}

static void zth_noop(void) {}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  { "AAudioStreamBuilder_delete", (uintptr_t)&AAudioStreamBuilder_delete },
  { "AAudioStreamBuilder_openStream", (uintptr_t)&AAudioStreamBuilder_openStream },
  { "AAudioStreamBuilder_setBufferCapacityInFrames", (uintptr_t)&AAudioStreamBuilder_setBufferCapacityInFrames },
  { "AAudioStreamBuilder_setChannelCount", (uintptr_t)&AAudioStreamBuilder_setChannelCount },
  { "AAudioStreamBuilder_setDataCallback", (uintptr_t)&AAudioStreamBuilder_setDataCallback },
  { "AAudioStreamBuilder_setDeviceId", (uintptr_t)&AAudioStreamBuilder_setDeviceId },
  { "AAudioStreamBuilder_setDirection", (uintptr_t)&AAudioStreamBuilder_setDirection },
  { "AAudioStreamBuilder_setErrorCallback", (uintptr_t)&AAudioStreamBuilder_setErrorCallback },
  { "AAudioStreamBuilder_setFormat", (uintptr_t)&AAudioStreamBuilder_setFormat },
  { "AAudioStreamBuilder_setFramesPerDataCallback", (uintptr_t)&AAudioStreamBuilder_setFramesPerDataCallback },
  { "AAudioStreamBuilder_setPerformanceMode", (uintptr_t)&AAudioStreamBuilder_setPerformanceMode },
  { "AAudioStreamBuilder_setSampleRate", (uintptr_t)&AAudioStreamBuilder_setSampleRate },
  { "AAudioStreamBuilder_setSharingMode", (uintptr_t)&AAudioStreamBuilder_setSharingMode },
  { "AAudioStream_close", (uintptr_t)&AAudioStream_close },
  { "AAudioStream_getFramesRead", (uintptr_t)&AAudioStream_getFramesRead },
  { "AAudioStream_requestPause", (uintptr_t)&AAudioStream_requestPause },
  { "AAudioStream_requestStart", (uintptr_t)&AAudioStream_requestStart },
  { "AAudioStream_requestStop", (uintptr_t)&AAudioStream_requestStop },
  { "AAudioStream_waitForStateChange", (uintptr_t)&AAudioStream_waitForStateChange },
  { "AAudio_createStreamBuilder", (uintptr_t)&AAudio_createStreamBuilder },
  { "ANativeWindow_acquire", (uintptr_t)&ANativeWindow_acquire_fake },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake },
  { "ASharedMemory_create", (uintptr_t)&ASharedMemory_create_fake },
  { "AndroidBitmap_lockPixels", (uintptr_t)&AndroidBitmap_lockPixels_fake },
  { "AndroidBitmap_unlockPixels", (uintptr_t)&AndroidBitmap_unlockPixels_fake },
  { "_ZTH14armAsmCapacity", (uintptr_t)&zth_noop },
  { "_ZTH15armConstantPool", (uintptr_t)&zth_noop },
  { "_ZTH6armAsm", (uintptr_t)&zth_noop },
  { "_ZTH9armAsmPtr", (uintptr_t)&zth_noop },
  { "__FD_ISSET_chk", (uintptr_t)&fdset_isset_chk },
  { "__FD_SET_chk", (uintptr_t)&fdset_set_chk },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint_fake },
  { "__android_log_write", (uintptr_t)&__android_log_write_fake },
  { "__cmsg_nxthdr", (uintptr_t)&cmsg_nxthdr_stub },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__fread_chk", (uintptr_t)&__fread_chk_fake },
  { "__fwrite_chk", (uintptr_t)&__fwrite_chk_fake },
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__open_2", (uintptr_t)&open2_fake },
  { "__pread_chk", (uintptr_t)&__pread_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__write_chk", (uintptr_t)&__write_chk_fake },
  { "_ctype_", (uintptr_t)&g_ctype_ptr },
  { "abort", (uintptr_t)&abort_fake },
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "arc4random", (uintptr_t)&arc4random_fake },
  { "arc4random_buf", (uintptr_t)&arc4random_buf_fake },
  { "asin", (uintptr_t)&asin },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atoi", (uintptr_t)&atoi },
  { "bind", (uintptr_t)&net_bind },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&calloc },
  { "clearerr", (uintptr_t)&clearerr },
  { "clock", (uintptr_t)&clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "clock_nanosleep", (uintptr_t)&clock_nanosleep_fake },
  { "close", (uintptr_t)&close_fake },
  { "closedir", (uintptr_t)&closedir },
  { "closelog", (uintptr_t)&ret0 },
  { "connect", (uintptr_t)&net_connect },
  { "cos", (uintptr_t)&cos },
  { "ctime_r", (uintptr_t)&ctime_r },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "eglBindAPI", (uintptr_t)&eglBindAPIHook },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfigHook },
  { "eglCreateContext", (uintptr_t)&eglCreateContextHook },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurfaceHook },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurfaceHook },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContextHook },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurfaceHook },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplayHook },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddressHook },
  { "eglInitialize", (uintptr_t)&eglInitializeHook },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrentHook },
  { "eglQueryString", (uintptr_t)&eglQueryStringHook },
  { "eglQuerySurface", (uintptr_t)&eglQuerySurfaceHook },
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffersHook },
  { "eglSwapInterval", (uintptr_t)&eglSwapIntervalHook },
  { "exp", (uintptr_t)&exp },
  { "exp2", (uintptr_t)&exp2 },
  { "fallocate", (uintptr_t)&fallocate_fake },
  { "fchmod", (uintptr_t)&fchmod },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "fegetround", (uintptr_t)&fegetround },
  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fesetround", (uintptr_t)&fesetround },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgets", (uintptr_t)&fgets },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "free", (uintptr_t)&free },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
  { "freeifaddrs", (uintptr_t)&net_freeifaddrs },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "fstat", (uintptr_t)&fstat_fake },
  { "fstat64", (uintptr_t)&fstat_fake },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getaddrinfo", (uintptr_t)&net_getaddrinfo },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "getcwd", (uintptr_t)&getcwd },
  { "getenv", (uintptr_t)&getenv },
  { "getifaddrs", (uintptr_t)&net_getifaddrs },
  { "getpagesize", (uintptr_t)&getpagesize_fake },
  { "getpid", (uintptr_t)&getpid },
  { "getsockname", (uintptr_t)&net_getsockname },
  { "getsockopt", (uintptr_t)&net_getsockopt },
  { "gettid", (uintptr_t)&gettid_fake },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "inet_pton", (uintptr_t)&inet_pton },
  { "ioctl", (uintptr_t)&ioctl_fake },
  { "isatty", (uintptr_t)&isatty },
  { "isdigit_l", (uintptr_t)&isdigit_l },
  { "islower_l", (uintptr_t)&islower_l },
  { "isupper_l", (uintptr_t)&isupper_l },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "isxdigit_l", (uintptr_t)&isxdigit_l },
  { "localeconv", (uintptr_t)&localeconv },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "lockf", (uintptr_t)&lockf_fake },
  { "log", (uintptr_t)&log },
  { "log2", (uintptr_t)&log2 },
  { "longjmp", (uintptr_t)&longjmp },
  { "lseek", (uintptr_t)&lseek },
  { "lseek64", (uintptr_t)&lseek },
  { "malloc", (uintptr_t)&malloc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "mkstemp", (uintptr_t)&mkstemp },
  { "mktime", (uintptr_t)&mktime },
  { "mmap", (uintptr_t)&mmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "nanosleep", (uintptr_t)&nanosleep_park },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir },
  { "openlog", (uintptr_t)&ret0 },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "pow", (uintptr_t)&pow },
  { "prctl", (uintptr_t)&prctl_fake },
  { "printf", (uintptr_t)&printf },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_getcpuclockid", (uintptr_t)&pthread_getcpuclockid },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_setschedparam", (uintptr_t)&ret0 },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "qsort", (uintptr_t)&qsort },
  { "readdir", (uintptr_t)&readdir_fake },
  { "realloc", (uintptr_t)&realloc },
  { "recvfrom", (uintptr_t)&net_recvfrom },
  { "recvmsg", (uintptr_t)&net_recvmsg },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "rmdir", (uintptr_t)&rmdir },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_setaffinity", (uintptr_t)&sched_setaffinity_fake },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "select", (uintptr_t)&net_select },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sendto", (uintptr_t)&net_sendto },
  { "setjmp", (uintptr_t)&setjmp },
  { "setlocale", (uintptr_t)&setlocale },
  { "setsockopt", (uintptr_t)&net_setsockopt },
  { "shutdown", (uintptr_t)&net_shutdown },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigemptyset", (uintptr_t)&sigemptyset_fake },
  { "sin", (uintptr_t)&sin },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "socket", (uintptr_t)&net_socket },
  { "sscanf", (uintptr_t)&sscanf },
  { "stat", (uintptr_t)&stat_fake },
  { "stat64", (uintptr_t)&stat_fake },
  { "stderr", (uintptr_t)&stderr_ptr },
  { "stdout", (uintptr_t)&stdout_ptr },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strnlen", (uintptr_t)&strnlen },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "swprintf", (uintptr_t)&swprintf },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "syslog", (uintptr_t)&ret0 },
  { "tan", (uintptr_t)&tan },
  { "time", (uintptr_t)&time },
  { "tolower_l", (uintptr_t)&tolower_l_fake },
  { "toupper_l", (uintptr_t)&toupper_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "unlink", (uintptr_t)&unlink },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "usleep", (uintptr_t)&usleep },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
#ifdef USE_VULKAN
  // The core dlopen("libvulkan.so")s and dlsyms exactly these six to bootstrap
  // Vulkan; the rest it discovers via vk_gipa_hook (our vkGetInstanceProcAddr).
  // Three route through hooks (surface bridge + android->vi ext swap + present
  // counting); three are NVK's real entrypoints (declared by vulkan_core.h).
  { "vkGetInstanceProcAddr",                  (uintptr_t)&vk_gipa_hook },
  { "vkCreateInstance",                       (uintptr_t)&vkCreateInstance_hook },
  { "vkEnumerateInstanceExtensionProperties", (uintptr_t)&vkEnumerateInstanceExtensionProperties_hook },
  { "vkEnumerateInstanceLayerProperties",     (uintptr_t)&vkEnumerateInstanceLayerProperties },
  { "vkEnumerateInstanceVersion",             (uintptr_t)&vkEnumerateInstanceVersion },
  { "vkDestroyInstance",                      (uintptr_t)&vkDestroyInstance },
#endif
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },
  { "wctob", (uintptr_t)&wctob },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

// BSD ctype mask bits (newlib/bionic legacy layout)
#define _CTYPE_U 0x01  // upper
#define _CTYPE_L 0x02  // lower
#define _CTYPE_N 0x04  // digit
#define _CTYPE_S 0x08  // space
#define _CTYPE_P 0x10  // punct
#define _CTYPE_C 0x20  // control
#define _CTYPE_X 0x40  // hex digit
#define _CTYPE_B 0x80  // printable space (blank)

void update_imports(void) {
  // fill the legacy _ctype_ table from the C locale: index [c+1] for char c,
  // [0] left as the EOF guard (0).
  for (int c = 0; c < 256; c++) {
    unsigned char m = 0;
    if (isupper(c))  m |= _CTYPE_U;
    if (islower(c))  m |= _CTYPE_L;
    if (isdigit(c))  m |= _CTYPE_N;
    if (isspace(c))  m |= _CTYPE_S;
    if (ispunct(c))  m |= _CTYPE_P;
    if (iscntrl(c))  m |= _CTYPE_C;
    if (isxdigit(c)) m |= _CTYPE_X;
    if (c == ' ')    m |= _CTYPE_B;
    g_ctype_table[c + 1] = m;
  }
  g_ctype_table[0] = 0;
}
