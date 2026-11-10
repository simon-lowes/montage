#include "AudioDevice.h"

#include <QJsonArray>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "AudioDeviceInternal.h"

namespace montage::audiodev {

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

const char* hostId(Host h) {
    switch (h) {
        case Host::Qt: return "qt";
        case Host::Jack: return "jack";
        case Host::Alsa: return "alsa";
        case Host::CoreAudio: return "coreaudio";
        case Host::Wasapi: return "wasapi";
        case Host::WasapiExclusive: return "wasapi_exclusive";
        case Host::Asio: return "asio";
        case Host::Loopback: return "loopback";
    }
    return "qt";
}

std::optional<Host> hostFromId(const std::string& id) {
    for (Host h : {Host::Qt, Host::Jack, Host::Alsa, Host::CoreAudio, Host::Wasapi, Host::WasapiExclusive, Host::Asio, Host::Loopback})
        if (id == hostId(h)) return h;
    return std::nullopt;
}

bool hostBuilt(Host h) {
    switch (h) {
        case Host::Qt:
        case Host::Loopback: return true;
        case Host::Jack: return detail::jackLoadable();
        default: return detail::portAudioHas(h);
    }
}

std::vector<Host> availableHosts() {
    std::vector<Host> out{Host::Qt};
    for (Host h : {Host::Jack, Host::Alsa, Host::CoreAudio, Host::Wasapi, Host::WasapiExclusive, Host::Asio})
        if (hostBuilt(h)) out.push_back(h);
    return out;
}

std::vector<DeviceInfo> devices(Host h, std::string* error) {
    switch (h) {
        case Host::Qt: return {};
        case Host::Loopback: return {{"loopback", "Loopback", 64, 64, 48000}};
        case Host::Jack: return detail::jackDevices(error);
        default: return detail::portAudioDevices(h, error);
    }
}

int clampBuffer(int frames) {
    int b = kMinBuffer;
    while (b < kMaxBuffer && b < frames) b *= 2;
    // (the nearer power of two)
    if (b > kMinBuffer && frames > 0 && frames - b / 2 < b - frames) b /= 2;
    return b;
}

QJsonObject DeviceSettings::toJson() const {
    QJsonArray names;
    for (const std::string& n : inputNames) names.append(QString::fromStdString(n));
    QJsonObject o{{"host", hostId(host)},
                  {"output", QString::fromStdString(output)},
                  {"input", QString::fromStdString(input)},
                  {"sample_rate", sampleRate},
                  {"buffer", bufferFrames},
                  {"clock", QString::fromStdString(clock)},
                  {"inputs", inputs},
                  {"input_names", names},
                  {"auto_connect", autoConnect}};
    if (loopbackFrames > 0)
        o["loopback"] = QJsonObject{{"frames", loopbackFrames}, {"rate", loopbackRate}, {"buffer", loopbackBuffer}};
    return o;
}

DeviceSettings DeviceSettings::fromJson(const QJsonObject& o) {
    DeviceSettings s;
    s.host = hostFromId(o.value("host").toString().toStdString()).value_or(Host::Qt);
    s.output = o.value("output").toString().toStdString();
    s.input = o.value("input").toString().toStdString();
    s.sampleRate = std::clamp(o.value("sample_rate").toInt(0), 0, 384000);
    if (s.sampleRate > 0 && s.sampleRate < 8000) s.sampleRate = 0;
    s.bufferFrames = clampBuffer(o.value("buffer").toInt(256));
    s.clock = o.value("clock").toString().toStdString();
    s.inputs = std::clamp(o.value("inputs").toInt(2), 0, 64);
    for (const QJsonValue& v : o.value("input_names").toArray()) s.inputNames.push_back(v.toString().toStdString());
    s.autoConnect = o.value("auto_connect").toBool(true);
    const QJsonObject lb = o.value("loopback").toObject();
    s.loopbackFrames = std::max(0, lb.value("frames").toInt());
    s.loopbackRate = lb.value("rate").toInt();
    s.loopbackBuffer = lb.value("buffer").toInt();
    return s;
}

bool DeviceSettings::operator==(const DeviceSettings& o) const {
    return host == o.host && output == o.output && input == o.input && sampleRate == o.sampleRate && bufferFrames == o.bufferFrames &&
           clock == o.clock && inputs == o.inputs && inputNames == o.inputNames && autoConnect == o.autoConnect &&
           loopbackFrames == o.loopbackFrames && loopbackRate == o.loopbackRate && loopbackBuffer == o.loopbackBuffer;
}

std::string inputName(const DeviceSettings& s, int index) {
    if (index >= 0 && index < int(s.inputNames.size()) && !s.inputNames[size_t(index)].empty()) return s.inputNames[size_t(index)];
    return "In " + std::to_string(index + 1);
}

int roundTrip(const DeviceSettings& s, int rate, int buffer, Latency reported, bool* measured) {
    const bool m = s.loopbackFrames > 0 && s.loopbackRate == rate && s.loopbackBuffer == buffer;
    if (measured) *measured = m;
    return m ? s.loopbackFrames : std::max(0, reported.input) + std::max(0, reported.output);
}

int64_t findClick(const float* samples, int64_t n, int stride, float floor) {
    if (!samples || n <= 0 || stride <= 0) return -1;
    float peak = 0;
    for (int64_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(samples[i * stride]));
    if (peak < floor) return -1;
    for (int64_t i = 0; i < n; ++i)
        if (std::fabs(samples[i * stride]) >= peak * 0.5f) return i;
    return -1;
}

TakeAligner::Step TakeAligner::next(int64_t sample, int frames) {
    Step st;
    if (!started_) {
        if (clocked_ && sample < 0) {
            st.skip = frames;  // (the playback has not started: nothing of the take yet)
            return st;
        }
        started_ = true;
        if (!clocked_) skip_ = std::max<int64_t>(0, fallback_);
        else if (sample > start_) st.silence = std::min<int64_t>(sample - start_, int64_t(48000) * 60);
        else skip_ = start_ - sample;
    } else if (clocked_ && placed_ && skip_ == 0 && sample >= 0) {
        // A block that is not where the one before ended (frames lost to an input overflow, or come twice): the rest of
        // the take placed by its own sample again, not left early or late. (Within a block of it, kept as it comes.)
        const int64_t off = sample - next_, slack = std::max<int64_t>(frames, 256);
        if (off > slack) st.silence = std::min<int64_t>(off, int64_t(48000) * 60);
        else if (off < -slack) skip_ = -off;
    }
    if (clocked_ && sample >= 0) next_ = sample + frames, placed_ = true;
    st.skip = int(std::min<int64_t>(skip_, frames));
    skip_ -= st.skip;
    return st;
}

// ---------------------------------------------------------------------------
// The loopback

namespace {

std::mutex gLoopM;
LoopbackOptions gLoopOptions;

class LoopbackStream;
LoopbackStream* gLoop = nullptr;  // (the one open, under gLoopM)

class LoopbackStream : public Stream {
public:
    LoopbackStream(const DeviceSettings& s, int rate, int inputs, int outputs, StreamCallbacks cb)
        : rate_(rate > 0 ? rate : 48000), buffer_(clampBuffer(s.bufferFrames)), inputs_(inputs), outputs_(outputs),
          clock_(s.clock.empty() ? "Internal" : s.clock), cb_(std::move(cb)) {
        {
            std::lock_guard<std::mutex> lock(gLoopM);
            opt_ = gLoopOptions;
            gLoop = this;
        }
        // (never less than a buffer: what a callback hears was played before it, in a block before)
        delay_ = std::max(buffer_, std::max(0, opt_.reportedInput) + std::max(0, opt_.reportedOutput) + std::max(0, opt_.unreported));
        line_.assign(size_t(delay_) * size_t(std::max(1, inputs_)), 0.0f);
        thread_ = std::thread([this] { run(); });
    }
    ~LoopbackStream() override {
        {
            std::lock_guard<std::mutex> lock(gLoopM);
            if (gLoop == this) gLoop = nullptr;
        }
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    int sampleRate() const override { return rate_.load(); }
    int bufferFrames() const override { return buffer_; }
    int inputs() const override { return inputs_; }
    int outputs() const override { return outputs_; }
    Latency latency() const override { return {opt_.reportedInput, opt_.reportedOutput}; }
    int64_t xruns() const override { return xruns_.load(); }
    std::string device() const override { return "Loopback"; }
    std::vector<std::string> clockSources() const override { return {"Internal", "Word clock"}; }
    std::string clock() const override { return clock_; }

    void changeRate(int rate) { pendingRate_ = rate; }
    void stall(int ms) { stallMs_ = ms; }

private:
    void run() {
        std::vector<float> in(size_t(buffer_) * size_t(std::max(1, inputs_))), out(size_t(buffer_) * size_t(std::max(1, outputs_)));
        auto next = std::chrono::steady_clock::now();
        bool wasLate = false;
        while (running_) {
            if (const int r = pendingRate_.exchange(0); r > 0 && r != rate_) {
                rate_ = r;
                if (cb_.rateChanged) cb_.rateChanged(r);
            }
            const auto period = std::chrono::nanoseconds(int64_t(1e9 * buffer_ / rate_.load()));
            pull(in.data());
            std::fill(out.begin(), out.end(), 0.0f);
            if (const int ms = stallMs_.exchange(0); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (cb_.process) cb_.process(in.data(), inputs_, out.data(), outputs_, buffer_);
            push(out.data());
            next += period;
            // An interface's clock runs on whatever its callbacks do: blocks late here (a coarse system timer, a busy
            // computer) are made up at once, back to back, so its sample count keeps to the time. A callback late by
            // more than a block and a timer tick is an xrun (an interface would have run out), counted once while late;
            // a thread held up for seconds (a debugger) starts the time afresh.
            const auto now = std::chrono::steady_clock::now();
            const bool late = now > next + std::max<std::chrono::nanoseconds>(2 * period, std::chrono::milliseconds(25));
            if (late && !wasLate) ++xruns_;
            wasLate = late;
            if (now > next + std::chrono::seconds(2)) next = now;
            if (now < next) std::this_thread::sleep_until(next);
        }
    }
    // The delay line: a ring of delay_ frames (inputs_ channels), the oldest at ring_: read a block from there, and the
    // block just sent written where it was read, so it comes back delay_ frames on.
    void pull(float* in) {
        for (int i = 0; i < buffer_; ++i) {
            for (int c = 0; c < inputs_; ++c) in[size_t(i) * size_t(inputs_) + size_t(c)] = line_[size_t(ring_) * size_t(inputs_) + size_t(c)];
            ring_ = (ring_ + 1) % delay_;
        }
    }
    void push(const float* out) {
        int w = (ring_ + delay_ - buffer_) % delay_;
        for (int i = 0; i < buffer_; ++i) {
            for (int c = 0; c < inputs_; ++c) line_[size_t(w) * size_t(inputs_) + size_t(c)] = c < outputs_ ? out[size_t(i) * size_t(outputs_) + size_t(c)] : 0.0f;
            w = (w + 1) % delay_;
        }
    }

    std::atomic<int> rate_;
    const int buffer_, inputs_, outputs_;
    std::string clock_;
    StreamCallbacks cb_;
    LoopbackOptions opt_;
    int delay_ = 0, ring_ = 0;
    std::vector<float> line_;
    std::atomic<bool> running_{true};
    std::atomic<int> pendingRate_{0}, stallMs_{0};
    std::atomic<int64_t> xruns_{0};
    std::thread thread_;
};

}  // namespace

void setLoopbackOptions(const LoopbackOptions& o) {
    std::lock_guard<std::mutex> lock(gLoopM);
    gLoopOptions = o;
}

LoopbackOptions loopbackOptions() {
    std::lock_guard<std::mutex> lock(gLoopM);
    return gLoopOptions;
}

void loopbackChangeRate(int rate) {
    std::lock_guard<std::mutex> lock(gLoopM);
    if (gLoop) gLoop->changeRate(rate);
}

void loopbackStall(int ms) {
    std::lock_guard<std::mutex> lock(gLoopM);
    if (gLoop) gLoop->stall(ms);
}

std::unique_ptr<Stream> openStream(const DeviceSettings& s, int rate, int inputs, int outputs, const std::vector<std::string>& outputNames,
                                   StreamCallbacks callbacks, std::string* error) {
    inputs = std::clamp(inputs, 0, 64);
    outputs = std::clamp(outputs, 0, 64);
    switch (s.host) {
        case Host::Qt:
            if (error) *error = "Qt Multimedia's devices are opened by Qt";
            return nullptr;
        case Host::Loopback: return std::make_unique<LoopbackStream>(s, rate, inputs, outputs, std::move(callbacks));
        case Host::Jack: return detail::openJack(s, inputs, outputs, outputNames, std::move(callbacks), error);
        default: return detail::openPortAudio(s, rate, inputs, outputs, std::move(callbacks), error);
    }
}

// ---------------------------------------------------------------------------
// Resampling between a client's rate and the device's

namespace {

SwrContext* makeResampler(int channels, int from, int to) {
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, to, &layout, AV_SAMPLE_FMT_FLT, from, 0, nullptr) < 0 || swr_init(swr) < 0) {
        swr_free(&swr);
        swr = nullptr;
    }
    av_channel_layout_uninit(&layout);
    return swr;
}

struct OutputClient {
    int id = 0;
    const void* owner = nullptr;
    int rate = 48000, channels = 2;
    Engine::Render render;
    // (resampling, when the device runs at another rate)
    SwrContext* swr = nullptr;
    int swrFrom = 0, swrTo = 0;
    std::vector<float> scratch, fifo;
    int fifoFrames = 0;
    double head = -1;            // the timeline sample at the fifo's start
    int64_t blockSample = -1;    // this block's first frame on the timeline (client rate)
    std::atomic<int64_t> delivered{0};  // device frames sent
    std::atomic<int64_t> lastNs{0};
    std::atomic<int> lastBlock{0};
    ~OutputClient() { swr_free(&swr); }
};

struct InputClient {
    int id = 0;
    const void* owner = nullptr;
    int rate = 48000;
    Engine::Tap tap;
    SwrContext* swr = nullptr;
    int swrFrom = 0, swrTo = 0, channels = 0;
    std::vector<float> conv;
    ~InputClient() { swr_free(&swr); }
};

}  // namespace

struct Engine::Impl {
    // The stream and its settings: under openM. The clients: under m (which the audio thread takes too; the stream is
    // never closed with m held, as closing waits for the callback).
    mutable std::recursive_mutex openM;
    std::unique_ptr<Stream> stream;
    DeviceSettings settings;
    std::string error;
    int preferredRate = 48000, wantOutputs = 2;
    std::vector<std::string> outputNames;
    int rateChanges = 0;
    int64_t xrunBase = 0;
    int askedOutputs = 0;  // the outputs asked for when the stream was opened (a device may have fewer)

