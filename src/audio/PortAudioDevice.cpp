// Montage — ALSA, Core Audio and WASAPI (shared and exclusive) through PortAudio, built into Montage with only those
// host APIs (CMakeLists.txt). Without MONTAGE_WITH_PORTAUDIO none of them is offered.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "AudioDeviceInternal.h"
#include "RtGuard.h"

#ifdef MONTAGE_WITH_PORTAUDIO
#include <portaudio.h>
#ifdef _WIN32
#include <pa_win_wasapi.h>
#endif
#endif

namespace montage::audiodev::detail {

#ifndef MONTAGE_WITH_PORTAUDIO

bool portAudioHas(Host) { return false; }
std::vector<DeviceInfo> portAudioDevices(Host, std::string* error) {
    if (error) *error = "This build has no PortAudio";
    return {};
}
std::unique_ptr<Stream> openPortAudio(const DeviceSettings&, int, int, int, StreamCallbacks, std::string* error) {
    if (error) *error = "This build has no PortAudio";
    return nullptr;
}

#else

namespace {

// PortAudio is started the first time it is asked for, and kept. It lists the devices when it starts and never again,
// so a device plugged in later (or unplugged and plugged back in) is not there until it is started again, which it
// allows with no stream open: done for a list of devices, and for an open that does not find the device it was given,
// no more often than every few seconds (starting it asks every device what it can do).
std::mutex gPaM;
bool gPaTried = false, gPaUp = false;
int gPaStreams = 0;  // streams open (under gPaM)
std::chrono::steady_clock::time_point gPaListed;
constexpr std::chrono::seconds kRelistEvery{3};

bool paReady() {
    // (a start that failed, the sound system not up yet say, is tried again every few seconds, not given up for good)
    if (!gPaTried || (!gPaUp && std::chrono::steady_clock::now() - gPaListed >= kRelistEvery)) {
        gPaTried = true;
        gPaUp = Pa_Initialize() == paNoError;
        gPaListed = std::chrono::steady_clock::now();
    }
    return gPaUp;
}

// (under gPaM) True when the devices were listed again.
bool paRelist() {
    if (!paReady() || gPaStreams > 0 || std::chrono::steady_clock::now() - gPaListed < kRelistEvery) return false;
    Pa_Terminate();
    gPaUp = Pa_Initialize() == paNoError;
    gPaListed = std::chrono::steady_clock::now();
    return gPaUp;
}

PaHostApiTypeId paType(Host h) {
    switch (h) {
        case Host::Alsa: return paALSA;
        case Host::CoreAudio: return paCoreAudio;
        case Host::Wasapi:
        case Host::WasapiExclusive: return paWASAPI;
        case Host::Asio: return paASIO;
        case Host::Jack: return paJACK;
        default: return paInDevelopment;
    }
}

PaHostApiIndex paIndex(Host h) {
    if (h == Host::Jack || h == Host::Qt || h == Host::Loopback) return -1;  // (JACK goes through its own library)
#ifndef _WIN32
    if (h == Host::WasapiExclusive) return -1;
#endif
    const PaHostApiIndex i = Pa_HostApiTypeIdToHostApiIndex(paType(h));
    return i >= 0 ? i : -1;
}

std::string deviceId(PaDeviceIndex d) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(d);
    return info && info->name ? info->name : std::string();
}

PaDeviceIndex findDevice(PaHostApiIndex api, const std::string& id, bool output) {
    const PaHostApiInfo* h = Pa_GetHostApiInfo(api);
    if (!h) return paNoDevice;
    if (!id.empty())
        for (int k = 0; k < h->deviceCount; ++k) {
            const PaDeviceIndex d = Pa_HostApiDeviceIndexToDeviceIndex(api, k);
            const PaDeviceInfo* info = Pa_GetDeviceInfo(d);
            if (info && deviceId(d) == id && (output ? info->maxOutputChannels : info->maxInputChannels) > 0) return d;
        }
    return output ? h->defaultOutputDevice : h->defaultInputDevice;
}

// Whether the host lists a device of this name (with outputs, or with inputs).
bool listed(PaHostApiIndex api, const std::string& id, bool output) {
    const PaHostApiInfo* h = Pa_GetHostApiInfo(api);
    for (int k = 0; h && k < h->deviceCount; ++k) {
        const PaDeviceIndex d = Pa_HostApiDeviceIndexToDeviceIndex(api, k);
        const PaDeviceInfo* info = Pa_GetDeviceInfo(d);
        if (info && deviceId(d) == id && (output ? info->maxOutputChannels : info->maxInputChannels) > 0) return true;
    }
    return false;
}

class PaStreamBox : public Stream {
public:
    explicit PaStreamBox(StreamCallbacks cb) : cb_(std::move(cb)) {}
    ~PaStreamBox() override {
        if (stream_) {
            closing_ = true;
            std::lock_guard<std::mutex> lock(gPaM);
            Pa_StopStream(stream_);
            Pa_CloseStream(stream_);
            --gPaStreams;
        }
    }
    bool open(const DeviceSettings& s, int rate, int inputs, int outputs, std::string* error) {
        std::lock_guard<std::mutex> lock(gPaM);
        if (!paReady()) {
            if (error) *error = "PortAudio could not start";
            return false;
        }
        PaHostApiIndex api = paIndex(s.host);
        // (a device asked for by name that is not listed may have been plugged in since the devices were listed)
        if (api >= 0 && ((!s.output.empty() && outputs > 0 && !listed(api, s.output, true)) || (!s.input.empty() && inputs > 0 && !listed(api, s.input, false))) &&
            paRelist())
            api = paIndex(s.host);
        if (api < 0) {
            if (error) *error = std::string("This build or system has no ") + hostId(s.host);
            return false;
        }
        const PaDeviceIndex out = findDevice(api, s.output, true), in = findDevice(api, s.input, false);
        const PaDeviceInfo* oi = out != paNoDevice ? Pa_GetDeviceInfo(out) : nullptr;
        const PaDeviceInfo* ii = in != paNoDevice ? Pa_GetDeviceInfo(in) : nullptr;
        outputs = oi ? std::min(outputs, oi->maxOutputChannels) : 0;
        inputs = ii ? std::min(inputs, ii->maxInputChannels) : 0;
        if (outputs <= 0 && inputs <= 0) {
            if (error) *error = "The device has no outputs or inputs";
            return false;
        }
        const int buffer = clampBuffer(s.bufferFrames);
        if (rate <= 0) rate = int(oi ? oi->defaultSampleRate : ii->defaultSampleRate);
        PaStreamParameters op{}, ip{};
#ifdef _WIN32
        PaWasapiStreamInfo wasapiOut{}, wasapiIn{};
        for (PaWasapiStreamInfo* w : {&wasapiOut, &wasapiIn}) {
            w->size = sizeof(PaWasapiStreamInfo);
            w->hostApiType = paWASAPI;
            w->version = 1;
            w->flags = s.host == Host::WasapiExclusive ? paWinWasapiExclusive : 0;
        }
#endif
        if (outputs > 0) {
            op.device = out;
            op.channelCount = outputs;
            op.sampleFormat = paFloat32;
            op.suggestedLatency = double(buffer) / rate;
#ifdef _WIN32
            if (s.host == Host::Wasapi || s.host == Host::WasapiExclusive) op.hostApiSpecificStreamInfo = &wasapiOut;
#endif
        }
        if (inputs > 0) {
            ip.device = in;
            ip.channelCount = inputs;
            ip.sampleFormat = paFloat32;
            ip.suggestedLatency = double(buffer) / rate;
#ifdef _WIN32
            if (s.host == Host::Wasapi || s.host == Host::WasapiExclusive) ip.hostApiSpecificStreamInfo = &wasapiIn;
#endif
        }
        inputs_ = inputs, outputs_ = outputs, buffer_ = buffer;
        PaError e = Pa_OpenStream(&stream_, inputs > 0 ? &ip : nullptr, outputs > 0 ? &op : nullptr, rate, unsigned(buffer),
                                  paClipOff | paDitherOff, &PaStreamBox::callback, this);
        // (a rate the device does not take: its own instead, the engine resampling to it)
        const double own = oi ? oi->defaultSampleRate : ii->defaultSampleRate;
        if (e == paInvalidSampleRate && own > 0 && int(own + 0.5) != rate) {
            rate = int(own + 0.5);
            op.suggestedLatency = ip.suggestedLatency = double(buffer) / rate;
            e = Pa_OpenStream(&stream_, inputs > 0 ? &ip : nullptr, outputs > 0 ? &op : nullptr, rate, unsigned(buffer), paClipOff | paDitherOff,
                              &PaStreamBox::callback, this);
        }
        if (e != paNoError && inputs > 0 && outputs > 0) {
            // (some hosts take no duplex stream across two devices: the outputs alone, then)
            inputs_ = 0;
            e = Pa_OpenStream(&stream_, nullptr, &op, rate, unsigned(buffer), paClipOff | paDitherOff, &PaStreamBox::callback, this);
        }
        if (e != paNoError) {
            stream_ = nullptr;
            if (error) *error = std::string("The device could not be opened: ") + Pa_GetErrorText(e);
            return false;
        }
        ++gPaStreams;
        const PaStreamInfo* info = Pa_GetStreamInfo(stream_);
        rate_ = info && info->sampleRate > 0 ? int(info->sampleRate + 0.5) : rate;
        latency_.input = info && inputs_ > 0 ? int(info->inputLatency * rate_ + 0.5) : 0;
        latency_.output = info && outputs_ > 0 ? int(info->outputLatency * rate_ + 0.5) : 0;
        watch_.reset(rate_);
        name_ = oi ? oi->name : ii->name;
        in_.assign(size_t(kMaxBuffer) * size_t(std::max(1, inputs_)), 0.0f);
        none_.assign(size_t(std::max(buffer_, kMaxBuffer)) * 2, 0.0f);  // (made here: the callback allocates nothing)
        // (a stream that ends without being stopped: its device went away, unplugged or taken; the engine is told)
        Pa_SetStreamFinishedCallback(stream_, &PaStreamBox::finished);
        if ((e = Pa_StartStream(stream_)) != paNoError) {
            if (error) *error = std::string("The device would not start: ") + Pa_GetErrorText(e);
            return false;
        }
        return true;
    }

