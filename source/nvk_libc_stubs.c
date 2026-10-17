/* nvk_libc_stubs.c -- POSIX entry points Mesa needs that Horizon's newlib lacks.
 *
 * libnvk_support.a from the nxvk portlib package pulls in Mesa's disk cache,
 * drirc XML config and a few util paths which were written for a POSIX host.
 * Linking it against devkitA64 leaves these fifteen undefined:
 *
 *   dirfd flock fstatat getegid geteuid getgid getpwuid_r getuid
 *   posix_memalign pthread_sigmask regcomp regexec regfree sysconf writev
 *
 * None of them has a meaning on Horizon: there is one user, no signals, no
 * advisory file locks, no drirc file to parse and no <regex.h>. They only have
 * to exist and behave so Mesa takes its graceful fallbacks -- the shader disk
 * cache stays off unless the app turns it on through MESA_SHADER_CACHE_PATH,
 * which is exactly what switch/docs/bringing-an-application.md documents.
 *
 * Only compiled for the pkg-config link path: the vendored 23-archive tree
 * carries its own stubs for these, so defining them there would collide.
 *
 * Types: <regex.h>, <pwd.h> and <sys/uio.h> are not guaranteed to exist in a
 * devkitA64 sysroot, so each is probed with __has_include and a local,
 * ABI-compatible declaration is used when it is missing. Only the symbol name
 * matters at link time and the stubs never dereference those objects.
 */
#ifdef NETHERSX2_NVK_ICD_ONLY

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <signal.h> /* sigset_t */
#include <switch.h> /* svcGetInfo, InfoType_*, CUR_PROCESS_HANDLE, R_FAILED */

#ifdef NETHERSX2_VK_DIAGNOSTIC
#include "hooks/vk.h" /* vk_diag_note; this whole file is the VK link path */
#endif

/* NVK_STUBS_NO_SYS_HEADERS forces the fallback declarations even on a host that
 * has these headers, which is the only way to compile-check the branch a
 * devkitA64 sysroot without <regex.h>/<pwd.h>/<sys/uio.h> actually takes. */
#if defined(__has_include) && !defined(NVK_STUBS_NO_SYS_HEADERS)
#if __has_include(<pwd.h>)
#include <pwd.h>
#define NVK_HAVE_PWD_H 1
#endif
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#define NVK_HAVE_UIO_H 1
#endif
#if __has_include(<regex.h>)
#include <regex.h>
#define NVK_HAVE_REGEX_H 1
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#endif

/* Horizon runs a single process as one user. Anything but a non-zero uid would
 * make Mesa's disk_cache_enabled() take its "running as root, refuse" branch. */
#define NVK_FAKE_UID 1000

uid_t getuid(void) { return NVK_FAKE_UID; }
uid_t geteuid(void) { return NVK_FAKE_UID; }
gid_t getgid(void) { return NVK_FAKE_UID; }
gid_t getegid(void) { return NVK_FAKE_UID; }

/* Mesa asks for the home directory to place the shader cache. Report "no such
 * user" so it falls back to MESA_SHADER_CACHE_PATH / XDG_CACHE_HOME instead of
 * writing somewhere unintended. */
#ifdef NVK_HAVE_PWD_H
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;
  return ENOENT;
}
#else
int getpwuid_r(unsigned int uid, void *pwd, char *buf, unsigned long buflen,
               void **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;
  return ENOENT;
}
#endif

/* NOT "unknown". Mesa's os_get_total_physical_memory() is
 * sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE), and nvk_physical_device_create()
 * feeds that through os_get_gpu_heap_size() to size NVK's only memory heap. When
 * it comes back 0 the driver fails with
 *
 *   vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
 *             "Failed to query total system memory")
 *
 * nvk_enumerate_physical_device() propagates that, vk_common_EnumeratePhysicalDevices()
 * returns it verbatim, and the core sees vkEnumeratePhysicalDevices() == -3 with
 * zero adapters -- so GS init dies before vkCreateDevice is ever called. Returning
 * -1 here looked harmless ("Mesa will take its graceful fallback") and instead
 * removed the GPU entirely.
 *
 * The honest figure on Horizon is what this process still has left: main.c's
 * __libnx_initheap() claims the newlib heap and deliberately leaves
 * GPU_RESERVE_MB unmapped for exactly this purpose, and NVK/nvmap allocates from
 * that remainder rather than from our heap. It also tracks reality over time --
 * nvk_get_sysmem_heap_available() calls this on every VK_EXT_memory_budget
 * query, so the reported budget shrinks as NVK allocates. */
