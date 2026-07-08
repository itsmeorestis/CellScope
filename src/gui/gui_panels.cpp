#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include "core/app.h"
#include "core/main_funcs.h"
#include "decode/band_plan.h"
#include "i18n/i18n.h"
#include "util/log.h"
#include "version.h"
#include "gui/waterfall.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

// Kick off source startup. Most backends open instantly, but LibreSDR/UHD
// blocks for several seconds in multi_usrp::make() (firmware + FPGA upload), so
// open the device on a worker thread (prepare()) and surface an animated
// "Initializing..." bar. When the worker finishes, the main loop finalizes the
// start on the GUI thread (startActive), so shared app state is only ever
// touched from the render thread.
static void beginStartActive(App& app)
{
#ifdef HAS_LIBRESDR
    if (app.sourceMode == 6)
    {
        if (app.startThread.joinable())
            app.startThread.join();

        // Query available antennas from the hardware (lightweight probe)
        // so the antenna picker in the device panel shows the real list.
        std::vector<std::string> ants = app.libre.rxAntennas();
        if (!ants.empty())
        {
            // Clamp the saved antenna name to one that actually exists.
            bool found = false;
            for (auto& a : ants)
                if (a == std::string(app.libreAntenna)) { found = true; break; }
            if (!found)
                std::strncpy(app.libreAntenna, ants[0].c_str(), sizeof(app.libreAntenna) - 1);
        }

        // Apply config up front so the worker's open() uses the right settings.
        app.libre.setFpgaImage(app.libreFpgaPath);
        app.libre.setSampleRate(app.libreSampleRateMHz * 1e6);
        app.libre.setCenterFreq(app.centerFreqMHz * 1e6);
        app.libre.setGain((double)app.libreGainDb);
        app.libre.setAntenna(app.libreAntenna);
        app.libre.setDcBlock(app.dcBlock);

        app.status = "Initializing LibreSDR...";
        app.startErr.clear();
        app.startReady.store(false);
        app.starting.store(true);
        app.startThread = std::thread([&app]() {
            std::string err;
            bool ok = app.libre.prepare(app.deviceIndex, err);
            app.startErr = ok ? std::string()
                              : (err.empty() ? std::string("open failed") : err);
            app.startReady.store(true);
        });
        return;
    }
#endif
    startActive(app);
}

