#pragma once
// Minimal linux/udp.h shim for MinGW (srsRAN pcap.c only needs struct udphdr).
#include <stdint.h>
struct udphdr {
  uint16_t source;
  uint16_t dest;
  uint16_t len;
  uint16_t check;
};