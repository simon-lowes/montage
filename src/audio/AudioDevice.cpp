#include "AudioDevice.h"

#include <QJsonArray>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

#include <QtGlobal>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "AudioDeviceInternal.h"
#include "RtGuard.h"

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
        next_ = start_;
        if (!clocked_) skip_ = std::max<int64_t>(0, fallback_);
        else if (sample > start_) st.silence = std::min<int64_t>(sample - start_, rate_ * 60);
        else skip_ = start_ - sample;
    } else if (clocked_ && placed_ && skip_ == 0 && sample >= 0) {
        // A block that is not where the take has reached: frames lost to an input overflow (or a few lost each time,
        // which add up) are made up with silence so the rest lands by its own sample, not early. Within a block of it
        // a block is kept as it comes. A jump back (the playback looped or was moved back, or frames came twice) or
        // forward by more than two seconds (moved on) is not the device's: the take goes on from there, nothing of it
        // passed over and no silence put in.
        const int64_t off = sample - next_, slack = std::max<int64_t>(frames, 256), far = 2 * rate_;
        if (off > slack && off <= far) st.silence = off;
        else if (off < -slack || off > far) next_ = sample;
    }
    st.skip = int(std::min<int64_t>(skip_, frames));
    skip_ -= st.skip;
    if (clocked_ && sample >= 0) placed_ = true;
    next_ += st.silence + (frames - st.skip);
    return st;
}

// ---------------------------------------------------------------------------
// The loopback

namespace {

std::mutex gLoopM;
LoopbackOptions gLoopOptions;

class LoopbackStream;
LoopbackStream* gLoop = nullptr;  // (the one open, under gLoopM)

// The calling thread put ahead of the programs it shares the computer with, where the system allows it (a busy computer
// otherwise keeps it waiting past the cushion of sound made ahead); never real-time, which a plug-in that ran away would
// keep the computer for. On macOS: the class of work the user is waiting on, run first and woken from its timed waits
// with the least slack.
void aheadOfOthers() {
#ifdef __linux__
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), -10);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// The test device's own thread, as a driver's is. On macOS that is Core Audio's: woken on time for each block of
// `periodNs`, with up to half of it to run (it only copies: the mixing and the plug-ins are on the feed thread); where
// the system refuses, and elsewhere, as aheadOfOthers.
void asADriversThread(int64_t periodNs) {
#ifdef __APPLE__
    mach_timebase_info_data_t base{};
    if (mach_timebase_info(&base) == KERN_SUCCESS && base.numer > 0) {
        const auto ticks = [&](int64_t ns) { return uint32_t(double(ns) * base.denom / base.numer); };
        thread_time_constraint_policy_data_t policy{};
        policy.period = ticks(periodNs);
        policy.computation = ticks(std::max<int64_t>(periodNs / 2, 50000));
        policy.constraint = ticks(periodNs);
        policy.preemptible = 1;
        if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS)
            return;
    }
#endif
    aheadOfOthers();
}

class LoopbackStream : public Stream {
public:
    LoopbackStream(const DeviceSettings& s, int rate, int inputs, int outputs, StreamCallbacks cb)
        : rate_(rate > 0 ? rate : 48000), buffer_(clampBuffer(s.bufferFrames)), inputs_(inputs), outputs_(outputs),
          clock_(s.clock.empty() ? "Internal" : s.clock), named_(!s.output.empty() || !s.input.empty()), cb_(std::move(cb)) {
        {
            std::lock_guard<std::mutex> lock(gLoopM);
            opt_ = gLoopOptions;
            gLoop = this;
        }
        reportedOut_ = opt_.reportedOutput;
        // (never less than the most a callback takes: what a callback hears was played before it, in a block before)
        const int most = opt_.seed && opt_.doubleEvery > 0 ? 2 * buffer_ : buffer_;
        delay_ = std::max(most, std::max(0, opt_.reportedInput) + std::max(0, opt_.reportedOutput) + std::max(0, opt_.unreported));
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
    Latency latency() const override { return {opt_.reportedInput, reportedOut_.load()}; }
    int64_t xruns() const override { return xruns_.load(); }
    std::string device() const override { return "Loopback"; }
    std::vector<std::string> clockSources() const override { return {"Internal", "Word clock"}; }
    std::string clock() const override { return clock_; }

    void unplug() {
        // (as a device pulled out: no more callbacks, and its owner told)
        if (gone_.exchange(true)) return;
        if (cb_.lost) cb_.lost("The loopback was unplugged");
    }
    bool named() const { return named_; }
    LoopbackCounts counts() const { return {taken_.load(), sounding_.load()}; }
    void changeRate(int rate, int reportedOutput) {
        pendingOut_ = reportedOutput;
        pendingRate_ = rate;
    }
    void stall(int ms) { stallMs_ = ms; }

private:
    void run() {
        asADriversThread(int64_t(1e9 * buffer_ / rate_.load()));
        std::vector<float> in(size_t(2 * buffer_) * size_t(std::max(1, inputs_))), out(size_t(2 * buffer_) * size_t(std::max(1, outputs_)));
        auto next = std::chrono::steady_clock::now();
        bool wasLate = false;
        // (a busy computer's callbacks, the same for the same seed)
        uint32_t dice = opt_.seed;
        const auto roll = [&](int every) {
            if (!opt_.seed || every <= 0) return false;
            dice = dice * 1664525u + 1013904223u;
            return (dice >> 8) % uint32_t(every) == 0;
        };
        while (running_) {
            if (const int r = pendingRate_.exchange(0); r > 0 && r != rate_) {
                if (const int o = pendingOut_.exchange(-1); o >= 0) reportedOut_ = o;
                rate_ = r;
                if (cb_.rateChanged) cb_.rateChanged(r);
            }
            const auto period = std::chrono::nanoseconds(int64_t(1e9 * buffer_ / rate_.load()));
            const bool late = roll(opt_.lateEvery), twice = roll(opt_.doubleEvery);
            const int frames = twice ? 2 * buffer_ : buffer_;
            pull(in.data(), frames);
            std::fill(out.begin(), out.end(), 0.0f);
            if (late) std::this_thread::sleep_for(period);  // (then made up at once below: its time has passed)
            if (const int ms = stallMs_.exchange(0); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (cb_.process && !gone_.load()) cb_.process(in.data(), inputs_, out.data(), outputs_, frames);
            push(out.data(), frames);
            next += twice ? 2 * period : period;
            // An interface's clock runs on whatever its callbacks do: blocks late here (a coarse system timer, a busy
            // computer) are made up at once, back to back, so its sample count keeps to the time. A callback late by
            // more than a block and a timer tick is an xrun (an interface would have run out), counted once while late;
            // a thread held up for seconds (a debugger) starts the time afresh.
            const auto now = std::chrono::steady_clock::now();
            const bool behind = now > next + std::max<std::chrono::nanoseconds>(2 * period, std::chrono::milliseconds(25));
            if (behind && !wasLate) ++xruns_;
            wasLate = behind;
            if (now > next + std::chrono::seconds(2)) next = now;
            if (now < next) rtguard::sleepUntil(next);  // (on time: on Windows not to the system timer's next tick)
        }
    }
    // The delay line: a ring of delay_ frames (inputs_ channels), the oldest at ring_: read a block from there, and the
    // block just sent written where it was read, so it comes back delay_ frames on.
    void pull(float* in, int frames) {
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < inputs_; ++c) in[size_t(i) * size_t(inputs_) + size_t(c)] = line_[size_t(ring_) * size_t(inputs_) + size_t(c)];
            ring_ = (ring_ + 1) % delay_;
        }
    }
    void push(const float* out, int frames) {
        int w = (ring_ + delay_ - frames) % delay_;
        int64_t sounding = 0;
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < inputs_; ++c) line_[size_t(w) * size_t(inputs_) + size_t(c)] = c < outputs_ ? out[size_t(i) * size_t(outputs_) + size_t(c)] : 0.0f;
            if (outputs_ > 0 && out[size_t(i) * size_t(outputs_)] != 0.0f) ++sounding;
            w = (w + 1) % delay_;
        }
        if (!gone_.load()) {
            sounding_ += sounding;
            taken_ += frames;
        }
    }

    std::atomic<int> rate_;
    const int buffer_, inputs_, outputs_;
    std::string clock_;
    const bool named_;  // opened by the device's name (not the host's default)
    StreamCallbacks cb_;
    LoopbackOptions opt_;
    int delay_ = 0, ring_ = 0;
    std::vector<float> line_;
    std::atomic<bool> running_{true}, gone_{false};
    std::atomic<int> pendingRate_{0}, stallMs_{0}, pendingOut_{-1}, reportedOut_{0};
    std::atomic<int64_t> xruns_{0}, taken_{0}, sounding_{0};
    std::thread thread_;
};

}  // namespace

