// CellScope LTE engine implementation. Compiled inside the third_party/lte
// CMake scope so it inherits the MinGW compat shims; it is the ONLY bridge
// translation unit that includes srsRAN headers.
#include "lte_engine.h"
#include "lte_diag.h"

#include "srsran/srsran.h"
#include "srsran/phy/ue/ue_cell_search.h"
#include "srsran/phy/ue/ue_mib.h"
#include "srsran/phy/ue/ue_sync.h"
#include "srsran/phy/resampling/resample_arb.h"
#undef I // <complex.h> defines I, which breaks C++ if left set

// LTESniffer decode core (downlink) + FALCON blind search.
#include "Phy.h"
#include "SubframeInfo.h"
#include "SubframeInfoConsumer.h"
#include "Sniffer_dependency.h"
#include "cs_lte_hook.h"
#include "falcon/util/RNTIManager.h"
#include "falcon/phy/falcon_phch/falcon_dci.h"
#include "srsran/asn1/rrc/bcch_msg.h"
#undef I

#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace cellscope::lte {

namespace {
uint64_t now_ms()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Anti-alias integer decimator (windowed-sinc FIR). srsran_resample_arb is an
// 8-tap near-unity resampler and aliases heavily on large decimation (e.g.
// 10 MHz -> 1.92 MHz for HackRF/Airspy), so we integer-decimate with a proper
// low-pass first, then let resample_arb handle the small fractional residual.
struct DecimFir {
    int                D = 1;
    std::vector<float> h;
    std::vector<cf_t>  hist;
    int                pos   = 0;
    int                phase = 0;

    void init(double in_rate, double out_rate)
    {
        D = (int)std::lround(in_rate / out_rate);
        if (D < 1) {
            D = 1;
        }

        // Anti-alias cutoff (cycles/sample of input) and tap count. Two cases:
        //   D >= 2: prefilter for the integer decimation, cutoff just under the
        //           decimated Nyquist (0.45/D).
        //   D == 1: no samples are dropped, BUT stage 2 (srsran_resample_arb) may
        //           still be a fractional DOWNsample (e.g. RTL 2.4 -> 1.92 MHz,
        //           ratio 1.25 rounds to D=1). The arb resampler has no anti-alias
        //           filter, so energy above the output Nyquist folds onto the
        //           PDCCH subcarriers -- robust PBCH survives but PDCCH decode
        //           fails (0 DCIs). Low-pass at the output Nyquist here. When
        //           D == 1 and out >= in (unity/upsample) there is no aliasing,
        //           so skip the filter and let resample_arb handle it.
        double fc;
        int    ntaps;
        if (D >= 2) {
            ntaps = 8 * D + 1;      // more taps for larger ratios
            fc    = 0.45 / D;
        } else {
            const double ratio = out_rate / in_rate; // < 1 when downsampling
            if (ratio >= 0.999) {
                h.clear();
                return; // unity or upsample: no aliasing; resample_arb handles it
            }
            fc = 0.45 * ratio; // just under the output Nyquist
            // Hamming transition width ~= 3.3/M (normalized to input fs); size
            // the filter to the guard band between fc and Nyquist.
            const double tw = std::max(0.02, ratio * 0.5 - fc);
            ntaps = (int)std::ceil(3.3 / tw);
            if (ntaps < 17) {
                ntaps = 17;
            }
            if ((ntaps & 1) == 0) {
                ntaps++; // odd -> symmetric with integer group delay
            }
        }

        const int M = ntaps - 1;
        h.resize(ntaps);
        double sum = 0.0;
        for (int i = 0; i < ntaps; ++i) {
            double x   = i - M / 2.0;
            double s   = (std::fabs(x) < 1e-9) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * x) / (M_PI * x);
            double w   = 0.54 - 0.46 * std::cos(2.0 * M_PI * i / M); // Hamming
            h[i]       = (float)(s * w);
            sum       += h[i];
        }
        for (auto& v : h) {
            v /= (float)sum; // unity DC gain
        }
        hist.assign(ntaps, cf_t{});
        pos   = 0;
        phase = 0;
    }

    // Append n input samples, emit decimated outputs into `out`.
    void process(const cf_t* in, int n, std::vector<cf_t>& out)
    {
        const int ntaps = (int)h.size();
        for (int i = 0; i < n; ++i) {
            hist[pos] = in[i];
            pos       = (pos + 1) % ntaps;
            if (++phase >= D) {
                phase = 0;
                cf_t acc = 0;
                for (int j = 0; j < ntaps; ++j) {
                    int hi = pos - 1 - j;
                    hi %= ntaps;
                    if (hi < 0) {
                        hi += ntaps;
                    }
                    acc += h[j] * hist[hi];
                }
                out.push_back(acc);
            }
        }
    }
};
} // namespace

struct LteEngine::Impl {
    EngineConfig cfg;

    // --- IQ input ring (resampled to SRSRAN_CS_SAMP_FREQ = 1.92 MHz) ---
    mutable std::mutex        ring_mtx;
    std::condition_variable   ring_cv;
    std::deque<cf_t>          ring;
    size_t                    ring_cap = 0;

    // Two-stage resampling input_rate -> 1.92 MHz:
    //   1) DecimFir integer decimation (anti-aliased) by D
    //   2) srsran_resample_arb for the fractional residual (ratio ~1)
    // Guards all resampler state (decim, resampler, decim_out, resample_out,
    // cfg_out_rate, resample_bypass). push_resampled() runs on the external SDR
    // capture thread while start()/configure_resampler() can reinitialize this
    // state from the GUI thread; without this lock a restart mid-stream races
    // the vector reallocations and corrupts memory.
    mutable std::mutex        resamp_mtx;
    DecimFir                  decim;
    std::vector<cf_t>         decim_out;
    srsran_resample_arb_t     resampler{};
    bool                      resample_bypass = false;
    double                    resample_ratio  = 1.0; // out/inter; sizes resample_out
    std::vector<cf_t>         resample_out;
    // Output rate the resampler currently targets (SDR-thread owned) and the
    // rate the worker wants (1.92 MHz for search, cell rate for decode).
    std::atomic<double>       target_rate{SRSRAN_CS_SAMP_FREQ};
    double                    cfg_out_rate = 0.0;

    // CFO estimate shared with MCSTracking (updated by decode workers).
    std::atomic<float>        est_cfo{0.0f};

    // --- worker + state ---
    std::thread               worker;
    std::atomic<bool>         run{false};
    std::atomic<EngineState>  st{EngineState::Idle};

