#include "ReferenceOutput.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/History.h"
#include "render/ColorSpace.h"

namespace montage::output {

namespace {

bool sameRate(Rational a, Rational b) { return int64_t(a.num) * b.den == int64_t(b.num) * a.den; }

std::string rateText(Rational fps) {
    const double v = fps.toDouble();
    char buf[16];
    if (std::fabs(v - std::round(v)) < 1e-6) std::snprintf(buf, sizeof buf, "%d", int(std::lround(v)));
    else std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

}  // namespace

std::string modeName(int width, int height, Rational fps, bool interlaced) {
    // 16:9 by its height ("1080p25"), others by both sides ("2048x1080p24"); interlaced by its field rate ("1080i50").
    const std::string size = int64_t(width) * 9 == int64_t(height) * 16 ? std::to_string(height)
                                                                        : std::to_string(width) + "x" + std::to_string(height);
    return size + (interlaced ? "i" + rateText({fps.num * 2, fps.den}) : "p" + rateText(fps));
}

std::vector<Mode> standardModes() {
    std::vector<Mode> out;
    auto add = [&](int w, int h, Rational fps, bool interlaced = false) {
        out.push_back({modeName(w, h, fps, interlaced), w, h, fps, interlaced, int64_t(out.size())});
    };
    const Rational hd[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
    for (Rational r : {Rational{50, 1}, Rational{60000, 1001}, Rational{60, 1}}) add(1280, 720, r);
    for (Rational r : hd) add(1920, 1080, r);
    add(1920, 1080, {25, 1}, true);
    add(1920, 1080, {30000, 1001}, true);
    add(1920, 1080, {30, 1}, true);
    for (Rational r : {Rational{24000, 1001}, Rational{24, 1}, Rational{25, 1}}) add(2048, 1080, r);
    for (Rational r : hd) add(3840, 2160, r);
    for (Rational r : {Rational{24000, 1001}, Rational{24, 1}, Rational{25, 1}, Rational{50, 1}, Rational{60, 1}}) add(4096, 2160, r);
    return out;
}

const Mode* matchMode(const std::vector<Mode>& modes, int width, int height, Rational fps) {
    const Mode* best = nullptr;
    auto better = [&](const Mode& m) {
        // Its size first, progressive before interlaced; then the smallest that holds the picture; then the largest.
        auto rank = [&](const Mode& x) {
            const bool same = x.width == width && x.height == height;
            const bool holds = x.width >= width && x.height >= height;
            return std::make_tuple(same ? 0 : holds ? 1 : 2, x.interlaced ? 1 : 0,
                                   holds ? int64_t(x.width) * x.height : -int64_t(x.width) * x.height);
        };
        return !best || rank(m) < rank(*best);
    };
    for (const Mode& m : modes)
        if (sameRate(m.fps, fps) && better(m)) best = &m;
    return best;
}

const Mode* findMode(const std::vector<Mode>& modes, const std::string& name) {
    for (const Mode& m : modes)
        if (m.name == name) return &m;
    return nullptr;
}

size_t rowBytes(PixelFormat f, int width) {
    switch (f) {
        case PixelFormat::V210: return video::v210RowBytes(width);
        case PixelFormat::Uyvy8: return size_t(width) * 2;
        case PixelFormat::P216: return size_t(width) * 2;
    }
    return 0;
}

size_t frameBytes(PixelFormat f, int width, int height) {
    return rowBytes(f, width) * size_t(height) * (f == PixelFormat::P216 ? 2 : 1);
}

Timecode timecodeOf(FrameTime frame, Rational fps) {
    Timecode tc;
    const std::string text = formatTimecode(std::max<FrameTime>(0, frame), fps, true);
    char sep = ':';
    std::sscanf(text.c_str(), "%d:%d:%d%c%d", &tc.hours, &tc.minutes, &tc.seconds, &sep, &tc.frames);
    tc.dropFrame = sep == ';';
    return tc;
}

bool signalFor(const Sequence& s, const Config& c, PixelFormat format, Signal& out, std::string* error) {
    out = Signal{};
    out.format = format;
    const std::string id = c.rec709 ? "rec709" : sequenceColorSpace(s).id;
    if (id == "rec2020") {
        out.colorSpace = "rec2020";
    } else if (id == "rec2100pq" || id == "p3d65pq") {
        out.colorSpace = "rec2100pq";
        out.eotf = Eotf::Pq;
    } else if (id == "rec2100hlg") {
        out.colorSpace = "rec2100hlg";
        out.eotf = Eotf::Hlg;
    } else {
        out.colorSpace = "rec709";  // (sRGB and P3 pictures as BT.709 video)
    }
    out.bt2020 = out.colorSpace != "rec709";
    out.matrix = out.bt2020 ? video::YcbcrMatrix::Bt2020 : video::YcbcrMatrix::Bt709;
    if (out.eotf != Eotf::Sdr && format == PixelFormat::Uyvy8) {
        if (error) *error = "HDR needs a 10-bit signal (or the BT.709 picture)";
        return false;
    }
    // HDR10's static metadata as exports state it: mastered on P3-D65 at the sequence's peak, its light levels
    // measured or else the peak's.
    out.hdrMetadata = out.eotf == Eotf::Pq && c.hdrMetadata;
    out.masteringPeak = std::clamp(s.hdrPeakNits, 100.0, 10000.0);
    const bool analysed = s.hdrMaxCll > 0;
    out.maxCll = analysed ? int(std::clamp(std::lround(s.hdrMaxCll), 1L, 10000L)) : int(std::lround(out.masteringPeak));
    out.maxFall = analysed ? int(std::clamp(std::lround(s.hdrMaxFall), 1L, long(out.maxCll)))
                           : int(std::lround(std::min(out.masteringPeak, 400.0)));
    return true;
}

Rect fitRect(int w, int h, int W, int H, Config::Scaling scaling) {
    if (w <= 0 || h <= 0) return {0, 0, W, H};
    double k = 1;
    if (scaling == Config::Scaling::Fit) k = std::min(double(W) / w, double(H) / h);
    else if (scaling == Config::Scaling::Fill) k = std::max(double(W) / w, double(H) / h);
    Rect r;
    r.width = std::max(1, int(std::lround(w * k)));
    r.height = std::max(1, int(std::lround(h * k)));
    r.x = (W - r.width) / 2;
    r.y = (H - r.height) / 2;
    return r;
}

Image fitInto(const Image& img, int W, int H, Config::Scaling scaling) {
    Image out(W, H);
    if (img.empty()) return out;
    const Rect r = fitRect(img.width, img.height, W, H, scaling);
    const int x0 = std::max(0, r.x), x1 = std::min(W, r.x + r.width), y0 = std::max(0, r.y), y1 = std::min(H, r.y + r.height);
    if (x0 >= x1 || y0 >= y1) return out;
    const bool same = r.width == img.width && r.height == img.height;
    const double kx = double(img.width) / r.width, ky = double(img.height) / r.height;
    parallelRows(y1 - y0, [&](int a, int b) {
        for (int y = y0 + a; y < y0 + b; ++y) {
            float* o = out.at(x0, y);
            if (same) {
                std::memcpy(o, img.at(x0 - r.x, y - r.y), size_t(x1 - x0) * 4 * sizeof(float));
                continue;
            }
            const double sy = std::clamp((y - r.y + 0.5) * ky - 0.5, 0.0, double(img.height - 1));
            const int iy = int(sy), jy = std::min(iy + 1, img.height - 1);
            const float fy = float(sy - iy);
            for (int x = x0; x < x1; ++x, o += 4) {
                const double sx = std::clamp((x - r.x + 0.5) * kx - 0.5, 0.0, double(img.width - 1));
                const int ix = int(sx), jx = std::min(ix + 1, img.width - 1);
                const float fx = float(sx - ix);
                const float *p00 = img.at(ix, iy), *p10 = img.at(jx, iy), *p01 = img.at(ix, jy), *p11 = img.at(jx, jy);
                for (int c = 0; c < 4; ++c)
                    o[c] = (p00[c] * (1 - fx) + p10[c] * fx) * (1 - fy) + (p01[c] * (1 - fx) + p11[c] * fx) * fy;
            }
        }
    });
    return out;
}

std::string ndiColorInfo(const Signal& s) {
    const char* transfer = s.eotf == Eotf::Pq ? "bt_2100_pq" : s.eotf == Eotf::Hlg ? "bt_2100_hlg" : s.bt2020 ? "bt_2020" : "bt_709";
    const char* gamut = s.bt2020 ? "bt_2020" : "bt_709";
    return std::string("<ndi_color_info transfer=\"") + transfer + "\" matrix=\"" + gamut + "\" primaries=\"" + gamut + "\"/>";
}

// ---------------------------------------------------------------------------

Scheduler::Scheduler(std::shared_ptr<Backend> backend) : backend_(std::move(backend)) {}

Scheduler::~Scheduler() { stop(); }

bool Scheduler::start(const Device& device, const Mode& mode, const Sequence& s, const Config& config, std::string* error) {
    stop();
    auto has = [&](PixelFormat f) { return std::find(device.formats.begin(), device.formats.end(), f) != device.formats.end(); };
    PixelFormat format = PixelFormat::Uyvy8;
    if (config.depth > 8) format = has(PixelFormat::V210) ? PixelFormat::V210 : has(PixelFormat::P216) ? PixelFormat::P216 : PixelFormat::Uyvy8;
    if (!has(format)) {
        if (error) *error = "The device takes none of Montage's pixel formats";
        return false;
    }
    Signal signal;
    if (!signalFor(s, config, format, signal, error)) return false;
    if (signal.eotf != Eotf::Sdr && !device.hdr && !config.rec709 && signal.hdrMetadata) signal.hdrMetadata = false;
    device_ = device;
    mode_ = mode;
    signal_ = signal;
    config_ = config;
    seqSpace_ = sequenceColorSpace(s).id;
    peakNits_ = s.hdrPeakNits;
    seqFps_ = s.fps.valid() ? s.fps : Rational{25, 1};
    audioRate_ = s.sampleRate > 0 ? s.sampleRate : 48000;
    // Embedded audio: the channels asked for as the device takes them (the next count up), at a rate it takes.
    audioChannels_ = 0;
    const bool rateOk = device.audioRates.empty() ||
                        std::find(device.audioRates.begin(), device.audioRates.end(), audioRate_) != device.audioRates.end();
    if (config.audioChannels > 0 && rateOk && !device.audioChannels.empty()) {
        std::vector<int> counts = device.audioChannels;
        std::sort(counts.begin(), counts.end());
        audioChannels_ = counts.back();
        for (int n : counts)
            if (n >= config.audioChannels) {
                audioChannels_ = n;
                break;
            }
    }
    const Rect r = fitRect(s.width, s.height, mode.width, mode.height, config.scaling);
    renderScale_ = s.width > 0 ? double(r.width) / s.width : 1;
    black_ = pack(Image(mode.width, mode.height));
    if (!backend_->open(device, mode, signal_, audioChannels_, audioRate_, error)) return false;
    {
        std::lock_guard lock(m_);
        pending_.clear();
        packed_.clear();
        stats_ = Stats{};
    }
    running_ = true;
    packer_ = std::thread([this] { packLoop(); });
    sender_ = std::thread([this] { sendLoop(); });
    return true;
}

void Scheduler::stop() {
    if (!packer_.joinable() && !sender_.joinable()) return;
    running_ = false;
    wake_.notify_all();
    backend_->wake();
    if (packer_.joinable()) packer_.join();
    if (sender_.joinable()) sender_.join();
    backend_->close();
}

void Scheduler::submit(FrameTime t, std::shared_ptr<const Image> img) {
    if (!running_ || !img || img->empty()) return;
    {
        std::lock_guard lock(m_);
        pending_[t] = std::move(img);
        // Only a few wait: those nearest the playhead.
        while (pending_.size() > 6) {
            auto first = pending_.begin(), last = std::prev(pending_.end());
            pending_.erase(std::llabs(first->first - position_) > std::llabs(last->first - position_) ? first : last);
        }
    }
    wake_.notify_all();
}

bool Scheduler::ready(FrameTime t) const {
    std::lock_guard lock(m_);
    return packed_.count(t) > 0;
}

void Scheduler::clearFrames() {
    std::lock_guard lock(m_);
    pending_.clear();
    packed_.clear();
}

void Scheduler::setPosition(FrameTime t, double speed) {
    std::lock_guard lock(m_);
    position_ = t;
    speed_ = speed;
}

void Scheduler::setAudioSource(AudioSource source) {
    std::lock_guard lock(m_);
    audio_ = std::move(source);
}

Scheduler::Stats Scheduler::stats() const {
    std::lock_guard lock(m_);
    Stats s = stats_;
    s.receivers = backend_->receivers();
    return s;
}

std::shared_ptr<const Scheduler::Packed> Scheduler::pack(const Image& img) const {
    const Image* src = &img;
    Image converted;
    if (seqSpace_ != signal_.colorSpace) {
        const ColorSpace* from = findColorSpace(seqSpace_);
        const ColorSpace* to = findColorSpace(signal_.colorSpace);
        if (from && to) {
            converted = img;
            convertColor(converted, *from, *to, peakNits_);
            src = &converted;
        }
    }
    Image fitted;
    if (src->width != mode_.width || src->height != mode_.height) {
        fitted = fitInto(*src, mode_.width, mode_.height, config_.scaling);
        src = &fitted;
    }
    const video::Ycbcr422 yc = video::toYcbcr422(*src, signal_.matrix);
    auto out = std::make_shared<Packed>();
    out->data.resize(frameBytes(signal_.format, mode_.width, mode_.height));
    const size_t stride = rowBytes(signal_.format, mode_.width);
    switch (signal_.format) {
        case PixelFormat::V210: video::packV210(yc, out->data.data(), stride); break;
        case PixelFormat::Uyvy8: video::packUyvy(yc, out->data.data(), stride); break;
        case PixelFormat::P216: video::packP216(yc, out->data.data(), stride); break;
    }
    return out;
}

void Scheduler::packLoop() {
    while (running_) {
        FrameTime t = 0;
        std::shared_ptr<const Image> img;
        {
            std::unique_lock lock(m_);
            wake_.wait(lock, [&] { return !running_ || !pending_.empty(); });
            if (!running_) return;
            // The one nearest the playhead first.
            auto best = pending_.begin();
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (std::llabs(it->first - position_) < std::llabs(best->first - position_)) best = it;
            t = best->first;
            img = std::move(best->second);
            pending_.erase(best);
        }
        std::shared_ptr<const Packed> p = pack(*img);
        std::lock_guard lock(m_);
        packed_[t] = std::move(p);
        // Kept: the frames nearest the playhead.
        while (packed_.size() > 40) {
            auto first = packed_.begin(), last = std::prev(packed_.end());
            packed_.erase(std::llabs(first->first - position_) > std::llabs(last->first - position_) ? first : last);
        }
    }
}

void Scheduler::sendLoop() {
    std::shared_ptr<const Packed> last = black_;
    FrameTime lastFrame = -1;
    int64_t tick = 0, cursor = 0;
    bool audioOn = false;
    std::vector<float> audio;
    const double modeFps = mode_.fps.toDouble() > 0 ? mode_.fps.toDouble() : 25;
    const double seqFps = seqFps_.toDouble();
    while (running_) {
        FrameTime pos = 0, target = 0;
        double speed = 0;
        AudioSource source;
        std::shared_ptr<const Packed> frame;
        {
            std::lock_guard lock(m_);
            pos = position_;
            speed = speed_;
            source = audio_;
            const int dir = speed > 0 ? 1 : speed < 0 ? -1 : 0;
            target = pos + FrameTime(config_.videoOffset) * dir;
            auto it = packed_.find(target);
            if (it != packed_.end()) frame = it->second;
            // Frames passed are let go.
            if (dir > 0) packed_.erase(packed_.begin(), packed_.lower_bound(target - 2));
            else if (dir < 0) packed_.erase(packed_.upper_bound(target + 2), packed_.end());
        }
        const bool playing = speed != 0;
        if (frame) {
            last = frame;
            lastFrame = target;
        }
        const bool late = !frame && playing;
        // This output frame's samples: 48000 / 29.97 alternates 1601 and 1602 (8008 in five frames).
        const int n = int(std::llround((tick + 1) * audioRate_ / modeFps) - std::llround(tick * audioRate_ / modeFps));
        audio.assign(size_t(std::max(0, n)) * size_t(audioChannels_), 0.0f);
        if (audioChannels_ > 0 && speed == 1 && source) {
            // Sound runs on from where it was, unless it has drifted more than a frame from the picture's.
            const FrameTime shown = lastFrame >= 0 ? lastFrame : pos;
            const int64_t want = std::llround(double(shown) * audioRate_ / seqFps) - int64_t(config_.audioOffsetMs) * audioRate_ / 1000;
            if (!audioOn || std::llabs(cursor - want) > n * 3 / 2) cursor = want;
            if (cursor + n > 0) source(cursor, n, audioChannels_, audio.data());
            cursor += n;
            audioOn = true;
        } else {
            audioOn = false;
        }
        VideoFrame vf;
        vf.width = mode_.width;
        vf.height = mode_.height;
        vf.stride = rowBytes(signal_.format, mode_.width);
        vf.format = signal_.format;
        vf.data = last->data.data();
        vf.frame = lastFrame >= 0 ? lastFrame : pos;
        vf.timecode = timecodeOf(vf.frame, seqFps_);
        vf.fps = mode_.fps;
        Audio a;
        a.samples = audio.empty() ? nullptr : audio.data();
        a.frames = audioChannels_ > 0 ? n : 0;
        a.channels = audioChannels_;
        a.rate = audioRate_;
        {
            std::lock_guard lock(m_);
            ++stats_.sent;
            if (late) ++stats_.late;
            stats_.last = vf.frame;
        }
        const bool ok = backend_->send(vf, a);
        ++tick;
        if (!ok) {
            std::lock_guard lock(m_);
            if (running_) stats_.error = "The output stopped";
            running_ = false;
            wake_.notify_all();
            return;
        }
    }
}

// ---------------------------------------------------------------------------

std::vector<Device> VirtualBackend::devices() {
    Device d;
    d.id = "virtual";
    d.name = "Virtual Output";
    d.modes = standardModes();
    d.formats = {PixelFormat::V210, PixelFormat::Uyvy8, PixelFormat::P216};
    d.hdr = true;
    d.audioChannels = {2, 8, 16};
    d.audioRates.clear();
    return {d};
}

bool VirtualBackend::open(const Device&, const Mode& mode, const Signal& signal, int audioChannels, int, std::string*) {
    std::lock_guard lock(m_);
    open_ = true;
    woken_ = false;
    permits_ = 0;
    mode_ = mode;
    signal_ = signal;
    channels_ = audioChannels;
    sent_.clear();
    audio_.clear();
    next_ = std::chrono::steady_clock::now();
    return true;
}

bool VirtualBackend::send(const VideoFrame& frame, const Audio& a) {
    std::unique_lock lock(m_);
    if (woken_ || !open_) return false;
    Sent s;
    s.frame = frame.frame;
    s.timecode = frame.timecode;
    s.data.assign(frame.data, frame.data + frameBytes(frame.format, frame.width, frame.height));
    s.audioFrames = a.frames;
    sent_.push_back(std::move(s));
    if (sent_.size() > 4) sent_[sent_.size() - 5].data.clear();  // (only the last few frames' pictures kept)
    if (a.samples) audio_.insert(audio_.end(), a.samples, a.samples + size_t(a.frames) * size_t(a.channels));
    else audio_.insert(audio_.end(), size_t(a.frames) * size_t(a.channels), 0.0f);
    cv_.notify_all();
    // Then the frame's time on the wire.
    if (manual_) {
        cv_.wait(lock, [&] { return permits_ > 0 || woken_ || !open_; });
        if (permits_ > 0) --permits_;
    } else {
        next_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / frame.fps.toDouble()));
        cv_.wait_until(lock, next_, [&] { return woken_ || !open_; });
    }
    return !woken_ && open_;
}

void VirtualBackend::close() {
    std::lock_guard lock(m_);
    open_ = false;
    cv_.notify_all();
}

void VirtualBackend::wake() {
    std::lock_guard lock(m_);
    woken_ = true;
    cv_.notify_all();
}

void VirtualBackend::step(int frames) {
    std::lock_guard lock(m_);
    permits_ += frames;
    cv_.notify_all();
}

bool VirtualBackend::waitSent(size_t count, int ms) {
    std::unique_lock lock(m_);
    return cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return sent_.size() >= count; });
}

std::vector<VirtualBackend::Sent> VirtualBackend::sent() const {
    std::lock_guard lock(m_);
    return sent_;
}

std::vector<float> VirtualBackend::audio() const {
    std::lock_guard lock(m_);
    return audio_;
}

Mode VirtualBackend::openMode() const {
    std::lock_guard lock(m_);
    return mode_;
}

Signal VirtualBackend::openSignal() const {
    std::lock_guard lock(m_);
    return signal_;
}

std::vector<std::shared_ptr<Backend>> backends() {
    static const std::vector<std::shared_ptr<Backend>> all = [] {
        std::vector<std::shared_ptr<Backend>> v;
        const char* virt = std::getenv("MONTAGE_VIRTUAL_OUTPUT");
        if (virt && *virt) v.push_back(std::make_shared<VirtualBackend>());
        return v;
    }();
    return all;
}

std::shared_ptr<Backend> findBackend(const std::string& name) {
    for (const auto& b : backends())
        if (b->name() == name) return b;
    return nullptr;
}

}  // namespace montage::output
