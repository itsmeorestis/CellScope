#include "sdr/libresdr_source.h"

#include "util/log.h"

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/device.hpp>
#include <uhd/exception.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <exception>

// Real UHD handles live here so the public header stays UHD/Boost-free.
struct LibreSdrSource::Impl
{
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::rx_streamer::sptr      stream;
};

namespace {
// Resolve the FPGA image path, trying the given path and a "../" fallback so it
// works whether the app runs from the build dir or the project root.
std::string resolveFpga(const std::string& path)
{
    std::string alt = "../" + path;
    for (const std::string& c : {path, alt})
    {
        if (FILE* f = std::fopen(c.c_str(), "rb"))
        {
            std::fclose(f);
            return c;
        }
    }
    return path; // let UHD report a clear error if truly missing
}
} // namespace

LibreSdrSource::LibreSdrSource() : impl_(new Impl()) {}

LibreSdrSource::~LibreSdrSource()
{
    stop();
}

std::vector<SdrDeviceInfo> LibreSdrSource::listDevices()
{
    std::vector<SdrDeviceInfo> out;
    try
    {
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        uhd::device_addrs_t found = uhd::device::find(hint);
        for (size_t i = 0; i < found.size(); ++i)
        {
            SdrDeviceInfo info;
            info.index = (int)i;
            info.name = found[i].has_key("product") ? found[i]["product"] : "LibreSDR";
            info.serial = found[i].has_key("serial") ? found[i]["serial"] : "";
            out.push_back(info);
        }
    }
    catch (const std::exception& e)
    {
        logWrite("LibreSDR find failed: %s", e.what());
    }
    return out;
}

bool LibreSdrSource::prepare(int deviceIndex, std::string& err)
{
    if (running_.load() || prepared_)
        return true;

    std::string fpga = resolveFpga(fpgaPath_);

    try
    {
        uhd::device_addr_t args;
        args["type"] = "b200";
        args["fpga"] = fpga; // load OUR bitstream onto the device at make()

        // Select a specific device by discovery order when more than one.
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        uhd::device_addrs_t found = uhd::device::find(hint);
        if (found.empty())
        {
            err = "No LibreSDR/B210 device found";
            return false;
        }
        if (deviceIndex < 0 || deviceIndex >= (int)found.size())
            deviceIndex = 0;
        if (found[deviceIndex].has_key("serial"))
            args["serial"] = found[deviceIndex]["serial"];

        logWrite("LibreSDR: opening via UHD (fpga=%s)", fpga.c_str());
        impl_->usrp = uhd::usrp::multi_usrp::make(args);

        impl_->usrp->set_rx_rate(sampleRate_);
        sampleRate_ = impl_->usrp->get_rx_rate();

        impl_->usrp->set_rx_freq(uhd::tune_request_t(centerFreq_));
        centerFreq_ = impl_->usrp->get_rx_freq();

        if (gainDb_ < 0.0)
        {
            try { impl_->usrp->set_rx_agc(true); }
            catch (const std::exception&) { impl_->usrp->set_rx_gain(40.0); }
        }
        else
        {
            impl_->usrp->set_rx_gain(gainDb_);
        }

        double bw = bandwidth_ > 0.0 ? bandwidth_ : sampleRate_;
        impl_->usrp->set_rx_bandwidth(bw);

        try { impl_->usrp->set_rx_antenna(antenna_); }
        catch (const std::exception&) { /* antenna name not supported */ }

        uhd::stream_args_t sargs("fc32", "sc16");
        impl_->stream = impl_->usrp->get_rx_stream(sargs);
    }
    catch (const std::exception& e)
    {
        err = std::string("LibreSDR open failed: ") + e.what();
        if (impl_) { impl_->stream.reset(); impl_->usrp.reset(); }
        return false;
    }

    prepared_ = true;
    return true;
}

std::vector<std::string> LibreSdrSource::rxAntennas()
{
    std::vector<std::string> out;
    // If we already have a prepared/open device, query it directly.
    if (impl_ && impl_->usrp)
    {
        try { out = impl_->usrp->get_rx_antennas(); }
        catch (const std::exception&) {}
        return out;
    }
    // Attempt a lightweight probe without a full open. Only works when no
    // session is active, but that's fine — this is always called from the GUI
    // thread before the prepare() worker starts.
    try
    {
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        uhd::device_addrs_t found = uhd::device::find(hint);
        if (!found.empty())
        {
            // Probe the first device just to read the antenna list.
            uhd::device_addr_t args;
            args["type"] = "b200";
            if (found[0].has_key("serial"))
                args["serial"] = found[0]["serial"];
            auto probe = uhd::usrp::multi_usrp::make(args);
            out = probe->get_rx_antennas();
        }
    }
    catch (const std::exception&) {}
    if (out.empty())
        out = {"RX2", "TX/RX"}; // sane fallback for standard B210
    return out;
}

