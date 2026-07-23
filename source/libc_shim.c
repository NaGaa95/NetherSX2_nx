/* libc_shim.c -- bionic-compatible libc wrappers for the 2.1.131 libs
 *
 * libGame.so and libc++_shared.so are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches
 * is passed straight through from imports.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}

long __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
  (void)dstlen; (void)srclen;
  return strncpy(dst, src, n);
}

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  value[0] = '\0';
  return 0;
}

unsigned long getauxval_fake(unsigned long type) {
  (void)type;
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID:
      return gettid_fake();
  }
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

int sched_get_priority_max_fake(int policy) {
  (void)policy;
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  (void)msg;
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1;
}

int __register_atfork_fake(void) {
  return 0;
}

extern int __cxa_atexit(void (*destructor)(void *), void *argument, void *dso_handle);
extern void __cxa_finalize(void *dso_handle);

static char core_dso_handle;
static int core_finalized;

int __cxa_atexit_fake(void (*destructor)(void *), void *argument, void *dso_handle) {
  (void)dso_handle;
  return __cxa_atexit(destructor, argument, &core_dso_handle);
}

void __cxa_finalize_fake(void *dso_handle) {
  (void)dso_handle;
  libc_finalize_core();
}

void libc_finalize_core(void) {
  if (!__atomic_exchange_n(&core_finalized, 1, __ATOMIC_ACQ_REL))
    __cxa_finalize(&core_dso_handle);
}

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3;
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:

      return -1;
  }
}

// The core's frame timer needs a monotonic sub-millisecond clock.
#define FAKE_EPOCH_BASE 1700000000ull

int clock_gettime_fake(int clk_id, struct timespec *tp) {
  (void)clk_id;
  if (!tp)
    return -1;
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq();
  const u64 tick = armGetSystemTick();
  tp->tv_sec = (time_t)(FAKE_EPOCH_BASE + tick / freq);
  tp->tv_nsec = (long)(((tick % freq) * 1000000000ull) / freq);
  return 0;
}

const char *fix_path(const char *path) {
  static _Thread_local char buf[2][1024];
  static _Thread_local int which = 0;

  if (!path)
    return path;
  // Redirect Android application paths into the data directory.
  static const char *const android_prefixes[] = {
    "/data/user/0/xyz.aethersx2.android/files",
    "/data/data/xyz.aethersx2.android/files",
    "/storage/emulated/0/Android/data/xyz.aethersx2.android/files",
    "/sdcard/Android/data/xyz.aethersx2.android/files",
  };
  for (unsigned i = 0; i < sizeof(android_prefixes) / sizeof(*android_prefixes); i++) {
    size_t plen = strlen(android_prefixes[i]);
    if (!strncmp(path, android_prefixes[i], plen)) {
      const char *rest = path + plen;
      char *out = buf[which];
      which ^= 1;
      snprintf(out, sizeof(buf[0]), "%s%s", DATA_ROOT, rest);
      return out;
    }
  }

  return path; // absolute SD path (or relative) -- use as-is
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  int fd = open(fix_path(path), convert_open_flags(flags), mode);
  return fd;
}

// bionic's fortified open with no variadic mode (read/existing files)
int open2_fake(const char *path, int flags) {
  return open(fix_path(path), convert_open_flags(flags), 0666);
}

int mkdir_fake(const char *path, unsigned int mode) {
  return mkdir(fix_path(path), mode);
}

int remove_fake(const char *path) {
  return remove(fix_path(path));
}

int rename_fake(const char *from, const char *to) {
  const char *f = fix_path(from);
  const char *t = fix_path(to);
  // Horizon rename does not replace an existing destination.
  remove(t);
  return rename(f, t);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  const int ret = stat(fix_path(path), &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, st);
  return ret;
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // NOTE: not thread-safe
  struct dirent *e = readdir((DIR *)dirp);
  if (!e)
    return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return (void *)1;
}

void freelocale_fake(void *loc) {
  (void)loc;
}

void *uselocale_fake(void *loc) {
  (void)loc;
  return (void *)1;
}

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha)
WRAP_ISW_L(iswblank)
WRAP_ISW_L(iswcntrl)
WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower)
WRAP_ISW_L(iswprint)
WRAP_ISW_L(iswpunct)
WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper)
WRAP_ISW_L(iswxdigit)
WRAP_ISW_L(towlower)
WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) {
  (void)loc;
  return strcoll(a, b);
}

size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc;
  return strftime(s, max, fmt, (const struct tm *)tm);
}

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc;
  return strtold(s, end);
}

long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoull(s, end, base);
}

int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) {
  (void)loc;
  return wcscoll(a, b);
}

size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) {
  (void)loc;
  return wcsxfrm(dst, src, n);
}

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  // ascii-ish naive conversion
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr)
// libc++_shared initializes std::cout/cerr against &__sF[1]/&__sF[2];
// these wrappers absorb accesses to those fake FILEs and forward the rest
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
    (void)ptr; (void)size;
    return n;
  }
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) {
    (void)s;
    return 0;
  }
  return fputs(s, f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return fclose(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
    (void)fmt;
    ret = 0;
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
    (void)fmt; (void)va;
    return 0;
  }
  return vfprintf(f, fmt, va);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int getc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return getc(f);
}

// fopen with a large stream buffer for the big game archives: the engine
// streams the multi-hundred-MB .dat packs with many small reads/seeks, and
// fsdev round trips dominate without buffering.
FILE *fopen_fake(const char *path, const char *mode) {
  // CPU topology: the Switch has no /sys, and the core reads these to size its
  // worker-thread pool -- a failed/garbage read here can spawn a runaway number
  // of threads. Report 4 present/online CPUs (matches the 3 app cores we use).
  if (path && strchr(mode, 'r') && !strncmp(path, "/sys/devices/system/cpu/", 24)) {
    const char *leaf = path + 24;
    const char *content = (!strcmp(leaf, "kernel_max")) ? "63\n" : "0-3\n";
    FILE *m = fmemopen((void *)content, strlen(content), "r");

    if (m) return m;
  }
  const char *p = fix_path(path);
  FILE *f = fopen(p, mode);
  if (f && strchr(mode, 'r')) {
    const char *ext = strrchr(p, '.');
    if (ext && strcasecmp(ext, ".dat") == 0)
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  return f;
}

// ---------------------------------------------------------------------------
// ANativeWindow -> NWindow mapping
// ---------------------------------------------------------------------------

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env;
  if (surface == NULL)
    return NULL;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  nwindowSetCrop(win, 0, 0, screen_width, screen_height);
  return win;
}

int ANativeWindow_getWidth_fake(void *win) {
  (void)win;
  return screen_width;
}

int ANativeWindow_getHeight_fake(void *win) {
  (void)win;
  return screen_height;
}

void ANativeWindow_acquire_fake(void *win) {
  (void)win; // single default window; no refcount
}

void ANativeWindow_release_fake(void *win) {
  (void)win; // never destroy the default window
}

int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)format;
  if (w > 0 && h > 0)
    nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection
// (bionic types are plain structs the game allocates; we stash a pointer
// to the real object in their first bytes, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  Semaphore sem;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) {
    free(*s);
    *s = NULL;
  }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s)
    semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s)
    semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem))
    return 0;
  errno = EAGAIN;
  return -1;
}

// ---------------------------------------------------------------------------
// libjnigraphics: AndroidBitmap_* (screenshots/covers -- stubbable)
// The core calls Bitmap.createBitmap via JNI (jni_fake returns a fake object)
// then locks its pixels here. We hand back a transient owned buffer; screenshot
// content is discarded. Returns ANDROID_BITMAP_RESULT_SUCCESS (0).
// ---------------------------------------------------------------------------

#define ANDROID_BITMAP_RESULT_SUCCESS 0

static void *g_bitmap_scratch = NULL;
static size_t g_bitmap_scratch_size = 0;

int AndroidBitmap_lockPixels_fake(void *env, void *bitmap, void **addrPtr) {
  (void)env; (void)bitmap;
  // a generous scratch (max plausible cover/screenshot at 4 bytes/px)
  const size_t need = (size_t)1920 * 1080 * 4;
  if (g_bitmap_scratch_size < need) {
    free(g_bitmap_scratch);
    g_bitmap_scratch = malloc(need);
    g_bitmap_scratch_size = g_bitmap_scratch ? need : 0;
  }
  if (addrPtr) *addrPtr = g_bitmap_scratch;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

int AndroidBitmap_unlockPixels_fake(void *env, void *bitmap) {
  (void)env; (void)bitmap;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

#ifndef PROT_NONE
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#define MAP_ANON 0x20
#endif
#ifndef MAP_FIXED
#define MAP_FIXED 0x10
#endif

#define MMAP_PAGE               0x1000
#define ASHMEM_FD_BASE 0x40000000
#define MAX_ASHMEM     16
#define FASTMEM_AREA_SIZE       0x100000000ULL
#define FASTMEM_PAGE_COUNT      (FASTMEM_AREA_SIZE / MMAP_PAGE)
#define FASTMEM_BACKING_MIN     0x08000000ULL
#define FASTMEM_MAPPED_PAGE_LIMIT 16384u
#define FASTMEM_ENTRY_EMPTY     UINT32_MAX
#define FASTMEM_ENTRY_PROTECTED 0x80000000u
#define FASTMEM_ENTRY_LAZY      0x40000000u
#define FASTMEM_ENTRY_REMAP     0x20000000u
#define FASTMEM_ENTRY_PAGE_MASK 0x000fffffu
#define FASTMEM_ENTRY_FD_SHIFT  20
#define FASTMEM_FAULT_BATCH_PAGES 16u
#define FASTMEM_REVERSE_END     UINT32_MAX

typedef struct {
  int used;
  void *raw;
  void *ptr;
  size_t size;
  VirtmemReservation *reservation;
  int alias_backed;
  uint32_t *reverse_heads;
  size_t reverse_page_count;
} AshmemRegion;

static AshmemRegion g_ashmem[MAX_ASHMEM];
static Mutex g_ashmem_lock;
static Mutex g_fastmem_lock;
static FastmemMode g_fastmem_mode = FASTMEM_MODE_OFF;
static uintptr_t g_fastmem_base;
static uint32_t *g_fastmem_pages;
static uint32_t *g_fastmem_reverse_next;
static unsigned g_fastmem_live_pages;
static char g_smc_reason[128] = "not configured";

void fastmem_set_mode(FastmemMode mode) {
  if (mode < FASTMEM_MODE_OFF || mode > FASTMEM_MODE_ON)
    mode = FASTMEM_MODE_OFF;
  g_fastmem_mode = mode;
}

FastmemMode fastmem_get_mode(void) {
  return g_fastmem_mode;
}

static void smc_probe_reason(const char *stage, Result rc) {
  snprintf(g_smc_reason, sizeof(g_smc_reason), "%s failed (0x%08x)", stage,
           (unsigned)rc);
}

/*
 * Exercise the permission sequence used by the wrapper's SMC path. The initial
 * RW permission turns the canonical AliasCode mapping into AliasCodeData. That
 * state is reprotected with svcSetMemoryPermission; the process-memory SVC is
 * only valid for the initial code-to-data transition. A fastmem SharedCode
 * alias must still be unmapped before its writable backing becomes read-only.
 */
