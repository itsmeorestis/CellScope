#pragma once
// Minimal pwd.h shim for MinGW. srsRAN's dft_fftw.c uses getpwuid()->pw_dir
// only to locate an FFTW wisdom file in $HOME; returning NULL pw_dir makes it
// fall back to the cwd, which is fine for our use.
#include <sys/types.h>

struct passwd {
  char* pw_name;
  char* pw_dir;
};

static inline struct passwd* getpwuid(uid_t uid)
{
  (void)uid;
  return 0;
}