    mutable std::mutex        status_mtx;
    std::string               status = "Idle";

    mutable std::mutex        cell_mtx;
    bool                      have_cell = false;
    CellInfo                  cell_info;

    // --- decoded data model (populated in the decode phase) ---
    mutable std::mutex            ue_mtx;
    std::map<uint16_t, UeStat>    ues;
    mutable std::mutex            traffic_mtx;
    std::deque<TrafficMsg>        traffic;

    // Rate/history tracking (guarded by ue_mtx).
    std::map<uint16_t, uint64_t>  prev_bytes;   // dl_bytes at last rate update
    std::deque<HistPoint>         history_pts;
    std::deque<ConnEvent>         conn_events;
    uint64_t                      decode_start_ms = 0;
    uint64_t                      last_rate_ms    = 0;

    // Per-UE throughput history for a small watched set (guarded by ue_mtx).
    std::set<uint16_t>                       watched;
    std::map<uint16_t, std::deque<HistPoint>> ue_hist;

    // Latest constellation (interleaved I,Q) for the constellation-target UE.
    mutable std::mutex        const_mtx;
    std::vector<float>        const_pts;
    uint16_t                  const_rnti = 0;

    // ---- VoLTE voice-call detection (per-RNTI grant cadence) ----
    struct VoiceTrack {
        std::deque<std::pair<int64_t, uint16_t>> grants; // (unwrapped tti ms, tb bytes)
        int64_t  abs_ms        = 0;
        uint32_t last_raw      = 0;
        bool     has_raw       = false;
        int      voice_secs    = 0;   // consecutive seconds matching
        int      idle_secs     = 0;
        bool     call_active   = false;
        bool     call_sps      = false;
        uint64_t call_start_ms = 0;
        float    score         = 0.0f;
    };
    std::map<uint16_t, VoiceTrack> vtrack;   // guarded by ue_mtx
    std::deque<CallEvent>          call_log;  // guarded by ue_mtx

    void set_status(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(status_mtx);
        status = s;
    }