uint64_t nvk_host_physical_bytes(void) {
  uint64_t total = 0, used = 0;

  if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))
      || total == 0)
    return 0;
  if (R_FAILED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))
    used = 0;
  if (total <= used)
    return 0;
  return total - used;
}

long sysconf(int name) {
#ifdef _SC_PAGESIZE
  if (name == _SC_PAGESIZE) return 4096;
#endif
#ifdef _SC_PAGE_SIZE
  if (name == _SC_PAGE_SIZE) return 4096;
#endif
#ifdef _SC_PHYS_PAGES
  if (name == _SC_PHYS_PAGES) {
    const uint64_t bytes = nvk_host_physical_bytes();
    const long pages = bytes >= 4096 ? (long)(bytes / 4096) : -1L;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    /* Once: NVK re-queries this for every memory-budget report. */
    static int logged;
    if (!logged) {
      logged = 1;
      vk_diag_note("sysconf(_SC_PHYS_PAGES): process free=%llu B -> %ld pages "
                   "(NVK takes %d%% of that as its heap size)",
                   (unsigned long long)bytes, pages,
                   bytes <= 1024ull * 1024 * 1024 ? 25 :
                   bytes <= 4096ull * 1024 * 1024 ? 50 : 75);
    }
#endif
    if (pages > 0) return pages;
  }
#endif
  errno = EINVAL;
  return -1;
}

/* No file descriptor backs a DIR stream on Horizon, and nothing contends for
 * the cache directory, so both are reported as "nothing to do, succeeded". */
int dirfd(DIR *dirp) { (void)dirp; return -1; }
int flock(int fd, int operation) { (void)fd; (void)operation; return 0; }

/* dirfd is always -1 above, so every caller is effectively path-relative. */
int fstatat(int fd, const char *path, struct stat *st, int flags) {
  (void)fd;
  if (!path || !st) { errno = EFAULT; return -1; }
#ifdef AT_SYMLINK_NOFOLLOW
  if ((flags & AT_SYMLINK_NOFOLLOW) && lstat(path, st) == 0) return 0;
#else
  (void)flags;
#endif
  return stat(path, st);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (!memptr) return EINVAL;
  if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
  /* C11 aligned_alloc wants size to be a multiple of alignment. */
  size_t rounded = (size + alignment - 1) & ~(alignment - 1);
  void *p = aligned_alloc(alignment, rounded ? rounded : alignment);
  if (!p) return ENOMEM;
  *memptr = p;
  return 0;
}

/* Signals do not exist on Horizon; Mesa only masks them around thread setup. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set;
  if (oldset) memset(oldset, 0, sizeof(*oldset));
  return 0;
}

#ifdef NVK_HAVE_UIO_H
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
#else
struct iovec { void *iov_base; size_t iov_len; };
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
#endif
  if (!iov || iovcnt < 0) { errno = EFAULT; return -1; }
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) return total ? total : -1;
    total += n;
    if ((size_t)n < iov[i].iov_len) break; /* short write: nothing more fits */
  }
  return total;
}

/* drirc pattern matching. There is no config file to read on Horizon, so a
 * pattern never compiles and never matches; Mesa then applies no overrides. */
#ifndef NVK_HAVE_REGEX_H
typedef struct { void *opaque; } regex_t;
typedef struct { size_t rm_so; size_t rm_eo; } regmatch_t;
#define NVK_REG_NOMATCH 1
#else
#define NVK_REG_NOMATCH REG_NOMATCH
#endif

int regcomp(regex_t *preg, const char *regex, int cflags) {
  (void)preg; (void)regex; (void)cflags;
  return NVK_REG_NOMATCH;
}
int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
  (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags;
  return NVK_REG_NOMATCH;
}
void regfree(regex_t *preg) { (void)preg; }

#endif /* NETHERSX2_NVK_ICD_ONLY */