static int memory_smc_probe(void) {
  void *raw = memalign(MMAP_PAGE, MMAP_PAGE);
  if (!raw) {
    snprintf(g_smc_reason, sizeof(g_smc_reason), "probe allocation failed");
    return 0;
  }
  memset(raw, 0, MMAP_PAGE);

  VirtmemReservation *canonical_reservation = NULL;
  VirtmemReservation *fastmem_reservation = NULL;
  void *canonical = NULL;
  void *fastmem_alias = NULL;
  int canonical_mapped = 0;
  int fastmem_mapped = 0;
  int passed = 0;
  const Handle self = envGetOwnProcessHandle();

  virtmemLock();
  canonical = virtmemFindCodeMemory(MMAP_PAGE, MMAP_PAGE);
  canonical_reservation = canonical ? virtmemAddReservation(canonical, MMAP_PAGE) : NULL;
  if (g_fastmem_mode != FASTMEM_MODE_OFF) {
    fastmem_alias = virtmemFindAslr(MMAP_PAGE, 0);
    fastmem_reservation = fastmem_alias ?
        virtmemAddReservation(fastmem_alias, MMAP_PAGE) : NULL;
  }
  virtmemUnlock();

  if (!canonical || !canonical_reservation ||
      (g_fastmem_mode != FASTMEM_MODE_OFF &&
       (!fastmem_alias || !fastmem_reservation))) {
    snprintf(g_smc_reason, sizeof(g_smc_reason), "probe address reservation failed");
    goto cleanup;
  }

  Result rc = svcMapProcessCodeMemory(self, (u64)canonical, (u64)raw, MMAP_PAGE);
  if (R_FAILED(rc)) {
    smc_probe_reason("MapProcessCodeMemory", rc);
    goto cleanup;
  }
  canonical_mapped = 1;

  rc = svcSetProcessMemoryPermission(self, (u64)canonical, MMAP_PAGE, Perm_Rw);
  if (R_FAILED(rc)) {
    smc_probe_reason("SetPermission(RW)", rc);
    goto cleanup;
  }

  if (g_fastmem_mode != FASTMEM_MODE_OFF) {
    rc = svcMapProcessMemory(fastmem_alias, self, (u64)canonical, MMAP_PAGE);
    if (R_FAILED(rc)) {
      smc_probe_reason("MapProcessMemory", rc);
      goto cleanup;
    }
    fastmem_mapped = 1;
  }

  if (fastmem_mapped) {
    rc = svcUnmapProcessMemory(fastmem_alias, self, (u64)canonical, MMAP_PAGE);
    if (R_FAILED(rc)) {
      smc_probe_reason("UnmapProcessMemory", rc);
      goto cleanup;
    }
    fastmem_mapped = 0;
  }

  // The runtime reverse index performs this same alias-first ordering before
  // forwarding PCSX2's canonical read-only request to Horizon. The mapping is
  // AliasCodeData now, so normal current-process reprotection is required.
  rc = svcSetMemoryPermission(canonical, MMAP_PAGE, Perm_R);
  if (R_FAILED(rc)) {
    smc_probe_reason("SetMemoryPermission(R) after alias unmap", rc);
    goto cleanup;
  }

  rc = svcSetMemoryPermission(canonical, MMAP_PAGE, Perm_Rw);
  if (R_FAILED(rc)) {
    smc_probe_reason("SetMemoryPermission(RW restore)", rc);
    goto cleanup;
  }

  if (g_fastmem_mode != FASTMEM_MODE_OFF) {
    // Also verify that a page can be remapped after its code was invalidated.
    rc = svcMapProcessMemory(fastmem_alias, self, (u64)canonical, MMAP_PAGE);
    if (R_FAILED(rc)) {
      smc_probe_reason("MapProcessMemory restore", rc);
      goto cleanup;
    }
    fastmem_mapped = 1;
    rc = svcUnmapProcessMemory(fastmem_alias, self, (u64)canonical, MMAP_PAGE);
    if (R_FAILED(rc)) {
      smc_probe_reason("UnmapProcessMemory restore", rc);
      goto cleanup;
    }
    fastmem_mapped = 0;
  }

  snprintf(g_smc_reason, sizeof(g_smc_reason),
           g_fastmem_mode == FASTMEM_MODE_OFF ?
               "canonical R/RW probe passed" :
               "canonical R/RW and fastmem alias-first probe passed");
  passed = 1;

cleanup:
  if (fastmem_mapped) {
    const Result rc = svcUnmapProcessMemory(fastmem_alias, self,
                                            (u64)canonical, MMAP_PAGE);
    if (R_FAILED(rc)) {
      if (passed)
        smc_probe_reason("probe alias cleanup", rc);
      // Leave every related object reserved: the kernel mapping still owns it.
      return 0;
    }
  }

  if (canonical_mapped) {
    const Result rc = svcUnmapProcessCodeMemory(self, (u64)canonical,
                                                (u64)raw, MMAP_PAGE);
    if (R_FAILED(rc)) {
      if (passed)
        smc_probe_reason("probe code cleanup", rc);
      return 0;
    }
  }

  virtmemLock();
  if (fastmem_reservation)
    virtmemRemoveReservation(fastmem_reservation);
  if (canonical_reservation)
    virtmemRemoveReservation(canonical_reservation);
  virtmemUnlock();
  free(raw);
  return passed;
}