void setLoopbackOptions(const LoopbackOptions& o) {
    std::lock_guard<std::mutex> lock(gLoopM);
    gLoopOptions = o;
    if (gLoop && (o.unplugged || (o.away && gLoop->named()))) gLoop->unplug();
}

LoopbackCounts loopbackCounts() {
    std::lock_guard<std::mutex> lock(gLoopM);
    return gLoop ? gLoop->counts() : LoopbackCounts{};
}

LoopbackOptions loopbackOptions() {
    std::lock_guard<std::mutex> lock(gLoopM);
    return gLoopOptions;
}

void loopbackChangeRate(int rate, int reportedOutput) {
    std::lock_guard<std::mutex> lock(gLoopM);
    if (gLoop) gLoop->changeRate(rate, reportedOutput);
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
        case Host::Loopback:
            if (const int ms = loopbackOptions().openMs; ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (const LoopbackOptions o = loopbackOptions(); o.unplugged || (o.away && (!s.output.empty() || !s.input.empty()))) {
                if (error) *error = "The loopback is unplugged";
                return nullptr;
            }
            return std::make_unique<LoopbackStream>(s, rate, inputs, outputs, std::move(callbacks));
        case Host::Jack: return detail::openJack(s, inputs, outputs, outputNames, std::move(callbacks), error);
        default: return detail::openPortAudio(s, rate, inputs, outputs, std::move(callbacks), error);
    }
}

// ---------------------------------------------------------------------------
// The engine. Three kinds of thread meet here, and none waits for another:
//
//  - The device's callback (the driver's thread) copies what is ready out of a ring and what it hears into another,
//    counts, and raises a signal. It takes no lock, allocates nothing and builds nothing (audio/RtGuard.h counts, should
//    that ever stop being so). Everything it touches was made before the stream started.
//  - The feed thread (one for the engine) keeps the output ring a few blocks ahead: it asks each output for its sound,
//    resamples where the device runs at another rate, and sums. It hands the input ring's blocks to the inputs' taps,
//    each with its place on the timeline. Mixing, plug-ins, resamplers being built, files being written: all of it
//    happens here, where a lock or an allocation costs a little of the cushion and never the driver's deadline.
//  - The callers (the window's thread, mostly) add and remove outputs and inputs and ask what has been heard. What they
//    change reaches the feed thread as a list swapped in one step; what they ask is read from counters.

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

int64_t powerOfTwoAtLeast(int64_t n) {
    int64_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// What one open stream's callback works with. Made whole before the stream starts and never resized; the callback
// keeps it alive (its closure holds it), so a feed pass that began before the stream closed still has it.
struct Io {
    // Output: frames of outCh channels at the device's rate, written by the feed thread and read by the callback.
    int outCh = 2;
    int64_t outCap = 0;  // frames, a power of two
    std::vector<float> out;
    std::atomic<int64_t> written{0}, played{0};
    std::atomic<int64_t> flushTo{-1};      // what is queued up to here is let go (the rate changed under it)
    std::atomic<bool> primed{false};       // the ring has been filled since outputs began: running dry now is an underrun
    std::atomic<int64_t> underruns{0};
    std::atomic<int64_t> lastNs{0};        // when the last callback ran
    std::atomic<int> lastFrames{0};        // the ring's frames it played
    std::atomic<int> maxBlock{0};          // the most frames a callback has asked for
    // Input: the device's blocks as they came (inCh channels), each with the output ring's place sent out beside it.
    int inCh = 0;
    int64_t inCap = 0;
    std::vector<float> in;
    std::atomic<int64_t> inWritten{0}, inRead{0};
    struct Block {
        int frames = 0, channels = 0;
        int ready = 0;        // the frames of it the output ring had (fewer than `frames`: it ran dry)
        int64_t outPos = 0;   // the output ring's place sent out in this block
        int64_t device = 0;   // the device's frame count at its start
    };
    std::vector<Block> blocks;
    std::atomic<int64_t> blocksWritten{0}, blocksRead{0};
    // Every callback's place in the output ring, kept whether or not anything listens (so an input added just before
    // a playback starts has the device's own count of what went out before it, not a guess from the time).
    std::vector<Block> history;
    std::atomic<int64_t> historyWritten{0};
    std::atomic<int> listeners{0};  // inputs added: blocks are kept only while something listens
    std::atomic<int64_t> overflows{0};  // input blocks let go because the feed thread was not keeping up
    std::atomic<int64_t> deviceFrames{0};
    // A loopback measurement in progress: armed by the caller with its buffer made, played and heard by the callback.
    std::atomic<int> measureState{0};  // 0 none, 1 armed, 2 listening, 3 done
    int measureOut = 0, measureIn = 0;
    int64_t emitAt = 0;
    std::vector<float> measured;
    int64_t measuredCount = 0;
    std::shared_ptr<rtguard::Wake> wake;
    // (the feed thread's own)
    int feedRate = 0;
    int cushion = 0;
    int64_t seenUnderruns = 0;
    std::deque<Block> sent;  // the callbacks' places in the output ring, as the device took them, oldest first
    int64_t historyRead = 0;
    bool sentFromStart = true;  // `sent` reaches back to the stream's first frame (none were let go unread)

    // The callbacks' places since the last call, onto `sent` (the feed thread's).
    void takeHistory() {
        const int64_t hw = historyWritten.load(std::memory_order_acquire);
        const int64_t size = int64_t(history.size());
        if (hw - historyRead > size) {
            // (the feed thread was held up longer than is kept: what went before is not known)
            historyRead = hw - size;
            sent.clear();
            sentFromStart = false;
        }
        for (; historyRead < hw; ++historyRead) {
            const Block b = history[size_t(historyRead) & size_t(size - 1)];
            // (written over while it was read: the callbacks ran that far ahead; what is left is not known)
            if (historyWritten.load(std::memory_order_acquire) - historyRead > size) {
                sent.clear();
                sentFromStart = false;
                continue;
            }
            sent.push_back(b);
        }
        while (sent.size() > 4096) {
            sent.pop_front();
            sentFromStart = false;
        }
    }

    // What went out at the device's frame `at`: the output ring's place (At); before the stream began (Before: nothing
    // did); or not known, further back than is kept (Unknown). Where the ring had run dry at that frame (silence went
    // out), the next place that did go out and how many device frames later (`back`): the timeline is counted back
    // from there (Before when nothing has gone out since).
    enum class Heard { At, Before, Unknown };
    Heard placeAt(int64_t at, int64_t& pos, int64_t& back) const {
        if (at < 0 && sentFromStart) return Heard::Before;
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (at >= it->device + it->frames) break;  // (later than what has been kept: not yet known)
            if (at < it->device) continue;
            if (at - it->device < it->ready) {
                pos = it->outPos + (at - it->device);
                back = 0;
                return Heard::At;
            }
            for (auto later = it.base(); later != sent.end(); ++later)
                if (later->ready > 0) {
                    pos = later->outPos;
                    back = later->device - at;
                    return Heard::At;
                }
            return Heard::Before;
        }
        return Heard::Unknown;
    }

    // The stretches of `frames` device frames from `from` that heard the output ring's sound, as (offset, length):
    // not those where the ring had run dry and silence went out. Frames from before what is kept count as heard.
    void heardRuns(int64_t from, int frames, std::vector<uint8_t>& dry, std::vector<std::pair<int, int>>& runs) const {
        dry.assign(size_t(std::max(0, frames)), 0);
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (it->device + it->frames <= from) break;
            const int64_t lo = std::max(from, it->device + it->ready), hi = std::min(from + frames, it->device + int64_t(it->frames));
            for (int64_t d = lo; d < hi; ++d) dry[size_t(d - from)] = 1;
        }
        runs.clear();
        for (int i = 0; i < frames;) {
            if (dry[size_t(i)]) {
                ++i;
                continue;
            }
            int j = i;
            while (j < frames && !dry[size_t(j)]) ++j;
            runs.emplace_back(i, j - i);
            i = j;
        }
    }

    Io(int outputs, int inputs, std::shared_ptr<rtguard::Wake> w) : outCh(std::max(1, outputs)), inCh(std::max(0, inputs)), wake(std::move(w)) {
        outCap = 65536;
        out.assign(size_t(outCap) * size_t(outCh), 0.0f);
        inCap = inCh > 0 ? 65536 : 0;
        in.assign(size_t(inCap) * size_t(inCh), 0.0f);
        blocks.assign(4096, Block{});
        history.assign(8192, Block{});
    }

    void process(const float* input, int ic, float* output, int oc, int frames) noexcept;
};

