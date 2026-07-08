#include "lte_diag.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#else
#include <csignal>
#include <ctime>
#include <execinfo.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace cellscope::lte {

#if defined(_WIN32)

namespace {
std::mutex g_log_mtx;
FILE*      g_log = nullptr;

void ensure_open_locked()
{
    if (!g_log) {
        g_log = std::fopen("cellscope_lte.log", "a");
    }
}

void log_stack_locked(void* const* frames, unsigned short n)
{
    HANDLE  proc = GetCurrentProcess();
    HMODULE exe  = GetModuleHandleW(nullptr);
    MODULEINFO mi = {};
    GetModuleInformation(proc, exe, &mi, sizeof(mi));
    uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
    uintptr_t end  = base + mi.SizeOfImage;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    for (unsigned short i = 0; i < n; ++i) {
        DWORD64   addr = (DWORD64)frames[i];
        uintptr_t a    = (uintptr_t)frames[i];
        // For our own (GCC/DWARF) module, dbghelp can't symbolize; log an
        // RVA that resolves with: addr2line -f -C -e CellScope.exe 0x<140000000+RVA>
        if (a >= base && a < end) {
            std::fprintf(g_log, "    #%02u CellScope.exe+0x%llx  (addr2line 0x%llx)\n", i,
                         (unsigned long long)(a - base),
                         (unsigned long long)(0x140000000ULL + (a - base)));
            continue;
        }
        char  buf[sizeof(SYMBOL_INFO) + 256];
        auto* sym         = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 disp      = 0;
        if (SymFromAddr(proc, addr, &disp, sym)) {
            std::fprintf(g_log, "    #%02u %p  %s+0x%llx\n", i, (void*)addr, sym->Name,
                         (unsigned long long)disp);
        } else {
            std::fprintf(g_log, "    #%02u %p\n", i, (void*)addr);
        }
    }
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    ensure_open_locked();
    if (g_log) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        std::fprintf(g_log, "\n*** UNHANDLED EXCEPTION code=0x%08lx addr=%p ***\n",
                     (unsigned long)er->ExceptionCode, er->ExceptionAddress);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            std::fprintf(g_log, "    access violation %s address %p\n",
                         er->ExceptionInformation[0] ? "writing" : "reading",
                         (void*)er->ExceptionInformation[1]);
        }
        void*          frames[40];
        unsigned short nf = CaptureStackBackTrace(0, 40, frames, nullptr);
        log_stack_locked(frames, nf);
        std::fflush(g_log);
    }
    return EXCEPTION_EXECUTE_HANDLER; // log then terminate
}

void on_terminate()
{
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        ensure_open_locked();
        if (g_log) {
            std::fprintf(g_log, "\n*** std::terminate (uncaught exception in a thread) ***\n");
            std::fflush(g_log);
        }
    }
    try {
        std::exception_ptr e = std::current_exception();
        if (e) {
            std::rethrow_exception(e);
        }
    } catch (const std::exception& ex) {
        diagLog(std::string("  what(): ") + ex.what());
    } catch (...) {
        diagLog("  unknown (non-std) exception");
    }
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        if (g_log) {
            void*          frames[40];
            unsigned short nf = CaptureStackBackTrace(0, 40, frames, nullptr);
            log_stack_locked(frames, nf);
            std::fflush(g_log);
        }
    }
    std::abort();
}
} // namespace

void diagInstallCrashHandler()
{
    static bool installed = false;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    if (installed) {
        return;
    }
    installed = true;
    SetUnhandledExceptionFilter(crash_filter);
    std::set_terminate(on_terminate);
}

void diagLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    ensure_open_locked();
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, msg.c_str());
        std::fflush(g_log);
    }
}

#else // ---------------------------------------------------------------- POSIX

namespace {
std::mutex g_log_mtx;
FILE*      g_log = nullptr;

void ensure_open_locked()
{
    if (!g_log) {
        g_log = std::fopen("cellscope_lte.log", "a");
    }
}

void log_stack_locked()
{
    void* frames[40];
    int   n = backtrace(frames, 40);
    // backtrace_symbols() allocates; safe here because the crash/terminate
    // handlers log then abort (best-effort diagnostics, not signal-async-safe).
    char** syms = backtrace_symbols(frames, n);
    for (int i = 0; i < n; ++i) {
        if (syms && syms[i]) {
            std::fprintf(g_log, "    #%02d %s\n", i, syms[i]);
        } else {
            std::fprintf(g_log, "    #%02d %p\n", i, frames[i]);
        }
    }
    std::free(syms);
}

void crash_handler(int sig)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    ensure_open_locked();
    if (g_log) {
        std::fprintf(g_log, "\n*** FATAL SIGNAL %d ***\n", sig);
        log_stack_locked();
        std::fflush(g_log);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void on_terminate()
{
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        ensure_open_locked();
        if (g_log) {
            std::fprintf(g_log, "\n*** std::terminate (uncaught exception in a thread) ***\n");
            std::fflush(g_log);
        }
    }
    try {
        std::exception_ptr e = std::current_exception();
        if (e) {
            std::rethrow_exception(e);
        }
    } catch (const std::exception& ex) {
        diagLog(std::string("  what(): ") + ex.what());
    } catch (...) {
        diagLog("  unknown (non-std) exception");
    }
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        if (g_log) {
            log_stack_locked();
            std::fflush(g_log);
        }
    }
    std::abort();
}
} // namespace

void diagInstallCrashHandler()
{
    static bool installed = false;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    if (installed) {
        return;
    }
    installed = true;
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGFPE, crash_handler);
    std::signal(SIGILL, crash_handler);
    std::signal(SIGBUS, crash_handler);
    std::set_terminate(on_terminate);
}

void diagLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_log_mtx);
    ensure_open_locked();
    if (g_log) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tmv;
        localtime_r(&ts.tv_sec, &tmv);
        std::fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                     (int)(ts.tv_nsec / 1000000), msg.c_str());
        std::fflush(g_log);
    }
}

#endif // _WIN32

} // namespace cellscope::lte
