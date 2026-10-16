/* corelog.c -- the core's console, captured to the SD card.
 *
 * libemucore.so has exactly one output channel: Bionic's logcat. Every
 * Console.Error and Log::Write in PCSX2 lands in __android_log_print, and every
 * stdout/stderr write lands in a fake FILE that libc_shim.c absorbs. Horizon has
 * no console, so all of it was silently discarded -- which is why a GS that
 * fails to initialise dies with no message anywhere and no way to tell which of
 * a dozen checks tripped.
 *
 * This module gives the core its voice back: one file under DATA_ROOT, fully
 * buffered so steady-state logging costs nothing, flushed and fsynced when the
 * core reports an error or is about to die. The abort path records the caller as
 * core+0xOFFSET, which is enough to find the faulting instruction in
 * libemucore.so without a debugger.
 *
 * Always compiled and always on: an abort() that throws away its own reason is a
 * defect in any build, and the file is capped so it cannot fill the card.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "corelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"   // DATA_ROOT
#include "so_util.h"  // so_module

// libemucore.so, loaded by main.c (same pattern as dl_emu.c).
extern so_module emu_mod;

#define CORELOG_PATH   DATA_ROOT "/nethersx2-core.log"
#define CORELOG_BUFFER (64 * 1024)
#define CORELOG_LIMIT  (2 * 1024 * 1024) // stop rather than fill the card
#define CORELOG_LINE   1024

static const char *const corelog_priority[] = {
  "?", "?", "V", "D", "I", "W", "E", "F", "S"
};

// Zero-initialised libnx Mutex is unlocked; hooks/vk.c relies on the same.
static Mutex corelog_mutex;
static FILE *corelog_file;
static char  corelog_buffer[CORELOG_BUFFER];
static char  corelog_abort_text[512];
static long  corelog_written;

// Caller holds corelog_mutex.
static void
corelog_emit_locked(int priority, const char *tag, const char *text) {
  if (!corelog_file || corelog_written >= CORELOG_LIMIT)
    return;

  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  const char *level =
      (priority >= 0 && priority <= 8) ? corelog_priority[priority] : "?";
  const int has_tag = (tag && *tag);

  const int n = fprintf(corelog_file, "[%lld.%03lld] [%s] %s%s%s\n",
                        (long long)time.tv_sec,
                        (long long)(time.tv_nsec / 1000000),
                        level, has_tag ? tag : "", has_tag ? ": " : "", text);
  if (n > 0)
    corelog_written += n;

  // Errors, fatals and a full buffer must survive whatever happens next.
  if (priority >= CORELOG_ERROR) {
    fflush(corelog_file);
    fsync(fileno(corelog_file));
  }

  if (corelog_written >= CORELOG_LIMIT) {
    fprintf(corelog_file, "-- core log stopped at %ld bytes --\n",
            corelog_written);
    fflush(corelog_file);
    fsync(fileno(corelog_file));
    fclose(corelog_file);
    corelog_file = NULL;
  }
}

// Render an address as core+0xOFFSET when it falls inside libemucore.so.
static void
corelog_format_address(char *out, size_t size, const void *address) {
  const uintptr_t a = (uintptr_t)address;
  const uintptr_t base = (uintptr_t)emu_mod.load_base;

  if (base && a >= base && a < base + emu_mod.load_size)
    snprintf(out, size, "core+0x%llx", (unsigned long long)(a - base));
  else
    snprintf(out, size, "0x%llx", (unsigned long long)a);
}

void
corelog_open(void) {
  mutexLock(&corelog_mutex);
  if (corelog_file) {
    fclose(corelog_file);
    corelog_file = NULL;
  }
  corelog_written = 0;
  corelog_abort_text[0] = '\0';

  mkdir(DATA_ROOT, 0777);
  corelog_file = fopen(CORELOG_PATH, "w");
  if (corelog_file) {
    setvbuf(corelog_file, corelog_buffer, _IOFBF, sizeof(corelog_buffer));
    fprintf(corelog_file, "NetherSX2 core console\n");
    corelog_written = 24;
    fflush(corelog_file);
    fsync(fileno(corelog_file));
  }
  mutexUnlock(&corelog_mutex);
}

void
corelog_flush(void) {
  mutexLock(&corelog_mutex);
  if (corelog_file) {
    fflush(corelog_file);
    fsync(fileno(corelog_file));
  }
  mutexUnlock(&corelog_mutex);
}

void
corelog_close(void) {
  mutexLock(&corelog_mutex);
  if (corelog_file) {
    fflush(corelog_file);
    fsync(fileno(corelog_file));
    fclose(corelog_file);
    corelog_file = NULL;
  }
  mutexUnlock(&corelog_mutex);
}

void
corelog_vlog(int priority, const char *tag, const char *format, va_list args) {
  char line[CORELOG_LINE];
  if (!format)
    return;
  vsnprintf(line, sizeof(line), format, args);

  mutexLock(&corelog_mutex);
  corelog_emit_locked(priority, tag, line);
  mutexUnlock(&corelog_mutex);
}

void
corelog_log(int priority, const char *tag, const char *format, ...) {
  va_list args;
  va_start(args, format);
  corelog_vlog(priority, tag, format, args);
  va_end(args);
}

void
corelog_text(int priority, const char *tag, const char *text) {
  mutexLock(&corelog_mutex);
  corelog_emit_locked(priority, tag, text ? text : "(null)");
  mutexUnlock(&corelog_mutex);
}

void
corelog_abort_message(const char *message) {
  if (!message)
    return;
  mutexLock(&corelog_mutex);
  snprintf(corelog_abort_text, sizeof(corelog_abort_text), "%s", message);
  mutexUnlock(&corelog_mutex);
}

void
corelog_death(const char *reason, const void *called_from) {
  char where[64];
  char line[CORELOG_LINE];
  corelog_format_address(where, sizeof(where), called_from);

  mutexLock(&corelog_mutex);
  snprintf(line, sizeof(line), "*** core terminated: %s, called from %s%s%s ***",
           reason ? reason : "(unknown reason)", where,
           corelog_abort_text[0] ? ", abort message: " : "",
           corelog_abort_text[0] ? corelog_abort_text : "");
  corelog_emit_locked(CORELOG_FATAL, "NetherSX2", line);
  mutexUnlock(&corelog_mutex);
}

void
corelog_crash_marker(unsigned long long pc, unsigned long long lr) {
  char pc_where[64], lr_where[64];
  corelog_format_address(pc_where, sizeof(pc_where), (const void *)(uintptr_t)pc);
  corelog_format_address(lr_where, sizeof(lr_where), (const void *)(uintptr_t)lr);

  char line[256];
  const int length = snprintf(line, sizeof(line),
      "[F] [NetherSX2]: *** CPU exception, pc=%s lr=%s ***\n", pc_where, lr_where);
  if (length <= 0)
    return;

  // Raw and mutex-free: this runs on the exception stack, and the crashing
  // thread may itself be inside corelog_emit_locked().
  const int fd = open(CORELOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd >= 0) {
    (void)write(fd, line, (size_t)length < sizeof(line) ? (size_t)length
                                                        : sizeof(line) - 1);
    fsync(fd);
    close(fd);
  }
}