    // Blocking pop of exactly n samples into dst. Returns false if stopped.
    bool pop(cf_t* dst, uint32_t n)
    {
        std::unique_lock<std::mutex> lk(ring_mtx);
        ring_cv.wait(lk, [&] { return !run.load() || ring.size() >= n; });
        if (!run.load()) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            dst[i] = ring[i];
        }
        ring.erase(ring.begin(), ring.begin() + n);
        return true;
    }

    // srsRAN ue_sync recv callback: pull `nsamples` from the ring.
    static int recv_cb(void* h, cf_t* data[SRSRAN_MAX_CHANNELS], uint32_t nsamples, srsran_timestamp_t* t)
    {
        auto* self = static_cast<Impl*>(h);
        if (t) {
            std::memset(t, 0, sizeof(*t));
        }
        if (!self->pop(data[0], nsamples)) {
            return SRSRAN_ERROR; // aborts the scan/decode loop on stop
        }
        return (int)nsamples;
    }

    // Configure the two-stage resampler for a given output rate. Caller MUST
    // hold resamp_mtx (push_resampled holds it; start() acquires it explicitly).
    void configure_resampler(double out_rate)
    {
        decim.init(cfg.input_sample_rate_hz, out_rate);
        const double inter = cfg.input_sample_rate_hz / (double)decim.D;
        const double rate  = out_rate / inter;
        resample_bypass    = (rate > 0.999 && rate < 1.001);
        resample_ratio     = rate;
        if (!resample_bypass) {
            srsran_resample_arb_init(&resampler, (float)rate, false);
        }
        ring_cap     = (size_t)(out_rate * 0.5); // ~0.5 s buffer
        cfg_out_rate = out_rate;
    }

    void push_resampled(const cf_t* in, int n_in)
    {
        // Serialize the whole resample against start()/configure_resampler().
        std::lock_guard<std::mutex> rlk(resamp_mtx);

        // Reconfigure if the worker changed the target rate (search -> decode).
        double tr = target_rate.load();
        if (tr != cfg_out_rate) {
            configure_resampler(tr);
            std::lock_guard<std::mutex> lk(ring_mtx);
            ring.clear();
        }

        // Stage 1: anti-aliased decimation / prefilter. Runs whenever init()
        // built a filter -- for D >= 2 (integer decimation) AND for the D == 1
        // fractional-downsample case (e.g. 2.4 -> 1.92 MHz), where process()
        // emits one filtered sample per input (no sample dropping) so the arb
        // resampler downstream sees a band-limited signal.
        const cf_t* stage = in;
        int         ns    = n_in;
        if (!decim.h.empty()) {
            decim_out.clear();
            decim.process(in, n_in, decim_out);
            stage = decim_out.data();
            ns    = (int)decim_out.size();
        }
        if (ns <= 0) {
            return;
        }

        // Stage 2: fractional resample to the target rate.
        const cf_t* src = stage;
        int         n   = ns;
        if (!resample_bypass) {
            // srsran_resample_arb_compute writes ~ns*ratio samples. Size by the
            // ACTUAL ratio (plus margin) -- when decoding a wide cell the target
            // rate can be several times the input rate (e.g. 50 PRB = 15.36 MHz
            // from a 2.88 MHz RTL, ratio 5.3), and the old fixed ns*2+64 bound
            // overflowed the buffer and corrupted the heap.
            const size_t need = (size_t)std::ceil(ns * resample_ratio) + 64;
            if (resample_out.size() < need) {
                resample_out.resize(need);
            }
            n   = srsran_resample_arb_compute(&resampler, (cf_t*)stage, resample_out.data(), ns);
            src = resample_out.data();
        }

        std::lock_guard<std::mutex> lk(ring_mtx);
        for (int i = 0; i < n; ++i) {
            ring.push_back(src[i]);
        }
        // Bound memory: drop oldest if the consumer falls behind.
        if (ring.size() > ring_cap) {
            ring.erase(ring.begin(), ring.begin() + (ring.size() - ring_cap));
        }
        ring_cv.notify_one();
    }

    // Find-or-create a UE row (caller must hold ue_mtx). Emits a "new phone
    // connected" event on first creation.
    UeStat& ue_ref(uint16_t rnti, uint64_t ts)
    {
        auto it = ues.find(rnti);
        if (it != ues.end()) {
            return it->second;
        }
        UeStat u;
        u.rnti          = rnti;
        u.first_seen_ms = ts;
        u.last_seen_ms  = ts;
        auto res = ues.emplace(rnti, u);
        ConnEvent ev;
        ev.ts_ms = ts;
        ev.t_sec = decode_start_ms ? (ts - decode_start_ms) / 1000.0 : 0.0;
        ev.rnti  = rnti;
        conn_events.push_back(ev);
        while (conn_events.size() > 200) {
            conn_events.pop_front();
        }
        return res.first->second;
    }

    static bool is_ue_rnti(uint16_t rnti)
    {
        return rnti != SRSRAN_SIRNTI && rnti != SRSRAN_PRNTI &&
               !(rnti >= SRSRAN_RARNTI_START && rnti <= SRSRAN_RARNTI_END) && rnti != 0;
    }

    void push_traffic(uint64_t ts, uint16_t rnti, uint8_t dir, const std::string& chan,
                      const std::string& summary, uint32_t len)
    {
        TrafficMsg m;
        m.ts_ms     = ts;
        m.rnti      = rnti;
        m.direction = dir;
        m.channel   = chan;
        m.summary   = summary;
        m.len       = len;
        std::lock_guard<std::mutex> lk(traffic_mtx);
        traffic.push_back(std::move(m));
        while (traffic.size() > 1000) {
            traffic.pop_front();
        }
    }

    // Called by the decode consumer (worker threads) for each decoded DL DCI.
    void on_dl_dci(uint16_t rnti, uint32_t bytes, int mcs, uint32_t sfn, uint32_t sf_idx)
    {
        const uint64_t ts = now_ms();
        const bool is_si  = (rnti == SRSRAN_SIRNTI);
        const bool is_pag = (rnti == SRSRAN_PRNTI);
        const bool is_rar = (rnti >= SRSRAN_RARNTI_START && rnti <= SRSRAN_RARNTI_END);
        const bool is_ue  = is_ue_rnti(rnti);

        const char* chan = is_si ? "BCCH/SIB" : is_pag ? "PCCH/Paging" : is_rar ? "RAR" : "DL-SCH";

        if (is_ue) {
            std::lock_guard<std::mutex> lk(ue_mtx);
            UeStat& u = ue_ref(rnti, ts);
            u.last_seen_ms = ts;
            u.dl_bytes += bytes;
            u.dl_msgs++;
            if (mcs >= 0) {
                u.last_mcs = mcs;
            }
            // Record grant timing (unwrapped TTI in ms) for voice cadence
            // analysis. TTI is the true 1 ms air-interface time (jitter-free).
            VoiceTrack& vt  = vtrack[rnti];
            uint32_t    raw = sfn * 10 + sf_idx;
            if (!vt.has_raw) {
                vt.has_raw = true;
                vt.abs_ms  = raw;
            } else {
                int32_t d = (int32_t)raw - (int32_t)vt.last_raw;
                if (d < 0) d += 10240; // TTI wraps at 1024 frames
                vt.abs_ms += d;
            }
            vt.last_raw = raw;
            vt.grants.emplace_back(vt.abs_ms, (uint16_t)std::min<uint32_t>(bytes, 65535));
            while (vt.grants.size() > 400 || (!vt.grants.empty() && vt.abs_ms - vt.grants.front().first > 3000)) {
                vt.grants.pop_front();
            }
        }

        // Traffic log entry (bounded).
        {
            char summary[96];
            std::snprintf(summary, sizeof(summary), "SFN %u.%u  MCS %d  %u B", sfn, sf_idx, mcs, bytes);
            push_traffic(ts, rnti, 0, chan, summary, bytes);
        }
    }

    // Called (via the cs_lte_hook) when the decoder extracts a human-readable
    // identity/message (paging TMSI/IMSI, RRC setup contention res, attach TMSI).
    void on_identity(uint16_t rnti, const char* idType, const char* idValue, const char* msg)
    {
        const uint64_t ts    = now_ms();
        std::string    ident = std::string(idType) + " " + idValue;
        if (is_ue_rnti(rnti)) {
            std::lock_guard<std::mutex> lk(ue_mtx);
            UeStat& u      = ue_ref(rnti, ts);
            u.last_seen_ms = ts;
            u.identity     = ident;
        }
        push_traffic(ts, rnti, 0, msg && msg[0] ? msg : "RRC/NAS", ident, 0);
    }

    // Trampoline for the cs_lte_hook C function pointer.
    static void identity_cb(void* user, uint16_t rnti, const char* idType,
                            const char* idValue, const char* msg)
    {
        static_cast<Impl*>(user)->on_identity(rnti, idType, idValue, msg);
    }

    void on_constellation(uint16_t rnti, const float* iq, int nPairs)
    {
        std::lock_guard<std::mutex> lk(const_mtx);
        const_rnti = rnti;
        const_pts.assign(iq, iq + (size_t)nPairs * 2);
    }

    static void constellation_cb(void* user, uint16_t rnti, const float* iq, int nPairs)
    {
        static_cast<Impl*>(user)->on_constellation(rnti, iq, nPairs);
    }

    void on_sib(const uint8_t* pdu, int len);
    static void sib_cb(void* user, const uint8_t* pdu, int len)
    {
        static_cast<Impl*>(user)->on_sib(pdu, len);
    }

    // Once/sec: compute per-UE bit rates + push a cell-wide history point.
    void update_rates()
    {
        std::lock_guard<std::mutex> lk(ue_mtx);
        const uint64_t now = now_ms();
        if (last_rate_ms == 0) {
            last_rate_ms = now;
        }
        double dt = (now - last_rate_ms) / 1000.0;
        if (dt < 0.2) {
            return;
        }
        double total_bps = 0.0;
        int    active     = 0;
        for (auto& kv : ues) {
            uint16_t rnti = kv.first;
            UeStat&  u    = kv.second;
            uint64_t prev = prev_bytes.count(rnti) ? prev_bytes[rnti] : 0;
            double   inst = (double)(u.dl_bytes - prev) * 8.0 / dt;
            // Exponential moving average: decode under-samples subframes (single
            // worker), so raw per-second deltas are bursty and snap to 0 even
            // while a UE is active. The EMA decays smoothly instead.
            const double alpha = 0.4;
            u.dl_bps         = alpha * inst + (1.0 - alpha) * u.dl_bps;
            if (u.dl_bps < 1.0) {
                u.dl_bps = 0.0; // avoid a long ~0 tail
            }
            prev_bytes[rnti] = u.dl_bytes;
            total_bps += u.dl_bps;
            // "Active" = seen traffic in the last 3 s (recency, not this window).
            if (now - u.last_seen_ms < 3000) {
                active++;
            }
        }
        HistPoint hp;
        hp.t_sec      = decode_start_ms ? (now - decode_start_ms) / 1000.0 : 0.0;
        hp.dl_kbps    = total_bps / 1000.0;
        hp.active_ues = active;
        history_pts.push_back(hp);
        while (history_pts.size() > 600) { // ~10 min at 1 Hz
            history_pts.pop_front();
        }

        // Per-UE history for the watched set.
        for (uint16_t rnti : watched) {
            auto uit = ues.find(rnti);
            double kbps = (uit != ues.end()) ? uit->second.dl_bps / 1000.0 : 0.0;
            HistPoint up;
            up.t_sec      = hp.t_sec;
            up.dl_kbps    = kbps;
            up.active_ues = 0;
            auto& dq = ue_hist[rnti];
            dq.push_back(up);
            while (dq.size() > 600) {
                dq.pop_front();
            }
        }
        analyze_voice_locked(now);
        last_rate_ms = now;
    }

    // Per-RNTI VoLTE detection (caller holds ue_mtx). VoLTE active speech is
    // ~50 downlink grants/s at a 20 ms cadence with small, consistent TBs; we
    // score each UE's grant cadence and require it to be sustained.
    void analyze_voice_locked(uint64_t now)
    {
        for (auto& kv : ues) {
            uint16_t rnti = kv.first;
            UeStat&  u    = kv.second;
            auto     vit  = vtrack.find(rnti);
            if (vit == vtrack.end()) {
                u.voice = false;
                u.voice_score = 0.0f;
                continue;
            }
            VoiceTrack& vt = vit->second;
            while (!vt.grants.empty() && vt.abs_ms - vt.grants.front().first > 3000) {
                vt.grants.pop_front();
            }
            const int n = (int)vt.grants.size();

            bool  voice_now = false;
            bool  sps       = false;
            float best      = 0.0f;
            if (n >= 8) {
                std::vector<int64_t> times;
                std::vector<uint16_t> sizes;
                times.reserve(n);
                sizes.reserve(n);
                double totbytes = 0;
                for (auto& g : vt.grants) { times.push_back(g.first); sizes.push_back(g.second); totbytes += g.second; }
                const int tol = 3;
                auto matchFrac = [&](int P) -> float {
                    int m = 0;
                    for (int64_t t : times) {
                        int64_t lo = t - P - tol, hi = t - P + tol;
                        auto a = std::lower_bound(times.begin(), times.end(), lo);
                        if (a != times.end() && *a <= hi) m++;
                    }
                    return (float)m / (float)times.size();
                };
                best = std::max(matchFrac(20), matchFrac(40));
                std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
                uint16_t med     = sizes[sizes.size() / 2];
                double   span_s  = (times.back() - times.front()) / 1000.0;
                double   bitrate = span_s > 0.2 ? totbytes * 8.0 / span_s : 0.0;
                const bool sizeok = (med >= 15 && med <= 250);
                const bool brok   = (bitrate >= 2000 && bitrate <= 64000);
                if (n >= 30 && best >= 0.75f && sizeok && brok) {
                    voice_now = true;
                    sps       = false;
                } else if (n < 30 && best >= 0.90f && sizeok && brok) {
                    voice_now = true; // sparse but very clean cadence => likely SPS
                    sps       = true;
                }
            }
            u.voice_score = best;

            // Sustain gates (fewer false positives): need 3 s to start, 3 s to end.
            if (voice_now) { vt.voice_secs++; vt.idle_secs = 0; }
            else           { vt.idle_secs++; }

            if (!vt.call_active && vt.voice_secs >= 3) {
                vt.call_active   = true;
                vt.call_sps      = sps;
                vt.call_start_ms = now;
                CallEvent ce;
                ce.ts_ms   = now;
                ce.t_sec   = decode_start_ms ? (now - decode_start_ms) / 1000.0 : 0.0;
                ce.rnti    = rnti;
                ce.started = true;
                ce.sps     = sps;
                ce.identity = u.identity;
                call_log.push_back(ce);
                while (call_log.size() > 200) call_log.pop_front();
            } else if (vt.call_active && vt.idle_secs >= 3) {
                vt.call_active = false;
                CallEvent ce;
                ce.ts_ms      = now;
                ce.t_sec      = decode_start_ms ? (now - decode_start_ms) / 1000.0 : 0.0;
                ce.rnti       = rnti;
                ce.started    = false;
                ce.sps        = vt.call_sps;
                ce.duration_s = (now - vt.call_start_ms) / 1000.0;
                ce.identity   = u.identity;
                call_log.push_back(ce);
                while (call_log.size() > 200) call_log.pop_front();
                vt.voice_secs = 0;
            }
            u.voice         = vt.call_active;
            u.call_start_ms = vt.call_start_ms;
        }

        // Bound memory: drop voice tracks for UEs no longer present.
        for (auto it = vtrack.begin(); it != vtrack.end();) {
            if (!ues.count(it->first)) it = vtrack.erase(it);
            else ++it;
        }
    }

    bool search_cell(CellInfo& out);
    bool decode_mib(CellInfo& io);
    void run_decode(CellInfo& ci);
    void run_worker();

    struct Consumer; // SubframeInfoConsumer that feeds on_dl_dci()
};