bool LibreSdrSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    // Open the device if prepare() was not called ahead of time (e.g. the
    // generic/synchronous start path). When prepare() already ran on a worker
    // thread this is a no-op and streaming begins immediately.
    if (!prepared_ && !prepare(deviceIndex, err))
        return false;

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);
    stopReq_.store(false);
    running_.store(true);

    try
    {
        uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        cmd.stream_now = true;
        impl_->stream->issue_stream_cmd(cmd);
    }
    catch (const std::exception& e)
    {
        err = std::string("LibreSDR stream start failed: ") + e.what();
        running_.store(false);
        prepared_ = false;
        impl_->stream.reset();
        impl_->usrp.reset();
        cb_ = nullptr;
        return false;
    }

    prepared_ = false; // consumed
    rxThread_ = std::thread([this]() { rxLoop(); });
    return true;
}

void LibreSdrSource::stop()
{
    stopReq_.store(true);
    if (impl_ && impl_->stream)
    {
        try
        {
            uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            impl_->stream->issue_stream_cmd(cmd);
        }
        catch (const std::exception&) {}
    }
    if (rxThread_.joinable())
        rxThread_.join();

    running_.store(false);
    cb_ = nullptr;
    prepared_ = false;
    if (impl_)
    {
        impl_->stream.reset();
        impl_->usrp.reset();
    }
}

void LibreSdrSource::rxLoop()
{
    const size_t maxSamps = impl_->stream->get_max_num_samps();
    std::vector<std::complex<float>> buff(maxSamps);
    uhd::rx_metadata_t md;

    while (!stopReq_.load())
    {
        size_t n = 0;
        try
        {
            n = impl_->stream->recv(&buff.front(), buff.size(), md, 1.0);
        }
        catch (const std::exception& e)
        {
            logWrite("LibreSDR recv exception: %s", e.what());
            break;
        }

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
            continue;
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
            continue; // UHD auto-recovers; dropped samples are expected under load
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            logWrite("LibreSDR recv error: %s", md.strerror().c_str());
            continue;
        }
        if (n == 0 || !cb_)
            continue;

        if (scratch_.size() < n * 2)
            scratch_.resize(n * 2);
        float* out = scratch_.data();

        if (dcBlock_.load())
        {
            float offRe = dcOffRe_, offIm = dcOffIm_;
            const float rate = dcRate_;
            for (size_t i = 0; i < n; ++i)
            {
                float re = buff[i].real();
                float im = buff[i].imag();
                float ore = re - offRe;
                offRe += ore * rate;
                float oim = im - offIm;
                offIm += oim * rate;
                out[i * 2]     = ore;
                out[i * 2 + 1] = oim;
            }
            dcOffRe_ = offRe;
            dcOffIm_ = offIm;
        }
        else
        {
            for (size_t i = 0; i < n; ++i)
            {
                out[i * 2]     = buff[i].real();
                out[i * 2 + 1] = buff[i].imag();
            }
        }

        cb_(out, (int)n);
    }
}

void LibreSdrSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    std::lock_guard<std::mutex> lk(ctrlMutex_);
    if (impl_ && impl_->usrp)
    {
        try
        {
            impl_->usrp->set_rx_freq(uhd::tune_request_t(hz));
            centerFreq_ = impl_->usrp->get_rx_freq();
        }
        catch (const std::exception& e) { logWrite("LibreSDR set_rx_freq: %s", e.what()); }
    }
}

void LibreSdrSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    std::lock_guard<std::mutex> lk(ctrlMutex_);
    if (impl_ && impl_->usrp)
    {
        try
        {
            impl_->usrp->set_rx_rate(hz);
            sampleRate_ = impl_->usrp->get_rx_rate();
            double bw = bandwidth_ > 0.0 ? bandwidth_ : sampleRate_;
            impl_->usrp->set_rx_bandwidth(bw);
        }
        catch (const std::exception& e) { logWrite("LibreSDR set_rx_rate: %s", e.what()); }
    }
}

void LibreSdrSource::setGain(double db)
{
    gainDb_ = db;
    std::lock_guard<std::mutex> lk(ctrlMutex_);
    if (impl_ && impl_->usrp)
    {
        try
        {
            if (db < 0.0)
                impl_->usrp->set_rx_agc(true);
            else
            {
                impl_->usrp->set_rx_agc(false);
                impl_->usrp->set_rx_gain(db);
            }
        }
        catch (const std::exception& e) { logWrite("LibreSDR set_rx_gain: %s", e.what()); }
    }
}