int memory_smc_initialize(void) {
  return memory_smc_probe();
}

const char *memory_smc_reason(void) {
  return g_smc_reason;
}

// returns the region for a fake ashmem fd, or NULL if fd isn't one of ours
static AshmemRegion *ashmem_get(int fd) {
  if (fd < ASHMEM_FD_BASE || fd >= ASHMEM_FD_BASE + MAX_ASHMEM) return NULL;
  AshmemRegion *r = &g_ashmem[fd - ASHMEM_FD_BASE];
  return r->used == 1 ? r : NULL;
}

static int ashmem_make_alias(AshmemRegion *region, void *raw, size_t size) {
  virtmemLock();
  void *canonical = virtmemFindCodeMemory(size, MMAP_PAGE);
  VirtmemReservation *reservation = canonical ? virtmemAddReservation(canonical, size) : NULL;
  virtmemUnlock();
  if (!canonical || !reservation)
    return -1;

  const size_t reverse_page_count = size / MMAP_PAGE;
  uint32_t *reverse_heads = NULL;
  if (g_fastmem_mode != FASTMEM_MODE_OFF) {
    reverse_heads = malloc(reverse_page_count * sizeof(*reverse_heads));
    if (!reverse_heads) {
      virtmemLock();
      virtmemRemoveReservation(reservation);
      virtmemUnlock();
      return -1;
    }
    memset(reverse_heads, 0xff, reverse_page_count * sizeof(*reverse_heads));
  }

  const Handle self = envGetOwnProcessHandle();
  Result rc = svcMapProcessCodeMemory(self, (u64)canonical, (u64)raw, size);
  if (R_FAILED(rc)) {
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    free(reverse_heads);
    return -1;
  }
  rc = svcSetProcessMemoryPermission(self, (u64)canonical, size, Perm_Rw);
  if (R_FAILED(rc)) {
    const Result undo = svcUnmapProcessCodeMemory(self, (u64)canonical, (u64)raw, size);
    if (R_FAILED(undo)) {
      free(reverse_heads);
      return -2;
    }
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    free(reverse_heads);
    return -1;
  }

  region->raw = raw;
  region->ptr = canonical;
  region->size = size;
  region->reservation = reservation;
  region->alias_backed = 1;
  region->reverse_heads = reverse_heads;
  region->reverse_page_count = reverse_page_count;

  return 0;
}

int ASharedMemory_create_fake(const char *name, size_t size) {
  size_t sz = (size + 0xFFF) & ~(size_t)0xFFF;
  void *p = memalign(0x1000, sz);
  if (!p) {

    errno = ENOMEM;
    return -1;
  }
  memset(p, 0, sz);

  mutexLock(&g_ashmem_lock);
  int index = -1;
  for (int i = 0; i < MAX_ASHMEM; i++) {
    if (!g_ashmem[i].used) {
      g_ashmem[i].used = -1;
      index = i;
      break;
    }
  }
  mutexUnlock(&g_ashmem_lock);
  if (index < 0) { free(p); errno = EMFILE; return -1; }

  AshmemRegion region = { .used = 1, .raw = p, .ptr = p, .size = sz };
  int alias_result = 0;
  // Large ashmem regions contain the EE backing store and must always use the
  // reprotectable AliasCodeData mapping required by page-based SMC detection.
  if (sz >= FASTMEM_BACKING_MIN)
    alias_result = ashmem_make_alias(&region, p, sz);
  if (alias_result < 0) {
    mutexLock(&g_ashmem_lock);
    g_ashmem[index].used = 0;
    mutexUnlock(&g_ashmem_lock);
    if (alias_result == -1)
      free(p);
    errno = ENOMEM;
    return -1;
  }

  mutexLock(&g_ashmem_lock);
  g_ashmem[index] = region;
  mutexUnlock(&g_ashmem_lock);
  const int fd = ASHMEM_FD_BASE + index;

  return fd;
}

// Ashmem mappings outlive their descriptors.
int close_fake(int fd) {
  if (ashmem_get(fd)) return 0;
  return close(fd);
}

int ashmem_ioctl(int fd, unsigned long req, int *ret) {
  AshmemRegion *r = ashmem_get(fd);
  if (!r) return -1;
  switch (req & 0xffffffff) {
    case 0x00007704: *ret = (int)r->size; break;   // ASHMEM_GET_SIZE
    case 0x00007706: *ret = 0x7;          break;   // ASHMEM_GET_PROT_MASK -> RWX
    default:         *ret = 0;            break;   // SET_NAME/SET_SIZE/SET_PROT/PIN... -> ok
  }

  return 0;
}

#define MAX_MMAP_REGIONS 256

typedef struct {
  void *dst;
  void *src;
  size_t size;
  int is_code;
  VirtmemReservation *rv;
  void *code_rx;
  void *code_rw;
  VirtmemReservation *code_rx_rv;
  Handle code_handle;
} MmapRegion;

static MmapRegion g_mmaps[MAX_MMAP_REGIONS];
static Mutex g_mmap_lock;

#define MAX_JIT_REDIRECTS 8

typedef struct {
  uintptr_t lo;
  uintptr_t hi;
  intptr_t delta;
} JitRedirect;

static JitRedirect g_jit_redir[MAX_JIT_REDIRECTS];

static void remove_reservation(VirtmemReservation **reservation) {
  if (!reservation || !*reservation)
    return;
  virtmemLock();
  virtmemRemoveReservation(*reservation);
  virtmemUnlock();
  *reservation = NULL;
}

static void jit_redirect_remove_locked(uintptr_t rx) {
  for (int i = 0; i < MAX_JIT_REDIRECTS; i++) {
    if (__atomic_load_n(&g_jit_redir[i].lo, __ATOMIC_ACQUIRE) != rx)
      continue;
    __atomic_store_n(&g_jit_redir[i].lo, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_jit_redir[i].hi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jit_redir[i].delta, 0, __ATOMIC_RELAXED);
    return;
  }
}