// ---- cell search (PSS/SSS) ----
bool LteEngine::Impl::search_cell(CellInfo& out)
{
    srsran_ue_cellsearch_t cs;
    if (srsran_ue_cellsearch_init_multi(&cs, 8, recv_cb, cfg.nof_rx_antennas, this)) {
        set_status("cellsearch init failed");
        return false;
    }
    srsran_ue_cellsearch_set_nof_valid_frames(&cs, 4);

    srsran_ue_cellsearch_result_t found[3] = {};
    uint32_t                      max_nid2 = 0;
    int                           ret      = srsran_ue_cellsearch_scan(&cs, found, &max_nid2);

    bool ok = false;
    if (ret > 0 && found[max_nid2].psr > 2.0f) {
        out.pci      = (int)found[max_nid2].cell_id;
        out.peak     = found[max_nid2].peak;
        out.cfo_hz   = found[max_nid2].cfo;
        out.freq_mhz = cfg.center_freq_mhz;
        ok           = true;
    }
    srsran_ue_cellsearch_free(&cs);
    return ok;
}

// ---- MIB decode (bandwidth, ports, SFN) ----
bool LteEngine::Impl::decode_mib(CellInfo& io)
{
    srsran_cell_t cell = {};
    cell.id            = (uint32_t)io.pci;
    cell.cp            = SRSRAN_CP_NORM;
    cell.nof_ports     = 1;
    cell.nof_prb       = SRSRAN_UE_MIB_NOF_PRB;

    srsran_ue_mib_sync_t mib;
    if (srsran_ue_mib_sync_init_multi(&mib, recv_cb, cfg.nof_rx_antennas, this)) {
        set_status("mib sync init failed");
        return false;
    }
    if (srsran_ue_mib_sync_set_cell(&mib, cell)) {
        srsran_ue_mib_sync_free(&mib);
        return false;
    }

    uint8_t  bch[SRSRAN_BCH_PAYLOAD_LEN] = {};
    uint32_t nof_ports                   = 0;
    int      sfn_offset                  = 0;
    int      ret = srsran_ue_mib_sync_decode(&mib, 40, bch, &nof_ports, &sfn_offset);

    bool ok = false;
    if (ret == SRSRAN_UE_MIB_FOUND) {
        uint32_t sfn = 0;
        srsran_pbch_mib_unpack(bch, &cell, &sfn);
        io.nof_prb   = (int)cell.nof_prb;
        io.nof_ports = (int)nof_ports;
        io.sfn       = (int)sfn;
        ok           = true;
    }
    srsran_ue_mib_sync_free(&mib);
    return ok;
}

