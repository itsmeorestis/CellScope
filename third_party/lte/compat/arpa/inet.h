#pragma once
#include <netinet/in.h>

// Network byte-order helpers as inline functions (scoped to translation units
// that include <arpa/inet.h>, so they don't collide with winsock's htonl
// declaration pulled in by fmt/windows.h). x86-64 is little-endian.
#include <stdint.h>
#ifndef CS_HAVE_NTOH
#define CS_HAVE_NTOH 1
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
#endif