void drawControls(App& app)
{
    ImGui::Begin((std::string(_L("Control")) + "###Control").c_str());

    bool running = app.active->running();
    bool starting = app.starting.load();

    ImGui::BeginDisabled(running || starting);
    {
        // Explicit (label, mode) pairs so indices stay stable regardless of
        // which optional backends are compiled in.
        struct SrcOpt { const char* label; int mode; };
        static const SrcOpt opts[] = {
            {"RTL-SDR", 0}, {"WAV file", 1}, {"SDR++ Server", 2},
            {"HackRF", 3}, {"Dual RTL", 4},
#ifdef HAS_AIRSPY
            {"Airspy", 5},
#endif
#ifdef HAS_LIBRESDR
            {"LibreSDR", 6},
#endif
        };
        const int nOpts = (int)(sizeof(opts) / sizeof(opts[0]));
        const char* cur = "RTL-SDR";
        for (int i = 0; i < nOpts; ++i)
            if (opts[i].mode == app.sourceMode) cur = opts[i].label;
        if (ImGui::BeginCombo(_L("Source"), cur))
        {
            for (int i = 0; i < nOpts; ++i)
                if (ImGui::Selectable(opts[i].label, opts[i].mode == app.sourceMode))
                    app.sourceMode = opts[i].mode;
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (starting)
    {
        ImGui::BeginDisabled(true);
        ImGui::Button(_L("Start"), ImVec2(120, 0));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", _L("Initializing LibreSDR"));
        // Indeterminate animated bar: UHD's multi_usrp::make() gives no progress,
        // so a negative fraction drives ImGui's scrolling "busy" animation.
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0.0f),
                           _L("Loading firmware + FPGA..."));
    }
    else if (!running)
    {
        bool canStart = (app.sourceMode == 1) ? (app.wavPath[0] != '\0') : true;
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button(_L("Start"), ImVec2(120, 0)))
            beginStartActive(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(app.status.c_str());
    }
    else
    {
        if (ImGui::Button(_L("Stop"), ImVec2(120, 0)))
        {
            app.active->stop();
            app.decoders.stop();
            app.decoders.removeAll();
            if (app.dualMode)
            {
                app.sdrB.stop();
                app.decodersB.stop();
                app.decodersB.removeAll();
            }
            app.dualMode = false;
            app.status = "Idle";
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(app.status.c_str());
    }

    // Version + update banner.
    ImGui::TextDisabled("CellScope v" CELLSCOPE_VERSION);
    {
        VersionCheck::State st = app.verCheck.state();
        if (st == VersionCheck::UpdateAvailable)
        {
            std::string latest = app.verCheck.latestVersion();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Update available: v%s",
                               latest.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Get update"))
            {
#if defined(_WIN32)
                std::string url = app.verCheck.productUrl();
                if (url.empty()) url = "https://sarahsforge.dev/login";
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        else if (st == VersionCheck::UpToDate)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "  (up to date)");
        }
        else if (st == VersionCheck::Unreleased)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "  (unreleased)");
        }
        else if (st == VersionCheck::Checking)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("  checking for updates...");
        }
    }

    ImGui::Separator();

    if (app.sourceMode == 0)
    {
        // ---- RTL-SDR ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());

        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("No RTL-SDR devices. Click Refresh.");
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        }
        if (ImGui::Checkbox(_L("Auto gain (AGC)"), &app.autoGain))
        {
            if (running)
                app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        }
        if (!app.autoGain)
        {
            if (ImGui::SliderFloat("Gain (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f"))
            {
                if (running)
                    app.sdr.setGain((double)app.gainDb);
            }
        }
        if (ImGui::Checkbox(_L("Bias-T"), &app.biasTee))
        {
            if (running)
                app.sdr.setBiasTee(app.biasTee);
        }
        if (ImGui::InputFloat("PPM", &app.ppm, 0.1f, 1.0f, "%.2f"))
        {
            if (running)
                app.sdr.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running)
                app.sdr.setDcBlock(app.dcBlock);
        }
    }
    else if (app.sourceMode == 1)
    {
        // ---- WAV file ----
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##wavpath", app.wavPath, sizeof(app.wavPath));
        ImGui::SameLine();
        if (ImGui::Button(_L("Browse...")))
            openWavDialog(app.wavPath, sizeof(app.wavPath));

        if (ImGui::Checkbox(_L("Loop"), &app.wavLoop))
        {
            if (running)
                app.wav.setLoop(app.wavLoop);
        }
        if (ImGui::InputDouble("Center label (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;

        if (running)
        {
            ImGui::ProgressBar((float)app.wav.progress(), ImVec2(-1, 0));
            ImGui::Text("WAV: %d ch, %d-bit, %.1f kHz",
                        app.wav.channels(), app.wav.bits(), app.wav.sampleRate() / 1e3);
        }
    }
    else if (app.sourceMode == 2)
    {
        // ---- SDR++ Server ----
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("Host", app.serverHost, sizeof(app.serverHost));
        ImGui::InputInt("Port", &app.serverPort);
        const char* sampleTypes[] = {"int8 (low BW)", "int16", "float32 (high BW)"};
        ImGui::Combo("Sample type", &app.serverSampleType, sampleTypes, 3);
        ImGui::Checkbox("Compression (zstd)", &app.serverCompression);
        ImGui::EndDisabled();

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.server.setCenterFreq(app.centerFreqMHz * 1e6);
        }

        // Sample-rate combo, populated from the server's source-module UI once
        // connected. Selecting one drives the server's rate via a UI action.
        if (running)
        {
            auto labels = app.server.sampleRateLabels();
            auto values = app.server.sampleRateValues();
            int curIdx = app.server.currentSampleRateIndex();
            if (!labels.empty())
            {
                const char* preview = (curIdx >= 0 && curIdx < (int)labels.size())
                                          ? labels[curIdx].c_str()
                                          : "(select)";
                if (ImGui::BeginCombo("Sample rate", preview))
                {
                    for (int i = 0; i < (int)labels.size(); ++i)
                    {
                        bool sel = (i == curIdx);
                        if (ImGui::Selectable(labels[i].c_str(), sel) && i < (int)values.size())
                        {
                            app.server.setSampleRate(values[i]);
                            app.viewA.resetView = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextDisabled("Sample rate: (no rate control exposed by server)");
            }
            ImGui::Text("Server sample rate: %.4f MHz", app.server.sampleRate() / 1e6);
        }
        else
        {
            ImGui::TextDisabled("Connect to list the server's sample rates.");
        }
        ImGui::TextDisabled("Gain and device are configured on the SDR++ server.");
    }
    else if (app.sourceMode == 3)
    {
        // ---- HackRF (native) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.hack.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.hack.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::InputDouble("Sample rate (MHz)", &app.hackSampleRateMHz, 1.0, 2.0, "%.3f"))
        {
            if (app.hackSampleRateMHz < 2.0) app.hackSampleRateMHz = 2.0;
            if (app.hackSampleRateMHz > 20.0) app.hackSampleRateMHz = 20.0;
            app.viewA.resetView = true;
            if (running)
                app.hack.setSampleRate(app.hackSampleRateMHz * 1e6);
        }
        if (ImGui::SliderInt("LNA (IF) dB", &app.hackLna, 0, 40, "%d"))
        {
            app.hackLna = (app.hackLna / 8) * 8;
            if (running) app.hack.setLnaGain(app.hackLna);
        }
        if (ImGui::SliderInt("VGA (BB) dB", &app.hackVga, 0, 62, "%d"))
        {
            app.hackVga = (app.hackVga / 2) * 2;
            if (running) app.hack.setVgaGain(app.hackVga);
        }
        if (ImGui::Checkbox("RF amp (+~11 dB)", &app.hackAmp))
        {
            if (running) app.hack.setAmpEnable(app.hackAmp);
        }
        if (ImGui::Checkbox("Bias-T (antenna power)", &app.hackBias))
        {
            if (running) app.hack.setBiasTee(app.hackBias);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.hack.setDcBlock(app.dcBlock);
        }
    }
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
    {
        // ---- Airspy (native) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.airspy.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.airspySampleRateIdx, kAirspyRateLabels, kAirspyNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setSampleRate(kAirspyRates[app.airspySampleRateIdx]);
        }

        ImGui::Separator();
        if (ImGui::RadioButton("Sensitive", app.airspyGainMode == 0)) app.airspyGainMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Linear", app.airspyGainMode == 1)) app.airspyGainMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Free", app.airspyGainMode == 2)) app.airspyGainMode = 2;

        if (app.airspyGainMode == 0)
        {
            if (ImGui::SliderInt("Sensitivity gain", &app.airspySenseGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(0), app.airspy.setSenseGain(app.airspySenseGain);
            }
        }
        else if (app.airspyGainMode == 1)
        {
            if (ImGui::SliderInt("Linearity gain", &app.airspyLinearGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(1), app.airspy.setLinearGain(app.airspyLinearGain);
            }
        }
        else
        {
            if (ImGui::Checkbox("LNA AGC", &app.airspyLnaAgc))
            {
                if (running) app.airspy.setLnaAgc(app.airspyLnaAgc);
            }
            ImGui::BeginDisabled(app.airspyLnaAgc);
            if (ImGui::SliderInt("LNA gain", &app.airspyLnaGain, 0, 15))
            {
                if (running) app.airspy.setLnaGain(app.airspyLnaGain);
            }
            ImGui::EndDisabled();

            if (ImGui::Checkbox("Mixer AGC", &app.airspyMixerAgc))
            {
                if (running) app.airspy.setMixerAgc(app.airspyMixerAgc);
            }
            ImGui::BeginDisabled(app.airspyMixerAgc);
            if (ImGui::SliderInt("Mixer gain", &app.airspyMixerGain, 0, 15))
            {
                if (running) app.airspy.setMixerGain(app.airspyMixerGain);
            }
            ImGui::EndDisabled();

            if (ImGui::SliderInt("VGA gain", &app.airspyVgaGain, 0, 15))
            {
                if (running) app.airspy.setVgaGain(app.airspyVgaGain);
            }
        }
        if (ImGui::Checkbox("Bias T (antenna power)", &app.airspyBias))
        {
            if (running) app.airspy.setBiasTee(app.airspyBias);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.airspy.setDcBlock(app.dcBlock);
        }
    }
#endif
#ifdef HAS_LIBRESDR
    else if (app.sourceMode == 6)
    {
        // ---- LibreSDR (USRP B210 clone via UHD) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.libre.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("FPGA image", app.libreFpgaPath, sizeof(app.libreFpgaPath));
        ImGui::EndDisabled();

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.libre.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::InputDouble("Sample rate (MHz)", &app.libreSampleRateMHz, 0.5, 1.0, "%.3f"))
        {
            if (app.libreSampleRateMHz < 0.2) app.libreSampleRateMHz = 0.2;
            if (app.libreSampleRateMHz > 56.0) app.libreSampleRateMHz = 56.0;
            app.viewA.resetView = true;
            if (running)
                app.libre.setSampleRate(app.libreSampleRateMHz * 1e6);
        }
        std::vector<std::string> libreAnts = app.libre.rxAntennas();
        if (libreAnts.empty())
            libreAnts = {"RX2", "TX/RX"}; // fallback
        ImGui::BeginDisabled(running);
        if (ImGui::BeginCombo("Antenna", app.libreAntenna))
        {
            for (int i = 0; i < (int)libreAnts.size(); ++i)
            {
                bool sel = (libreAnts[i] == std::string(app.libreAntenna));
                if (ImGui::Selectable(libreAnts[i].c_str(), sel))
                {
                    std::strncpy(app.libreAntenna, libreAnts[i].c_str(), sizeof(app.libreAntenna) - 1);
                    app.libreAntenna[sizeof(app.libreAntenna) - 1] = 0;
                    if (running)
                        app.libre.setAntenna(app.libreAntenna);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::SliderFloat("Gain (dB)", &app.libreGainDb, 0.0f, 76.0f, "%.1f"))
        {
            if (running)
                app.libre.setGain((double)app.libreGainDb);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.libre.setDcBlock(app.dcBlock);
        }
    }
#endif
    if (app.sourceMode == 4)
    {
        // ---- Dual RTL: two independent RTL-SDRs ----
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "RTL A (Spectrum / Waterfall A)");
        ImGui::Separator();
        if (ImGui::Button("Refresh A"))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device A", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center A (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;
        ImGui::Combo("Rate A (MHz)", &app.sampleRateIdx, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain A", &app.autoGain)) {}
        if (!app.autoGain)
            ImGui::SliderFloat("Gain A (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T A", &app.biasTee);
        ImGui::InputFloat("PPM A", &app.ppm, 0.1f, 1.0f, "%.2f");
        ImGui::Checkbox("DC block A", &app.dcBlock);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "RTL B (Spectrum / Waterfall B)");
        ImGui::Separator();
        if (ImGui::Button("Refresh B"))
            app.devices = app.sdrB.listDevices();
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndexB, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device B", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndexB == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndexB = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center B (MHz)", &app.centerFreqMHzB, 0.1, 1.0, "%.4f"))
            app.viewB.resetView = true;
        ImGui::Combo("Rate B (MHz)", &app.sampleRateIdxB, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain B", &app.autoGainB)) {}
        if (!app.autoGainB)
            ImGui::SliderFloat("Gain B (dB)", &app.gainDbB, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T B", &app.biasTeeB);
        ImGui::InputFloat("PPM B", &app.ppmB, 0.1f, 1.0f, "%.2f");
        ImGui::Checkbox("DC block B", &app.dcBlock); // same dcblock toggle
    }

    ImGui::Separator();
    ImGui::Combo("FFT size", &app.fftSizeIdx, kFftLabels, kNumFftSizes);
    ImGui::SliderFloat(_L("Averaging"), &app.avgAlpha, 0.0f, 0.98f, "%.2f");
    ImGui::Checkbox(_L("Auto-scale dB"), &app.autoScale);
    ImGui::SliderFloat(_L("dB min"), &app.dbMin, -140.0f, 0.0f, "%.0f");
    ImGui::SliderFloat(_L("dB max"), &app.dbMax, -140.0f, 20.0f, "%.0f");
    if (app.dbMax < app.dbMin + 5.0f)
        app.dbMax = app.dbMin + 5.0f;

    if (ImGui::Button(_L("Reset view (fit band)")))
    {
        app.viewA.resetView = true;
        app.viewB.resetView = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("drag=pan  scroll=zoom  dbl-click=fit");

    ImGui::BeginDisabled(app.sourceMode == 1);
    ImGui::Checkbox("Pan/scroll retunes SDR (browse band)", &app.bandBrowse);
    ImGui::EndDisabled();
    if (app.sourceMode == 1)
        ImGui::TextDisabled("  (WAV: tuning is fixed to the file)");

    // Band plan bar along bottom of spectrum
    if (ImGui::Checkbox(_L("Band Plan"), &app.showBandPlan));
    if (app.showBandPlan)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##bpr"))
            scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
        ImGui::SameLine();
        if (ImGui::SmallButton("Folder##bpf"))
        {
#if defined(_WIN32)
            ShellExecuteA(nullptr, "open", app.bandPlanDir, nullptr, nullptr, SW_SHOW);
#endif
        }
        if (app.bandPlanNames.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(no .json bandplans in bandplans/)");
        }
        else
        {
            if (app.bandPlanIdx >= (int)app.bandPlanNames.size())
                app.bandPlanIdx = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel", &app.bandPlanIdx,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
                    app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
            }
        }
    }

    if (app.dualMode)
    {
        if (ImGui::Checkbox(_L("Band Plan (B)"), &app.showBandPlanB));
        if (app.showBandPlanB && !app.bandPlanNames.empty())
        {
            if (app.bandPlanIdxB >= (int)app.bandPlanNames.size())
                app.bandPlanIdxB = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel-b", &app.bandPlanIdxB,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdxB >= 0 && app.bandPlanIdxB < (int)app.bandPlanPaths.size())
                    app.bandPlanLoadedB = loadBandPlan(app.bandPlanPaths[app.bandPlanIdxB]);
            }
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Display")))
    {
        if (ImGui::SliderInt(_L("Font size"), &app.fontSize, 8, 24, "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            if (app.fontSize < 8)  app.fontSize = 8;
            if (app.fontSize > 24) app.fontSize = 24;
        }
        ImGui::TextDisabled("  Restart to apply");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("IQ Recorder"))
    {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("IQ file", app.iqRecPath, sizeof(app.iqRecPath));
        // Pre-buffer slider (disabled above 3 Msps)
        double fs = app.active->running() ? app.active->sampleRate() : 0.0;
        if (!app.active->running())
            ImGui::BeginDisabled();
        bool overLimit = (fs > 3.0e6);
        if (overLimit)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Pre-buffer (s)", &app.iqBufferSec, 0.0f, 60.0f, "%.0f"))
        {
            app.iqRecorder.configurePrebuffer(fs, app.iqBufferSec);
        }
        if (overLimit)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(disabled > 3 Msps)");
        }
        else if (app.iqBufferSec > 0.0f && fs > 0.0)
        {
            size_t bytes = (size_t)(fs * app.iqBufferSec * 2 * sizeof(float));
            char mem[32];
            if (bytes >= 1024 * 1024 * 1024)
                std::snprintf(mem, sizeof(mem), "(~%.1f GB)", (double)bytes / (1024.0 * 1024.0 * 1024.0));
            else
                std::snprintf(mem, sizeof(mem), "(~%.0f MB)", (double)bytes / (1024.0 * 1024.0));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mem);
        }
        if (!app.active->running())
            ImGui::EndDisabled();
        bool iqRec = app.iqRecorder.isRecording();
        if (iqRec)
        {
            if (ImGui::Button("Stop##iqrec"))
                app.iqRecorder.stop();
        }
        else
        {
            if (ImGui::Button("Start##iqrec"))
            {
                if (app.active && app.active->running())
                    app.iqRecorder.start(app.iqRecPath, app.active->sampleRate());
            }
        }
        if (app.iqRecorder.isRecording())
        {
            double sec = app.iqRecorder.elapsed();
            int m = (int)(sec / 60), s = (int)(sec) % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC %02d:%02d  —  %s",
                               m, s, app.iqRecorder.path().c_str());
        }
    }

    if (running)
    {
        if (app.sourceMode == 0)
        {
            double maxF = app.sdr.tunerMaxFreq();
            if (maxF > 0.0 && app.centerFreqMHz * 1e6 > maxF)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
                ImGui::TextWrapped("  WARNING: %.0f MHz is above this tuner's ~%.0f MHz "
                                   "ceiling. The PLL can't lock here - pick a lower "
                                   "frequency or use an R820T2/R828D dongle.",
                                   app.centerFreqMHz, maxF / 1e6);
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::End();
}

void drawSpectrum(App& app, SpectrumView& v, DecoderManager& mgr, const char* title,
                         bool allowBandBrowse, bool voiceView)
{
    ImGui::Begin(title);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    std::string plotId = std::string("##plot_") + title;
    if (ImPlot::BeginPlot(plotId.c_str(), ImVec2(-1, -1), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("MHz", "dB", 0, 0);

        bool bandValid = (v.curN > 0 && v.freqMHz.front() < v.freqMHz.back());
        if (bandValid)
        {
            double bandSpan = v.freqMHz.back() - v.freqMHz.front();
            ImPlot::SetupAxisZoomConstraints(ImAxis_X1, bandSpan * 1e-4, bandSpan);
        }
        if (v.resetView && bandValid)
            ImPlot::SetupAxisLimits(ImAxis_X1, v.freqMHz.front(), v.freqMHz.back(), ImGuiCond_Always);
        if (app.autoScale || v.resetView)
            ImPlot::SetupAxisLimits(ImAxis_Y1, app.dbMin, app.dbMax, ImGuiCond_Always);

        if (v.curN > 0)
        {
            ImPlot::PlotLine("PSD", v.freqMHz.data(), v.avg.data(), v.curN);
        }

        auto decs = mgr.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.locked ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)  // green = locked
                                  : ImVec4(0.9f, 0.7f, 0.2f, 1.0f); // orange = unlocked
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                mgr.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        v.viewXminMHz = lim.X.Min;
        v.viewXmaxMHz = lim.X.Max;

        // Band-browse retuning: in dual mode use explicit SDR pointers,
        // otherwise use app.active (which covers RTL/WAV/SDR++/HackRF).
        SdrSource* browseSdr;
        if (app.dualMode)
            browseSdr = voiceView ? static_cast<SdrSource*>(&app.sdrB)
                                  : static_cast<SdrSource*>(&app.sdr);
        else
            browseSdr = app.active;
        if (allowBandBrowse && app.bandBrowse && app.sourceMode != 1 &&
            browseSdr->running() && !v.resetView)
        {
            double viewCtr = 0.5 * (v.viewXminMHz + v.viewXmaxMHz);
            double viewHalf = 0.5 * (v.viewXmaxMHz - v.viewXminMHz);
            double sdrCtr = browseSdr->centerFreq() / 1e6;
            double fsMHz = browseSdr->sampleRate() / 1e6;
            double halfBand = 0.5 * fsMHz;
            double marginL = (viewCtr - viewHalf) - (sdrCtr - halfBand);
            double marginR = (sdrCtr + halfBand) - (viewCtr + viewHalf);
            double minMargin = std::min(marginL, marginR);
            double trigger = fsMHz * (app.browseEdgePct * 0.01);
            bool moved = std::fabs(viewCtr - app.lastRetuneCtr) > fsMHz * (app.browseMinMovePct * 0.01);
            auto now = std::chrono::steady_clock::now();
            double sinceMs =
                std::chrono::duration<double, std::milli>(now - app.lastRetune).count();
            if (fsMHz > 0.0 && minMargin < trigger && moved && sinceMs > app.browseThrottleMs)
            {
                if (voiceView)
                {
                    // Retune SDR B preserving decoders on manager B
                    std::vector<std::pair<double, int>> keep;
                    for (auto& s : app.decodersB.status())
                        keep.push_back({s.freqMHz, s.baud});
                    app.centerFreqMHzB = viewCtr;
                    app.sdrB.setCenterFreq(viewCtr * 1e6);
                    app.decodersB.removeAll();
                    app.decodersB.configure(app.sdrB.sampleRate(), app.sdrB.centerFreq());
                    for (auto& k : keep)
                        app.decodersB.addDecoder(k.first * 1e6, k.second);
                }
                else
                {
                    retunePreserving(app, viewCtr);
                }
                app.lastRetune = now;
                app.lastRetuneCtr = viewCtr;
            }
        }

        ImVec2 pp = ImPlot::GetPlotPos();
        ImVec2 ps = ImPlot::GetPlotSize();
        v.specLeftInset = pp.x - origin.x;
        v.specRightInset = (origin.x + availW) - (pp.x + ps.x);

        if (bandValid)
            v.resetView = false;

        // Drag-to-place decoder: Ctrl+mousedown starts placing, move shows a white
        // preview line through the spectrum and waterfall, release creates the decoder.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl)
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                app.placingDecoder = true;
                app.placingVoiceView = voiceView;
                app.placingFreqMHz = mp.x;
            }
        if (app.placingDecoder && app.placingVoiceView == voiceView)
            {
                app.placingFreqMHz = mp.x;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && app.placingDecoder)
            {
                app.placingDecoder = false;
                mgr.addDecoder(mp.x * 1e6, kPlaceholderBaud);
            }
        }
        else if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            // Ctrl released or cursor left the plot that started the placement.
            app.placingDecoder = false;
        }

        // Drag-to-place preview line redraw
        if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            ImPlotRect lim = ImPlot::GetPlotLimits();
            float xMin = (float)lim.X.Min;
            float xMax = (float)lim.X.Max;
            if (xMax > xMin)
            {
                float frac = ((float)app.placingFreqMHz - xMin) / (xMax - xMin);
                ImVec2 pp = ImPlot::GetPlotPos();
                ImVec2 ps = ImPlot::GetPlotSize();
                float px = pp.x + frac * ps.x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(px, pp.y), ImVec2(px, pp.y + ps.y),
                            IM_COL32(255, 40, 40, 200), 1.5f);
            }
        }

        // --- Band plan: solid coloured bar along the bottom ---
        bool showBp = voiceView ? app.showBandPlanB : app.showBandPlan;
        const BandPlan& bp = voiceView ? app.bandPlanLoadedB : app.bandPlanLoaded;
        if (showBp && bp.valid && v.curN > 0)
        {
            const ImPlotRect vp = ImPlot::GetPlotLimits();
            double viewLo = vp.X.Min, viewHi = vp.X.Max;
            if (viewHi <= viewLo) { viewLo = v.freqMHz.front(); viewHi = v.freqMHz.back(); }
            auto* dl = ImPlot::GetPlotDrawList();
            constexpr float kBandH = 28.0f;
            ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
            float bandTop = pp.y + ps.y - kBandH;
            float bandBot = bandTop + kBandH;
            float pxPerMHz = (float)(ps.x / (viewHi - viewLo));
            for (auto& e : bp.entries)
            {
                if (e.hiMHz < viewLo || e.loMHz > viewHi) continue;
                float loPx = pp.x + (float)((std::max(e.loMHz, viewLo) - viewLo) * pxPerMHz);
                float hiPx = pp.x + (float)((std::min(e.hiMHz, viewHi) - viewLo) * pxPerMHz);
                dl->AddRectFilled(ImVec2(loPx, bandTop), ImVec2(hiPx, bandBot), e.color);
                float segW = hiPx - loPx;
                if (segW > 50 && !e.label.empty())
                {
                    float lw = ImGui::CalcTextSize(e.label.c_str()).x;
                    if (lw < segW - 4)
                    {
                        float cx = loPx + (segW - lw) * 0.5f;
                        float cy = bandTop + (kBandH - ImGui::GetTextLineHeight()) * 0.5f;
                        dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 230), e.label.c_str());
                    }
                }
            }
        }

        ImPlot::EndPlot();
        v.fftSkip = false;
    }
    ImGui::End();
}

