// CellScope LTE engine — clean interface for the rest of the application.
//
// This header exposes ONLY STL/POD types. All srsRAN / FALCON / LTESniffer
// includes live in lte_engine.cpp, which is compiled inside the third_party/lte
// CMake scope (with the MinGW compat shims). The GUI/app never sees srsRAN
// headers, which keeps CellScope's own build clean.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cellscope::lte {

enum class EngineState {
    Idle,
    Searching,   // scanning for PSS/SSS
    CellFound,   // PCI + MIB decoded
    Decoding,    // per-subframe PDCCH/PDSCH decode running
    Error
};

// A detected LTE cell.
struct CellInfo {
    int    pci       = -1;   // physical cell id (0..503)
    int    nof_prb   = 0;    // bandwidth in PRB (6/15/25/50/75/100)
    int    nof_ports = 0;    // # of antenna ports at the eNB
    double cfo_hz    = 0.0;  // carrier frequency offset estimate (live during decode)
    float  peak      = 0.0f; // correlation peak (search confidence)
    double freq_mhz  = 0.0;  // center frequency it was found at
    int    sfn       = -1;   // last decoded system frame number
    double decode_rate_hz = 0.0; // sample rate this cell needs to decode fully
    bool   bw_limited     = false; // true if decode_rate_hz exceeds the SDR input
                                   // rate: only the center is captured, so PDCCH
                                   // (hence all traffic) cannot be recovered
    // ---- from SIB1 (broadcast, plaintext) ----
    bool        have_sib = false;
    std::string plmn;        // "MCC-MNC", e.g. "310-260"
    std::string oper;        // carrier name if known
    uint32_t    tac     = 0; // tracking area code
    uint64_t    cell_id = 0; // 28-bit E-UTRAN cell identity
    int         band    = 0; // frequency band indicator
    bool        barred  = false;
};

// Per-UE (per-RNTI) running statistics — the "who is on the cell / who uses
// more data" model. Populated during the decode phase.
struct UeStat {
    uint16_t    rnti          = 0;
    uint64_t    first_seen_ms = 0; // -> "new phone connected" event
    uint64_t    last_seen_ms  = 0;
    uint64_t    dl_bytes      = 0;
    uint64_t    ul_bytes      = 0;
    uint32_t    dl_msgs       = 0;
    uint32_t    ul_msgs       = 0;
    int         last_mcs      = -1;
    double      dl_bps        = 0.0; // current downlink rate (bits/s, ~1 s window)
    std::string identity;          // TMSI/IMSI/IMEI when mapped, else empty
    bool        voice         = false; // active VoLTE call detected (heuristic)
    float       voice_score   = 0.0f;  // 0..1 cadence-match confidence
    uint64_t    call_start_ms = 0;     // when the current call was first flagged
};

// A detected voice-call start/stop event.
struct CallEvent {
    uint64_t    ts_ms      = 0;
    double      t_sec      = 0.0;   // seconds since decode start
    uint16_t    rnti       = 0;
    bool        started    = true;  // true=call started, false=call ended
    double      duration_s = 0.0;   // for "ended" events
    bool        sps        = false; // detected via semi-persistent pattern
    std::string identity;           // TMSI/IMSI if known
};

// One point of the cell-wide time series (for plotting).
struct HistPoint {
    double t_sec       = 0.0; // seconds since decode start
    double dl_kbps     = 0.0; // total downlink rate across all UEs
    int    active_ues  = 0;   // UEs with traffic in this window
};

// A "new phone connected" event (first time an RNTI is seen).
struct ConnEvent {
    uint64_t ts_ms = 0;
    double   t_sec = 0.0; // seconds since decode start
    uint16_t rnti  = 0;
};

// One human-readable decoded message for the traffic log.
struct TrafficMsg {
    uint64_t    ts_ms     = 0;
    uint16_t    rnti      = 0;
    uint8_t     direction = 0;  // 0 = downlink, 1 = uplink
    std::string channel;        // "BCCH" / "PCCH" / "DL-SCH" / "RRC" / "NAS" ...
    std::string summary;        // decoded, human-readable description
    uint32_t    len       = 0;  // payload length in bytes
};

struct EngineConfig {
    double input_sample_rate_hz = 1920000.0; // rate of the IQ handed to feed()
    double center_freq_mhz      = 0.0;        // for display/tagging only
    int    nof_rx_antennas      = 1;
    int    force_pci            = -1;         // -1 => auto cell search
    int    force_nof_prb        = -1;         // -1 => take from MIB
};

// Thread model: feed() is called from the SDR capture thread; the engine runs
// its own worker thread for sync/decode; the GUI thread calls the snapshot/
// drain/query methods. All public methods are thread-safe.
class LteEngine {
public:
    LteEngine();
    ~LteEngine();

    LteEngine(const LteEngine&)            = delete;
    LteEngine& operator=(const LteEngine&) = delete;

    void start(const EngineConfig& cfg);
    void stop();
    bool running() const;

    // Push interleaved complex<float> IQ at cfg.input_sample_rate_hz.
    void feed(const std::complex<float>* iq, std::size_t n);

    EngineState state() const;
    std::string statusText() const;

    // Returns true and fills `out` if a cell has been found.
    bool cell(CellInfo& out) const;

    std::vector<UeStat>     snapshotUes() const;   // copy of current UE table
    std::vector<TrafficMsg> drainTraffic();        // moves out pending messages

    // Cell-wide DL throughput + active-UE time series for plotting.
    std::vector<HistPoint>  history() const;
    // Recent "new phone connected" events (most recent last), bounded.
    std::vector<ConnEvent>  connections() const;

    // Watch a small set of RNTIs: the engine keeps a per-UE throughput history
    // for each so the GUI can plot them individually (used to track "my phone").
    void                    setWatched(const std::vector<uint16_t>& rntis);
    std::vector<HistPoint>  ueHistory(uint16_t rnti) const;

    // Latest equalized PDSCH constellation for the constellation-target UE
    // (the first pinned RNTI). Returns interleaved I,Q floats; sets outRnti.
    std::vector<float>      constellation(uint16_t& outRnti) const;

    // Detected voice-call events (start/stop), most recent last (bounded).
    std::vector<CallEvent>  callLog() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cellscope::lte