    mutable std::mutex m;
    std::vector<std::shared_ptr<OutputClient>> outputs;
    std::vector<std::shared_ptr<InputClient>> inputs;
    int nextId = 1;
    std::vector<float> mixBuf;

    std::atomic<int> devRate{0}, devBuffer{0}, roundTripFrames{0};
    std::atomic<int64_t> deviceFrames{0};
    std::atomic<int> outLatency{0};
    std::atomic<int> rateEvents{0};
    // The device or server went away (on its own thread, which must not wait for openM: the stream is closed by the
    // next call that holds it).
    std::atomic<bool> lost{false};
    std::mutex lostM;
    std::string lostWhy;
    void reapLocked() {
        if (!lost.exchange(false)) return;
        closeLocked();
        std::lock_guard<std::mutex> l(lostM);
        error = lostWhy;
    }

    // A loopback measurement in progress.
    std::atomic<int> measureState{0};  // 0 none, 1 armed, 2 listening, 3 done
    int measureOut = 0, measureIn = 0;
    int64_t emitAt = 0;
    std::vector<float> measured;
    int64_t measuredCount = 0;

    void process(const float* in, int inCh, float* out, int outCh, int frames);
    bool openLocked(std::string* why);
    void closeLocked();
    void refreshRoundTrip();
    int outputChannelsWanted() const;
};

namespace {

// A client's sound for `frames` device frames (its channels), converted to the device's rate where they differ.
void produce(OutputClient& c, int devRate, int frames, float* dst) {
    const int ch = c.channels;
    if (c.rate == devRate || devRate <= 0) {
        if (c.swr) swr_free(&c.swr);
        c.blockSample = c.render(dst, ch, frames);
        return;
    }
    if (!c.swr || c.swrFrom != c.rate || c.swrTo != devRate) {
        swr_free(&c.swr);
        c.swr = makeResampler(ch, c.rate, devRate);
        c.swrFrom = c.rate, c.swrTo = devRate;
        c.fifoFrames = 0;
        c.head = -1;
    }
    if (!c.swr) {
        std::fill(dst, dst + size_t(frames) * size_t(ch), 0.0f);
        c.blockSample = -1;
        return;
    }
    for (int guard = 0; c.fifoFrames < frames && guard < 8; ++guard) {
        const int need = frames - c.fifoFrames;
        const int m = int(std::ceil(double(need) * c.rate / devRate)) + 4;
        if (c.scratch.size() < size_t(m) * size_t(ch)) c.scratch.resize(size_t(m) * size_t(ch));
        const int64_t s = c.render(c.scratch.data(), ch, m);
        if (c.fifoFrames == 0) {
            // (the resampler holds some of what it was given back: its first output is that much earlier)
            const double held = double(swr_get_delay(c.swr, c.rate));
            c.head = s < 0 ? -1 : double(s) - held;
        }
        const int cap = swr_get_out_samples(c.swr, m);
        if (cap <= 0) break;
        if (c.fifo.size() < size_t(c.fifoFrames + cap) * size_t(ch)) c.fifo.resize(size_t(c.fifoFrames + cap) * size_t(ch));
        uint8_t* o = reinterpret_cast<uint8_t*>(c.fifo.data() + size_t(c.fifoFrames) * size_t(ch));
        const uint8_t* i = reinterpret_cast<const uint8_t*>(c.scratch.data());
        const int got = swr_convert(c.swr, &o, cap, &i, m);
        if (got < 0) break;
        c.fifoFrames += got;
    }
    const int n = std::min(frames, c.fifoFrames);
    std::copy(c.fifo.begin(), c.fifo.begin() + long(n) * ch, dst);
    std::fill(dst + size_t(n) * size_t(ch), dst + size_t(frames) * size_t(ch), 0.0f);
    std::copy(c.fifo.begin() + long(n) * ch, c.fifo.begin() + long(c.fifoFrames) * ch, c.fifo.begin());
    c.fifoFrames -= n;
    c.blockSample = c.head < 0 ? -1 : int64_t(std::llround(c.head));
    if (c.head >= 0) c.head += double(n) * c.rate / devRate;
}

}  // namespace

void Engine::Impl::process(const float* in, int inCh, float* out, int outCh, int frames) {
    std::fill(out, out + size_t(frames) * size_t(outCh), 0.0f);
    const int rate = devRate.load();
    const int64_t base = deviceFrames.load();
    {
        std::lock_guard<std::mutex> lock(m);
        const int64_t now = nowNs();
        for (const auto& c : outputs) {
            if (mixBuf.size() < size_t(frames) * size_t(c->channels)) mixBuf.resize(size_t(frames) * size_t(c->channels));
            produce(*c, rate, frames, mixBuf.data());
            const int n = std::min(c->channels, outCh);
            for (int i = 0; i < frames; ++i)
                for (int k = 0; k < n; ++k) out[size_t(i) * size_t(outCh) + size_t(k)] += mixBuf[size_t(i) * size_t(c->channels) + size_t(k)];
            c->lastBlock = frames;
            c->lastNs = now;
            c->delivered += frames;
        }
        // A loopback measurement: a click out, then what comes back.
        int state = measureState.load();
        if (state == 1) {
            emitAt = base + 2 * int64_t(frames);  // (from the block after next: the stream is settled by then)
            measuredCount = 0;
            measureState = state = 2;
        }
        if (state == 2) {
            if (emitAt >= base && emitAt < base + frames && measureOut < outCh) out[size_t(emitAt - base) * size_t(outCh) + size_t(measureOut)] = 0.9f;
            for (int i = 0; i < frames && measuredCount < int64_t(measured.size()); ++i)
                if (base + i >= emitAt) measured[size_t(measuredCount++)] = measureIn < inCh ? in[size_t(i) * size_t(inCh) + size_t(measureIn)] : 0.0f;
            if (measuredCount >= int64_t(measured.size())) measureState = 3;
        }
        const int rt = roundTripFrames.load();
        for (const auto& t : inputs) {
            // Where this block belongs: its clock's newest output less the round trip (both at the input's rate).
            int64_t sample = -1;
            if (t->owner)
                for (const auto& c : outputs)
                    if (c->owner == t->owner && c->blockSample >= 0) {
                        const double at = double(c->blockSample) * t->rate / c->rate;
                        sample = int64_t(std::llround(at - double(rt) * t->rate / std::max(1, rate)));
                    }
            if (t->rate == rate || rate <= 0 || inCh <= 0) {
                if (t->swr) swr_free(&t->swr);
                if (t->tap) t->tap(in, inCh, frames, sample);
                continue;
            }
            if (!t->swr || t->swrFrom != rate || t->swrTo != t->rate || t->channels != inCh) {
                swr_free(&t->swr);
                t->swr = makeResampler(inCh, rate, t->rate);
                t->swrFrom = rate, t->swrTo = t->rate, t->channels = inCh;
            }
            if (!t->swr) continue;
            const int cap = swr_get_out_samples(t->swr, frames);
            if (cap <= 0) continue;
            if (t->conv.size() < size_t(cap) * size_t(inCh)) t->conv.resize(size_t(cap) * size_t(inCh));
            uint8_t* o = reinterpret_cast<uint8_t*>(t->conv.data());
            const uint8_t* i = reinterpret_cast<const uint8_t*>(in);
            const int got = swr_convert(t->swr, &o, cap, &i, frames);
            if (got <= 0) continue;
            if (sample >= 0) sample -= int64_t(std::llround(double(swr_get_delay(t->swr, t->rate))));
            if (t->tap) t->tap(t->conv.data(), inCh, got, sample);
        }
    }
    deviceFrames += frames;
}

int Engine::Impl::outputChannelsWanted() const {
    int n = std::max(2, wantOutputs);
    std::lock_guard<std::mutex> lock(m);
    for (const auto& c : outputs) n = std::max(n, c->channels);
    return std::min(n, 64);
}

void Engine::Impl::refreshRoundTrip() {
    if (!stream) {
        roundTripFrames = 0;
        return;
    }
    roundTripFrames = roundTrip(settings, stream->sampleRate(), stream->bufferFrames(), stream->latency());
    outLatency = std::max(0, stream->latency().output);
}

bool Engine::Impl::openLocked(std::string* why) {
    closeLocked();
    lost = false;
    if (settings.host == Host::Qt) return false;
    const int rate = settings.sampleRate > 0 ? settings.sampleRate : preferredRate;
    StreamCallbacks cb;
    cb.process = [this](const float* in, int inCh, float* out, int outCh, int frames) { process(in, inCh, out, outCh, frames); };
    cb.rateChanged = [this](int r) {
        devRate = r;
        ++rateEvents;
    };
    cb.lost = [this](const std::string& w) {
        {
            std::lock_guard<std::mutex> l(lostM);
            lostWhy = w.empty() ? "The audio device went away" : w;
        }
        lost = true;
    };
    std::string err;
    deviceFrames = 0;
    askedOutputs = outputChannelsWanted();
    stream = openStream(settings, rate, settings.inputs, askedOutputs, outputNames, std::move(cb), &err);
    if (!stream) {
        error = err.empty() ? "The audio device could not be opened" : err;
        if (why) *why = error;
        return false;
    }
    error.clear();
    devRate = stream->sampleRate();
    devBuffer = stream->bufferFrames();
    rateEvents = 0;
    refreshRoundTrip();
    return true;
}

void Engine::Impl::closeLocked() {
    // (outside m: closing waits for a callback that may be waiting for m)
    std::unique_ptr<Stream> s = std::move(stream);
    s.reset();
    devRate = 0;
    devBuffer = 0;
    roundTripFrames = 0;
    outLatency = 0;
    xrunBase = 0;
}

Engine::Engine() : d_(std::make_unique<Impl>()) {}

Engine::~Engine() {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->closeLocked();
}

Engine& Engine::shared() {
    static Engine e;
    return e;
}

void Engine::setSettings(const DeviceSettings& s) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    const DeviceSettings old = d_->settings;
    d_->settings = s;
    d_->settings.bufferFrames = clampBuffer(s.bufferFrames);
    // Only the measured loopback changed: nothing to open again.
    DeviceSettings a = old, b = d_->settings;
    a.loopbackFrames = b.loopbackFrames, a.loopbackRate = b.loopbackRate, a.loopbackBuffer = b.loopbackBuffer;
    if (a == b && (d_->stream || s.host == Host::Qt)) {
        d_->refreshRoundTrip();
        return;
    }
    if (s.host == Host::Qt) {
        d_->closeLocked();
        d_->error.clear();
        return;
    }
    d_->openLocked(nullptr);
}