void LteEngine::Impl::run_worker()
{
    // Outer loop: acquire a cell, decode it, and on sustained sync loss fall back
    // here to re-acquire. Previously run_decode spun forever once sync was lost,
    // so a brief fade/CFO-drift dropped decode permanently until the user stopped.
    while (run.load()) {
        int attempts = 0; // reset per re-acquisition cycle
        st.store(EngineState::Searching);
        // Search + MIB run at the 1.92 MHz cell-search rate; run_decode retunes
        // the pipeline to the cell rate, so reset it before (re-)searching.
        target_rate.store(SRSRAN_CS_SAMP_FREQ);

        CellInfo info;
        bool     found = false;
        while (run.load()) {
            if (!search_cell(info)) {
                ++attempts;
                // After sustained failures, hint that this may not be LTE at all.
                // CellScope decodes LTE only; a strong carrier here with no PSS/SSS
                // is most likely 5G NR (different PHY, not decodable) or non-cellular.
                if (attempts >= 5) {
                    set_status("No LTE cell found (attempt " + std::to_string(attempts) +
                               "). If a carrier is present it is likely 5G NR or non-LTE "
                               "— not decodable by CellScope.");
                } else {
                    set_status("Searching for cell... (attempt " + std::to_string(attempts) + ")");
                }
                continue; // keep scanning as more IQ streams in
            }
            set_status("Cell PCI " + std::to_string(info.pci) + " found, decoding MIB...");

            if (decode_mib(info)) {
                {
                    std::lock_guard<std::mutex> lk(cell_mtx);
                    cell_info = info;
                    have_cell = true;
                }
                st.store(EngineState::CellFound);
                set_status("Cell PCI " + std::to_string(info.pci) + ", " +
                           std::to_string(info.nof_prb) + " PRB, " +
                           std::to_string(info.nof_ports) + " ports");
                found = true;
                break;
            }
            set_status("MIB decode failed, re-searching...");
        }

        // ---- DECODE PHASE ----
        // Switch the input pipeline to the cell's sample rate and run the
        // PDCCH blind-search + PDSCH decode loop (FALCON + LTESniffer core).
        // Returns on stop OR sustained sync loss -> loop re-acquires.
        if (run.load() && found) {
            run_decode(info);
        }
    }
}

// SubframeInfoConsumer: extracts per-DCI RNTI + PDSCH TBS/MCS from each decoded
// subframe and feeds the engine's UE table + traffic log.
struct LteEngine::Impl::Consumer : public SubframeInfoConsumer {
    Impl* impl = nullptr;
    void consumeDCICollection(const SubframeInfo& si) override
    {
        try {
            auto&    dc    = const_cast<SubframeInfo&>(si).getDCICollection();
            uint32_t sfn   = dc.get_sfn();
            uint32_t sfidx = dc.get_sf_idx();
            for (auto& dci : dc.getDLSnifferDCI_DL()) {
                // Pick the grant matching the UE's MCS table (64QAM vs 256QAM);
                // fall back to whichever grant actually carries a TB. Without
                // this, high-MCS (256QAM) traffic — e.g. a speed test — reads 0.
                auto tbsOf = [](const srsran_pdsch_grant_t* g, int& mcsOut) -> uint32_t {
                    if (!g) return 0;
                    uint32_t b = 0;
                    for (int cw = 0; cw < SRSRAN_MAX_CODEWORDS; ++cw) {
                        const srsran_ra_tb_t& tb = g->tb[cw];
                        if (tb.enabled && tb.tbs > 0) {
                            b += (uint32_t)(tb.tbs / 8);
                            if (mcsOut < 0) mcsOut = (int)tb.mcs_idx;
                        }
                    }
                    return b;
                };
                int      mcs64 = -1, mcs256 = -1;
                uint32_t b64   = tbsOf(dci.ran_pdsch_grant.get(), mcs64);
                uint32_t b256  = tbsOf(dci.ran_pdsch_grant_256.get(), mcs256);
                uint32_t bytes;
                int      mcs;
                if (dci.mcs_table == DL_SNIFFER_256QAM_TABLE) {
                    bytes = b256 ? b256 : b64;
                    mcs   = b256 ? mcs256 : mcs64;
                } else {
                    bytes = b64 ? b64 : b256;
                    mcs   = b64 ? mcs64 : mcs256;
                }
                impl->on_dl_dci(dci.rnti, bytes, mcs, sfn, sfidx);
            }
        } catch (const std::exception& e) {
            diagLog(std::string("consumer exception: ") + e.what());
        } catch (...) {
            diagLog("consumer unknown exception");
        }
    }
};

// ---- SIB1 operator lookup (best-effort; PLMN is always shown regardless) ----
static std::string operator_name(int mcc, int mnc)
{
    if (mcc == 310 || mcc == 311 || mcc == 312 || mcc == 313 || mcc == 316) { // USA
        switch (mnc) {
            case 260: case 160: case 200: case 210: case 220: case 230:
            case 240: case 250: case 270: case 310: case 490: case 660: case 800:
                return "T-Mobile US";
            case 410: case 150: case 170: case 380: case 560: case 680: case 70:
                return "AT&T";
            case 4: case 10: case 12: case 13: case 480:
                return "Verizon";
            case 120: return "Sprint";
            default:  return "USA";
        }
    }
    if (mcc == 202) { // Greece
        switch (mnc) {
            case 1:  case 14: return "Cosmote";
            case 5:            return "Vodafone GR";
            case 9:  case 10: return "Nova";
            default:           return "Greece";
        }
    }
    if (mcc == 234 || mcc == 235) return "UK";
    if (mcc == 262) return "Germany";
    if (mcc == 302) return "Canada";
    if (mcc == 505) return "Australia";
    return "";
}

