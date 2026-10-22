// Montage — reference output over Open Media Transport (media/Omt.h): the program as an OMT source on the network
// ("HOST (Montage)"), 16-bit or 8-bit 4:2:2 in VMX with its sound as 32-bit float, at the sequence's own size and rate.
// Each picture carries its timecode (also its timestamp, in 100 ns from midnight as NDI's timecode is) and NDI's colour
// element, since OMT's own header says only BT.601 or BT.709. Montage paces the frames itself: OMT has no clock.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

#include "ReferenceOutput.h"
#include "media/Omt.h"

namespace montage::output {

namespace {

std::string timecodeText(const Timecode& t) {
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d%c%02d", t.hours, t.minutes, t.seconds, t.dropFrame ? ';' : ':', t.frames);
    return b;
}

class OmtBackend : public Backend {
public:
    ~OmtBackend() override { close(); }
    std::string name() const override { return "OMT"; }
    bool available(std::string* why) override { return omt::vmxAvailable(why); }
    std::vector<Device> devices() override {
        if (!omt::vmxAvailable()) return {};
        Device d;
        d.id = "Montage";
        d.name = "OMT source \"" + omt::fullName(d.id) + "\"";
        d.formats = {PixelFormat::P216, PixelFormat::Uyvy8};
        d.hdr = true;  // (its colour element; there is no mastering display metadata)
        d.audioChannels = {2, 8, 16};
        d.audioRates.clear();
        return {d};
    }
    bool open(const Device& device, const Mode& mode, const Signal& signal, int, int, std::string* error) override {
        close();
        omt::VideoHeader size;
        size.width = mode.width, size.height = mode.height;
        if (!omt::plausible(size)) {
            if (error)
                *error = "OMT carries pictures up to " + std::to_string(omt::kMaxWidth) + " x " + std::to_string(omt::kMaxHeight) +
                         " with an even width, not " + std::to_string(mode.width) + " x " + std::to_string(mode.height);
            return false;
        }
        omt::Sender::Options o;
        o.name = device.id.empty() ? "Montage" : device.id;
#ifdef MONTAGE_VERSION
        o.version = MONTAGE_VERSION;
#endif
        std::lock_guard lock(m_);
        if (!sender_.start(o, error)) return false;
        colorInfo_ = ndiColorInfo(signal);
        colorSpace_ = signal.matrix == video::YcbcrMatrix::Bt709 ? 709 : 0;  // (BT.2020's matrix: in the colour element)
        woken_ = false;
        next_ = std::chrono::steady_clock::now();
        return true;
    }
    bool send(const VideoFrame& f, const Audio& a) override {
        if (!sender_.running()) return false;
        // 100 ns units, from the frame's timecode (the output's frames at its rate).
        const int64_t timestamp = f.fps.num > 0 ? f.timecodeFrame * int64_t(10000000) * f.fps.den / f.fps.num : 0;
        if (a.frames > 0 && a.channels > 0) {
            planar_.assign(size_t(a.frames) * size_t(a.channels), 0.0f);
            if (a.samples)
                for (int c = 0; c < a.channels; ++c)
                    for (int i = 0; i < a.frames; ++i) planar_[size_t(c) * size_t(a.frames) + size_t(i)] = a.samples[size_t(i) * size_t(a.channels) + size_t(c)];
            sender_.sendAudio(planar_.data(), a.channels, a.frames, a.frames, a.rate, timestamp);
        }
        omt::VideoOut v;
        v.format = f.format == PixelFormat::P216 ? omt::PictureFormat::P216 : omt::PictureFormat::Uyvy;
        v.width = f.width;
        v.height = f.height;
        v.stride = int(f.stride);
        v.data = f.data;
        v.rateN = f.fps.num;
        v.rateD = f.fps.den;
        v.colorSpace = colorSpace_;
        v.timestamp = timestamp;
        v.metadata = colorInfo_ + "<montage_timecode value=\"" + timecodeText(f.timecode) + "\"/>";
        if (!sender_.sendVideo(v)) return false;
        // Then the frame's time on the wire (caught up, not hurried, after a stall).
        std::unique_lock lock(m_);
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / f.fps.toDouble()));
        next_ += period;
        const auto now = std::chrono::steady_clock::now();
        if (next_ < now - 2 * period) next_ = now;
        cv_.wait_until(lock, next_, [&] { return woken_; });
        return !woken_;
    }
    void close() override {
        sender_.stop();
        std::lock_guard lock(m_);
        woken_ = false;
    }
    void wake() override {
        std::lock_guard lock(m_);
        woken_ = true;
        cv_.notify_all();
    }
    int receivers() override { return sender_.running() ? sender_.connections() : -1; }
    std::string address() override { return sender_.running() ? "omt://" + omt::machineName() + ":" + std::to_string(sender_.port()) : std::string(); }

private:
    omt::Sender sender_;
    std::mutex m_;
    std::condition_variable cv_;
    bool woken_ = false;
    std::chrono::steady_clock::time_point next_;
    std::string colorInfo_;
    int colorSpace_ = 709;
    std::vector<float> planar_;
};

}  // namespace

std::shared_ptr<Backend> makeOmtBackend() { return std::make_shared<OmtBackend>(); }

}  // namespace montage::output