// The device's callback.
void Io::process(const float* input, int ic, float* output, int oc, int frames) noexcept {
    const rtguard::Scope scope;
    std::fill(output, output + size_t(frames) * size_t(oc), 0.0f);
    const int64_t base = deviceFrames.load(std::memory_order_relaxed);
    if (frames > maxBlock.load(std::memory_order_relaxed)) maxBlock.store(frames, std::memory_order_relaxed);
    // What is ready, onto the first outputs; silence where it ran out.
    int64_t r = played.load(std::memory_order_relaxed);
    if (const int64_t f = flushTo.exchange(-1); f > r) r = f;
    const int64_t w = written.load(std::memory_order_acquire);
    const int n = int(std::clamp<int64_t>(w - r, 0, frames));
    const int k = std::min(oc, outCh);
    const int64_t mask = outCap - 1;
    for (int i = 0; i < n; ++i) {
        const float* src = out.data() + size_t((r + i) & mask) * size_t(outCh);
        float* dst = output + size_t(i) * size_t(oc);
        for (int c = 0; c < k; ++c) dst[c] = src[c];
    }
    if (n < frames && primed.load(std::memory_order_relaxed)) underruns.fetch_add(1, std::memory_order_relaxed);
    // Where this callback's frames were in the output ring (before its input block is handed on: the feed thread finds
    // it there by the time it takes the block).
    {
        const int64_t hw = historyWritten.load(std::memory_order_relaxed);
        history[size_t(hw) & (history.size() - 1)] = Block{frames, 0, n, r, base};
        historyWritten.store(hw + 1, std::memory_order_release);
    }
    // A loopback measurement: a click out, then what comes back.
    int state = measureState.load();
    if (state == 1) {
        emitAt = base + 2 * int64_t(frames);  // (from the block after next: the stream is settled by then)
        measuredCount = 0;
        measureState = state = 2;
    }
    if (state == 2) {
        if (emitAt >= base && emitAt < base + frames && measureOut < oc) output[size_t(emitAt - base) * size_t(oc) + size_t(measureOut)] = 0.9f;
        for (int i = 0; i < frames && measuredCount < int64_t(measured.size()); ++i)
            if (base + i >= emitAt) measured[size_t(measuredCount++)] = input && measureIn < ic ? input[size_t(i) * size_t(ic) + size_t(measureIn)] : 0.0f;
        if (measuredCount >= int64_t(measured.size())) measureState = 3;
    }
    // What came in, for the feed thread, with the place in the output ring sent out beside it.
    if (input && ic > 0 && inCh > 0 && listeners.load(std::memory_order_relaxed) > 0) {
        const int64_t bw = blocksWritten.load(std::memory_order_relaxed), br = blocksRead.load(std::memory_order_acquire);
        const int64_t iw = inWritten.load(std::memory_order_relaxed), ir = inRead.load(std::memory_order_acquire);
        if (bw - br < int64_t(blocks.size()) && inCap - (iw - ir) >= frames) {
            const int kc = std::min(ic, inCh);
            const int64_t imask = inCap - 1;
            for (int i = 0; i < frames; ++i) {
                float* dst = in.data() + size_t((iw + i) & imask) * size_t(inCh);
                const float* src = input + size_t(i) * size_t(ic);
                for (int c = 0; c < kc; ++c) dst[c] = src[c];
            }
            blocks[size_t(bw) & (blocks.size() - 1)] = Block{frames, kc, n, r, base};
            inWritten.store(iw + frames, std::memory_order_release);
            blocksWritten.store(bw + 1, std::memory_order_release);
        } else {
            overflows.fetch_add(1, std::memory_order_relaxed);
        }
    }
    lastFrames.store(n, std::memory_order_relaxed);
    lastNs.store(nowNs(), std::memory_order_relaxed);
    played.store(r + n, std::memory_order_release);
    deviceFrames.store(base + frames, std::memory_order_relaxed);
    wake->post();
}

// Where a stretch of the output ring is on an output's timeline.
struct Segment {
    int64_t pos = 0, frames = 0;  // the ring's frames
    double sample = -1;           // the timeline sample (the output's rate) at pos, -1 when it is not the timeline's
    double step = 1;              // samples a ring frame
    double made = 0;              // the output's own frames made before pos (since it was added)
};

struct OutputClient {
    int id = 0;
    const void* owner = nullptr;
    int rate = 48000, channels = 2;
    Engine::Render render;
    // (the feed thread's: resampling, when the device runs at another rate)
    const Io* io = nullptr;
    SwrContext* swr = nullptr;
    int swrFrom = 0, swrTo = 0;
    std::vector<float> block, scratch, fifo;
    int fifoFrames = 0;
    double head = -1;  // the timeline sample at the fifo's start
    std::deque<Segment> segments;
    // The sound mixed into the ring so far, in time: frames at whatever rate the device ran at when they were mixed,
    // so the clock holds when its rate changes.
    std::atomic<int64_t> renderedNs{0};
    int64_t renderedRem = 0;
    // Its own frames mixed into the ring, and of them those the device has taken (as far as the last feed pass saw).
    double made = 0;
    std::atomic<int64_t> taken{0};
    ~OutputClient() { swr_free(&swr); }

