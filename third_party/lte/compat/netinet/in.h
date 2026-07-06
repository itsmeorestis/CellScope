#pragma once
// Minimal POSIX shim for vendored srsRAN net headers on MinGW.
// The actual socket .c files are excluded from the build; only the struct
// types are needed so aggregating headers (srsran.h) parse cleanly.
#include <stdint.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
  in_addr_t s_addr;
};

struct sockaddr_in {
  short          sin_family;
  in_port_t      sin_port;
  struct in_addr sin_addr;
  char           sin_zero[8];
};
