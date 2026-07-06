#pragma once
// CellScope hook: lets the vendored LTESniffer PDSCH decoder report decoded
// human-readable identity/message events back to the CellScope LTE engine
// without the decoder needing to know about the engine. A plain function
// pointer keeps this ABI-simple and avoids <functional> across the boundary.
#include <cstdint>

namespace cellscope {

typedef void (*LteEventFn)(void*        user,
                           uint16_t     rnti,
                           const char*  idType,   // "TMSI" / "IMSI" / "Contention Resolution" ...
                           const char*  idValue,  // the decoded value string
                           const char*  msg);     // "Paging" / "RRC Connection Setup" ...

// Registered by the engine before decode starts; cleared on stop.
void setLteEventCb(LteEventFn fn, void* user);

// Called by the decoder (any worker thread) when it decodes an identity/message.
void emitLteEvent(uint16_t rnti, const char* idType, const char* idValue, const char* msg);

// ---- Constellation (equalized PDSCH QAM symbols) for one watched UE ----
// iq is interleaved I,Q floats; nPairs = number of complex symbols.
typedef void (*LteConstFn)(void* user, uint16_t rnti, const float* iq, int nPairs);
void setLteConstCb(LteConstFn fn, void* user);
// Only symbols for this RNTI are forwarded (0 = disabled). Keeps volume tiny.
void setLteConstRnti(uint16_t rnti);
// Called by the decoder with equalized symbols (syms is a cf_t*/_Complex float*).
void emitLteConst(uint16_t rnti, const float* iq, int nPairs);

} // namespace cellscope