DeviceSettings Engine::settings() const {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    return d_->settings;
}

bool Engine::active() const {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    return d_->settings.host != Host::Qt;
}

bool Engine::isOpen() const {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    return d_->stream != nullptr;
}

void Engine::setPreferredRate(int rate) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (rate <= 0 || rate == d_->preferredRate) return;
    d_->preferredRate = rate;
    // (JACK runs at its server's rate whatever is asked; others open again at the sequence's)
    if (d_->stream && d_->settings.sampleRate == 0 && d_->settings.host != Host::Jack && d_->stream->sampleRate() != rate) d_->openLocked(nullptr);
}

void Engine::setOutputChannels(int channels) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    channels = std::clamp(channels, 2, 64);
    if (channels == d_->wantOutputs) return;
    d_->wantOutputs = channels;
    if (d_->stream && d_->askedOutputs < channels) d_->openLocked(nullptr);
}

void Engine::setOutputNames(const std::vector<std::string>& names) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (names == d_->outputNames) return;
    d_->outputNames = names;
    if (d_->stream) d_->stream->renameOutputs(names);
}

int Engine::addOutput(const void* owner, int rate, int channels, Render render, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (d_->settings.host == Host::Qt) {
        if (error) *error = "No audio device is chosen";
        return -1;
    }
    auto c = std::make_shared<OutputClient>();
    c->owner = owner;
    c->rate = rate > 0 ? rate : 48000;
    c->channels = std::clamp(channels, 1, 64);
    c->render = std::move(render);
    {
        std::lock_guard<std::mutex> l(d_->m);
        c->id = d_->nextId++;
        d_->outputs.push_back(c);
    }
    // (open, or open again with more channels)
    if (!d_->stream || d_->askedOutputs < c->channels) d_->openLocked(nullptr);
    if (!d_->stream) {
        if (error) *error = d_->error;
        std::lock_guard<std::mutex> l(d_->m);
        d_->outputs.erase(std::remove(d_->outputs.begin(), d_->outputs.end(), c), d_->outputs.end());
        return -1;
    }
    return c->id;
}