int jit_redirect_lookup(uintptr_t address, uintptr_t *redirected) {
  for (int i = 0; i < MAX_JIT_REDIRECTS; i++) {
    const uintptr_t lo = __atomic_load_n(&g_jit_redir[i].lo, __ATOMIC_ACQUIRE);
    if (!lo || address < lo)
      continue;
    const uintptr_t hi = __atomic_load_n(&g_jit_redir[i].hi, __ATOMIC_RELAXED);
    if (address >= hi)
      continue;
    const intptr_t delta = __atomic_load_n(&g_jit_redir[i].delta, __ATOMIC_RELAXED);
    *redirected = (uintptr_t)((intptr_t)address + delta);
    return 1;
  }
  return 0;
}

static Result release_jit_region_locked(MmapRegion *region) {
  if (!region || region->is_code != 1)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  jit_redirect_remove_locked((uintptr_t)region->dst);

  if (region->code_rw) {
    const Result rc = svcControlCodeMemory(region->code_handle,
                                            CodeMapOperation_UnmapOwner,
                                            region->code_rw, region->size, 0);
    if (R_FAILED(rc))
      return rc;
    region->code_rw = NULL;
    remove_reservation(&region->rv);
  }

  if (region->code_rx) {
    const Result rc = svcControlCodeMemory(region->code_handle,
                                            CodeMapOperation_UnmapSlave,
                                            region->code_rx, region->size, 0);
    if (R_FAILED(rc))
      return rc;
    region->code_rx = NULL;
    remove_reservation(&region->code_rx_rv);
  }

  if (region->code_handle != INVALID_HANDLE) {
    const Result rc = svcCloseHandle(region->code_handle);
    if (R_FAILED(rc))
      return rc;
    region->code_handle = INVALID_HANDLE;
  }

  free(region->src);
  memset(region, 0, sizeof(*region));
  return 0;
}

typedef void (*BionicSiginfoHandler)(int, void *, void *);
typedef struct {
  uint32_t flags;
  uint32_t reserved;
  BionicSiginfoHandler handler;
  uint64_t mask;
  void (*restorer)(void);
} BionicSigaction;

_Static_assert(sizeof(BionicSigaction) == 32, "unexpected bionic sigaction size");

#define BIONIC_SA_SIGINFO 4u
static BionicSigaction g_signal_actions[64];
static uint8_t g_signal_action_set[64];
static Mutex g_signal_lock;

typedef struct {
  uint32_t unhandled;
  uint8_t data[508];
} FaultUcontext;

static void signal_chain_marker(int sig, void *info, void *context) {
  (void)sig;
  (void)info;
  FaultUcontext *uc = (FaultUcontext *)context;
  if (uc)
    uc->unhandled = 1;
}

static BionicSigaction default_signal_action(void) {
  BionicSigaction action = {0};
  action.flags = BIONIC_SA_SIGINFO;
  action.handler = signal_chain_marker;
  return action;
}

int sigaction_fake(int sig, const void *act, void *old_act) {
  if (sig < 0 || sig >= (int)(sizeof(g_signal_actions) / sizeof(g_signal_actions[0]))) {
    errno = EINVAL;
    return -1;
  }

  mutexLock(&g_signal_lock);
  const BionicSigaction current = g_signal_action_set[sig]
                                      ? g_signal_actions[sig]
                                      : default_signal_action();
  if (old_act)
    memcpy(old_act, &current, sizeof(current));
  if (act) {
    memcpy(&g_signal_actions[sig], act, sizeof(g_signal_actions[sig]));
    __atomic_store_n(&g_signal_action_set[sig], 1, __ATOMIC_RELEASE);
  }
  mutexUnlock(&g_signal_lock);
  return 0;
}

static int fastmem_contains(uintptr_t addr, size_t size) {
  const uintptr_t base = __atomic_load_n(&g_fastmem_base, __ATOMIC_ACQUIRE);
  return base && size <= FASTMEM_AREA_SIZE && addr >= base &&
         addr - base <= FASTMEM_AREA_SIZE - size;
}

static uint32_t fastmem_pack_entry(int index, size_t offset, int protected_page) {
  uint32_t entry = ((uint32_t)index << FASTMEM_ENTRY_FD_SHIFT) |
                   ((uint32_t)(offset / MMAP_PAGE) & FASTMEM_ENTRY_PAGE_MASK);
  if (protected_page)
    entry |= FASTMEM_ENTRY_PROTECTED;
  return entry;
}

static AshmemRegion *fastmem_entry_region(uint32_t entry, size_t *offset) {
  const int index = (int)((entry >> FASTMEM_ENTRY_FD_SHIFT) & 0xf);
  AshmemRegion *region = &g_ashmem[index];
  if (region->used != 1 || !region->alias_backed)
    return NULL;
  *offset = (size_t)(entry & FASTMEM_ENTRY_PAGE_MASK) * MMAP_PAGE;
  if (*offset >= region->size)
    return NULL;
  return region;
}

/* Reverse links let the canonical mprotect path find only the guest aliases
 * which share a backing page. Scanning all 1,048,576 fastmem slots for every
 * compiled EE page would erase the performance gain from page protection. */
static int fastmem_reverse_insert_locked(size_t fastmem_page, uint32_t entry) {
  if (!g_fastmem_reverse_next || fastmem_page >= FASTMEM_PAGE_COUNT ||
      g_fastmem_reverse_next[fastmem_page] != FASTMEM_REVERSE_END)
    return 0;

  size_t source_offset;
  AshmemRegion *region = fastmem_entry_region(entry, &source_offset);
  const size_t source_page = source_offset / MMAP_PAGE;
  if (!region || !region->reverse_heads || source_page >= region->reverse_page_count)
    return 0;

  g_fastmem_reverse_next[fastmem_page] = region->reverse_heads[source_page];
  region->reverse_heads[source_page] = (uint32_t)fastmem_page;
  return 1;
}

static int fastmem_reverse_remove_locked(size_t fastmem_page, uint32_t entry) {
  if (!g_fastmem_reverse_next || fastmem_page >= FASTMEM_PAGE_COUNT)
    return 0;

  size_t source_offset;
  AshmemRegion *region = fastmem_entry_region(entry, &source_offset);
  const size_t source_page = source_offset / MMAP_PAGE;
  if (!region || !region->reverse_heads || source_page >= region->reverse_page_count)
    return 0;

  uint32_t *link = &region->reverse_heads[source_page];
  size_t guard = 0;
  while (*link != FASTMEM_REVERSE_END) {
    const uint32_t current = *link;
    if (current >= FASTMEM_PAGE_COUNT || guard++ >= FASTMEM_PAGE_COUNT)
      return 0;
    if (current == fastmem_page) {
      *link = g_fastmem_reverse_next[current];
      g_fastmem_reverse_next[current] = FASTMEM_REVERSE_END;
      return 1;
    }
    link = &g_fastmem_reverse_next[current];
  }
  return 0;
}