void drawWaterfall(App& app, SpectrumView& v, const char* title)
{
    (void)app;
    ImGui::Begin(title);

    float uMin = 0.0f, uMax = 1.0f;
    float xLo = 0.0f, xHi = 1.0f;
    if (v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        if (bandSpan > 0.0 && viewSpan > 0.0)
        {
            double visLo = std::max(bandMin, v.viewXminMHz);
            double visHi = std::min(bandMax, v.viewXmaxMHz);
            if (visHi > visLo)
            {
                uMin = (float)((visLo - bandMin) / bandSpan);
                uMax = (float)((visHi - bandMin) / bandSpan);
                xLo = (float)((visLo - v.viewXminMHz) / viewSpan);
                xHi = (float)((visHi - v.viewXminMHz) / viewSpan);
            }
            else
            {
                xLo = xHi = 0.0f;
            }
        }
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left = std::max(0.0f, v.specLeftInset);
    float right = std::max(0.0f, v.specRightInset);
    float w = avail.x - left - right;
    if (w < 1.0f)
    {
        w = avail.x;
        left = 0.0f;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);

    ImVec2 wfP0 = ImGui::GetCursorScreenPos();
    v.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);
    v.fftSkip = false;

    // Drag-to-place preview line: white vertical line through the waterfall
    // at the frequency the user is hovering, so they can centre on a signal.
    if (app.placingDecoder && app.placingVoiceView == (&v == &app.viewB) && v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        double visLo = std::max(bandMin, v.viewXminMHz);
        double visHi = std::min(bandMax, v.viewXmaxMHz);
        if (bandSpan > 0 && viewSpan > 0 &&
            app.placingFreqMHz >= visLo && app.placingFreqMHz <= visHi)
        {
            float u = (float)((app.placingFreqMHz - visLo) / (visHi - visLo));
            float pixFrac = xLo + u * (xHi - xLo);
            float px = wfP0.x + pixFrac * w;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(px, wfP0.y), ImVec2(px, wfP0.y + avail.y),
                        IM_COL32(255, 40, 40, 200), 1.5f);
        }
    }
    ImGui::End();
}