void Engine::removeOutput(int id) {
    std::shared_ptr<OutputClient> gone;  // (freed once the audio thread lets go)
    std::lock_guard<std::mutex> l(d_->m);
    for (auto it = d_->outputs.begin(); it != d_->outputs.end(); ++it)
        if ((*it)->id == id) {
            gone = *it;
            d_->outputs.erase(it);
            break;
        }
}

double Engine::heardSeconds(int id) const {
    std::shared_ptr<OutputClient> c;
    {
        std::lock_guard<std::mutex> l(d_->m);
        for (const auto& o : d_->outputs)
            if (o->id == id) c = o;
    }
    const int rate = d_->devRate.load();
    if (!c || rate <= 0) return -1;
    const int64_t delivered = c->delivered.load();
    if (delivered <= 0) return -1;
    const int block = c->lastBlock.load();
    const double since = double(nowNs() - c->lastNs.load()) / 1e9 * rate;
    const double heard = double(delivered - block) + std::clamp(since, 0.0, double(block)) - d_->outLatency.load();
    return heard / rate;
}

int Engine::addInput(const void* clockOwner, int rate, Tap tap, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (d_->settings.host == Host::Qt) {
        if (error) *error = "No audio device is chosen";
        return -1;
    }
    if (!d_->stream && !d_->openLocked(error)) return -1;
    if (d_->stream->inputs() <= 0) {
        if (error) *error = "The audio device was opened with no inputs";
        return -1;
    }
    auto t = std::make_shared<InputClient>();
    t->owner = clockOwner;
    t->rate = rate > 0 ? rate : d_->stream->sampleRate();
    t->tap = std::move(tap);
    std::lock_guard<std::mutex> l(d_->m);
    t->id = d_->nextId++;
    d_->inputs.push_back(t);
    return t->id;
}