    void place(int64_t pos, int frames, double sample, double step) {
        const double before = made;
        made += double(frames) * step;
        if (!segments.empty()) {
            Segment& last = segments.back();
            const bool follows = last.pos + last.frames == pos && last.step == step &&
                                 ((last.sample < 0 && sample < 0) || (last.sample >= 0 && sample >= 0 && std::fabs(last.sample + double(last.frames) * last.step - sample) < 1e-3));
            if (follows) {
                last.frames += frames;
                return;
            }
        }
        segments.push_back({pos, frames, sample, step, before});
        while (segments.size() > 512) segments.pop_front();
    }
    // The device has taken the ring up to `played`: how much of this output's that was.
    void noteTaken(int64_t played) {
        if (segments.empty() || played < segments.front().pos) return;
        for (auto it = segments.rbegin(); it != segments.rend(); ++it)
            if (played >= it->pos) {
                const int64_t into = std::min<int64_t>(played - it->pos, it->frames);
                const int64_t t = int64_t(std::llround(it->made + double(into) * it->step));
                if (t > taken.load(std::memory_order_relaxed)) taken.store(t, std::memory_order_relaxed);
                return;
            }
    }
    // The timeline sample sent out at the ring's `pos` and how many samples a ring frame is; false when nothing of the
    // timeline was.
    bool at(int64_t pos, double& sample, double& step) const {
        for (auto it = segments.rbegin(); it != segments.rend(); ++it)
            if (pos >= it->pos && pos < it->pos + it->frames) {
                if (it->sample < 0) return false;
                sample = double(std::llround(it->sample + double(pos - it->pos) * it->step));
                step = it->step;
                return true;
            }
        return false;
    }
};

struct InputClient {
    int id = 0;
    const void* owner = nullptr;
    int rate = 48000;
    Engine::Tap tap;
    // (the feed thread's)
    SwrContext* swr = nullptr;
    int swrFrom = 0, swrTo = 0, channels = 0;
    std::vector<float> conv;
    ~InputClient() { swr_free(&swr); }
};

struct LostListener {
    int id = 0;
    std::function<void(const std::string&)> told;
};

// The outputs, inputs and listeners as the feed thread sees them: a list made whole by a caller and swapped in.
struct Clients {
    std::vector<std::shared_ptr<OutputClient>> outputs;
    std::vector<std::shared_ptr<InputClient>> inputs;
    std::vector<std::shared_ptr<LostListener>> listeners;
};

// A shared pointer handed between threads: held only for as long as copying or swapping the pointer takes.
template <typename T>
class Handed {
public:
    std::shared_ptr<T> get() const {
        std::lock_guard<std::mutex> lock(m_);
        return p_;
    }
    void set(std::shared_ptr<T> p) {
        std::lock_guard<std::mutex> lock(m_);
        p_.swap(p);
    }

private:
    mutable std::mutex m_;
    std::shared_ptr<T> p_;
};

}  // namespace

struct Engine::Impl {
    // The stream and its settings: under openM. The lists of outputs and inputs: under m. Neither is taken by the
    // device's callback or by the feed thread: a caller may hold them for as long as opening a device takes.
    mutable std::recursive_mutex openM;
    std::unique_ptr<Stream> stream;
    // Whether a stream is open and a device other than Qt Multimedia's chosen, as they were when openM was last let
    // go: what isOpen() and active() say while another thread holds it (opening a device can take a second).
    std::atomic<bool> openNow{false}, activeNow{false};
    DeviceSettings settings;
    DeviceSettings opened;  // what the stream was opened with (the settings, or what stood in for a device that is gone)
    bool standIn = false;
    std::chrono::steady_clock::time_point standInTried;  // when the device chosen was last asked for, with a stand-in open
    std::string error;
    int preferredRate = 48000, wantOutputs = 2;
    std::vector<std::string> outputNames;
    int64_t xrunBase = 0, underrunBase = 0, overflowBase = 0;
    int askedOutputs = 0;  // the outputs asked for when the stream was opened (a device may have fewer)

    mutable std::mutex m;
    std::vector<std::shared_ptr<OutputClient>> outputs;
    std::vector<std::shared_ptr<InputClient>> inputs;
    std::vector<std::shared_ptr<LostListener>> listeners;
    int nextId = 1;
    Handed<const Clients> clients;
    Handed<Io> io, lostIo;

    std::atomic<int> devRate{0}, devBuffer{0}, roundTripFrames{0};
    std::atomic<int> outLatency{0};
    std::atomic<int> rateEvents{0};
    std::atomic<int64_t> resamplers{0};  // resamplers built (by the feed thread) since the engine began
    std::atomic<int64_t> dropouts{-1};
    // The rate changed since the latencies were last asked for (they are asked again by the next call that can hold
    // openM: the device's callback that says so cannot).
    std::atomic<bool> latencyStale{false};
    // The device or server went away (on its own thread, which must not wait for openM: the stream is closed by the
    // next call that holds it).
    std::atomic<bool> lost{false};
    std::atomic<bool> lostPending{false};  // (for the feed thread to tell of)
    std::mutex lostM;
    std::string lostWhy;
    void reapLocked() {
        if (!lost.exchange(false)) return;
        closeLocked();
        std::lock_guard<std::mutex> l(lostM);
        error = lostWhy;
    }

    // The feed thread.
    std::shared_ptr<rtguard::Wake> wake = std::make_shared<rtguard::Wake>();
    std::thread feeder;
    std::thread::id feederId;
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> pass{0};  // odd while a pass is under way
    std::vector<float> mix, heard;
    std::vector<uint8_t> dryFrames;
    std::vector<std::pair<int, int>> heardRuns;

    Impl() {
        clients.set(std::make_shared<const Clients>());
        feeder = std::thread([this] { feed(); });
    }
    ~Impl() {
        quit = true;
        wake->post();
        if (feeder.joinable()) feeder.join();
    }