void drawDecoders(App& app)
{
    ImGui::Begin((std::string(_L("Decoders")) + "###Decoders").c_str());

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
    ImGui::Text("%d active  |  %d sub-band(s)  %d threads", (int)decs.size(),
                app.decoders.subbandCount() + (app.dualMode ? app.decodersB.subbandCount() : 0),
                app.decoders.workerCount() + (app.dualMode ? app.decodersB.workerCount() : 0));
    uint64_t drops = app.decoders.drops() + (app.dualMode ? app.decodersB.drops() : 0);
    ImGui::SameLine();
    if (drops > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  drops: %llu",
                           (unsigned long long)drops);
    else
        ImGui::TextDisabled("  drops: 0");
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Remove all")))
    {
        app.decoders.removeAll();
        if (app.dualMode) app.decodersB.removeAll();
    }

    if (ImGui::Checkbox("Save decoders on restart", &app.saveDecoders))
    {
    }

    ImGui::Separator();

    if (ImGui::BeginTable("##decs", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Freq MHz");
        ImGui::TableSetupColumn("Baud");
        ImGui::TableSetupColumn("Msgs");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        int toRemove = -1;
    bool toRemoveB = false;
        for (auto& d : decs)
        {
            int uid = d.channelId + (d.isB ? 100000 : 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char selid[24];
            std::snprintf(selid, sizeof(selid), "##sel%d", uid);
            ImVec4 c = d.locked ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
                                : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Header, c);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.x*1.3f, c.y*1.3f, c.z*1.3f, 1.0f));
            bool sel = (app.selectedDecoder == d.channelId);
            if (ImGui::Selectable(selid, sel, ImGuiSelectableFlags_None))
            {
                app.selectedDecoder = d.channelId;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextColored(c, "%s", d.locked ? "LOCK" : "--");
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", d.freqMHz);
            ImGui::TableNextColumn();
            if (d.baud == kEgcBaud)
            {
                if (d.egcCType == 1)
                    ImGui::TextUnformatted("EGC (NCS)");
                else if (d.egcCType == 2)
                    ImGui::TextUnformatted("EGC (LES)");
                else
                    ImGui::TextUnformatted("EGC");
            }
            else if (d.baud == kPlaceholderBaud)
                ImGui::TextUnformatted("Placeholder");
            else
                ImGui::Text("%d", d.baud);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)d.msgs);
            ImGui::TableNextColumn();
            char btn[24];
            std::snprintf(btn, sizeof(btn), "X##%d", uid);
            if (ImGui::SmallButton(btn))
            {
                toRemove = d.channelId;
                toRemoveB = d.isB;
            }
        }
        ImGui::EndTable();
        if (toRemove >= 0)
        {
            if (toRemoveB) app.decodersB.removeDecoder(toRemove);
            else           app.decoders.removeDecoder(toRemove);
        }
    }

    ImGui::End();
}

