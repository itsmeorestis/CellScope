#pragma once
// CellScope: central force-included compat shim for building vendored srsRAN /
// FALCON C/C++ sources under MinGW-w64. Keep this minimal and portable.

// C11 <threads.h> time base constant is not always visible in MinGW's <time.h>
// even though timespec_get() is declared.
#include <time.h>
#ifndef TIME_UTC
#define TIME_UTC 1
#endif

// MinGW's CRT has neither posix_memalign nor C11 aligned_alloc, and srsRAN
// frees these buffers with plain free() (so _aligned_malloc is unusable).
// The manual SIMD kernels (LV_HAVE_*) are disabled in this build, so the
// autovectorized code only uses unaligned moves and 16-byte malloc alignment
// is sufficient for correctness. NOTE: revisit if manual SIMD is enabled.
#ifdef __cplusplus
extern "C" {
#endif
#include <stdlib.h>
#include <errno.h>
#ifndef CS_HAVE_POSIX_MEMALIGN
#define CS_HAVE_POSIX_MEMALIGN 1
static inline int posix_memalign(void** memptr, size_t alignment, size_t size)
{
  (void)alignment;
  // Add generous trailing slack. Real posix_memalign/glibc rounds allocations
  // up to alignment/chunk boundaries, leaving slack that srsRAN's vectorized
  // (auto-vectorized at -O3 -march=native) kernels write into past the logical
  // end. A plain malloc(size) has no such slack, so an over-write corrupts the
  // NEXT heap block. 64 bytes covers the widest SIMD store. malloc() is 16-byte
  // aligned on x86-64, which satisfies the (SIMD-disabled) alignment needs, and
  // the result stays free()-compatible.
  void* p = malloc(size + 64);
  if (!p) {
    return ENOMEM;
  }
  *memptr = p;
  return 0;
}
#endif
#ifdef __cplusplus
}
#endif

// ---- POSIX gaps used by srsRAN common/threads.c on MinGW ----
#include <string.h>
#ifndef bzero
#define bzero(ptr, len) memset((ptr), 0, (len))
#endif
// BSD index()/rindex() (srsRAN phy/io/filesource.c) are strchr()/strrchr().
#ifndef index
#define index(s, c)  strchr((s), (c))
#endif
#ifndef rindex
#define rindex(s, c) strrchr((s), (c))
#endif

// <endian.h> host<->little/big helpers are absent on MinGW. x86-64 is
// little-endian, so LE conversions are identity and BE use bswap builtins.
#include <stdint.h>
#ifndef htole16
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
#define htobe16(x) __builtin_bswap16((uint16_t)(x))
#define htobe32(x) __builtin_bswap32((uint32_t)(x))
#define htobe64(x) __builtin_bswap64((uint64_t)(x))
#define be16toh(x) __builtin_bswap16((uint16_t)(x))
#define be32toh(x) __builtin_bswap32((uint32_t)(x))
#define be64toh(x) __builtin_bswap64((uint64_t)(x))
#endif

// CPU-affinity is Linux-only; CellScope does not pin threads. Provide no-op
// cpu_set_t machinery so srsRAN's affinity helpers compile.
#include <pthread.h>
#ifndef CS_HAVE_CPU_SET
#define CS_HAVE_CPU_SET 1
typedef struct {
  unsigned long __bits[16];
} cs_cpu_set_t;
#define cpu_set_t cs_cpu_set_t
#define CPU_SETSIZE 1024
#define CPU_ZERO(s)     memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(c, s)   ((void)(c), (void)(s))
#define CPU_CLR(c, s)   ((void)(c), (void)(s))
#define CPU_ISSET(c, s) (0)
#ifdef __cplusplus
extern "C" {
#endif
static inline int pthread_attr_setaffinity_np(pthread_attr_t* a, size_t sz, const cpu_set_t* s)
{ (void)a; (void)sz; (void)s; return 0; }
static inline int pthread_getaffinity_np(pthread_t t, size_t sz, cpu_set_t* s)
{ (void)t; (void)sz; (void)s; return 0; }
static inline int pthread_setaffinity_np(pthread_t t, size_t sz, const cpu_set_t* s)
{ (void)t; (void)sz; (void)s; return 0; }
#ifdef __cplusplus
}
#endif
#endif

// C++ gaps for MinGW when building srsRAN's bundled fmt / srslog.
#ifdef __cplusplus
#include <array>   // bundled fmt/core.h uses std::array without including it
#include <ctime>
#ifndef CS_HAVE_LOCALTIME_R
#define CS_HAVE_LOCALTIME_R 1
static inline struct tm* localtime_r(const time_t* t, struct tm* out)
{
  if (localtime_s(out, t) == 0) {
    return out;
  }
  return 0;
}
#ifndef gmtime_r
static inline struct tm* gmtime_r(const time_t* t, struct tm* out)
{
  if (gmtime_s(out, t) == 0) {
    return out;
  }
  return 0;
}
#endif
#endif
#endif