    void publishLocked() {  // (with m held)
        clients.set(std::make_shared<const Clients>(Clients{outputs, inputs, listeners}));
        if (const auto i = io.get()) i->listeners = int(inputs.size());
        wake->post();
    }
    // Once this returns the feed thread is working from the list as it is now: nothing of an output or input taken
    // off it is called again. (Not waited for on the feed thread itself: a tap that takes itself off.)
    void settle() {
        if (std::this_thread::get_id() == feederId) return;
        const uint64_t at = pass.load();
        if (!(at & 1)) return;  // (between passes: the next reads the list first)
        while (pass.load() == at && !quit.load()) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    void feed();
    void feedOutputs(Io& io, const Clients& c, int rate);
    void feedInputs(Io& io, const Clients& c, int rate);
    void deliver(InputClient& t, const float* data, int ch, int frames, int64_t sample, int rate);
    bool openLocked(std::string* why);
    void retryChosenLocked();
    bool openWith(const DeviceSettings& s, std::string* why);
    void closeLocked();
    void refreshRoundTrip();
    int outputChannelsWanted() const;
};

namespace {

// An output's sound for `frames` device frames (its channels), converted to the device's rate where they differ: the
// timeline sample of the first (-1 none), and how many samples a device frame is.
double produce(OutputClient& c, int devRate, int frames, float* dst, std::atomic<int64_t>& built) {
    const int ch = c.channels;
    if (c.rate == devRate || devRate <= 0) {
        if (c.swr) swr_free(&c.swr);
        return double(c.render(dst, ch, frames));
    }
    if (!c.swr || c.swrFrom != c.rate || c.swrTo != devRate) {
        swr_free(&c.swr);
        c.swr = makeResampler(ch, c.rate, devRate);
        ++built;
        c.swrFrom = c.rate, c.swrTo = devRate;
        c.fifoFrames = 0;
        c.head = -1;
    }
    if (!c.swr) {
        std::fill(dst, dst + size_t(frames) * size_t(ch), 0.0f);
        return -1;
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
    const double first = c.head < 0 ? -1 : double(std::llround(c.head));
    if (c.head >= 0) c.head += double(n) * c.rate / devRate;
    return first;
}

}  // namespace

void Engine::Impl::feed() {
    feederId = std::this_thread::get_id();
    aheadOfOthers();
    std::shared_ptr<Io> held;
    while (!quit.load()) {
        ++pass;
        held = io.get();
        const std::shared_ptr<const Clients> c = clients.get();
        if (held && c) {
            const int rate = devRate.load();
            feedOutputs(*held, *c, rate);
            feedInputs(*held, *c, rate);
        }
        if (lostPending.exchange(false) && c) {
            // A device is gone (its stream may be closed already, by a caller that found out first): what it brought
            // before it went is handed on whole, and then those who record or play through it are told.
            if (const std::shared_ptr<Io> gone = lostIo.get()) {
                feedInputs(*gone, *c, gone->feedRate > 0 ? gone->feedRate : devRate.load());
                lostIo.set(nullptr);
            }
            std::string why;
            {
                std::lock_guard<std::mutex> l(lostM);
                why = lostWhy;
            }
            for (const auto& l : c->listeners)
                if (l->told) l->told(why);
        }
        ++pass;
        wake->wait(held ? 2 : 50);
    }
}

void Engine::Impl::feedOutputs(Io& io, const Clients& c, int rate) {
    if (c.outputs.empty()) {
        io.primed = false;
        return;
    }
    const int block = std::max(32, io.maxBlock.load() > 0 ? io.maxBlock.load() : devBuffer.load());
    // The cushion: two of the device's blocks and no less than 20 ms; twice as much each time the ring runs dry (a
    // busy computer: after one dropout the next is a long way off), up to 100 ms.
    const int floor = std::max(2 * block, rate > 0 ? rate / 50 : 960);
    if (io.cushion < floor) io.cushion = floor;
    if (const int64_t u = io.underruns.load(); u != io.seenUnderruns) {
        io.seenUnderruns = u;
        io.cushion = std::min(io.cushion * 2, std::max(4 * block, rate > 0 ? rate / 10 : 4800));
    }
    io.cushion = int(std::min<int64_t>(io.cushion, io.outCap / 2));
    if (rate != io.feedRate) {
        // The device runs at another rate: what was mixed for the old one is let go, and the outputs' resamplers are
        // built for the new one below, here on the feed thread.
        if (io.feedRate != 0) io.flushTo = io.written.load();
        io.feedRate = rate;
        for (const auto& o : c.outputs) o->segments.clear();
    }
    const int quantum = std::min(block, 512);
    const int64_t mask = io.outCap - 1;
    // What the device has taken of each, from its own count (not what is mixed ahead of it).
    {
        const int64_t played = io.played.load(std::memory_order_acquire);
        for (const auto& o : c.outputs)
            if (o->io == &io) o->noteTaken(played);
    }
    bool wrote = false;
    for (int guard = 0; guard < 4096; ++guard) {
        const int64_t w = io.written.load(std::memory_order_relaxed);
        const int64_t r = std::max(io.played.load(std::memory_order_acquire), io.flushTo.load());
        if (w - r >= io.cushion) break;
        const int n = quantum;
        mix.assign(size_t(n) * size_t(io.outCh), 0.0f);
        for (const auto& o : c.outputs) {
            if (o->io != &io) {
                // (a stream opened anew: nothing of the last one's places holds)
                o->io = &io;
                o->segments.clear();
                o->fifoFrames = 0;
                o->head = -1;
            }
            if (o->block.size() < size_t(n) * size_t(o->channels)) o->block.resize(size_t(n) * size_t(o->channels));
            const double first = produce(*o, rate, n, o->block.data(), resamplers);
            const int k = std::min(o->channels, io.outCh);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < k; ++ch) mix[size_t(i) * size_t(io.outCh) + size_t(ch)] += o->block[size_t(i) * size_t(o->channels) + size_t(ch)];
            o->place(w, n, first, o->rate == rate || rate <= 0 ? 1.0 : double(o->rate) / rate);
        }
        for (int i = 0; i < n; ++i)
            std::copy(mix.begin() + std::ptrdiff_t(size_t(i) * size_t(io.outCh)), mix.begin() + std::ptrdiff_t(size_t(i + 1) * size_t(io.outCh)),
                      io.out.begin() + std::ptrdiff_t(size_t((w + i) & mask) * size_t(io.outCh)));
        io.written.store(w + n, std::memory_order_release);
        // (counted after the ring has it: a reader between the two sees a little less heard, never more)
        if (rate > 0)
            for (const auto& o : c.outputs) {
                const int64_t ns = int64_t(n) * 1000000000 + o->renderedRem;
                o->renderedNs += ns / rate;
                o->renderedRem = ns % rate;
            }
        wrote = true;
    }
    if (wrote && !io.primed.load()) io.primed = true;
}

void Engine::Impl::feedInputs(Io& io, const Clients& c, int rate) {
    const int rt = roundTripFrames.load();
    io.takeHistory();
    for (;;) {
        const int64_t br = io.blocksRead.load(std::memory_order_relaxed);
        if (br >= io.blocksWritten.load(std::memory_order_acquire)) break;
        io.takeHistory();  // (to this block's callback at least)
        const Io::Block b = io.blocks[size_t(br) & (io.blocks.size() - 1)];
        const int64_t ir = io.inRead.load(std::memory_order_relaxed);
        const int ch = b.channels;
        heard.resize(size_t(b.frames) * size_t(std::max(1, ch)));
        for (int i = 0; i < b.frames; ++i) {
            const float* src = io.in.data() + size_t((ir + i) & (io.inCap - 1)) * size_t(io.inCh);
            std::copy(src, src + ch, heard.begin() + std::ptrdiff_t(size_t(i) * size_t(ch)));
        }
        io.inRead.store(ir + b.frames, std::memory_order_release);
        io.blocksRead.store(br + 1, std::memory_order_release);
        // What this block heard went out a round trip of the device's frames before it: its place in the output ring,
        // from the device's own count of what each callback took (kept callback by callback, so a ring that ran dry in
        // between, or a callback that ran late, does not move it).
        const int64_t from = b.device - rt;
        int64_t heardPos = 0, heardBack = 0;
        const Io::Heard placed = io.placeAt(from, heardPos, heardBack);
        // Where the ring ran dry (the feed thread fell behind) the timeline stood still while the device went on: a
        // clocked input takes only the stretches that heard it move, each at its own place, so a take lands where it
        // was played. (One stretch, the whole block, where nothing ran dry or what was sent is not known.)
        io.heardRuns(from, b.frames, dryFrames, heardRuns);
        const bool whole = heardRuns.size() == 1 && heardRuns[0].first == 0 && heardRuns[0].second == b.frames;
        for (const auto& t : c.inputs) {
            const OutputClient* clock = nullptr;
            if (t->owner)
                for (const auto& o : c.outputs)
                    if (o->owner == t->owner) clock = o.get();
            if (!clock || whole) {
                // Where this block belongs: its clock's output at that place (none where what went out then was not
                // its timeline, or went out before the stream began). Only where the device's count does not reach
                // back that far (held up longer than is kept), the output sent out beside the block less the round
                // trip. Both at the input's rate; before the output's first sample it is negative (nothing of the
                // timeline yet).
                // (macOS CI, 23 and 25 Nov: an ADR take one and two device buffers early, never seen on Linux. Before,
                // a block whose heard place was not the timeline's (the program paused on the device a moment before)
                // or was not yet kept took the output sent beside it less the round trip, which is wrong by however long
                // the ring ran dry or the callbacks ran late in between: the likely cause.)
                int64_t sample = -1;
                double at = -1, step = 1;
                if (clock && placed == Io::Heard::At && clock->at(heardPos, at, step))
                    sample = int64_t(std::llround((at - double(heardBack) * step) * t->rate / clock->rate));
                else if (clock && placed == Io::Heard::Unknown && clock->at(b.outPos, at, step))
                    sample = int64_t(std::llround((at - double(rt) * step) * t->rate / clock->rate));
                deliver(*t, heard.data(), ch, b.frames, sample, rate);
                continue;
            }
            for (const auto& [off, len] : heardRuns) {
                int64_t pos = 0, back = 0, sample = -1;
                double at = -1, step = 1;
                if (io.placeAt(from + off, pos, back) == Io::Heard::At && clock->at(pos, at, step))
                    sample = int64_t(std::llround((at - double(back) * step) * t->rate / clock->rate));
                deliver(*t, heard.data() + size_t(off) * size_t(std::max(1, ch)), ch, len, sample, rate);
            }
        }
    }
}

void Engine::Impl::deliver(InputClient& t, const float* data, int ch, int frames, int64_t sample, int rate) {
    // At the device's rate as it is; else through the input's resampler (built here, on the feed thread), its first
    // frame that much earlier.
    if (t.rate == rate || rate <= 0 || ch <= 0) {
        if (t.swr) swr_free(&t.swr);
        if (t.tap) t.tap(data, ch, frames, sample);
        return;
    }
    if (!t.swr || t.swrFrom != rate || t.swrTo != t.rate || t.channels != ch) {
        swr_free(&t.swr);
        t.swr = makeResampler(ch, rate, t.rate);
        ++resamplers;
        t.swrFrom = rate, t.swrTo = t.rate, t.channels = ch;
    }
    if (!t.swr) return;
    const int cap = swr_get_out_samples(t.swr, frames);
    if (cap <= 0) return;
    if (t.conv.size() < size_t(cap) * size_t(ch)) t.conv.resize(size_t(cap) * size_t(ch));
    uint8_t* o = reinterpret_cast<uint8_t*>(t.conv.data());
    const uint8_t* i = reinterpret_cast<const uint8_t*>(data);
    const int got = swr_convert(t.swr, &o, cap, &i, frames);
    if (got <= 0) return;
    if (sample >= 0) sample -= int64_t(std::llround(double(swr_get_delay(t.swr, t.rate))));
    if (t.tap) t.tap(t.conv.data(), ch, got, sample);
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
    roundTripFrames = roundTrip(opened, stream->sampleRate(), stream->bufferFrames(), stream->latency());
    outLatency = std::max(0, stream->latency().output);
}

bool Engine::Impl::openWith(const DeviceSettings& s, std::string* why) {
    const int rate = s.sampleRate > 0 ? s.sampleRate : preferredRate;
    askedOutputs = outputChannelsWanted();
    // The rings, made before the first callback can run; the callback's closure keeps them.
    auto next = std::make_shared<Io>(askedOutputs, std::clamp(s.inputs, 0, 64), wake);
    {
        std::lock_guard<std::mutex> l(m);
        next->listeners = int(inputs.size());
    }
    StreamCallbacks cb;
    cb.process = [next](const float* in, int inCh, float* out, int outCh, int frames) { next->process(in, inCh, out, outCh, frames); };
    cb.rateChanged = [this](int r) {
        devRate = r;
        ++rateEvents;
        latencyStale = true;
        wake->post();
    };
    cb.lost = [this, next](const std::string& w) {
        {
            std::lock_guard<std::mutex> l(lostM);
            lostWhy = w.empty() ? "The audio device went away" : w;
        }
        lostIo.set(next);
        lostPending = true;
        lost = true;
        wake->post();
    };
    std::string err;
    stream = openStream(s, rate, s.inputs, askedOutputs, outputNames, std::move(cb), &err);
    openNow = stream != nullptr;
    if (!stream) {
        if (why) *why = err.empty() ? "The audio device could not be opened" : err;
        return false;
    }
    opened = s;
    devRate = stream->sampleRate();
    devBuffer = stream->bufferFrames();
    rateEvents = 0;
    latencyStale = false;
    refreshRoundTrip();
    io.set(std::move(next));
    wake->post();
    return true;
}

// (nightly 25 Nov: once a stand-in was open nothing asked for the device chosen again, PortAudio lists devices again
// only with no stream open, and the scrub retry only ran with none open: a device plugged back in was not used until
// another was chosen and then it) A stand-in that nothing plays or records through is closed and the device chosen
// asked for again, when a client comes to it, no more often than every few seconds (an open that does not find its
// device has PortAudio list every device again): back, it is used from then on; not, the stand-in opens again.
void Engine::Impl::retryChosenLocked() {
    if (!standIn || !stream || std::chrono::steady_clock::now() - standInTried < std::chrono::seconds(3)) return;
    {
        std::lock_guard<std::mutex> l(m);
        if (!outputs.empty() || !inputs.empty()) return;
    }
    openLocked(nullptr);
}

bool Engine::Impl::openLocked(std::string* why) {
    closeLocked();
    lost = false;
    standIn = false;
    if (settings.host == Host::Qt) return false;
    std::string err;
    if (openWith(settings, &err)) {
        error.clear();
        return true;
    }
    // The device named is not there (unplugged, or its server stopped): the host's own choice stands in for it until
    // it can be opened again, where the host has another (the loopback's default too, so that this is tested).
    if (!settings.output.empty() || !settings.input.empty()) {
        DeviceSettings alt = settings;
        alt.output.clear();
        alt.input.clear();
        std::string altErr;
        if (openWith(alt, &altErr)) {
            standIn = true;
            standInTried = std::chrono::steady_clock::now();
            error = err;
            return true;
        }
    }
    error = err.empty() ? "The audio device could not be opened" : err;
    if (why) *why = error;
    return false;
}

void Engine::Impl::closeLocked() {
    // (the stream first: closing it waits for a callback under way, which waits for nothing)
    std::unique_ptr<Stream> s = std::move(stream);
    openNow = false;
    s.reset();
    if (qEnvironmentVariableIsSet("MONTAGE_AUDIO_GUARD_LOG")) {
        if (const auto i = io.get()) {
            const rtguard::Counts g = rtguard::counts();
            qInfo("Audio device closed: %lld callbacks, in them %lld allocations, %lld locks, %lld waits; %lld underruns, %lld input blocks lost",
                  static_cast<long long>(g.callbacks), static_cast<long long>(g.allocations), static_cast<long long>(g.locks),
                  static_cast<long long>(g.waits), static_cast<long long>(i->underruns.load()), static_cast<long long>(i->overflows.load()));
        }
    }
    io.set(nullptr);
    devRate = 0;
    devBuffer = 0;
    roundTripFrames = 0;
    outLatency = 0;
    xrunBase = underrunBase = overflowBase = 0;
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
    d_->activeNow = s.host != Host::Qt;
    // Only the measured loopback changed: nothing to open again.
    DeviceSettings a = old, b = d_->settings;
    a.loopbackFrames = b.loopbackFrames, a.loopbackRate = b.loopbackRate, a.loopbackBuffer = b.loopbackBuffer;
    // (the same device chosen again while a stand-in plays for it: asked for again, below)
    if (a == b && ((d_->stream && !d_->standIn) || s.host == Host::Qt)) {
        if (d_->stream) d_->opened = d_->settings;
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

// (neither waits for a device being opened or closed on another thread: they say what was so before it)
bool Engine::active() const {
    std::unique_lock<std::recursive_mutex> lock(d_->openM, std::try_to_lock);
    if (!lock.owns_lock()) return d_->activeNow.load();
    d_->reapLocked();
    return d_->settings.host != Host::Qt;
}

bool Engine::isOpen() const {
    std::unique_lock<std::recursive_mutex> lock(d_->openM, std::try_to_lock);
    if (!lock.owns_lock()) return d_->openNow.load();
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
    d_->retryChosenLocked();
    auto c = std::make_shared<OutputClient>();
    c->owner = owner;
    c->rate = rate > 0 ? rate : 48000;
    c->channels = std::clamp(channels, 1, 64);
    c->render = std::move(render);
    {
        std::lock_guard<std::mutex> l(d_->m);
        c->id = d_->nextId++;
        d_->outputs.push_back(c);  // (counted when the stream is opened; the feed thread is given it once it is)
    }
    // (open, or open again with more channels)
    if (!d_->stream || d_->askedOutputs < c->channels) d_->openLocked(nullptr);
    std::lock_guard<std::mutex> l(d_->m);
    if (!d_->stream) {
        if (error) *error = d_->error;
        d_->outputs.erase(std::remove(d_->outputs.begin(), d_->outputs.end(), c), d_->outputs.end());
        return -1;
    }
    d_->publishLocked();
    return c->id;
}

void Engine::removeOutput(int id) {
    std::shared_ptr<OutputClient> gone;  // (freed once the feed thread lets go)
    {
        std::lock_guard<std::mutex> l(d_->m);
        for (auto it = d_->outputs.begin(); it != d_->outputs.end(); ++it)
            if ((*it)->id == id) {
                gone = *it;
                d_->outputs.erase(it);
                break;
            }
        if (gone) d_->publishLocked();
    }
    if (gone) d_->settle();
}

double Engine::heardSeconds(int id) const {
    std::shared_ptr<OutputClient> c;
    {
        std::lock_guard<std::mutex> l(d_->m);
        for (const auto& o : d_->outputs)
            if (o->id == id) c = o;
    }
    // (a device's latencies can change with its rate, a JACK graph's do: asked again before the output latency is
    // taken off, rather than when the device panel next asks; never waited for, as this is the picture's clock)
    if (d_->latencyStale.load() && d_->openM.try_lock()) {
        d_->latencyStale = false;
        d_->refreshRoundTrip();
        d_->openM.unlock();
    }
    const int rate = d_->devRate.load();
    const std::shared_ptr<Io> io = d_->io.get();
    if (!c || !io || rate <= 0) return -1;
    // What has been mixed for it, less what the ring still holds: what the device has been sent.
    const int64_t rendered = c->renderedNs.load();
    const int64_t queued = std::max<int64_t>(0, io->written.load() - std::max(io->played.load(), io->flushTo.load()));
    const int64_t delivered = rendered - queued * 1000000000 / rate;
    if (delivered <= 0) return -1;
    // (the last block as far as the time since it was asked for says it has played)
    const int64_t block = int64_t(io->lastFrames.load()) * 1000000000 / rate;
    const int64_t since = nowNs() - io->lastNs.load();
    return double(delivered - block + std::clamp<int64_t>(since, 0, block)) / 1e9 - double(d_->outLatency.load()) / rate;
}

int64_t Engine::takenFrames(int id) const {
    std::lock_guard<std::mutex> l(d_->m);
    for (const auto& o : d_->outputs)
        if (o->id == id) return o->taken.load(std::memory_order_relaxed);
    return -1;
}

int Engine::addInput(const void* clockOwner, int rate, Tap tap, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (d_->settings.host == Host::Qt) {
        if (error) *error = "No audio device is chosen";
        return -1;
    }
    d_->retryChosenLocked();
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
    d_->publishLocked();
    return t->id;
}

void Engine::removeInput(int id) {
    std::shared_ptr<InputClient> gone;
    {
        std::lock_guard<std::mutex> l(d_->m);
        for (auto it = d_->inputs.begin(); it != d_->inputs.end(); ++it)
            if ((*it)->id == id) {
                gone = *it;
                d_->inputs.erase(it);
                break;
            }
        if (gone) d_->publishLocked();
    }
    if (gone) d_->settle();
}

int Engine::addLostListener(std::function<void(const std::string&)> told) {
    auto l = std::make_shared<LostListener>();
    l->told = std::move(told);
    std::lock_guard<std::mutex> lock(d_->m);
    l->id = d_->nextId++;
    d_->listeners.push_back(l);
    d_->publishLocked();
    return l->id;
}

void Engine::removeLostListener(int id) {
    bool gone = false;
    {
        std::lock_guard<std::mutex> lock(d_->m);
        for (auto it = d_->listeners.begin(); it != d_->listeners.end(); ++it)
            if ((*it)->id == id) {
                d_->listeners.erase(it);
                gone = true;
                break;
            }
        if (gone) d_->publishLocked();
    }
    if (gone) d_->settle();
}

int Engine::roundTripAt(int rate) const {
    const int dev = d_->devRate.load();
    if (dev <= 0) return 0;
    return int(std::llround(double(d_->roundTripFrames.load()) * rate / dev));
}

bool Engine::statusIfFree(EngineStatus& out) const {
    std::unique_lock<std::recursive_mutex> lock(d_->openM, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    out = status();
    return true;
}

EngineStatus Engine::status() const {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    EngineStatus s;
    s.host = d_->settings.host;
    s.error = d_->error;
    s.resamplers = d_->resamplers.load();
    if (!d_->stream) return s;
    Stream& st = *d_->stream;
    s.open = true;
    s.standIn = d_->standIn;
    s.device = st.device();
    s.rate = st.sampleRate();
    s.buffer = st.bufferFrames();
    s.inputs = st.inputs();
    s.outputs = st.outputs();
    s.latency = st.latency();
    s.roundTrip = roundTrip(d_->opened, s.rate, s.buffer, s.latency, &s.measured);
    s.xruns = std::max<int64_t>(0, st.xruns() - d_->xrunBase);
    if (const auto io = d_->io.get()) {
        s.underruns = std::max<int64_t>(0, io->underruns.load() - d_->underrunBase);
        s.inputsLost = std::max<int64_t>(0, io->overflows.load() - d_->overflowBase);
        s.cushion = io->cushion;
    }
    s.rateChanges = d_->rateEvents.load();
    s.ports = st.ports();
    s.clocks = st.clockSources();
    s.clock = st.clock();
    s.load = st.load();
    // (the latencies can change while it runs: a JACK graph joined anew, a rate changed)
    const_cast<Impl*>(d_.get())->refreshRoundTrip();
    return s;
}

int64_t Engine::dropouts() const {
    // (read beside the meters many times a second: never waited for behind a device being opened)
    if (d_->openM.try_lock()) {
        int64_t n = d_->stream ? 0 : -1;
        if (d_->stream) n += std::max<int64_t>(0, d_->stream->xruns() - d_->xrunBase);
        if (const std::shared_ptr<Io> io = d_->io.get())
            n += std::max<int64_t>(0, io->underruns.load() - d_->underrunBase) + std::max<int64_t>(0, io->overflows.load() - d_->overflowBase);
        d_->dropouts = n;
        d_->openM.unlock();
    }
    return d_->dropouts.load();
}

void Engine::resetXruns() {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    if (d_->stream) d_->xrunBase = d_->stream->xruns();
    if (const auto io = d_->io.get()) {
        d_->underrunBase = io->underruns.load();
        d_->overflowBase = io->overflows.load();
    }
}

bool Engine::connect(const std::string& from, const std::string& to) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    d_->reapLocked();
    return d_->stream && d_->stream->connect(from, to);
}

void Engine::holdStateForTest(int ms) {
    std::lock_guard<std::recursive_mutex> lock(d_->openM);
    std::lock_guard<std::mutex> l(d_->m);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int Engine::measureLoopback(int output, int input, double seconds, std::string* error) {
    int rate = 0, buffer = 0;
    std::shared_ptr<Io> io;
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
        io = d_->io.get();
        if (const int state = io ? io->measureState.load() : 1; state == 1 || state == 2) {
            if (error) *error = "A measurement is already under way";
            return -1;
        }
        // (its buffer made here, before the callback is told of it)
        io->measureOut = output;
        io->measureIn = input;
        io->measured.assign(size_t(std::max(1024, int(rate * std::clamp(seconds, 0.05, 2.0)))), 0.0f);
        io->measureState = 1;
    }
    // (the device's own thread plays and listens: here only waited for, a little longer than it takes)
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(1000 * std::clamp(seconds, 0.05, 2.0)) + 2000);
    while (io->measureState.load() != 3 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<float> got;
    if (io->measureState.load() == 3) {
        got.swap(io->measured);
        io->measureState = 0;
    } else {
        // (given up: the callback is let finish the block it may be in before the buffer is anyone else's)
        io->measureState = 0;
        const int64_t at = io->deviceFrames.load();
        const auto wait = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (io->deviceFrames.load() == at && std::chrono::steady_clock::now() < wait) std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    if (d_->stream) {
        d_->opened.loopbackFrames = int(at);
        d_->opened.loopbackRate = rate;
        d_->opened.loopbackBuffer = buffer;
    }
    d_->refreshRoundTrip();
    return int(at);
}

// ---- Feed.

struct Feed::Ring {
    std::vector<float> buf;
    int64_t capacity = 0;  // frames
    int channels = 2;
    std::atomic<int64_t> written{0}, read{0}, dropTo{-1};

    // What each render made (the feed thread's, a few blocks ahead of the device): from which of its frames, how many
    // were asked for and how many had sound, and the counts before it. Kept until the device has taken them.
    struct Made {
        int64_t from = 0;
        int frames = 0, sound = 0;
        int64_t played = 0, starved = 0;
    };
    mutable std::mutex madeM;
    mutable std::deque<Made> made;
    int64_t asked = 0, mixedSound = 0, mixedShort = 0;  // (the feed thread's)

    // The engine's feed thread: what is queued, and silence after it.
    void render(float* out, int ch, int frames) {
        if (const int64_t d = dropTo.exchange(-1); d >= 0 && d > read.load()) read = d;
        const int64_t r = read.load(), w = written.load(std::memory_order_acquire);
        const int64_t n = std::min<int64_t>(frames, w - r);
        for (int64_t i = 0; i < n; ++i) {
            const float* src = buf.data() + size_t((r + i) % capacity) * size_t(channels);
            float* dst = out + size_t(i) * size_t(ch);
            for (int k = 0; k < ch; ++k) dst[k] = k < channels ? src[k] : 0.0f;
        }
        std::fill(out + size_t(std::max<int64_t>(0, n)) * size_t(ch), out + size_t(frames) * size_t(ch), 0.0f);
        if (n > 0) read.store(r + n, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(madeM);
            made.push_back({asked, frames, int(std::max<int64_t>(0, n)), mixedSound, mixedShort});
            while (made.size() > 16384) made.pop_front();
        }
        asked += frames;
        mixedSound += std::max<int64_t>(0, n);
        // (frames short while it was playing: the writer did not keep up)
        if (n < frames && n > 0) mixedShort += frames - n;
    }
    int64_t readNow() const { return std::max(read.load(std::memory_order_acquire), dropTo.load()); }
    // Played and starved as far as the device has taken `taken` of its frames.
    std::pair<int64_t, int64_t> counts(int64_t taken) const {
        std::lock_guard<std::mutex> lock(madeM);
        while (made.size() > 1 && made[1].from <= taken) made.pop_front();
        if (made.empty() || taken <= made.front().from) return made.empty() ? std::pair<int64_t, int64_t>{0, 0} : std::pair{made.front().played, made.front().starved};
        const Made& m = made.front();
        const int64_t into = std::min<int64_t>(taken - m.from, m.frames);
        const int64_t played = m.played + std::min<int64_t>(into, m.sound);
        const int64_t starved = m.starved + (m.sound > 0 && m.sound < m.frames ? std::clamp<int64_t>(into - m.sound, 0, m.frames - m.sound) : 0);
        return {played, starved};
    }
};

Feed::Feed(Engine& engine) : engine_(engine) {}

Feed::~Feed() { close(); }

bool Feed::open(int rate, int channels, double seconds, std::string* error) {
    close();
    rate_ = rate > 0 ? rate : 48000;
    channels_ = std::clamp(channels, 1, 64);
    auto ring = std::make_shared<Ring>();
    ring->channels = channels_;
    ring->capacity = std::max<int64_t>(256, int64_t(std::clamp(seconds, 0.02, 30.0) * rate_));
    ring->buf.assign(size_t(ring->capacity) * size_t(channels_), 0.0f);
    const int id = engine_.addOutput(ring.get(), rate_, channels_, [ring](float* out, int ch, int frames) -> int64_t {
        ring->render(out, ch, frames);
        return -1;  // (not the timeline)
    }, error);
    if (id < 0) return false;
    ring_ = std::move(ring);
    id_ = id;
    return true;
}

void Feed::close() {
    if (id_ >= 0) engine_.removeOutput(id_);  // (the audio thread has let the ring go once this returns)
    id_ = -1;
    ring_.reset();
}

int64_t Feed::queued() const { return ring_ ? std::max<int64_t>(0, ring_->written.load() - ring_->readNow()) : 0; }

int64_t Feed::room() const { return ring_ ? std::max<int64_t>(0, ring_->capacity - queued()) : 0; }

int64_t Feed::push(const float* interleaved, int frames) {
    if (!ring_ || frames <= 0) return 0;
    Ring& r = *ring_;
    const int64_t n = std::min<int64_t>(frames, room());
    const int64_t w = r.written.load();
    for (int64_t i = 0; i < n; ++i)
        std::copy(interleaved + size_t(i) * size_t(channels_), interleaved + size_t(i + 1) * size_t(channels_),
                  r.buf.begin() + std::ptrdiff_t(size_t((w + i) % r.capacity) * size_t(channels_)));
    r.written.store(w + n, std::memory_order_release);
    return n;
}

int64_t Feed::played() const { return ring_ ? ring_->counts(std::max<int64_t>(0, engine_.takenFrames(id_))).first : 0; }

int64_t Feed::starved() const { return ring_ ? ring_->counts(std::max<int64_t>(0, engine_.takenFrames(id_))).second : 0; }

void Feed::clear() {
    if (ring_) ring_->dropTo = ring_->written.load();
}

// ---- RateWatch.

void RateWatch::reset(int nominal) {
    rate_ = nominal;
    candidate_ = 0;
    start_ = last_ = -1;
    frames_ = 0;
}

int RateWatch::add(int frames, int64_t ns) {
    if (last_ < 0 || ns - last_ > 500'000'000LL || ns < last_) {
        // (the first callback, or one after a gap: the frames are counted from here)
        start_ = last_ = ns;
        frames_ = 0;
        candidate_ = 0;
        return 0;
    }
    last_ = ns;
    frames_ += frames;
    if (ns - start_ < 1'000'000'000LL) return 0;
    const double measured = double(frames_) * 1e9 / double(ns - start_);
    start_ = ns;
    frames_ = 0;
    static const int standard[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 352800, 384000};
    int snapped = 0;
    for (int s : standard)
        if (std::fabs(measured - s) <= 0.02 * s) snapped = s;
    if (!snapped || rate_ <= 0 || std::fabs(measured - rate_) <= 0.04 * rate_) {
        candidate_ = 0;
        return 0;
    }
    if (candidate_ != snapped) {
        candidate_ = snapped;
        return 0;
    }
    rate_ = snapped;
    candidate_ = 0;
    return rate_;
}

}  // namespace montage::audiodev
