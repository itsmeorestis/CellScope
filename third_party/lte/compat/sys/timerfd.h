#pragma once
// Minimal sys/timerfd.h stub for MinGW. srsRAN's periodic_thread uses timerfd;
// CellScope drives the LTE engine from its own loop, so these stubs (which
// report failure) are never exercised on the hot path.
#include <time.h>
#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC 0
#endif
#ifdef __cplusplus
extern "C" {
#endif
static inline int timerfd_create(int clockid, int flags) { (void)clockid; (void)flags; return -1; }
static inline int timerfd_settime(int fd, int flags, const struct itimerspec* nv, struct itimerspec* ov)
{ (void)fd; (void)flags; (void)nv; (void)ov; return -1; }
#ifdef __cplusplus
}
#endif