void Engine::removeInput(int id) {
    std::shared_ptr<InputClient> gone;
    std::lock_guard<std::mutex> l(d_->m);
    for (auto it = d_->inputs.begin(); it != d_->inputs.end(); ++it)
        if ((*it)->id == id) {
            gone = *it;
            d_->inputs.erase(it);
            break;
        }
}

int Engine::roundTripAt(int rate) const {
    const int dev = d_->devRate.load();
    if (dev <= 0) return 0;
    return int(std::llround(double(d_->roundTripFrames.load()) * rate / dev));
}

EngineStatus Engine::status() const {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    EngineStatus s;
    s.host = d_->settings.host;
    s.error = d_->error;
    if (!d_->stream) return s;
    Stream& st = *d_->stream;
    s.open = true;
    s.device = st.device();
    s.rate = st.sampleRate();
    s.buffer = st.bufferFrames();
    s.inputs = st.inputs();
    s.outputs = st.outputs();
    s.latency = st.latency();
    s.roundTrip = roundTrip(d_->settings, s.rate, s.buffer, s.latency, &s.measured);
    s.xruns = std::max<int64_t>(0, st.xruns() - d_->xrunBase);
    s.rateChanges = d_->rateEvents.load();
    s.ports = st.ports();
    s.clocks = st.clockSources();
    s.clock = st.clock();
    s.load = st.load();
    // (the latencies can change while it runs: a JACK graph joined anew, a rate changed)
    const_cast<Impl*>(d_.get())->refreshRoundTrip();
    return s;
}