static Result fastmem_protect_backing_locked(AshmemRegion *region,
                                              size_t source_offset,
                                              size_t size) {
  if (!g_fastmem_pages || !g_fastmem_reverse_next)
    return 0;
  if (!region || !region->reverse_heads ||
      (source_offset & (MMAP_PAGE - 1)) || (size & (MMAP_PAGE - 1)) ||
      source_offset > region->size || size > region->size - source_offset)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  const size_t first_source_page = source_offset / MMAP_PAGE;
  const size_t source_page_count = size / MMAP_PAGE;
  if (first_source_page > region->reverse_page_count ||
      source_page_count > region->reverse_page_count - first_source_page)
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);

  const Handle self = envGetOwnProcessHandle();
  for (size_t page = 0; page < source_page_count; page++) {
    const size_t expected_offset = (first_source_page + page) * MMAP_PAGE;
    uint32_t alias = region->reverse_heads[first_source_page + page];
    size_t guard = 0;
    while (alias != FASTMEM_REVERSE_END) {
      if (alias >= FASTMEM_PAGE_COUNT || guard++ >= FASTMEM_PAGE_COUNT)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
      const uint32_t next_alias = g_fastmem_reverse_next[alias];
      const uint32_t entry = __atomic_load_n(&g_fastmem_pages[alias],
                                             __ATOMIC_ACQUIRE);
      size_t mapped_offset;
      AshmemRegion *mapped_region = fastmem_entry_region(entry, &mapped_offset);
      if (entry == FASTMEM_ENTRY_EMPTY || mapped_region != region ||
          mapped_offset != expected_offset)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

      if (!(entry & FASTMEM_ENTRY_PROTECTED)) {
        if (entry & FASTMEM_ENTRY_LAZY) {
          __atomic_store_n(&g_fastmem_pages[alias],
                           entry | FASTMEM_ENTRY_PROTECTED, __ATOMIC_RELEASE);
        } else {
          const uintptr_t destination = g_fastmem_base +
                                        (size_t)alias * MMAP_PAGE;
          const uintptr_t source = (uintptr_t)region->ptr + mapped_offset;
          const Result rc = svcUnmapProcessMemory((void *)destination, self,
                                                  source, MMAP_PAGE);
          if (R_FAILED(rc))
            return rc;
          if (g_fastmem_live_pages)
            g_fastmem_live_pages--;
          __atomic_store_n(&g_fastmem_pages[alias],
                           entry | FASTMEM_ENTRY_LAZY |
                               FASTMEM_ENTRY_PROTECTED | FASTMEM_ENTRY_REMAP,
                           __ATOMIC_RELEASE);
        }
      }
      alias = next_alias;
    }
  }

  return 0;
}

static Result fastmem_unmap_range_locked(uintptr_t addr, size_t size, int clear) {
  const uintptr_t base = g_fastmem_base;
  const size_t first = (size_t)((addr - base) / MMAP_PAGE);
  const size_t count = size / MMAP_PAGE;
  const Handle self = envGetOwnProcessHandle();

  for (size_t i = 0; i < count;) {
    uint32_t entry = __atomic_load_n(&g_fastmem_pages[first + i], __ATOMIC_ACQUIRE);
    if (entry == FASTMEM_ENTRY_EMPTY) {
      i++;
      continue;
    }

    if (entry & (FASTMEM_ENTRY_LAZY | FASTMEM_ENTRY_PROTECTED)) {
      if (clear) {
        if (!fastmem_reverse_remove_locked(first + i, entry))
          return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        __atomic_store_n(&g_fastmem_pages[first + i], FASTMEM_ENTRY_EMPTY,
                         __ATOMIC_RELEASE);
      } else if (!(entry & FASTMEM_ENTRY_LAZY)) {
        __atomic_store_n(&g_fastmem_pages[first + i],
                         entry | FASTMEM_ENTRY_LAZY | FASTMEM_ENTRY_PROTECTED |
                             FASTMEM_ENTRY_REMAP,
                         __ATOMIC_RELEASE);
      }
      i++;
      continue;
    }

    size_t source_offset;
    AshmemRegion *region = fastmem_entry_region(entry, &source_offset);
    if (!region)
      return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    size_t run = 1;
    while (i + run < count) {
      const uint32_t next_entry = __atomic_load_n(&g_fastmem_pages[first + i + run],
                                                   __ATOMIC_ACQUIRE);
      if (next_entry == FASTMEM_ENTRY_EMPTY ||
          (next_entry & (FASTMEM_ENTRY_LAZY | FASTMEM_ENTRY_PROTECTED)))
        break;
      size_t next_offset;
      AshmemRegion *next_region = fastmem_entry_region(next_entry, &next_offset);
      if (next_region != region || next_offset != source_offset + run * MMAP_PAGE)
        break;
      run++;
    }

    Result rc = svcUnmapProcessMemory((void *)(addr + i * MMAP_PAGE), self,
                                      (u64)((uintptr_t)region->ptr + source_offset),
                                      run * MMAP_PAGE);
    if (R_FAILED(rc)) {

      return rc;
    }
    if (g_fastmem_live_pages >= run)
      g_fastmem_live_pages -= (unsigned)run;
    else
      g_fastmem_live_pages = 0;
    for (size_t j = 0; j < run; j++) {
      const uint32_t old = __atomic_load_n(&g_fastmem_pages[first + i + j],
                                           __ATOMIC_RELAXED);
      if (clear && !fastmem_reverse_remove_locked(first + i + j, old))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
      const uint32_t next = clear ? FASTMEM_ENTRY_EMPTY
                                  : (old | FASTMEM_ENTRY_LAZY |
                                     FASTMEM_ENTRY_PROTECTED | FASTMEM_ENTRY_REMAP);
      __atomic_store_n(&g_fastmem_pages[first + i + j], next, __ATOMIC_RELEASE);
    }
    i += run;
  }
  return 0;
}

static Result fastmem_map_run_locked(size_t first, size_t count,
                                     AshmemRegion *region, size_t source_offset) {
  const uintptr_t destination = g_fastmem_base + first * MMAP_PAGE;
  const uintptr_t source = (uintptr_t)region->ptr + source_offset;
  const size_t size = count * MMAP_PAGE;

  const Result rc = svcMapProcessMemory((void *)destination, envGetOwnProcessHandle(),
                                        source, size);
  if (R_FAILED(rc))
    return rc;

  g_fastmem_live_pages += (unsigned)count;

  for (size_t i = 0; i < count; i++) {
    const uint32_t entry = __atomic_load_n(&g_fastmem_pages[first + i],
                                           __ATOMIC_RELAXED);
    __atomic_store_n(&g_fastmem_pages[first + i],
                     entry & ~(FASTMEM_ENTRY_LAZY | FASTMEM_ENTRY_PROTECTED |
                               FASTMEM_ENTRY_REMAP),
                     __ATOMIC_RELEASE);
  }
  return 0;
}

static void *fastmem_map_alias(void *addr, size_t size, int prot,
                               int ash_index, AshmemRegion *region, size_t offset) {
  const uintptr_t dst = (uintptr_t)addr;
  if (!g_fastmem_pages || !fastmem_contains(dst, size) ||
      !size || (size & (MMAP_PAGE - 1)) ||
      (dst & (MMAP_PAGE - 1)) || (offset & (MMAP_PAGE - 1)))
    return MAP_FAILED;

  mutexLock(&g_fastmem_lock);
  if (R_FAILED(fastmem_unmap_range_locked(dst, size, 1))) {
    mutexUnlock(&g_fastmem_lock);
    return MAP_FAILED;
  }

  const size_t count = size / MMAP_PAGE;
  const size_t first = (size_t)((dst - g_fastmem_base) / MMAP_PAGE);
  const uint32_t flags = FASTMEM_ENTRY_LAZY |
                         ((prot & PROT_WRITE) ? 0 : FASTMEM_ENTRY_PROTECTED);
  for (size_t i = 0; i < count; i++) {
    const uint32_t entry =
        fastmem_pack_entry(ash_index, offset + i * MMAP_PAGE, 0) | flags;
    __atomic_store_n(&g_fastmem_pages[first + i],
                     entry, __ATOMIC_RELEASE);
    if (!fastmem_reverse_insert_locked(first + i, entry)) {
      __atomic_store_n(&g_fastmem_pages[first + i], FASTMEM_ENTRY_EMPTY,
                       __ATOMIC_RELEASE);
      for (size_t j = 0; j < i; j++) {
        const uint32_t previous = __atomic_load_n(&g_fastmem_pages[first + j],
                                                   __ATOMIC_RELAXED);
        fastmem_reverse_remove_locked(first + j, previous);
        __atomic_store_n(&g_fastmem_pages[first + j], FASTMEM_ENTRY_EMPTY,
                         __ATOMIC_RELEASE);
      }
      mutexUnlock(&g_fastmem_lock);
      return MAP_FAILED;
    }
  }
  mutexUnlock(&g_fastmem_lock);
  return addr;
}

