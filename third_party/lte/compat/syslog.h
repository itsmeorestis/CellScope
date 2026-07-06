#pragma once
// Minimal syslog.h stub for MinGW. syslog is a Linux-only srslog sink that
// CellScope does not use; these no-op stubs let the sink compile/link.
#include <stdarg.h>

#define LOG_CONS    0x02
#define LOG_NDELAY  0x08
#define LOG_PID     0x01

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_SYSLOG  (5 << 3)
#define LOG_LOCAL0  (16 << 3)
#define LOG_LOCAL1  (17 << 3)
#define LOG_LOCAL2  (18 << 3)
#define LOG_LOCAL3  (19 << 3)
#define LOG_LOCAL4  (20 << 3)
#define LOG_LOCAL5  (21 << 3)
#define LOG_LOCAL6  (22 << 3)
#define LOG_LOCAL7  (23 << 3)

#ifdef __cplusplus
extern "C" {
#endif

static inline void openlog(const char* ident, int option, int facility)
{
  (void)ident;
  (void)option;
  (void)facility;
}

static inline void closelog(void) {}

static inline void syslog(int priority, const char* format, ...)
{
  (void)priority;
  (void)format;
}

#ifdef __cplusplus
}
#endif