void Engine::resetXruns() {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (d_->stream) d_->xrunBase = d_->stream->xruns();
}

bool Engine::connect(const std::string& from, const std::string& to) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    return d_->stream && d_->stream->connect(from, to);
}

int Engine::measureLoopback(int output, int input, double seconds, std::string* error) {
    int rate = 0, buffer = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(d_->openM);
        d_->reapLocked();
        if (d_->settings.host == Host::Qt) {
            if (error) *error = "Choose an audio device first";
            return -1;
        }
        if (!d_->stream && !d_->openLocked(error)) return -1;
        if (output < 0 || output >= d_->stream->outputs() || input < 0 || input >= d_->stream->inputs()) {
            if (error) *error = "No such output or input";
            return -1;
        }
        rate = d_->stream->sampleRate();
        buffer = d_->stream->bufferFrames();
        std::lock_guard<std::mutex> l(d_->m);
        d_->measureOut = output;
        d_->measureIn = input;
        d_->measured.assign(size_t(std::max(1024, int(rate * std::clamp(seconds, 0.05, 2.0)))), 0.0f);
        d_->measureState = 1;
    }
    // (the device's own thread plays and listens: here only waited for, a little longer than it takes)
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(1000 * std::clamp(seconds, 0.05, 2.0)) + 2000);
    while (d_->measureState.load() != 3 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<float> got;
    {
        std::lock_guard<std::mutex> l(d_->m);
        const bool done = d_->measureState.load() == 3;
        d_->measureState = 0;
        if (done) got.swap(d_->measured);
    }
    if (got.empty()) {
        if (error) *error = "The device did not run (no measurement)";
        return -1;
    }
    const int64_t at = findClick(got.data(), int64_t(got.size()));
    if (at < 0) {
        if (error) *error = "The click did not come back: join the output to the input and try again";
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    d_->settings.loopbackFrames = int(at);
    d_->settings.loopbackRate = rate;
    d_->settings.loopbackBuffer = buffer;
    d_->refreshRoundTrip();
    return int(at);
}

}  // namespace montage::audiodev