void drawSUs(App& app)
{
    ImGui::Begin((std::string(_L("Placeholder")) + "###Placeholder").c_str());

    unsigned long long suTotal = app.decoders.suLog().count();
    if (app.dualMode) suTotal += app.decodersB.suLog().count();
    ImGui::Text("%llu total", suTotal);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.suLog().clear();
        if (app.dualMode) app.decodersB.suLog().clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchsu", "Search...", app.searchBuf, sizeof(app.searchBuf));

    ImGui::Separator();

    auto msgs = app.decoders.suLog().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.suLog().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
    std::sort(msgs.begin(), msgs.end(),
              [](const DecodedMessage& a, const DecodedMessage& b) { return a.timeSec > b.timeSec; });
    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }
    if (ImGui::BeginTable("##sus", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthFixed, 220);
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (auto it = msgs.begin(); it != msgs.end(); ++it)
        {
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->hex;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
            ImGui::TableNextRow();

            // Colorize by SU type
            ImVec4 col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); // default gray
            if (it->suType == 0x30 && it->aesId != 0)      // Call progress — per-aircraft color
            {
                float hue = (it->aesId * 0.618033988749895f); // golden ratio conjugate
                hue = hue - (int)hue;
                ImGui::ColorConvertHSVtoRGB(hue, 0.8f, 0.9f, col.x, col.y, col.z);
            }
            else if (it->suType == 0x21)                    // Call announcement
                col = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);     // gold
            else if (it->suType >= 0x31 && it->suType <= 0x34) // C-channel assignment
                col = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);     // blue

            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextColored(col, "%s", it->text.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->hex.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ImPlot getter over the interleaved (I,Q) float pairs in app.constBuf.
static ImPlotPoint constGetter(int idx, void* data)
{
    const float* p = static_cast<const float*>(data);
    return ImPlotPoint(p[2 * idx], p[2 * idx + 1]);
}

void drawConstellation(App& app)
{
    ImGui::Begin((std::string(_L("Constellation")) + "###Constellation").c_str());

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
    int chan = app.selectedDecoder;
    bool valid = false;
    double freq = 0.0;
    for (auto& d : decs)
        if (d.channelId == chan)
        {
            valid = true;
            freq = d.freqMHz;
            break;
        }
    if (!valid && !decs.empty())
    {
        chan = decs.front().channelId;
        freq = decs.front().freqMHz;
    }

    if (decs.empty())
    {
        ImGui::TextDisabled("No decoders. Ctrl+click the spectrum to add one.");
        ImGui::End();
        return;
    }

    // Decoder selector (also selectable by clicking a row in Decoders panel).
    int preBaud = 0;
    bool preIsB = false;
    for (auto& d : decs)
    {
        if (d.channelId == chan)
        {
            preBaud = d.baud;
            preIsB = d.isB;
            break;
        }
    }
    char preview[128];
    const char* baudStr = (preBaud == kEgcBaud) ? "EGC" : nullptr;
    if (baudStr)
        std::snprintf(preview, sizeof(preview), "Channel %d  %.4f MHz  %s%s",
                      chan, freq, baudStr, preIsB ? " [B]" : "");
    else
        std::snprintf(preview, sizeof(preview), "Channel %d  %.4f MHz  @%d%s",
                      chan, freq, preBaud, preIsB ? " [B]" : "");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("Decoder", preview))
    {
        for (auto& d : decs)
        {
            char label[64];
            const char* b = (d.baud == kEgcBaud) ? "EGC" : nullptr;
            if (b)
                std::snprintf(label, sizeof(label), "Channel %d  %.4f MHz  %s%s",
                              d.channelId, d.freqMHz, b,
                              d.isB ? " [B]" : "");
            else
                std::snprintf(label, sizeof(label), "Channel %d  %.4f MHz  @%d%s",
                              d.channelId, d.freqMHz, d.baud,
                              d.isB ? " [B]" : "");
            if (ImGui::Selectable(label, d.channelId == chan))
            {
                app.selectedDecoder = d.channelId;
                chan = d.channelId;
                freq = d.freqMHz;
            }
        }
        ImGui::EndCombo();
    }

    int pairs = app.decoders.getConstellation(chan, app.constBuf, 1024);
    if (pairs == 0 && app.dualMode)
        pairs = app.decodersB.getConstellation(chan, app.constBuf, 1024);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d pts)", pairs);

    // Recompute the axis scale at most once per second so it holds steady
    // instead of jittering as the constellation data changes every frame.
    auto nowC = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(nowC - app.constLimTime).count() >= 1.0)
    {
        float m = 0.5f;
        for (float v : app.constBuf)
            m = std::max(m, std::fabs(v));
        app.constLim = m * 1.15;
        app.constLimTime = nowC;
    }
    double lim = app.constLim;

    if (ImPlot::BeginPlot("##const", ImVec2(-1, -1),
                          ImPlotFlags_Equal | ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels,
                          ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, -lim, lim, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -lim, lim, ImGuiCond_Always);
        if (pairs > 0)
            ImPlot::PlotScatterG("IQ", constGetter, app.constBuf.data(), pairs);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

void drawAbout(App& app)
{
    if (!app.showAbout)
        return;

    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin((std::string(_L("About CellScope")) + "###About CellScope").c_str(), &app.showAbout,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoCollapse))
    {
        ImGui::TextWrapped("CellScope v" CELLSCOPE_VERSION);
        ImGui::Separator();
        ImGui::TextWrapped("CellScope was created by Sarah Rose.");
        ImGui::Spacing();
        ImGui::TextWrapped("Built with components from:");
        ImGui::TextDisabled("  JAERO (Jontio)");
        ImGui::TextDisabled("  inmarsat-sniffer");
        ImGui::TextDisabled("  scytaleC (Thierry Leconte)");
        ImGui::TextDisabled("  DeDECTive (Sarah Rose)");
        ImGui::Spacing();
        ImGui::TextWrapped("Thanks to Arclamp VK4SUS for providing a server for accessing the satellite during development.");
        ImGui::Spacing();
        ImGui::TextWrapped("Thanks to Mike AA8IA for donating an Airspy R2 and Airspy Mini for development.");
    }
    ImGui::End();
}