void LteEngine::Impl::on_sib(const uint8_t* pdu, int len)
{
    {
        std::lock_guard<std::mutex> lk(cell_mtx);
        if (cell_info.have_sib) return; // already have it
    }
    asn1::rrc::bcch_dl_sch_msg_s msg;
    asn1::cbit_ref              bref(pdu, len);
    if (msg.unpack(bref) != asn1::SRSASN_SUCCESS) return;
    if (msg.msg.type().value != asn1::rrc::bcch_dl_sch_msg_type_c::types_opts::c1) return;
    if (msg.msg.c1().type().value != asn1::rrc::bcch_dl_sch_msg_type_c::c1_c_::types::sib_type1) return;

    auto& sib1 = msg.msg.c1().sib_type1();
    auto& cari = sib1.cell_access_related_info;

    int         mcc = -1, mnc = -1, mnc_dig = 0;
    std::string plmn;
    if (cari.plmn_id_list.size() > 0) {
        auto& p = cari.plmn_id_list[0].plmn_id;
        if (p.mcc_present) {
            mcc = p.mcc[0] * 100 + p.mcc[1] * 10 + p.mcc[2];
        }
        mnc_dig = (int)p.mnc.size();
        int m   = 0;
        for (uint32_t i = 0; i < p.mnc.size(); ++i) m = m * 10 + p.mnc[i];
        mnc = m;
        char buf[24];
        if (p.mcc_present) std::snprintf(buf, sizeof(buf), "%03d-%0*d", mcc, mnc_dig, mnc);
        else               std::snprintf(buf, sizeof(buf), "???-%0*d", mnc_dig, mnc);
        plmn = buf;
    }
    uint32_t tac    = (uint32_t)cari.tac.to_number();
    uint64_t cid    = (uint64_t)cari.cell_id.to_number();
    bool     barred = (std::strcmp(cari.cell_barred.to_string(), "barred") == 0);
    int      band   = (int)sib1.freq_band_ind;
    std::string oper = operator_name(mcc, mnc);

    std::lock_guard<std::mutex> lk(cell_mtx);
    cell_info.have_sib = true;
    cell_info.plmn     = plmn;
    cell_info.oper     = oper;
    cell_info.tac      = tac;
    cell_info.cell_id  = cid;
    cell_info.band     = band;
    cell_info.barred   = barred;
    diagLog("SIB1: plmn=" + plmn + " oper=" + oper + " tac=" + std::to_string(tac) +
            " ecgi=" + std::to_string(cid) + " band=" + std::to_string(band));
}

// ---- RNTI manager seeding (mirrors LTESniffer_Core) ----
static void setup_rnti_manager(RNTIManager& rm)
{
    int idx = falcon_dci_index_of_format_in_list(SRSRAN_DCI_FORMAT1A, falcon_ue_all_formats, nof_falcon_ue_all_formats);
    if (idx > -1) {
        rm.addEvergreen(SRSRAN_RARNTI_START, SRSRAN_RARNTI_END, (uint32_t)idx);
        rm.addEvergreen(SRSRAN_PRNTI, SRSRAN_SIRNTI, (uint32_t)idx);
    }
    idx = falcon_dci_index_of_format_in_list(SRSRAN_DCI_FORMAT1C, falcon_ue_all_formats, nof_falcon_ue_all_formats);
    if (idx > -1) {
        rm.addEvergreen(SRSRAN_RARNTI_START, SRSRAN_RARNTI_END, (uint32_t)idx);
        rm.addEvergreen(SRSRAN_PRNTI, SRSRAN_SIRNTI, (uint32_t)idx);
    }
    for (uint32_t f = 0; f < nof_falcon_ue_all_formats; f++) {
        rm.addForbidden(0x0, 0x0, f);
    }
}

