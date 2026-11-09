// Montage — ALSA, Core Audio and WASAPI (shared and exclusive) through PortAudio, built into Montage with only those
// host APIs (CMakeLists.txt). Without MONTAGE_WITH_PORTAUDIO none of them is offered.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "AudioDeviceInternal.h"

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

// PortAudio is started once, the first time it is asked for, and kept (its device lists are taken then).
std::mutex gPaM;
bool paReady() {
    static const bool ok = Pa_Initialize() == paNoError;
    return ok;
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

class PaStreamBox : public Stream {
public:
    explicit PaStreamBox(StreamCallbacks cb) : cb_(std::move(cb)) {}
    ~PaStreamBox() override {
        if (stream_) {
            Pa_StopStream(stream_);
            Pa_CloseStream(stream_);
        }
    }
    bool open(const DeviceSettings& s, int rate, int inputs, int outputs, std::string* error) {
        std::lock_guard<std::mutex> lock(gPaM);
        if (!paReady()) {
            if (error) *error = "PortAudio could not start";
            return false;
        }
        const PaHostApiIndex api = paIndex(s.host);
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
        const PaStreamInfo* info = Pa_GetStreamInfo(stream_);
        rate_ = info && info->sampleRate > 0 ? int(info->sampleRate + 0.5) : rate;
        latency_.input = info && inputs_ > 0 ? int(info->inputLatency * rate_ + 0.5) : 0;
        latency_.output = info && outputs_ > 0 ? int(info->outputLatency * rate_ + 0.5) : 0;
        name_ = oi ? oi->name : ii->name;
        in_.assign(size_t(kMaxBuffer) * size_t(std::max(1, inputs_)), 0.0f);
        if ((e = Pa_StartStream(stream_)) != paNoError) {
            if (error) *error = std::string("The device would not start: ") + Pa_GetErrorText(e);
            return false;
        }
        return true;
    }

    int sampleRate() const override { return rate_; }
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
        if (flags & (paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow)) ++self->xruns_;
        const float* in = static_cast<const float*>(input);
        if (!in && self->inputs_ > 0) {
            std::fill(self->in_.begin(), self->in_.end(), 0.0f);
            in = self->in_.data();
        }
        float* out = static_cast<float*>(output);
        if (!out) {
            // (an input-only stream: the outputs go nowhere)
            static thread_local std::vector<float> none;
            none.resize(size_t(frames) * 2);
            if (self->cb_.process) self->cb_.process(in, self->inputs_, none.data(), 0, int(frames));
            return paContinue;
        }
        if (self->cb_.process) self->cb_.process(in, self->inputs_, out, self->outputs_, int(frames));
        return paContinue;
    }

    StreamCallbacks cb_;
    PaStream* stream_ = nullptr;
    int rate_ = 0, buffer_ = 0, inputs_ = 0, outputs_ = 0;
    Latency latency_;
    std::string name_;
    std::vector<float> in_;
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