#ifdef HAS_LTE
static void lteFmtRate(double bps, char* out, size_t n)
{
    if (bps >= 1e6)      std::snprintf(out, n, "%.2f Mbps", bps / 1e6);
    else if (bps >= 1e3) std::snprintf(out, n, "%.1f kbps", bps / 1e3);
    else                 std::snprintf(out, n, "%.0f bps", bps);
}

// "LTE" panel: controls, state, cell info, DL-throughput chart, connection feed.
void drawLte(App& app)
{
    if (!app.showLte)
        return;

    ImGui::Begin((std::string(_L("LTE")) + "###LTE").c_str());

    namespace lte = cellscope::lte;

    const bool srcRunning = app.active && app.active->running();
    if (!app.lteRunning)
    {
        ImGui::BeginDisabled(!srcRunning);
        if (ImGui::Button("Start LTE decode"))
        {
            lte::EngineConfig cfg;
            cfg.input_sample_rate_hz = app.active->sampleRate();
            cfg.center_freq_mhz      = app.active->centerFreq() / 1e6;
            cfg.nof_rx_antennas      = 1;
            app.lteEngine.start(cfg);
            app.lteRunning = true;
        }
        ImGui::EndDisabled();
        if (!srcRunning)
            ImGui::TextDisabled("Start an SDR source first.");
    }
    else
    {
        if (ImGui::Button("Stop LTE decode"))
        {
            app.lteEngine.stop();
            app.lteRunning = false;
        }
    }
    ImGui::SameLine();

    const char* stateStr = "Idle";
    switch (app.lteEngine.state())
    {
        case lte::EngineState::Idle:      stateStr = "Idle"; break;
        case lte::EngineState::Searching: stateStr = "Searching"; break;
        case lte::EngineState::CellFound: stateStr = "Cell found"; break;
        case lte::EngineState::Decoding:  stateStr = "Decoding"; break;
        case lte::EngineState::Error:     stateStr = "Error"; break;
    }
    ImGui::Text("State: %s", stateStr);

    lte::CellInfo ci;
    if (app.lteEngine.cell(ci))
    {
        ImGui::SameLine();
        ImGui::TextDisabled("| PCI %d  %d PRB  %d ports  SFN %d  %.3f MHz",
                            ci.pci, ci.nof_prb, ci.nof_ports, ci.sfn, ci.freq_mhz);
    }
    if (ci.have_sib)
    {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                           "SIB1:  %s   PLMN %s   TAC 0x%X (%u)   Cell ID %llu   Band %d%s",
                           ci.oper.empty() ? "(operator unknown)" : ci.oper.c_str(),
                           ci.plmn.c_str(), ci.tac, ci.tac,
                           (unsigned long long)ci.cell_id, ci.band,
                           ci.barred ? "   [BARRED]" : "");
    }
    ImGui::TextWrapped("%s", app.lteEngine.statusText().c_str());

    // ---- Cell-wide DL throughput + active-UE count over time ----
    auto hist = app.lteEngine.history();
    if (ImPlot::BeginPlot("##lte_thr", ImVec2(-1, -80)))
    {
        ImPlot::SetupAxes("time (s)", "DL kbps",
                          ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxis(ImAxis_Y2, "active UEs", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        if (!hist.empty())
        {
            std::vector<float> tx, ty, ta;
            tx.reserve(hist.size());
            ty.reserve(hist.size());
            ta.reserve(hist.size());
            for (auto& h : hist)
            {
                tx.push_back((float)h.t_sec);
                ty.push_back((float)h.dl_kbps);
                ta.push_back((float)h.active_ues);
            }
            ImPlot::SetAxis(ImAxis_Y1);
            ImPlot::PlotShaded("DL kbps", tx.data(), ty.data(), (int)tx.size(), 0.0);
            ImPlot::PlotLine("DL kbps", tx.data(), ty.data(), (int)tx.size());
            ImPlot::SetAxis(ImAxis_Y2);
            ImPlot::PlotLine("active UEs", tx.data(), ta.data(), (int)tx.size());
        }
        // Overlay a throughput line for each pinned UE ("my phone").
        ImPlot::SetAxis(ImAxis_Y1);
        for (uint16_t rnti : app.lteWatched)
        {
            auto uh = app.lteEngine.ueHistory(rnti);
            if (uh.size() < 2) continue;
            std::vector<float> ux, uy;
            ux.reserve(uh.size());
            uy.reserve(uh.size());
            for (auto& h : uh) { ux.push_back((float)h.t_sec); uy.push_back((float)h.dl_kbps); }
            char lbl[24];
            std::snprintf(lbl, sizeof(lbl), "0x%04X", rnti);
            ImPlot::PlotLine(lbl, ux.data(), uy.data(), (int)ux.size());
        }
        ImPlot::EndPlot();
    }

    // ---- "New phone connected" feed ----
    auto conns = app.lteEngine.connections();
    ImGui::Text("New connections: %d", (int)conns.size());
    if (ImGui::BeginChild("lte_conns", ImVec2(0, 0), true))
    {
        for (auto it = conns.rbegin(); it != conns.rend(); ++it)
            ImGui::Text("t=%7.1fs   new UE  RNTI 0x%04X", it->t_sec, it->rnti);
    }
    ImGui::EndChild();

    ImGui::End();
}

// "UEs" panel: per-UE table sorted by current DL rate, with usage bars.
void drawLteUes(App& app)
{
    if (!app.showLte)
        return;

    ImGui::Begin((std::string(_L("UEs")) + "###UEs").c_str());

    namespace lte = cellscope::lte;

    uint64_t nowms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    auto ues = app.lteEngine.snapshotUes();

    // Push the current pinned set to the engine (for per-UE history plotting).
    {
        std::vector<uint16_t> w(app.lteWatched.begin(), app.lteWatched.end());
        app.lteEngine.setWatched(w);
    }

    double maxRate = 1.0;
    for (auto& u : ues) maxRate = std::max(maxRate, u.dl_bps);

    auto lower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        return s;
    };

    // Filter + pin controls.
    ImGui::SetNextItemWidth(170);
    ImGui::InputTextWithHint("##uefilter", "filter RNTI / identity...", app.lteUeFilter, sizeof(app.lteUeFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Pin top talker") && !ues.empty())
    {
        auto top = std::max_element(ues.begin(), ues.end(),
                                    [](const lte::UeStat& a, const lte::UeStat& b) { return a.dl_bytes < b.dl_bytes; });
        if (top != ues.end()) app.lteWatched.insert(top->rnti);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear pins")) app.lteWatched.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Calls only", &app.lteUeCallsOnly);
    int activeCalls = 0;
    for (auto& u : ues) if (u.voice) activeCalls++;
    ImGui::SameLine();
    ImGui::Text("UEs: %d   Pinned: %d   Calls: %d", (int)ues.size(), (int)app.lteWatched.size(), activeCalls);
    ImGui::TextDisabled("Click a row to pin/unpin it (tracks it on the chart).");

    // Apply text filter.
    std::string filt = lower(app.lteUeFilter);
    if (!filt.empty())
    {
        ues.erase(std::remove_if(ues.begin(), ues.end(),
                                 [&](const lte::UeStat& u) {
                                     char hex[16];
                                     std::snprintf(hex, sizeof(hex), "%04x", u.rnti);
                                     return std::string(hex).find(filt) == std::string::npos &&
                                            lower(u.identity).find(filt) == std::string::npos;
                                 }),
                  ues.end());
    }
    if (app.lteUeCallsOnly)
    {
        ues.erase(std::remove_if(ues.begin(), ues.end(),
                                 [](const lte::UeStat& u) { return !u.voice; }),
                  ues.end());
    }

    if (ImGui::BeginTable("lte_ues", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("RNTI", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 50);
        ImGui::TableSetupColumn("DL rate", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("DL total",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                90);
        ImGui::TableSetupColumn("msgs", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 60);
        ImGui::TableSetupColumn("MCS", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 40);
        ImGui::TableSetupColumn("identity", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();

        // Sort by the clicked header (default: DL total, descending). Re-sorted
        // every frame because the underlying values change continuously.
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
        {
            if (ss->SpecsCount > 0)
            {
                const ImGuiTableColumnSortSpecs& s = ss->Specs[0];
                const bool asc = (s.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(ues.begin(), ues.end(),
                          [&](const lte::UeStat& a, const lte::UeStat& b) {
                              if (s.ColumnIndex == 6)
                                  return asc ? a.identity < b.identity : a.identity > b.identity;
                              double d = 0.0;
                              switch (s.ColumnIndex)
                              {
                                  case 0: d = (double)a.rnti - (double)b.rnti; break;
                                  case 1: d = ((a.voice ? 1.0 : 0.0) + a.voice_score) -
                                             ((b.voice ? 1.0 : 0.0) + b.voice_score); break;
                                  case 2: d = a.dl_bps - b.dl_bps; break;
                                  case 3: d = (double)a.dl_bytes - (double)b.dl_bytes; break;
                                  case 4: d = (double)a.dl_msgs - (double)b.dl_msgs; break;
                                  case 5: d = (double)a.last_mcs - (double)b.last_mcs; break;
                                  default: break;
                              }
                              if (d == 0.0) return a.rnti < b.rnti; // stable tiebreak
                              return asc ? d < 0.0 : d > 0.0;
                          });
            }
        }
        // Pinned UEs always float to the top (keeps "my phone" visible).
        std::stable_partition(ues.begin(), ues.end(),
                              [&](const lte::UeStat& u) { return app.lteWatched.count(u.rnti) > 0; });

        for (auto& u : ues)
        {
            const bool pinned    = app.lteWatched.count(u.rnti) > 0;
            const bool recentNew = (nowms - u.first_seen_ms) < 5000;
            const bool active    = (nowms - u.last_seen_ms) < 3000;
            ImGui::TableNextRow();
            if (pinned)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(0.20f, 0.32f, 0.10f, 0.65f)));

            ImGui::TableNextColumn();
            ImVec4 col = recentNew ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                                   : (active ? ImVec4(1, 1, 1, 1) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            if (u.voice) col = ImVec4(1.0f, 0.65f, 0.15f, 1.0f); // on a call -> orange
            char label[32];
            std::snprintf(label, sizeof(label), "%s0x%04X", pinned ? "* " : "", u.rnti);
            ImGui::PushID((int)u.rnti);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(label, pinned, ImGuiSelectableFlags_SpanAllColumns))
            {
                if (pinned) app.lteWatched.erase(u.rnti);
                else        app.lteWatched.insert(u.rnti);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();

            ImGui::TableNextColumn();
            if (u.voice)
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "CALL");

            ImGui::TableNextColumn();
            {
                char rbuf[32];
                lteFmtRate(u.dl_bps, rbuf, sizeof(rbuf));
                ImGui::ProgressBar((float)(u.dl_bps / maxRate), ImVec2(-1.0f, 0), rbuf);
            }
            ImGui::TableNextColumn();
            {
                double kb = u.dl_bytes / 1024.0;
                if (kb >= 1024.0) ImGui::Text("%.2f MB", kb / 1024.0);
                else              ImGui::Text("%.1f KB", kb);
            }
            ImGui::TableNextColumn(); ImGui::Text("%u", u.dl_msgs);
            ImGui::TableNextColumn(); ImGui::Text("%d", u.last_mcs);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(u.identity.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// "Traffic" panel: rolling human-readable message log.
void drawLteTraffic(App& app)
{
    if (!app.showLte)
        return;

    ImGui::Begin((std::string(_L("Traffic")) + "###Traffic").c_str());

    namespace lte = cellscope::lte;

    static std::deque<lte::TrafficMsg> log;
    for (auto& m : app.lteEngine.drainTraffic())
        log.push_back(m);
    while (log.size() > 500)
        log.pop_front();

    auto lower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        return s;
    };

    ImGui::SetNextItemWidth(170);
    ImGui::InputTextWithHint("##trfilter", "filter RNTI / text...", app.lteTrafficFilter, sizeof(app.lteTrafficFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Pinned only", &app.lteTrafficWatchedOnly);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) log.clear();

    std::string filt = lower(app.lteTrafficFilter);

    ImGui::Text("Traffic (%d)", (int)log.size());
    if (ImGui::BeginChild("lte_traffic", ImVec2(0, 0), true))
    {
        for (auto it = log.rbegin(); it != log.rend(); ++it)
        {
            if (app.lteTrafficWatchedOnly && !app.lteWatched.count(it->rnti))
                continue;
            char line[192];
            std::snprintf(line, sizeof(line), "[%s] 0x%04X %s  %s (%u B)",
                          it->direction == 0 ? "DL" : "UL", it->rnti,
                          it->channel.c_str(), it->summary.c_str(), it->len);
            if (!filt.empty() && lower(line).find(filt) == std::string::npos)
                continue;
            const bool pinned = app.lteWatched.count(it->rnti) > 0;
            if (pinned)
                ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.5f, 1.0f), "%s", line);
            else
                ImGui::TextUnformatted(line);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// "Calls" panel: detected VoLTE calls (active + start/stop log).
void drawLteCalls(App& app)
{
    if (!app.showLte)
        return;

    ImGui::Begin((std::string(_L("Calls")) + "###Calls").c_str());

    namespace lte = cellscope::lte;

    uint64_t nowms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    auto ues = app.lteEngine.snapshotUes();
    int  activeCalls = 0;
    for (auto& u : ues) if (u.voice) activeCalls++;

    ImGui::Text("Active calls: %d", activeCalls);
    ImGui::TextDisabled("Heuristic: VoLTE ~20 ms grant cadence, small TBs, sustained.");

    if (ImGui::BeginTable("lte_active_calls", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 130)))
    {
        ImGui::TableSetupColumn("RNTI", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Conf", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Identity", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto& u : ues)
        {
            if (!u.voice) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "0x%04X", u.rnti);
            ImGui::TableNextColumn();
            double dur = u.call_start_ms ? (nowms - u.call_start_ms) / 1000.0 : 0.0;
            ImGui::Text("%d:%02d", (int)dur / 60, (int)dur % 60);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", u.voice_score * 100.0f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(u.identity.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    auto calls = app.lteEngine.callLog();
    ImGui::Text("Call log (%d)", (int)calls.size());
    if (ImGui::BeginChild("lte_calllog", ImVec2(0, 0), true))
    {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it)
        {
            if (it->started)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                   "t=%7.1fs  CALL START  0x%04X%s  %s",
                                   it->t_sec, it->rnti, it->sps ? " [SPS]" : "", it->identity.c_str());
            else
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                   "t=%7.1fs  call end    0x%04X  (%.0fs)  %s",
                                   it->t_sec, it->rnti, it->duration_s, it->identity.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
#endif // HAS_LTE

// ---------------------------------------------------------------------------
// Persistent settings: serialized into cellscope.ini alongside the ImGui dock
// layout via a custom settings handler.
// ---------------------------------------------------------------------------

void drawDockHost(App& app)
{
    // The default dock layout is a baked-in ImGui .ini loaded from main() (on
    // first run / version bump / Reset Layout). Here we only host the dockspace;
    // a saved cellscope.ini or that default provides the arrangement.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##CellScopeHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockId = ImGui::GetID("CellScopeDockSpace");
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_NoUndocking);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu(_L("View")))
        {
            if (ImGui::MenuItem(_L("Reset Layout")))
                app.forceDefaultLayout = true;
#ifdef HAS_LTE
            ImGui::MenuItem(_L("LTE"), nullptr, &app.showLte);
#endif
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_L("Help")))
        {
            if (ImGui::MenuItem(_L("About")))
                app.showAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