static int fastmem_lazy_fault_candidate(uintptr_t fault_addr) {
  uint32_t *pages = __atomic_load_n(&g_fastmem_pages, __ATOMIC_ACQUIRE);
  const uintptr_t base = __atomic_load_n(&g_fastmem_base, __ATOMIC_ACQUIRE);
  if (!pages || !base || fault_addr < base || fault_addr - base >= FASTMEM_AREA_SIZE)
    return 0;
  const size_t index = (size_t)((fault_addr - base) / MMAP_PAGE);
  const uint32_t entry = __atomic_load_n(&pages[index], __ATOMIC_ACQUIRE);
  return entry != FASTMEM_ENTRY_EMPTY && (entry & FASTMEM_ENTRY_LAZY) &&
         !(entry & FASTMEM_ENTRY_PROTECTED);
}

static int fastmem_try_resolve_fault(uintptr_t fault_addr) {
  if (!fastmem_contains(fault_addr, 1))
    return 0;

  mutexLock(&g_fastmem_lock);
  if (!g_fastmem_pages || !fastmem_contains(fault_addr, 1)) {
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }

  const size_t first = (size_t)((fault_addr - g_fastmem_base) / MMAP_PAGE);
  const uint32_t entry = __atomic_load_n(&g_fastmem_pages[first], __ATOMIC_ACQUIRE);
  if (entry == FASTMEM_ENTRY_EMPTY) {
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }
  if (entry & FASTMEM_ENTRY_PROTECTED) {
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }
  if (!(entry & FASTMEM_ENTRY_LAZY)) {
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }

  size_t source_offset;
  AshmemRegion *region = fastmem_entry_region(entry, &source_offset);
  if (!region) {
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }

  if (g_fastmem_live_pages >= FASTMEM_MAPPED_PAGE_LIMIT) {
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }

  const size_t available = FASTMEM_MAPPED_PAGE_LIMIT - g_fastmem_live_pages;
  size_t count = 1;
  while (count < FASTMEM_FAULT_BATCH_PAGES && count < available &&
         first + count < FASTMEM_PAGE_COUNT) {
    const uint32_t next = __atomic_load_n(&g_fastmem_pages[first + count],
                                          __ATOMIC_ACQUIRE);
    if (next == FASTMEM_ENTRY_EMPTY || !(next & FASTMEM_ENTRY_LAZY) ||
        (next & FASTMEM_ENTRY_PROTECTED))
      break;
    size_t next_offset;
    AshmemRegion *next_region = fastmem_entry_region(next, &next_offset);
    if (next_region != region || next_offset != source_offset + count * MMAP_PAGE)
      break;
    count++;
  }

  Result rc = fastmem_map_run_locked(first, count, region, source_offset);
  if (R_FAILED(rc) && count > 1)
    rc = fastmem_map_run_locked(first, 1, region, source_offset);
  mutexUnlock(&g_fastmem_lock);
  return R_SUCCEEDED(rc);
}

static int fastmem_managed_fault(uintptr_t addr) {
  if (fastmem_contains(addr, 1))
    return 1;
  for (int i = 0; i < MAX_ASHMEM; i++) {
    const AshmemRegion *region = &g_ashmem[i];
    if (region->used == 1 && region->alias_backed &&
        addr >= (uintptr_t)region->ptr && addr - (uintptr_t)region->ptr < region->size)
      return 1;
  }
  return 0;
}

static int fastmem_select_signal_action(int *signal, BionicSigaction *action) {
  static const int signals[] = {11, 7};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    const int candidate = signals[i];
    if (!__atomic_load_n(&g_signal_action_set[candidate], __ATOMIC_ACQUIRE))
      continue;
    const BionicSigaction selected = g_signal_actions[candidate];
    if ((uintptr_t)selected.handler <= 1 || selected.handler == signal_chain_marker)
      continue;
    *signal = candidate;
    *action = selected;
    return 1;
  }
  return 0;
}

int fastmem_fault_can_dispatch(uintptr_t pc, uintptr_t fault_addr) {
  (void)pc;
  if (!fastmem_managed_fault(fault_addr))
    return 0;

  if (g_fastmem_mode != FASTMEM_MODE_OFF &&
      fastmem_lazy_fault_candidate(fault_addr))
    return 1;

  int sig;
  BionicSigaction action;
  if (!fastmem_select_signal_action(&sig, &action))
    return 0;
  return 1;
}