// ---- full PDCCH/PDSCH decode loop (downlink) ----
void LteEngine::Impl::run_decode(CellInfo& ci)
{
    srsran_cell_t cell   = {};
    cell.id              = (uint32_t)ci.pci;
    cell.nof_prb         = (uint32_t)ci.nof_prb;
    cell.nof_ports       = ci.nof_ports > 0 ? (uint32_t)ci.nof_ports : 1;
    cell.cp              = SRSRAN_CP_NORM;
    cell.phich_length    = SRSRAN_PHICH_NORM;
    cell.phich_resources = SRSRAN_PHICH_R_1_6;

    const int srate = srsran_sampling_freq_hz(cell.nof_prb);
    if (srate <= 0) {
        set_status("decode: invalid PRB");
        return;
    }

    // Bandwidth check: a cell is decodable when the SDR captures its OCCUPIED
    // bandwidth (nof_prb * 180 kHz), not srsRAN's oversampled nominal 'srate'.
    // E.g. a 100-PRB cell occupies 18 MHz and decodes fine from a 20 Msps HackRF
    // even though srate is 23.04 MHz; comparing against srate would wrongly flag
    // it. If the input rate is below the occupied bandwidth we only capture the
    // center of the carrier: PSS/SSS/MIB (center 6 PRB) still decode so the cell
    // locks, but PDCCH spans the whole carrier and cannot be recovered -> 0 UEs.
    const double occupied_hz = cell.nof_prb * 180e3;
    const bool   bw_limited  = cfg.input_sample_rate_hz < occupied_hz;
    {
        std::lock_guard<std::mutex> lk(cell_mtx);
        cell_info.decode_rate_hz = occupied_hz; // occupied bandwidth needed
        cell_info.bw_limited     = bw_limited;
    }
    diagLog("run_decode bandwidth: prb=" + std::to_string(cell.nof_prb) +
            " occupied=" + std::to_string((long long)occupied_hz) +
            " srsran_srate=" + std::to_string(srate) +
            " input_rate=" + std::to_string((long long)cfg.input_sample_rate_hz) +
            (bw_limited ? " -> BANDWIDTH LIMITED (center only, no PDCCH)" : " -> ok"));

    // Switch the input pipeline to the cell's sample rate; the SDR thread
    // reconfigures the resampler and refills the ring at the new rate.
    target_rate.store((double)srate);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Build the decode machinery (mirrors LTESniffer_Core, downlink only).
    est_cfo.store(0.0f);
    HARQ       harq;
    harq.init_HARQ(0);
    UL_HARQ    ul_harq;
    ULSchedule ulsche(0, &ul_harq, false);
    ulsche.set_multi_offset(DL_MODE);
    // api_mode = 3 enables the downlink identity decoders (paging TMSI/IMSI,
    // RRC connection setup contention resolution, attach TMSI).
    MCSTracking            mcs_tracking(1, 0, false, DL_MODE, 3, est_cfo);
    LTESniffer_pcap_writer pcapwriter;
    pcapwriter.open("cellscope_lte_dl.pcap", "cellscope_lte_api.pcap", 0);

    // RNTIManager is now internally synchronized (recursive mutex on all
    // public paths), so multiple decode workers are race-safe again. 4 workers
    // lets us keep up with a busy cell in real time instead of dropping most
    // subframes.
    Phy phy(1, 4, "", "", false, 0.99, 5, &pcapwriter, &mcs_tracking, &harq, 1, 0, &ulsche);
    phy.getCommon().setShortcutDiscovery(true);
    auto consumer  = std::make_shared<Consumer>();
    consumer->impl = this;
    phy.getCommon().setDCIConsumer(consumer);

    // Route decoded identities/messages from the PDSCH decoder to on_identity().
    cellscope::setLteEventCb(&Impl::identity_cb, this);
    cellscope::setLteConstCb(&Impl::constellation_cb, this);
    cellscope::setLteSibCb(&Impl::sib_cb, this);

    if (!phy.setCell(cell)) {
        set_status("decode: setCell failed");
        pcapwriter.close();
        return;
    }

    srsran_ue_sync_t ue_sync;
    if (srsran_ue_sync_init_multi(&ue_sync, cell.nof_prb, cell.id == 1000, recv_cb, cfg.nof_rx_antennas, this)) {
        set_status("decode: ue_sync init failed");
        pcapwriter.close();
        return;
    }
    if (srsran_ue_sync_set_cell(&ue_sync, cell)) {
        set_status("decode: ue_sync set_cell failed");
        srsran_ue_sync_free(&ue_sync);
        pcapwriter.close();
        return;
    }
    ue_sync.cfo_correct_enable_track = true;

    std::shared_ptr<SubframeWorker> cur_worker = phy.getAvail();

    srsran_ue_mib_t ue_mib;
    if (srsran_ue_mib_init(&ue_mib, cur_worker->getBuffers()[0], cell.nof_prb) ||
        srsran_ue_mib_set_cell(&ue_mib, cell)) {
        set_status("decode: ue_mib init failed");
        srsran_ue_sync_free(&ue_sync);
        pcapwriter.close();
        return;
    }
    srsran_pbch_decode_reset(&ue_mib.pbch);

    setup_rnti_manager(phy.getCommon().getRNTIManager());

    st.store(EngineState::Decoding);
    set_status("Decoding cell PCI " + std::to_string(cell.id) + " (" +
               std::to_string(cell.nof_prb) + " PRB, " +
               std::to_string((int)(srate / 1e6)) + " Msps)");
    diagLog("run_decode enter: PCI=" + std::to_string(cell.id) + " prb=" + std::to_string(cell.nof_prb) +
            " ports=" + std::to_string(cell.nof_ports) + " srate=" + std::to_string(srate));

    const uint32_t max_num_samples = 3 * SRSRAN_SF_LEN_PRB(cell.nof_prb);
    uint32_t       sfn             = 0;
    uint64_t       sf_cnt          = 0;
    int            decode_state    = 0; // 0 = re-acquire MIB/SFN, 1 = PDSCH
    uint64_t       last_status_ms  = now_ms();
    uint64_t       last_sync_ms    = now_ms(); // last subframe sync; drives re-acquire
    {
        std::lock_guard<std::mutex> lk(ue_mtx);
        decode_start_ms = now_ms();
        last_rate_ms    = decode_start_ms;
        // Clear the UE model too: it belongs to the cell we are (re-)acquiring,
        // not a previous one. Without this the table kept showing stale UEs (and
        // a stale count) after the cell changed or dropped out.
        ues.clear();
        prev_bytes.clear();
        history_pts.clear();
        conn_events.clear();
        vtrack.clear();
        call_log.clear();
    }

    while (run.load()) {
      try {
        int ret = srsran_ue_sync_zerocopy(&ue_sync, cur_worker->getBuffers(), max_num_samples);
        if (ret == 1) {
            uint32_t sf_idx = srsran_ue_sync_get_sfidx(&ue_sync);
            if (decode_state == 0) {
                if (sf_idx == 0) {
                    uint8_t bch[SRSRAN_BCH_PAYLOAD_LEN];
                    int     sfn_offset = 0;
                    int     n          = srsran_ue_mib_decode(&ue_mib, bch, NULL, &sfn_offset);
                    if (n == SRSRAN_UE_MIB_FOUND) {
                        srsran_pbch_mib_unpack(bch, &cell, &sfn);
                        sfn          = (sfn + sfn_offset) % 1024;
                        decode_state = 1;
                        diagLog("run_decode: MIB re-sync, sfn=" + std::to_string(sfn));
                    }
                }
            } else {
                uint32_t tti = sfn * 10 + sf_idx;
                srsran_dl_sf_cfg_t dl_sf = {};
                dl_sf.tti     = tti;
                dl_sf.sf_type = SRSRAN_SF_NORM;
                cur_worker->prepare(sf_idx, sfn, (sf_cnt % 500) == 0, dl_sf);

                std::shared_ptr<SubframeWorker> next = phy.getAvailImmediate();
                if (next != nullptr) {
                    phy.putPending(std::move(cur_worker));
                    cur_worker = std::move(next);
                }
                // else: no worker free -> drop this subframe

                {
                    std::lock_guard<std::mutex> lk(cell_mtx);
                    cell_info.sfn = (int)sfn;
                }
            }
            if (sf_idx == 9) {
                sfn = (sfn + 1) % 1024;
            }
            sf_cnt++;
            last_sync_ms = now_ms(); // got a synced subframe
        }
      } catch (const std::exception& e) {
        diagLog(std::string("run_decode loop exception: ") + e.what());
      } catch (...) {
        diagLog("run_decode loop unknown exception");
      }

        // Periodic status update.
        uint64_t nowms = now_ms();
        if (nowms - last_status_ms > 1000) {
            update_rates();
            // Publish the live tracked carrier offset (Hz) so the UI shows the
            // real, current offset (not the coarse search-time value) and
            // auto-center can track drift.
            const double live_cfo = srsran_ue_sync_get_cfo(&ue_sync);
            {
                std::lock_guard<std::mutex> lk(cell_mtx);
                cell_info.cfo_hz = live_cfo;
            }
            size_t nue;
            {
                std::lock_guard<std::mutex> lk(ue_mtx);
                nue = ues.size();
            }
            // Always report continuous decode progress. On a too-wide cell add a
            // short "(partial)" note rather than replacing the status with a
            // warning -- decode keeps running and recovers whatever lands in the
            // captured center bandwidth.
            set_status("Decoding PCI " + std::to_string(cell.id) +
                       (bw_limited ? " (partial - width limited): " : ": ") +
                       std::to_string(nue) + " UEs, " + std::to_string(sf_cnt) + " subframes");
            diagLog("decode progress: " + std::to_string(nue) + " UEs, " +
                    std::to_string(sf_cnt) + " subframes, sfn=" + std::to_string(sfn) +
                    ", cfo=" + std::to_string((int)live_cfo) + " Hz" +
                    (bw_limited ? " [BW-LIMITED]" : ""));
            last_status_ms = nowms;

            // Sustained loss of subframe sync (fade, CFO drift, stream glitch):
            // bail out so run_worker re-acquires instead of spinning dead here.
            if (nowms - last_sync_ms > 3000) {
                diagLog("run_decode: no sync for >3s, re-acquiring cell");
                set_status("Sync lost - re-acquiring cell...");
                break;
            }
        }
    }

    diagLog("run_decode exit: " + std::to_string(sf_cnt) + " subframes processed");
    phy.joinPending();
    cellscope::setLteEventCb(nullptr, nullptr); // workers are done emitting
    cellscope::setLteConstCb(nullptr, nullptr);
    cellscope::setLteSibCb(nullptr, nullptr);
    srsran_ue_sync_free(&ue_sync);
    srsran_ue_mib_free(&ue_mib);
    pcapwriter.close();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
LteEngine::LteEngine() : impl_(std::make_unique<Impl>()) {}

LteEngine::~LteEngine()
{
    stop();
}

void LteEngine::start(const EngineConfig& cfg)
{
    stop();
    diagInstallCrashHandler();
    diagLog("LteEngine::start input_rate=" + std::to_string(cfg.input_sample_rate_hz) +
            " center_mhz=" + std::to_string(cfg.center_freq_mhz));
    impl_->cfg = cfg;

    const double out_rate = SRSRAN_CS_SAMP_FREQ; // 1.92 MHz for search/MIB
    impl_->target_rate.store(out_rate);
    {
        // Hold resamp_mtx: the SDR capture thread may still be inside
        // push_resampled() from a prior session while we reinitialize here.
        std::lock_guard<std::mutex> rlk(impl_->resamp_mtx);
        impl_->configure_resampler(out_rate);
    }
    {
        std::lock_guard<std::mutex> lk(impl_->ring_mtx);
        impl_->ring.clear();
    }
    impl_->have_cell = false;
    impl_->run.store(true);
    impl_->st.store(EngineState::Searching);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Input %.3f Msps -> decimate /%d -> 1.92 Msps; searching...",
                      cfg.input_sample_rate_hz / 1e6, impl_->decim.D);
        impl_->set_status(buf);
    }
    impl_->worker = std::thread([this] { impl_->run_worker(); });
}

