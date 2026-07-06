#include "cs_lte_hook.h"
#include <atomic>

namespace cellscope {

namespace {
std::atomic<LteEventFn> g_fn{nullptr};
std::atomic<void*>      g_user{nullptr};
}

void setLteEventCb(LteEventFn fn, void* user)
{
    g_user.store(user);
    g_fn.store(fn);
}

void emitLteEvent(uint16_t rnti, const char* idType, const char* idValue, const char* msg)
{
    LteEventFn fn = g_fn.load();
    if (fn) {
        fn(g_user.load(), rnti, idType ? idType : "", idValue ? idValue : "", msg ? msg : "");
    }
}

namespace {
std::atomic<LteConstFn> g_const_fn{nullptr};
std::atomic<void*>      g_const_user{nullptr};
std::atomic<uint16_t>   g_const_rnti{0};
}

void setLteConstCb(LteConstFn fn, void* user)
{
    g_const_user.store(user);
    g_const_fn.store(fn);
}

void setLteConstRnti(uint16_t rnti)
{
    g_const_rnti.store(rnti);
}

void emitLteConst(uint16_t rnti, const float* iq, int nPairs)
{
    if (rnti != g_const_rnti.load() || rnti == 0) {
        return;
    }
    LteConstFn fn = g_const_fn.load();
    if (fn && iq && nPairs > 0) {
        if (nPairs > 1024) nPairs = 1024; // cap for the GUI
        fn(g_const_user.load(), rnti, iq, nPairs);
    }
}

namespace {
std::atomic<LteSibFn> g_sib_fn{nullptr};
std::atomic<void*>    g_sib_user{nullptr};
}

void setLteSibCb(LteSibFn fn, void* user)
{
    g_sib_user.store(user);
    g_sib_fn.store(fn);
}

void emitLteSib(const uint8_t* pdu, int len)
{
    LteSibFn fn = g_sib_fn.load();
    if (fn && pdu && len > 0) {
        fn(g_sib_user.load(), pdu, len);
    }
}

} // namespace cellscope