int fastmem_dispatch_fault(uintptr_t pc, uintptr_t fault_addr) {
  if (!fastmem_fault_can_dispatch(pc, fault_addr))
    return 0;

  if (fastmem_try_resolve_fault(fault_addr))
    return 1;

  int sig;
  BionicSigaction action;
  if (!fastmem_select_signal_action(&sig, &action))
    return 0;

  uint8_t siginfo[128] __attribute__((aligned(16))) = {0};
  FaultUcontext context __attribute__((aligned(16))) = {0};
  *(int *)(siginfo + 0) = sig;
  *(uintptr_t *)(siginfo + 16) = fault_addr;
  *(uintptr_t *)((uint8_t *)&context + 440) = pc;

  if (action.flags & BIONIC_SA_SIGINFO)
    action.handler(sig, siginfo, &context);
  else
    ((void (*)(int))action.handler)(sig);
  return !context.unhandled;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  if (length == 0) return MAP_FAILED;
  const size_t len = (length + MMAP_PAGE - 1) & ~(size_t)(MMAP_PAGE - 1);

  AshmemRegion *ash = ashmem_get(fd);
  if (ash) {
    if (offset < 0 || (size_t)offset > ash->size || len > ash->size - (size_t)offset) {

      return MAP_FAILED;
    }

    if ((flags & MAP_FIXED) && ash->alias_backed &&
        fastmem_contains((uintptr_t)addr, len))
      return fastmem_map_alias(addr, len, prot, fd - ASHMEM_FD_BASE, ash, (size_t)offset);

    void *src = (char *)ash->ptr + offset;
    return src;
  }

  if (fd >= 0)
    return MAP_FAILED;            // other file-backed mappings unsupported

  if (prot == PROT_NONE && (flags & MAP_FIXED)) {
    if (g_fastmem_pages && fastmem_contains((uintptr_t)addr, len)) {
      mutexLock(&g_fastmem_lock);
      const Result result = fastmem_unmap_range_locked((uintptr_t)addr, len, 1);
      mutexUnlock(&g_fastmem_lock);
      if (R_FAILED(result))
        return MAP_FAILED;
    }
    return addr;
  }

  // PROT_NONE requests reserve address space without committing memory.
  if (prot == PROT_NONE) {
    virtmemLock();
    void *base = virtmemFindAslr(len, 0);
    VirtmemReservation *rv = base ? virtmemAddReservation(base, len) : NULL;
    virtmemUnlock();
    if (!base || !rv) {

      return MAP_FAILED;
    }

    uint32_t *fastmem_pages = NULL;
    uint32_t *fastmem_reverse_next = NULL;
    if (g_fastmem_mode != FASTMEM_MODE_OFF && len == FASTMEM_AREA_SIZE) {
      fastmem_pages = malloc(FASTMEM_PAGE_COUNT * sizeof(*fastmem_pages));
      fastmem_reverse_next =
          malloc(FASTMEM_PAGE_COUNT * sizeof(*fastmem_reverse_next));
      if (!fastmem_pages || !fastmem_reverse_next) {
        free(fastmem_pages);
        free(fastmem_reverse_next);
        virtmemLock();
        virtmemRemoveReservation(rv);
        virtmemUnlock();
        errno = ENOMEM;
        return MAP_FAILED;
      }
      memset(fastmem_pages, 0xff, FASTMEM_PAGE_COUNT * sizeof(*fastmem_pages));
      memset(fastmem_reverse_next, 0xff,
             FASTMEM_PAGE_COUNT * sizeof(*fastmem_reverse_next));
    }

    int tracked = 0;
    mutexLock(&g_mmap_lock);
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
      if (!g_mmaps[i].dst) {
        g_mmaps[i] = (MmapRegion){
          .dst = base,
          .size = len,
          .is_code = 2,
          .rv = rv,
          .code_handle = INVALID_HANDLE,
        };
        tracked = 1;
        break;
      }
    }
    mutexUnlock(&g_mmap_lock);
    if (!tracked) {
      free(fastmem_pages);
      free(fastmem_reverse_next);
      virtmemLock();
      virtmemRemoveReservation(rv);
      virtmemUnlock();
      errno = ENOMEM;
      return MAP_FAILED;
    }

    if (fastmem_pages) {
      __atomic_store_n(&g_fastmem_base, (uintptr_t)base, __ATOMIC_RELEASE);
      __atomic_store_n(&g_fastmem_reverse_next, fastmem_reverse_next,
                       __ATOMIC_RELEASE);
      __atomic_store_n(&g_fastmem_pages, fastmem_pages, __ATOMIC_RELEASE);

    }

    return base;
  }

  if (!(prot & PROT_EXEC)) {
    void *p = memalign(MMAP_PAGE, len);
    if (!p) {  return MAP_FAILED; }
    if (prot == PROT_NONE) { /* leave committed; PROT_NONE rarely enforced */ }
    int tracked = 0;
    mutexLock(&g_mmap_lock);
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
      if (!g_mmaps[i].dst) {
        g_mmaps[i] = (MmapRegion){
          .dst = p,
          .size = len,
          .code_handle = INVALID_HANDLE,
        };
        tracked = 1;
        break;
      }
    }
    mutexUnlock(&g_mmap_lock);
    if (!tracked) {
      free(p);
      errno = ENOMEM;
      return MAP_FAILED;
    }
    return p;
  }

  void *back = memalign(MMAP_PAGE, len);
  if (!back) {

    return MAP_FAILED;
  }
  Handle ch = INVALID_HANDLE;
  Result rcC = svcCreateCodeMemory(&ch, back, len);
  if (R_FAILED(rcC)) {

    free(back);
    return MAP_FAILED;
  }
  virtmemLock();
  void *rw = virtmemFindCodeMemory(len, MMAP_PAGE);
  VirtmemReservation *rvw = rw ? virtmemAddReservation(rw, len) : NULL;
  void *rx = virtmemFindCodeMemory(len, MMAP_PAGE);
  VirtmemReservation *rvx = rx ? virtmemAddReservation(rx, len) : NULL;
  virtmemUnlock();
  if (!rw || !rx || !rvw || !rvx) {
    remove_reservation(&rvw);
    remove_reservation(&rvx);
    svcCloseHandle(ch);
    free(back);
    errno = ENOMEM;
    return MAP_FAILED;
  }

  Result rcO = svcControlCodeMemory(ch, CodeMapOperation_MapOwner, rw, len, Perm_Rw);
  Result rcS = R_SUCCEEDED(rcO)
                   ? svcControlCodeMemory(ch, CodeMapOperation_MapSlave, rx, len, Perm_Rx)
                   : rcO;

  if (R_FAILED(rcO) || R_FAILED(rcS)) {
    if (R_SUCCEEDED(rcO))
      svcControlCodeMemory(ch, CodeMapOperation_UnmapOwner, rw, len, 0);
    svcCloseHandle(ch);
    remove_reservation(&rvw);
    remove_reservation(&rvx);
    free(back);
    return MAP_FAILED;
  }
  // Writes to the RX view are redirected to the RW alias by the exception handler.
  MmapRegion region = {
    .dst = rx,
    .src = back,
    .size = len,
    .is_code = 1,
    .rv = rvw,
    .code_rx = rx,
    .code_rw = rw,
    .code_rx_rv = rvx,
    .code_handle = ch,
  };
  int tracked = 0;
  mutexLock(&g_mmap_lock);
  int mmap_index = -1;
  int redirect_index = -1;
  for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
    if (!g_mmaps[i].dst) {
      mmap_index = i;
      break;
    }
  }
  for (int i = 0; i < MAX_JIT_REDIRECTS; i++) {
    if (!__atomic_load_n(&g_jit_redir[i].lo, __ATOMIC_ACQUIRE)) {
      redirect_index = i;
      break;
    }
  }
  if (mmap_index >= 0 && redirect_index >= 0) {
    g_mmaps[mmap_index] = region;
    __atomic_store_n(&g_jit_redir[redirect_index].hi, (uintptr_t)rx + len,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_jit_redir[redirect_index].delta,
                     (intptr_t)((char *)rw - (char *)rx), __ATOMIC_RELAXED);
    __atomic_store_n(&g_jit_redir[redirect_index].lo, (uintptr_t)rx, __ATOMIC_RELEASE);
    tracked = 1;
  }
  mutexUnlock(&g_mmap_lock);
  if (!tracked) {
    mutexLock(&g_mmap_lock);
    release_jit_region_locked(&region);
    mutexUnlock(&g_mmap_lock);

    errno = ENOMEM;
    return MAP_FAILED;
  }
  return rx;
}