void LteEngine::stop()
{
    if (!impl_->run.exchange(false)) {
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        return;
    }
    impl_->ring_cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->st.store(EngineState::Idle);
    impl_->set_status("Idle");
}

bool LteEngine::running() const
{
    return impl_->run.load();
}

void LteEngine::feed(const std::complex<float>* iq, std::size_t n)
{
    if (!impl_->run.load() || n == 0) {
        return;
    }
    impl_->push_resampled(reinterpret_cast<const cf_t*>(iq), (int)n);
}

EngineState LteEngine::state() const
{
    return impl_->st.load();
}

std::string LteEngine::statusText() const
{
    std::lock_guard<std::mutex> lk(impl_->status_mtx);
    return impl_->status;
}

bool LteEngine::cell(CellInfo& out) const
{
    std::lock_guard<std::mutex> lk(impl_->cell_mtx);
    if (!impl_->have_cell) {
        return false;
    }
    out = impl_->cell_info;
    return true;
}

std::vector<UeStat> LteEngine::snapshotUes() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    std::vector<UeStat> v;
    v.reserve(impl_->ues.size());
    for (auto& kv : impl_->ues) {
        v.push_back(kv.second);
    }
    return v;
}

std::vector<TrafficMsg> LteEngine::drainTraffic()
{
    std::lock_guard<std::mutex> lk(impl_->traffic_mtx);
    std::vector<TrafficMsg> v(impl_->traffic.begin(), impl_->traffic.end());
    impl_->traffic.clear();
    return v;
}

std::vector<HistPoint> LteEngine::history() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    return std::vector<HistPoint>(impl_->history_pts.begin(), impl_->history_pts.end());
}

std::vector<ConnEvent> LteEngine::connections() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    return std::vector<ConnEvent>(impl_->conn_events.begin(), impl_->conn_events.end());
}

void LteEngine::setWatched(const std::vector<uint16_t>& rntis)
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    std::set<uint16_t> next(rntis.begin(), rntis.end());
    impl_->watched = next;
    // Drop history for RNTIs no longer watched.
    for (auto it = impl_->ue_hist.begin(); it != impl_->ue_hist.end();) {
        if (!next.count(it->first)) {
            it = impl_->ue_hist.erase(it);
        } else {
            ++it;
        }
    }
    // Constellation feature removed from the UI; keep the decoder hook disabled.
    cellscope::setLteConstRnti(0);
}

std::vector<HistPoint> LteEngine::ueHistory(uint16_t rnti) const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    auto it = impl_->ue_hist.find(rnti);
    if (it == impl_->ue_hist.end()) {
        return {};
    }
    return std::vector<HistPoint>(it->second.begin(), it->second.end());
}

std::vector<float> LteEngine::constellation(uint16_t& outRnti) const
{
    std::lock_guard<std::mutex> lk(impl_->const_mtx);
    outRnti = impl_->const_rnti;
    return impl_->const_pts;
}

std::vector<CallEvent> LteEngine::callLog() const
{
    std::lock_guard<std::mutex> lk(impl_->ue_mtx);
    return std::vector<CallEvent>(impl_->call_log.begin(), impl_->call_log.end());
}

} // namespace cellscope::lte
