#pragma once
// Minimal execinfo.h stub for MinGW (srsRAN backtrace.c). No stack unwinding;
// crash diagnostics simply produce empty backtraces on Windows.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int backtrace(void** buffer, int size)
{
  (void)buffer;
  (void)size;
  return 0;
}

static inline char** backtrace_symbols(void* const* buffer, int size)
{
  (void)buffer;
  (void)size;
  return NULL;
}

static inline void backtrace_symbols_fd(void* const* buffer, int size, int fd)
{
  (void)buffer;
  (void)size;
  (void)fd;
}

#ifdef __cplusplus
}
#endif
