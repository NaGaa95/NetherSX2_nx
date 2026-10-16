/* corelog.h -- the core's console, captured to the SD card. See corelog.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef NETHERSX2_CORELOG_H
#define NETHERSX2_CORELOG_H

#include <stdarg.h>

// Bionic priorities (android/log.h): 2 VERBOSE, 3 DEBUG, 4 INFO, 5 WARN,
// 6 ERROR, 7 FATAL. ERROR and above are flushed and fsynced immediately.
#define CORELOG_VERBOSE 2
#define CORELOG_DEBUG   3
#define CORELOG_INFO    4
#define CORELOG_WARN    5
#define CORELOG_ERROR   6
#define CORELOG_FATAL   7

// Truncate and start DATA_ROOT/nethersx2-core.log. Safe to call once per run.
void corelog_open(void);
void corelog_close(void);
void corelog_flush(void);

void corelog_vlog(int priority, const char *tag, const char *format, va_list args);
void corelog_log(int priority, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
void corelog_text(int priority, const char *tag, const char *text);

// Bionic calls this just before abort(); keep the text for corelog_death().
void corelog_abort_message(const char *message);

// One durable line explaining why the process is about to disappear, with
// called_from rendered as core+0xOFFSET when it lies inside libemucore.so.
void corelog_death(const char *reason, const void *called_from);

// For the libnx CPU exception handler: no mutex and no stdio, because it runs on
// the exception stack and the crashing thread may already hold both. Only a
// marker is needed -- every ERROR and FATAL line reached the card already, since
// corelog_emit_locked() flushes and fsyncs those as it writes them.
void corelog_crash_marker(unsigned long long pc, unsigned long long lr);

#endif // NETHERSX2_CORELOG_H
