#ifndef SRSRAN_VERSION_H
#define SRSRAN_VERSION_H

// Vendored static version for CellScope (srsRAN2 / FALCON fork).
#define SRSRAN_VERSION_MAJOR 21
#define SRSRAN_VERSION_MINOR 10
#define SRSRAN_VERSION_PATCH 0
#define SRSRAN_VERSION_STRING "21.10.0"

#define SRSRAN_VERSION_ENCODE(major, minor, patch) ( \
    ((major) * 10000)                                \
  + ((minor) *   100)                                \
  + ((patch) *     1))

#define SRSRAN_VERSION SRSRAN_VERSION_ENCODE( \
  SRSRAN_VERSION_MAJOR,                       \
  SRSRAN_VERSION_MINOR,                       \
  SRSRAN_VERSION_PATCH)

#define SRSRAN_VERSION_CHECK(major,minor,patch)    \
  (SRSRAN_VERSION >= SRSRAN_VERSION_ENCODE(major,minor,patch))

#include "srsran/config.h"

SRSRAN_API char* srsran_get_version();
SRSRAN_API int   srsran_get_version_major();
SRSRAN_API int   srsran_get_version_minor();
SRSRAN_API int   srsran_get_version_patch();
SRSRAN_API int   srsran_check_version(int major, int minor, int patch);

#endif // SRSRAN_VERSION_H