    int sampleRate() const override { return rate_.load(); }
    int bufferFrames() const override { return buffer_; }
    int inputs() const override { return inputs_; }
    int outputs() const override { return outputs_; }
    Latency latency() const override { return latency_; }
    int64_t xruns() const override { return xruns_.load(); }
    std::string device() const override { return name_; }
    double load() const override { return stream_ ? Pa_GetStreamCpuLoad(stream_) : -1; }

private:
    static int callback(const void* input, void* output, unsigned long frames, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags,
                        void* arg) {
        auto* self = static_cast<PaStreamBox*>(arg);
        const rtguard::Scope scope;
        // PortAudio says nothing when the device's rate is changed under it (its control panel, another program): the
        // rate is judged from the frames the callbacks bring, and followed.
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (const int r = self->watch_.add(int(frames), ns); r > 0) {
            self->rate_ = r;
            if (self->cb_.rateChanged) self->cb_.rateChanged(r);
        }
        if (flags & (paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow)) ++self->xruns_;
        const float* in = static_cast<const float*>(input);
        if (!in && self->inputs_ > 0) {
            std::fill(self->in_.begin(), self->in_.end(), 0.0f);
            in = self->in_.data();
        }
        float* out = static_cast<float*>(output);
        if (!out) {
            // (an input-only stream: the outputs go nowhere; a block longer than the stream was opened for is passed over)
            if (self->cb_.process && size_t(frames) * 2 <= self->none_.size()) self->cb_.process(in, self->inputs_, self->none_.data(), 0, int(frames));
            return paContinue;
        }
        if (self->cb_.process) self->cb_.process(in, self->inputs_, out, self->outputs_, int(frames));
        return paContinue;
    }

