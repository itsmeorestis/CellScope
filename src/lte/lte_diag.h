// CellScope LTE diagnostics: crash handler + file logger. Kept in its own TU
// (no srsRAN headers) so it can include <windows.h>/<dbghelp.h> without the
// ERROR-macro clash against srsRAN's ERROR() logger.
#pragma once

#include <string>

namespace cellscope::lte {

// Installs a process-wide unhandled-exception filter + std::terminate handler
// that append a diagnostic (exception code, faulting address, symbolized stack)
// to cellscope_lte.log. Safe to call multiple times; only installs once.
void diagInstallCrashHandler();

// Append a timestamped line to cellscope_lte.log (thread-safe, flushed).
void diagLog(const std::string& msg);

} // namespace cellscope::lte