int mprotect_fake(void *addr, size_t length, int prot) {
  const size_t len = (length + MMAP_PAGE - 1) & ~(size_t)(MMAP_PAGE - 1);

  for (int i = 0; i < MAX_ASHMEM; i++) {
    AshmemRegion *region = &g_ashmem[i];
    const uintptr_t start = (uintptr_t)addr;
    if (region->used != 1 || !region->alias_backed || start < (uintptr_t)region->ptr ||
        len > region->size || start - (uintptr_t)region->ptr > region->size - len)
      continue;

    const u32 permission = (prot & PROT_WRITE) ? Perm_Rw
                           : (prot & PROT_READ) ? Perm_R
                                                : Perm_None;
    if (!(prot & PROT_WRITE) && g_fastmem_pages && region->reverse_heads) {
      mutexLock(&g_fastmem_lock);
      const Result alias_rc = fastmem_protect_backing_locked(
          region, start - (uintptr_t)region->ptr, len);
      mutexUnlock(&g_fastmem_lock);
      if (R_FAILED(alias_rc)) {
        errno = EACCES;
        return -1;
      }
    }
    Result rc = svcSetMemoryPermission((void *)start, len, permission);
    if (R_FAILED(rc)) {
      errno = EACCES;
      return -1;
    }
    return 0;
  }

  if (g_fastmem_pages && fastmem_contains((uintptr_t)addr, len)) {
    const uintptr_t start = (uintptr_t)addr;
    const size_t first = (size_t)((start - g_fastmem_base) / MMAP_PAGE);
    const size_t count = len / MMAP_PAGE;
    const int writable = (prot & PROT_WRITE) != 0;

    mutexLock(&g_fastmem_lock);
    for (size_t i = 0; i < count; i++) {
      uint32_t entry = __atomic_load_n(&g_fastmem_pages[first + i], __ATOMIC_ACQUIRE);
      if (entry == FASTMEM_ENTRY_EMPTY)
        continue;

      size_t source_offset;
      AshmemRegion *region = fastmem_entry_region(entry, &source_offset);
      if (!region) {
        mutexUnlock(&g_fastmem_lock);
        errno = EINVAL;
        return -1;
      }

      if (writable) {
        if (!(entry & FASTMEM_ENTRY_PROTECTED))
          continue;

        const int remap = (entry & FASTMEM_ENTRY_REMAP) != 0;
        uint32_t next = (entry | FASTMEM_ENTRY_LAZY) &
                        ~(FASTMEM_ENTRY_PROTECTED | FASTMEM_ENTRY_REMAP);
        __atomic_store_n(&g_fastmem_pages[first + i], next, __ATOMIC_RELEASE);
        if (!remap)
          continue;
        if (g_fastmem_live_pages >= FASTMEM_MAPPED_PAGE_LIMIT) {
          continue;
        }
        fastmem_map_run_locked(first + i, 1, region, source_offset);
        continue;
      }

      if (entry & FASTMEM_ENTRY_PROTECTED)
        continue;
      if (entry & FASTMEM_ENTRY_LAZY) {
        __atomic_store_n(&g_fastmem_pages[first + i],
                         entry | FASTMEM_ENTRY_PROTECTED, __ATOMIC_RELEASE);
        continue;
      }

      const uintptr_t destination = start + i * MMAP_PAGE;
      const uintptr_t source = (uintptr_t)region->ptr + source_offset;
      const Result rc = svcUnmapProcessMemory((void *)destination,
                                              envGetOwnProcessHandle(), source,
                                              MMAP_PAGE);
      if (R_FAILED(rc)) {
        mutexUnlock(&g_fastmem_lock);
        errno = EACCES;
        return -1;
      }
      if (g_fastmem_live_pages)
        g_fastmem_live_pages--;
      __atomic_store_n(&g_fastmem_pages[first + i],
                       entry | FASTMEM_ENTRY_LAZY | FASTMEM_ENTRY_PROTECTED |
                           FASTMEM_ENTRY_REMAP,
                       __ATOMIC_RELEASE);
    }
    mutexUnlock(&g_fastmem_lock);
    return 0;
  }

  return 0;
}

int munmap_fake(void *addr, size_t length) {
  const size_t len = (length + MMAP_PAGE - 1) & ~(size_t)(MMAP_PAGE - 1);
  uint32_t *released_fastmem_pages = NULL;
  uint32_t *released_fastmem_reverse_next = NULL;
  if (g_fastmem_pages && fastmem_contains((uintptr_t)addr, len)) {
    mutexLock(&g_fastmem_lock);
    if (R_FAILED(fastmem_unmap_range_locked((uintptr_t)addr, len, 1))) {
      mutexUnlock(&g_fastmem_lock);
      errno = EINVAL;
      return -1;
    }
    if ((uintptr_t)addr == g_fastmem_base && len == FASTMEM_AREA_SIZE) {
      released_fastmem_pages = g_fastmem_pages;
      released_fastmem_reverse_next = g_fastmem_reverse_next;
      __atomic_store_n(&g_fastmem_pages, NULL, __ATOMIC_RELEASE);
      __atomic_store_n(&g_fastmem_reverse_next, NULL, __ATOMIC_RELEASE);
      __atomic_store_n(&g_fastmem_base, 0, __ATOMIC_RELEASE);
      g_fastmem_live_pages = 0;
    }
    mutexUnlock(&g_fastmem_lock);
    if (!released_fastmem_pages)
      return 0;
  }

  mutexLock(&g_mmap_lock);
  for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
    if (g_mmaps[i].dst == addr) {
      MmapRegion *region = &g_mmaps[i];
      if (region->is_code == 1) {
        const Result rc = release_jit_region_locked(region);
        if (R_FAILED(rc)) {
          mutexUnlock(&g_mmap_lock);
          free(released_fastmem_pages);
          free(released_fastmem_reverse_next);
          errno = EBUSY;
          return -1;
        }
      } else if (region->is_code == 2) {
        remove_reservation(&region->rv);
        memset(region, 0, sizeof(*region));
      } else {
        free(region->dst);
        memset(region, 0, sizeof(*region));
      }
      mutexUnlock(&g_mmap_lock);
      free(released_fastmem_pages);
      free(released_fastmem_reverse_next);
      return 0;
    }
  }
  mutexUnlock(&g_mmap_lock);
  free(released_fastmem_pages);
  free(released_fastmem_reverse_next);
  return 0;
}

void libc_memory_shutdown(void) {
  mutexLock(&g_signal_lock);
  memset(g_signal_actions, 0, sizeof(g_signal_actions));
  memset(g_signal_action_set, 0, sizeof(g_signal_action_set));
  mutexUnlock(&g_signal_lock);

  const uintptr_t fastmem_base = __atomic_load_n(&g_fastmem_base, __ATOMIC_ACQUIRE);
  uint32_t *fastmem_pages = __atomic_load_n(&g_fastmem_pages, __ATOMIC_ACQUIRE);
  uint32_t *fastmem_reverse_next =
      __atomic_load_n(&g_fastmem_reverse_next, __ATOMIC_ACQUIRE);
  int fastmem_released = 1;
  if (fastmem_base || fastmem_pages || fastmem_reverse_next) {
    fastmem_released = 0;
    if (fastmem_base && fastmem_pages && fastmem_reverse_next) {
      mutexLock(&g_fastmem_lock);
      const Result rc = fastmem_unmap_range_locked(fastmem_base,
                                                    FASTMEM_AREA_SIZE, 1);
      if (R_SUCCEEDED(rc)) {
        __atomic_store_n(&g_fastmem_pages, NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&g_fastmem_reverse_next, NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&g_fastmem_base, 0, __ATOMIC_RELEASE);
        g_fastmem_live_pages = 0;
        fastmem_released = 1;
      }
      mutexUnlock(&g_fastmem_lock);
      if (fastmem_released) {
        free(fastmem_pages);
        free(fastmem_reverse_next);
      }
    }
  }

  mutexLock(&g_mmap_lock);
  for (int i = 0; i < MAX_JIT_REDIRECTS; i++)
    __atomic_store_n(&g_jit_redir[i].lo, 0, __ATOMIC_RELEASE);
  for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
    MmapRegion *region = &g_mmaps[i];
    if (!region->dst)
      continue;

    if (region->is_code == 1) {
      const Result rc = release_jit_region_locked(region);
      if (R_FAILED(rc))
        continue;
      continue;
    }

    if (region->is_code == 2) {
      if (!fastmem_released && (uintptr_t)region->dst == fastmem_base)
        continue;
      remove_reservation(&region->rv);
    } else {
      free(region->dst);
    }
    memset(region, 0, sizeof(*region));
  }
  memset(g_jit_redir, 0, sizeof(g_jit_redir));
  mutexUnlock(&g_mmap_lock);

  mutexLock(&g_ashmem_lock);
  for (int i = 0; i < MAX_ASHMEM; i++) {
    AshmemRegion *region = &g_ashmem[i];
    if (region->used != 1)
      continue;
    if (region->alias_backed) {
      if (!fastmem_released)
        continue;
      const Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
                                                   (u64)region->ptr,
                                                   (u64)region->raw,
                                                   region->size);
      if (R_FAILED(rc)) {
        continue;
      }
      remove_reservation(&region->reservation);
    }
    free(region->reverse_heads);
    free(region->raw);
    memset(region, 0, sizeof(*region));
  }
  mutexUnlock(&g_ashmem_lock);
}