    static void finished(void* arg) {
        auto* self = static_cast<PaStreamBox*>(arg);
        if (!self->closing_ && self->cb_.lost) self->cb_.lost("The audio device stopped (unplugged, or taken by another program)");
    }

    StreamCallbacks cb_;
    std::atomic<bool> closing_{false};
    PaStream* stream_ = nullptr;
    std::atomic<int> rate_{0};
    int buffer_ = 0, inputs_ = 0, outputs_ = 0;
    RateWatch watch_;  // (the audio thread's)
    Latency latency_;
    std::string name_;
    std::vector<float> in_, none_;
    std::atomic<int64_t> xruns_{0};
};

}  // namespace

bool portAudioHas(Host h) {
    std::lock_guard<std::mutex> lock(gPaM);
    return paReady() && paIndex(h) >= 0;
}

std::vector<DeviceInfo> portAudioDevices(Host h, std::string* error) {
    std::lock_guard<std::mutex> lock(gPaM);
    std::vector<DeviceInfo> out;
    paRelist();
    const PaHostApiIndex api = paReady() ? paIndex(h) : -1;
    const PaHostApiInfo* info = api >= 0 ? Pa_GetHostApiInfo(api) : nullptr;
    if (!info) {
        if (error) *error = std::string("No ") + hostId(h) + " here";
        return out;
    }
    for (int k = 0; k < info->deviceCount; ++k) {
        const PaDeviceIndex d = Pa_HostApiDeviceIndexToDeviceIndex(api, k);
        const PaDeviceInfo* di = Pa_GetDeviceInfo(d);
        if (!di) continue;
        out.push_back({deviceId(d), di->name ? di->name : "", di->maxInputChannels, di->maxOutputChannels, di->defaultSampleRate});
    }
    return out;
}

std::unique_ptr<Stream> openPortAudio(const DeviceSettings& s, int rate, int inputs, int outputs, StreamCallbacks callbacks, std::string* error) {
    auto st = std::make_unique<PaStreamBox>(std::move(callbacks));
    if (!st->open(s, rate, inputs, outputs, error)) return nullptr;
    return st;
}

#endif

}  // namespace montage::audiodev::detail
