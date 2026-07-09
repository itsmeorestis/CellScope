// CellScope shared application state — App struct, spectrum state, and constants.
#pragma once

#include "dsp/iq_ring.h"
#include "dsp/jfft.h"
#include "gui/waterfall.h"
#include "sdr/rtl_sdr_source.h"
#include "sdr/wav_file_source.h"
#include "sdr/sdrpp_server_source.h"
#include "sdr/iq_recorder.h"
#include "decode/band_plan.h"
#include "decode/decoder_manager.h"
#include "update/version_check.h"
#ifdef HAS_LTE
#include "lte/lte_engine.h"
#endif

#include <chrono>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <thread>
#ifdef HAS_LTE
#include <set>
#endif

struct SpectrumView
{
    IqRing ring{1u << 21};
    JFFT   fft;
    Waterfall waterfall;
    std::vector<std::complex<double>> iq;
    std::vector<double> window;
    std::vector<float> inst;
    std::vector<float> avg;
    std::vector<float> sortbuf;
    std::vector<float> freqMHz;
    int   curN = 0;
    float rmsDbfs = -120.0f;
    float frameDbMin = 0.0f, frameDbMax = -120.0f;
    double viewXminMHz = 0.0, viewXmaxMHz = 0.0;
    bool   resetView = true;
    float  specLeftInset = 0.0f, specRightInset = 0.0f;
    bool   fftSkip = false; // set by draw functions when panel is visible, read by processFft next frame
};

struct App
{
    RtlSdrSource    sdr;
    WavFileSource   wav;
    SdrppServerSource server;
    SdrSource*      active = &sdr;
    int  sourceMode = 0; // 0=RTL, 1=WAV, 2=SDR++ Server, 4=Dual RTL
    char wavPath[512] = "";
    bool wavLoop = true;
    char serverHost[128] = "localhost";
    int  serverPort = 5259;
    bool serverCompression = true;
    int  serverSampleType = 1;
    double serverSampleRateMHz = 2.0;

    SpectrumView     viewA;
    SpectrumView     viewB;
    DecoderManager   decoders;
    DecoderManager   decodersB;
    RtlSdrSource     sdrB;

    // Dual-SDR
    bool   dualMode = false;
    int    deviceIndexB = 1;
    double centerFreqMHzB = 1545.0;
    int    sampleRateIdxB = 2;  // 1.024 MHz (lower CPU in dual mode)
    bool   autoGainB = false;
    float  gainDbB = 40.0f;
    bool   biasTeeB = false;
    float  ppmB = 0.0f;

    int  newBaud = 1;
    bool placingDecoder = false;
    bool placingVoiceView = false; // true = started on voice SDR, false = primary
    double placingFreqMHz = 0.0;
    int  selectedDecoder = -1;
    std::vector<float> constBuf;
    double constLim = 1.0;
    std::chrono::steady_clock::time_point constLimTime;

    bool saveDecoders = false; // save decoders in INI for restart
    std::vector<std::pair<double,int>> savedDecoders;  // freqMHz, baud  (spectrum A)
    std::vector<std::pair<double,int>> savedDecodersB; // freqMHz, baud  (spectrum B)

    // IQ recorder
    IqRecorder iqRecorder;
    char iqRecPath[512] = "iq_record.wav";
    float iqBufferSec = 10.0f;  // IQ pre-buffer seconds (0 = disabled)

    bool showAbout = false;
    bool autoAddLes = true;       // auto-create decoders for discovered LES frequencies
    int  maxLesAutoDecoders = 4;  // cap on auto-created LES decoders

    VersionCheck verCheck;

#ifdef HAS_LTE
    // LTE decode engine (srsRAN/FALCON/LTESniffer). Fed the active SDR's IQ.
    cellscope::lte::LteEngine lteEngine;
    bool   lteRunning = false;   // user toggled LTE decode on
    bool   showLte    = true;
    std::set<uint16_t> lteWatched;      // pinned RNTIs ("my phone")
    char   lteUeFilter[64]      = "";   // filter UEs by RNTI/identity
    char   lteTrafficFilter[64] = "";   // filter Traffic by RNTI/text
    bool   lteTrafficWatchedOnly = false;
    bool   lteUeCallsOnly        = false; // show only UEs on a detected call
    // Auto-center: fold the decoded cell's live carrier-frequency offset back into
    // the RTL PPM correction so the cell stays within srsRAN's CFO tracking range
    // (cheap dongles drift tens of ppm, which stalls LTE lock -> constant retries).
    bool   lteAutoCenter  = false;   // continuously re-center as the offset drifts
    double lteLastCenterT = -1000.0; // ImGui::GetTime() of last correction (cooldown)
#endif

    std::vector<SdrDeviceInfo> devices;
    int deviceIndex = 0;

    double centerFreqMHz = 1545.0;
    int    sampleRateIdx = 9;
    bool   autoGain = false;
    float  gainDb = 40.0f;
    bool   biasTee = false;
    float  ppm = 0.0f;

    int   fftSizeIdx = 2;
    float avgAlpha = 0.6f;
    float dbMin = -80.0f;
    float dbMax = 0.0f;
    bool  dcBlock = true;
    bool  autoScale = true;

    std::string status = "Idle";

    bool   bandBrowse = true;
    std::chrono::steady_clock::time_point lastRetune;
    double lastRetuneCtr = 0.0;
    float  browseEdgePct = 24.5f;
    float  browseThrottleMs = 20.0f;
    float  browseMinMovePct = 0.10f;
    bool   acPosOnly = false;
    bool   showEmptyMsgs = false;
    bool   showBandPlan = false;
    bool   showBandPlanB = false;
    std::vector<std::string> bandPlanNames;  // display names (shared)
    std::vector<std::string> bandPlanPaths;  // full file paths (shared)
    int    bandPlanIdx = 0;
    int    bandPlanIdxB = 0;
    BandPlan bandPlanLoaded;
    BandPlan bandPlanLoadedB;
    char   bandPlanDir[256] = "bandplans";

    // Shared search buffer for the Messages / SUs / EGC / LES panels.
    char searchBuf[128] = {};

    int  layoutVersion = 0;
    bool forceDefaultLayout = false;

    // Font size (pt), persisted — requires restart to take effect.
    int  fontSize = 17;

    double lastConfiguredFs = 0.0;

    // Async source startup (used for backends whose open path can block for a
    // while, e.g. connecting to an SDR++ server over the network).
    // The worker only opens the device (prepare()); when it signals startReady
    // the GUI thread finalizes (startActive) so no shared state is touched off
    // the render thread.
    std::atomic<bool> starting{false};
    std::atomic<bool> startReady{false};
    std::string       startErr;
    std::thread       startThread;
};

// ---- shared constants ----
constexpr double kRates[] = {
    0.25e6, 0.9e6, 1.024e6, 1.2e6, 1.4e6, 1.536e6,
    1.8e6, 1.92e6, 2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};
constexpr const char* kRateLabels[] = {
    "0.25", "0.9", "1.024", "1.2", "1.4", "1.536",
    "1.8", "1.92", "2.048", "2.4", "2.56", "2.88", "3.2"};
constexpr int kNumRates = (int)(sizeof(kRates) / sizeof(kRates[0]));

constexpr int    kFftSizes[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536};
constexpr const char* kFftLabels[] = {"1024", "2048", "4096", "8192", "16384", "32768", "65536"};
constexpr int kNumFftSizes = (int)(sizeof(kFftSizes) / sizeof(kFftSizes[0]));

// Dock layout version: bump when the built-in default layout changes.
constexpr int kLayoutVersion = 18;
