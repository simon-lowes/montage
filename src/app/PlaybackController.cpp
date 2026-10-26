#include "PlaybackController.h"
#include "render/DialogueLoudness.h"
#include "render/TrimPasses.h"

#include <atomic>

#include "core/Adr.h"
#include "media/Loudness.h"
#include "render/RenderCache.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#include "core/Surround.h"
#include "render/AdaptiveHdr.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/HdrView.h"
#include "render/MonitorCalibration.h"
#include "render/ReferenceOutput.h"

namespace montage {

// ---------------------------------------------------------------------------
// Renders frames on a background thread. Only the newest request is served;
// while playing, idle time renders the frames ahead of the playhead into a
// small cache so playback keeps up with complex timelines.

class RenderWorker : public QObject {
    Q_OBJECT
public:
    struct Request {
        std::shared_ptr<const Project> project;
        Id sequence = 0;
        FrameTime frame = 0;
        double scale = 1;
        bool proxies = false;
        int direction = 0;  // playback step (+1, -2 ...), 0 when paused
        bool captions = false;
        double cacheScale = 0;  // the scale rendered previews are kept at (playback's), 0 = scale
        StereoView stereoView = StereoView::Left;  // how a stereo 3D sequence's eyes are shown
        bool hdr = false;  // HDR sequences also as light for the HDR viewer
        std::string trim;  // the trim pass shown (render/TrimPasses.h), "" = the master
        bool gpu = false;  // drawn on the GPU where it can be (render/GpuCompositor.h)
        std::shared_ptr<const Lut3D> hdrCalibration;  // the HDR viewer's display's calibration (render/MonitorCalibration.h)
        double hdrWhite = 203;                         // the nits that display shows SDR white at
        double hdrHeadroom = 1;                        // and its headroom (its peak over SDR white), for adaptive HDR metadata
        bool scopeWorking = false;  // the scopes read the working space (a sequence graded scene-referred)
    };
    void request(const Request& r) {
        {
            QMutexLocker lock(&m_);
            pending_ = r;
            has_ = true;
        }
        QMetaObject::invokeMethod(this, &RenderWorker::process, Qt::QueuedConnection);
    }
    // The outputs every frame rendered also goes to (the reference output, the remote review stream).
    void setSinks(std::vector<std::shared_ptr<output::Scheduler>> s) {
        QMutexLocker lock(&sinkM_);
        sinks_ = std::move(s);
    }

signals:
    void rendered(const QImage& image, montage::FrameTime t);
    void scopeRendered(const QImage& image, montage::FrameTime t, const QString& space, double peakNits);
    void hdrRendered(montage::HdrPicturePtr picture, montage::FrameTime t);

private:
    // A frame for the viewer (Rec.709) and, for sequences in another space, a small copy as delivered (16-bit code
    // values in the sequence's space) so the scopes measure what is exported, not its SDR preview.
    struct Rendered {
        QImage view, signal;
        HdrPicturePtr hdr;  // for the HDR viewer (HDR sequences, when it is on)
    };
    static constexpr int kSignalWidth = 480, kSignalHeight = 320;

    static QImage signalImage(const Image& img) {
        int w = img.width, h = img.height;
        if (w > kSignalWidth || h > kSignalHeight) {
            const double k = std::min(double(kSignalWidth) / w, double(kSignalHeight) / h);
            w = std::max(1, int(w * k)), h = std::max(1, int(h * k));
        }
        QImage out(w, h, QImage::Format_RGBA64);
        for (int y = 0; y < h; ++y) {
            const float* row = img.row(std::min(img.height - 1, int((y + 0.5) * img.height / h)));
            auto* o = reinterpret_cast<quint16*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const float* px = row + 4 * size_t(std::min(img.width - 1, int((x + 0.5) * img.width / w)));
                for (int c = 0; c < 4; ++c) o[4 * x + c] = quint16(std::lround(std::clamp(px[c], 0.0f, 1.0f) * 65535));
            }
        }
        return out;
    }

    // (A trim pass is in its target's space.)
    static bool ownSpace(const Sequence& s, const std::string& trim) { return trimTargetSpace(s, trim).id != "rec709"; }

    // The frame as delivered alone (a preview from the render cache has only its SDR picture).
    QImage renderSignal(const Request& r, FrameTime t) {
        const Sequence* s = r.project->findSequence(r.sequence);
        if (!s || !ownSpace(*s, r.trim)) return {};
        RenderOptions o;
        o.scale = r.scale;
        o.useProxies = r.proxies;
        o.offlineProxies = true;  // (viewing: an offline file plays its proxy)
        o.captions = r.captions;
        o.stereoView = r.stereoView;
        o.trimTarget = r.trim;
        return signalImage(renderProgramFrame(*r.project, *s, t, o));
    }

    // The frame in a scene-referred sequence's working space alone, for the scopes.
    QImage renderWorkingSignal(const Request& r, FrameTime t) {
        const Sequence* s = r.project->findSequence(r.sequence);
        if (!s || !sceneReferredWorking(*s)) return {};
        RenderOptions o;
        o.scale = r.scale;
        o.useProxies = r.proxies;
        o.offlineProxies = true;
        o.stereoView = r.stereoView;
        o.sceneLight = true;
        Image img = renderProgramFrame(*r.project, *s, t, o);
        convertExactly(img, linearColorSpace(workingColorSpace(*s).primaries), workingColorSpace(*s));
        return signalImage(img);
    }

    void emitFrame(Rendered& img, const Request& r, FrameTime t) {
        emit rendered(img.view, t);
        if (r.hdr) emit hdrRendered(img.hdr, t);  // (none for SDR sequences: the viewer shows the SDR picture)
        const Sequence* s = r.project->findSequence(r.sequence);
        if (!s) return;
        if (r.scopeWorking && sceneReferredWorking(*s)) {
            // (paused: what the grade works on; playing, the scopes keep the last)
            if (r.direction == 0)
                if (const QImage w = renderWorkingSignal(r, t); !w.isNull())
                    emit scopeRendered(w, t, QString::fromStdString(workingColorSpace(*s).id), s->hdrPeakNits);
            return;
        }
        if (!ownSpace(*s, r.trim)) {
            emit scopeRendered(img.view, t, QStringLiteral("rec709"), trimTargetPeak(*s, r.trim));
            return;
        }
        if (img.signal.isNull() && r.direction == 0) img.signal = renderSignal(r, t);  // paused: worth the render
        if (!img.signal.isNull()) emit scopeRendered(img.signal, t, QString::fromStdString(trimTargetSpace(*s, r.trim).id), trimTargetPeak(*s, r.trim));
    }

    static constexpr int kAhead = 24;
    static constexpr size_t kMaxCached = 48;

    // HDR frames carry about three times the bytes (half floats beside the SDR picture), so a third as many are kept,
    // and no more are rendered ahead than fit beside a few already shown.
    size_t cacheLimit() const { return ctx_.hdr ? kMaxCached / 3 : kMaxCached; }
    int ahead() const { return std::min(kAhead, int(cacheLimit()) - 4); }

    bool sameContext(const Request& r) const {
        return r.project == ctx_.project && r.sequence == ctx_.sequence && r.scale == ctx_.scale && r.proxies == ctx_.proxies &&
               r.captions == ctx_.captions && r.stereoView == ctx_.stereoView && r.hdr == ctx_.hdr &&
               r.trim == ctx_.trim && r.gpu == ctx_.gpu && r.hdrCalibration == ctx_.hdrCalibration && r.hdrWhite == ctx_.hdrWhite &&
               r.hdrHeadroom == ctx_.hdrHeadroom && r.scopeWorking == ctx_.scopeWorking;
    }

    Rendered render(const Request& r, FrameTime t) {
        const Sequence* s = r.project->findSequence(r.sequence);
        if (!s) return {};
        RenderOptions o;
        o.scale = r.scale;
        o.useProxies = r.proxies;
        o.offlineProxies = true;  // (viewing: an offline file plays its proxy)
        o.captions = r.captions;
        o.stereoView = r.stereoView;
        o.trimTarget = r.trim;
        o.displaySpace = "rec709";  // HDR and wide-gamut sequences are previewed tone mapped to SDR
        o.gpu = r.gpu;
        // A rendered preview of this frame (kept at playback's scale) is used as it is, paused too.
        RenderOptions co = o;
        if (r.cacheScale > 0) co.scale = r.cacheScale;
        const QByteArray key = frameKey(*r.project, *s, t, co);
        const bool hdr = r.hdr && trimTargetSpace(*s, r.trim).hdr();  // (a rendered preview has only its SDR picture)
        const std::vector<std::shared_ptr<output::Scheduler>> dests = sinks();  // (a rendered preview has no picture for them either)
        const bool dest = !dests.empty();
        if (!hdr && !dest && RenderCache::instance().has(key)) {
            QImage cached = RenderCache::instance().load(key);
            if (!cached.isNull()) return {cached, {}};
        }
        // Rendered in the sequence's own space, kept for the scopes, then shown as Rec.709 (as displaySpace does).
        Rendered out;
        RenderOptions own = o;
        // (Graded scene-referred, the SDR viewer has its own output transform: the delivered picture, for the scopes, is
        // rendered alone when paused.)
        const bool other = ownSpace(*s, r.trim) && !(sceneReferredWorking(*s) && !hdr && !dest);
        // A Rec.709 frame for the viewer alone comes back from the GPU as 8-bit, with no float picture in between.
        if (r.gpu && !other && !dest) {
            out.view = renderProgramFrameRgba8(*r.project, *s, t, o);
            return out;
        }
        if (other) own.displaySpace.clear();
        Image img = renderProgramFrame(*r.project, *s, t, own);
        // To the reference output as rendered (moved there when the viewer's picture needs no conversion).
        std::shared_ptr<const Image> sent;
        if (dest) {
            sent = other ? std::make_shared<const Image>(img) : std::make_shared<const Image>(std::move(img));
            for (const auto& d : dests) d->submit(t, sent);
        }
        const Image& shown = dest && !other ? *sent : img;
        if (other) {
            out.signal = signalImage(img);
            if (hdr) {
                auto pic = std::make_shared<HdrPicture>();
                pic->width = img.width;
                pic->height = img.height;
                pic->contentPeak = hdrContentPeak(trimTargetSpace(*s, r.trim), trimTargetPeak(*s, r.trim));
                hdrViewImage(img, trimTargetSpace(*s, r.trim), trimTargetPeak(*s, r.trim), pic->contentPeak, pic->rgba);  // (rolled off as drawn)
                // The master's adaptive HDR metadata (render/AdaptiveHdr.h), while it matches the cut: tone mapped by its
                // own curves to the display's headroom here, so the viewer has nothing left to roll off.
                if (const AdaptiveHdr* md = r.trim.empty() ? storedAdaptiveHdrAt(r.project, *s, t) : nullptr;
                    md && r.hdrHeadroom < pic->contentPeak) {
                    adaptiveViewImage(pic->rgba, *md, r.hdrHeadroom);
                    pic->contentPeak = std::max(1.0, r.hdrHeadroom);
                }
                if (r.hdrCalibration) calibrateHdrPicture(pic->rgba, pic->width, pic->height, *r.hdrCalibration, r.hdrWhite);
                out.hdr = std::move(pic);
            }
            convertColor(img, trimTargetSpace(*s, r.trim), rec709Space(), trimTargetPeak(*s, r.trim));
        }
        out.view = QImage(shown.width, shown.height, QImage::Format_RGBA8888);
        toRgba8(shown, out.view.bits(), size_t(out.view.bytesPerLine()));
        return out;
    }

    std::vector<std::shared_ptr<output::Scheduler>> sinks() {
        QMutexLocker lock(&sinkM_);
        std::vector<std::shared_ptr<output::Scheduler>> out;
        for (const auto& s : sinks_)
            if (s && s->running()) out.push_back(s);
        return out;
    }
    // Whether an output still wants frame `t` (let go there, or never given it).
    bool sinkWants(FrameTime t) {
        for (const auto& s : sinks())
            if (s->wants(t)) return true;
        return false;
    }

    void process() {
        Request r;
        {
            QMutexLocker lock(&m_);
            if (!has_) return;
            r = pending_;
            has_ = false;
        }
        if (!r.project) return;
        if (!sameContext(r)) {
            cache_.clear();
            ctx_ = r;
        }
        ctx_.direction = r.direction;
        ctx_.frame = r.frame;
        auto it = cache_.find(r.frame);
        // (A frame kept here but let go by the reference output is rendered again for it.)
        Rendered img = it != cache_.end() && !sinkWants(r.frame) ? it->second : render(r, r.frame);
        if (img.view.isNull()) return;
        emitFrame(img, r, r.frame);
        cache_[r.frame] = img;
        trim();
        if (r.direction != 0) QMetaObject::invokeMethod(this, &RenderWorker::prefetch, Qt::QueuedConnection);
    }

    // Renders one missing frame ahead of the playhead, then yields so new
    // requests are always served first.
    void prefetch() {
        {
            QMutexLocker lock(&m_);
            if (has_) return;
        }
        if (ctx_.direction == 0 || !ctx_.project) return;
        const Sequence* s = ctx_.project->findSequence(ctx_.sequence);
        if (!s) return;
        FrameTime end = s->duration();
        for (int k = 1, n = ahead(); k <= n; ++k) {
            FrameTime t = ctx_.frame + FrameTime(k) * ctx_.direction;
            if (t < 0 || t >= end) return;
            if (cache_.count(t) && !sinkWants(t)) continue;
            Rendered img = render(ctx_, t);
            if (img.view.isNull()) return;
            cache_[t] = img;
            trim();
            QMetaObject::invokeMethod(this, &RenderWorker::prefetch, Qt::QueuedConnection);
            return;
        }
    }

    // Drops cached frames: those furthest behind the playhead (as it moves) first, then the furthest ahead, so a frame
    // just rendered ahead is never the one let go.
    void trim() {
        const size_t limit = cacheLimit();
        const bool forward = ctx_.direction >= 0;
        while (cache_.size() > limit) {
            auto first = cache_.begin(), last = std::prev(cache_.end());
            auto behind = forward ? first : last;
            const bool passed = forward ? behind->first < ctx_.frame : behind->first > ctx_.frame;
            cache_.erase(passed ? behind : (forward ? last : first));
        }
    }

    QMutex m_;
    Request pending_;
    bool has_ = false;
    QMutex sinkM_;
    std::vector<std::shared_ptr<output::Scheduler>> sinks_;
    Request ctx_;  // context of the cached frames (render-thread only)
    std::map<FrameTime, Rendered> cache_;
};

// ---------------------------------------------------------------------------
// Pull-mode audio source feeding QAudioSink from the mixer.

class MixerDevice : public QIODevice {
    Q_OBJECT
public:
    explicit MixerDevice(QObject* parent) : QIODevice(parent) {
        mixer_.setNonBlocking(true);
        mixer_.setOfflineProxies(true);  // (an offline file's proxy is heard while editing)
    }

    void configure(std::shared_ptr<const Project> p, Id seq, int64_t startSample, bool int16, int channels = 2, bool idle = false) {
        auto speech = speechIn(p, seq);  // (worked out before the audio thread is held up, as the monitor's mixes are)
        std::optional<MonitorPlan> plan = planMonitorFor(p, seq);
        QMutexLocker lock(&m_);
        const Sequence* s = p ? p->findSequence(seq) : nullptr;
        project_ = std::move(p);
        seq_ = seq;
        sample_ = startSample;
        int16_ = int16;
        channels_ = std::max(1, channels);
        idle_ = idle;
        mixer_.reset();
        if (monitor_) {
            installMonitorPlan(std::move(plan));
            monitor_->reset();
        }
        // Loudness keeps integrating across plays of the same sequence until reset.
        QMutexLocker meterLock(&meterM_);
        const int rate = s ? s->sampleRate : 48000;
        if (!meter_ || seq != meterSeq_ || rate != meterRate_) {
            meter_ = std::make_unique<LoudnessMeter>(rate);
            meterSeq_ = seq;
            meterRate_ = rate;
        }
        stereo_ = std::make_unique<StereoAnalyser>(rate);
        spectrum_ = std::make_unique<SpectrumAnalyser>(rate);
        speech_ = std::move(speech);
    }
    void resetLoudness() {
        QMutexLocker lock(&meterM_);
        if (meter_) meter_->reset();
    }
    void setProject(std::shared_ptr<const Project> p) {
        Id seq = 0;
        {
            QMutexLocker lock(&m_);
            seq = seq_;
        }
        auto speech = speechIn(p, seq);  // (outside the locks the audio thread takes, as the monitor's mixes are)
        std::optional<MonitorPlan> plan = planMonitorFor(p, seq);
        QMutexLocker lock(&m_);
        project_ = std::move(p);
        if (monitor_) installMonitorPlan(std::move(plan));
        QMutexLocker meterLock(&meterM_);
        speech_ = std::move(speech);
    }
    // The monitor's mixes for a project worked out with its settings as they are (none without a monitor), and given
    // to it under the lock (worked out again there, should its settings have changed in between).
    std::optional<MonitorPlan> planMonitorFor(const std::shared_ptr<const Project>& p, Id seq) {
        std::optional<MonitorSettings> settings;
        {
            QMutexLocker lock(&m_);
            if (monitor_) settings = monitor_->settings();
        }
        if (!settings) return std::nullopt;
        return planMonitor(p, seq, *settings);
    }
    void installMonitorPlan(std::optional<MonitorPlan> plan) {  // (with m_ held)
        if (plan && plan->project == project_ && plan->sequence == seq_ && plan->settings == monitor_->settings()) monitor_->prepare(std::move(*plan));
        else monitor_->prepare(project_, seq_);
    }
    void setSpectrumKWeighting(bool on) { kWeighted_ = on; }
    // Global Mute: the meters still move, the speakers get silence.
    void setMuted(bool on) { muted_ = on; }
    void setAmbisonicBinaural(bool on) {
        QMutexLocker lock(&m_);
        mixer_.setAmbisonicBinaural(on);
        binaural_ = on;
        if (monitor_) monitor_->setAmbisonicBinaural(on);
    }
    // The monitoring section (off: the stereo mix as before).
    void setMonitoring(const MonitorSettings& m, int rate) {
        QMutexLocker lock(&m_);
        if (!m.on) {
            monitor_.reset();
            return;
        }
        if (!monitor_) {
            monitor_ = std::make_unique<Monitor>();
            monitor_->setNonBlocking(true);
            monitor_->setOfflineProxies(true);
            monitor_->setAmbisonicBinaural(binaural_);
        }
        monitor_->setSettings(m, rate);
        monitor_->prepare(project_, seq_);
    }
    void setCalibration(int speaker, const MonitorSettings& s, int rate) {
        {
            QMutexLocker lock(&m_);
            if (!monitor_ || monitor_->section().calibration() == speaker) return;
        }
        auto noise = calibrationNoiseFor(s, rate, speaker);  // (made while the audio thread plays on)
        QMutexLocker lock(&m_);
        if (monitor_) monitor_->section().setCalibration(speaker, std::move(noise));
    }
    void setTalkback(bool on) {
        QMutexLocker lock(&m_);
        if (monitor_) monitor_->section().setTalkback(on);
    }
    void feedTalkback(const float* mono, int frames) {
        QMutexLocker lock(&m_);
        if (monitor_) monitor_->section().feedTalkback(mono, frames);
    }
    // Stopped, with the sink kept running for calibration or talkback: no programme.
    void setIdle(bool on) { idle_ = on; }
    // Looped playback from sample `in` for `length` samples (as many as the loop's frames last, so fractional), wrapped
    // here sample-exactly, the picture following (0: no loop).
    void setLoop(int64_t in, double length) {
        QMutexLocker lock(&m_);
        loopIn_ = in;
        loopLength_ = length;
    }
    // An ADR cycle's beeps, in what is heard (never in the meters).
    void setCycle(const std::optional<AdrCycle>& c) {
        QMutexLocker lock(&m_);
        cycle_ = c;
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 16) + QIODevice::bytesAvailable(); }

signals:
    void levels(float l, float r, const QVector<float>& tracks);
    void loudness(double momentary, double shortTerm, double integrated, double range, double truePeak);
    void dialogueLoudness(double integrated, double speechShare);
    void analysis(montage::AudioAnalysisPtr a);

protected:
    qint64 readData(char* data, qint64 maxlen) override {
        const int channels = channels_;
        const int bytesPerFrame = (int16_ ? 2 : 4) * channels;
        int frames = int(maxlen / bytesPerFrame);
        if (frames <= 0) return 0;
        buf_.resize(size_t(frames) * 2);
        std::vector<MeterLevels> tl;
        std::optional<AdrCycle> cycle;
        int64_t first = 0;
        double fps = 30;
        int rate = 48000;
        int monitored = 0;  // the monitoring section's channels, when it is on
        const bool idle = idle_;
        {
            QMutexLocker lock(&m_);
            const Sequence* s = project_ ? project_->findSequence(seq_) : nullptr;
            if (monitor_) {
                monitored = monitor_->outputs();
                mon_.resize(size_t(frames) * size_t(monitored));
            }
            // (In pieces where a loop wraps.)
            for (int done = 0; done < frames;) {
                const int64_t v = sample_ + done;
                const int n = int(std::min<int64_t>(frames - done, untilWrap(v)));
                const int64_t at = looped(v);
                if (monitor_)
                    monitor_->render(at, n, mon_.data() + size_t(done) * size_t(monitored), buf_.data() + size_t(done) * 2, &tl,
                                     s && cycle_ && !idle ? &*cycle_ : nullptr, s && !idle);
                else if (s && !idle) mixer_.mix(*project_, *s, at, n, buf_.data() + size_t(done) * 2, &tl);
                else std::fill(buf_.begin() + done * 2, buf_.begin() + (done + n) * 2, 0.0f);
                done += n;
            }
            if (s && cycle_ && !monitored) {
                cycle = cycle_;
                fps = s->fpsValue();
                rate = s->sampleRate;
            }
            first = looped(sample_);
            if (!idle) sample_ += frames;
        }
        if (monitored) {
            // Through the monitoring section: its channels onto the device's (Global Mute silences them all).
            if (int16_) {
                auto* d = reinterpret_cast<int16_t*>(data);
                for (int i = 0; i < frames; ++i)
                    for (int c = 0; c < channels; ++c) {
                        const float v = muted_ || c >= monitored ? 0.0f : mon_[size_t(i) * size_t(monitored) + size_t(c)];
                        d[size_t(i) * size_t(channels) + size_t(c)] = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
                    }
            } else {
                auto* d = reinterpret_cast<float*>(data);
                for (int i = 0; i < frames; ++i)
                    for (int c = 0; c < channels; ++c)
                        d[size_t(i) * size_t(channels) + size_t(c)] = muted_ || c >= monitored ? 0.0f : mon_[size_t(i) * size_t(monitored) + size_t(c)];
            }
        } else if (idle) {
            std::memset(data, 0, size_t(frames) * size_t(bytesPerFrame));
        } else {
            std::vector<float> heard;  // what the speakers get, when it is not buf_ (which the meters still read)
            if (muted_) heard.assign(buf_.size(), 0.0f);  // (Global Mute silences the beeps too)
            else if (cycle) {
                heard = buf_;
                addAdrBeeps(*cycle, fps, rate, first, heard.data(), frames, 2);
            }
            const std::vector<float>& out = heard.empty() ? buf_ : heard;
            if (int16_) {
                auto* d = reinterpret_cast<int16_t*>(data);
                for (size_t i = 0; i < out.size(); ++i) d[i] = int16_t(std::lround(std::clamp(out[i], -1.0f, 1.0f) * 32767.0f));
            } else {
                std::memcpy(data, out.data(), size_t(frames) * 8);
            }
        }
        if (idle) return qint64(frames) * bytesPerFrame;  // (the meters rest)
        float pl = 0, pr = 0;
        for (int i = 0; i < frames; ++i) {
            pl = std::max(pl, std::fabs(buf_[size_t(i) * 2]));
            pr = std::max(pr, std::fabs(buf_[size_t(i) * 2 + 1]));
        }
        QVector<float> tracks;
        tracks.reserve(int(tl.size()) * 2);
        for (const auto& lv : tl) {
            tracks << lv.peakL << lv.peakR;
        }
        emit levels(pl, pr, tracks);
        // Loudness of what is heard, reported ten times a second.
        {
            QMutexLocker lock(&meterM_);
            if (meter_) {
                // Speech (the dialogue-gated reading) where the timeline says someone speaks, at the block's middle.
                const double mid = double(first + frames / 2) / meterRate_;
                const auto at = std::upper_bound(speech_.begin(), speech_.end(), std::make_pair(mid, 1e300));
                meter_->setSpeech(at != speech_.begin() && std::prev(at)->second > mid);
                meter_->add(buf_.data(), frames);
                if (stereo_) {
                    stereo_->add(buf_.data(), frames);
                    spectrum_->setKWeighting(kWeighted_);
                    spectrum_->add(buf_.data(), frames);
                }
                sinceReport_ += frames;
                if (sinceReport_ >= meterRate_ / 10) {
                    sinceReport_ = 0;
                    const LoudnessResult r = meter_->result();
                    emit loudness(meter_->momentary(), meter_->shortTerm(), r.valid ? r.integrated : -200.0, meter_->loudnessRange(),
                                  r.truePeakDb);
                    const LoudnessResult d = meter_->speechResult();
                    emit dialogueLoudness(d.valid ? d.integrated : -200.0, speech_.empty() ? -1.0 : meter_->speechShare());
                    if (stereo_) emit analysis(takeAudioAnalysis(*stereo_, *spectrum_));
                }
            }
        }
        return qint64(frames) * bytesPerFrame;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    // Where the play's `v`th sample (counted on from where it began) is on the timeline, and how many follow before the
    // loop wraps. (With m_ held.)
    int64_t looped(int64_t v) const {
        if (loopLength_ <= 0 || double(v - loopIn_) < loopLength_) return v;
        return loopIn_ + int64_t(std::floor(std::fmod(double(v - loopIn_), loopLength_)));
    }
    int64_t untilWrap(int64_t v) const {
        if (loopLength_ <= 0) return INT64_MAX;
        const double k = v < loopIn_ ? 0 : std::floor(double(v - loopIn_) / loopLength_);
        return std::max<int64_t>(1, int64_t(std::ceil(double(loopIn_) + (k + 1) * loopLength_)) - v);
    }
    // The timeline's own speech marks (whole Dialogue clips, else transcribed words), in seconds.
    static std::vector<std::pair<double, double>> speechIn(const std::shared_ptr<const Project>& p, Id seq) {
        const Sequence* s = p ? p->findSequence(seq) : nullptr;
        SpeechGate gate = SpeechGate::Auto;
        return s ? timelineSpeech(*p, *s, gate) : std::vector<std::pair<double, double>>{};
    }
    QMutex m_;
    std::shared_ptr<const Project> project_;
    Id seq_ = 0;
    int64_t sample_ = 0;  // (the play's samples so far, from where it began: looped() places them)
    int64_t loopIn_ = 0;
    double loopLength_ = 0;
    bool int16_ = false;
    std::atomic<int> channels_{2};
    std::atomic<bool> idle_{false};
    std::atomic<bool> muted_{false};
    std::optional<AdrCycle> cycle_;
    AudioMixer mixer_;
    std::unique_ptr<Monitor> monitor_;  // the monitoring section, while it is on
    bool binaural_ = false;
    std::vector<float> buf_, mon_;
    QMutex meterM_;
    std::unique_ptr<LoudnessMeter> meter_;
    std::unique_ptr<StereoAnalyser> stereo_;  // (afresh each play)
    std::unique_ptr<SpectrumAnalyser> spectrum_;
    std::atomic<bool> kWeighted_{false};
    Id meterSeq_ = 0;
    int meterRate_ = 48000;
    int64_t sinceReport_ = 0;
    std::vector<std::pair<double, double>> speech_;
};

// ---------------------------------------------------------------------------
// The reference output's embedded sound: what the speakers get (muted tracks, the live mix, Global Mute), mixed on
// the output's own thread.

struct OutputAudio {
    std::mutex m;
    std::shared_ptr<const Project> project;
    Id sequence = 0;
    bool muted = false;
    bool stereo = false;  // a stereo mix of any layout (else the sequence's own channels, the first of them sent)
    AudioMixer mixer;
    std::vector<float> buf;

    void mix(int64_t start, int frames, int channels, float* out) {
        std::lock_guard lock(m);
        const Sequence* s = project ? project->findSequence(sequence) : nullptr;
        if (!s || muted) return;
        // The sequence's own channels (5.1 and 7.1 as they are), into the first of the output's.
        const int n = stereo ? 2 : std::max(2, layoutChannels(s->audioLayout));
        buf.assign(size_t(frames) * size_t(n), 0.0f);
        if (n == 2) mixer.mix(*project, *s, start, frames, buf.data());
        else mixer.mixLayout(*project, *s, start, frames, buf.data());
        const int k = std::min(n, channels);
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < k; ++c) out[size_t(i) * size_t(channels) + size_t(c)] = buf[size_t(i) * size_t(n) + size_t(c)];
    }
};

// ---------------------------------------------------------------------------

PlaybackController::PlaybackController(QObject* parent) : QObject(parent) {
    project_ = std::make_shared<Project>(makeDefaultProject());
    heard_ = project_;
    renderThread_ = new QThread(this);
    renderThread_->setObjectName("montage-render");
    worker_ = new RenderWorker;
    worker_->moveToThread(renderThread_);
    connect(renderThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &RenderWorker::rendered, this, &PlaybackController::frameRendered, Qt::QueuedConnection);
    connect(worker_, &RenderWorker::scopeRendered, this, &PlaybackController::scopeFrameRendered, Qt::QueuedConnection);
    qRegisterMetaType<HdrPicturePtr>("montage::HdrPicturePtr");
    connect(worker_, &RenderWorker::hdrRendered, this, &PlaybackController::hdrFrameRendered, Qt::QueuedConnection);
    renderThread_->start();
    device_ = new MixerDevice(this);
    device_->setAmbisonicBinaural(ambisonicBinaural_);
    scrubMixer_.setAmbisonicBinaural(ambisonicBinaural_);
    scrubMixer_.setOfflineProxies(true);
    connect(device_, &MixerDevice::levels, this, &PlaybackController::audioLevels);
    connect(device_, &MixerDevice::loudness, this, &PlaybackController::loudness);
    connect(device_, &MixerDevice::dialogueLoudness, this, &PlaybackController::dialogueLoudness);
    qRegisterMetaType<AudioAnalysisPtr>("montage::AudioAnalysisPtr");
    connect(device_, &MixerDevice::analysis, this, &PlaybackController::audioAnalysis);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(8);
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::tick);
}

PlaybackController::~PlaybackController() {
    worker_->setSinks({});
    stopAudio(true);
    if (talkSource_) talkSource_->stop();
    renderThread_->quit();
    renderThread_->wait();
}

void PlaybackController::setProject(const Project& p, Id sequenceId) {
    project_ = std::make_shared<const Project>(p);
    sequenceId_ = sequenceId;
    updateHeard();
    device_->setProject(heard_);
    for (const auto& o : {output_, review_})
        if (o) o->clearFrames();  // (rendered again as they are asked for)
    syncOutput();
}

namespace {

// Sound for an output: the mixer's as the sequence's own channels (or a stereo mix, `stereo`).
std::shared_ptr<OutputAudio> outputAudioFor(output::Scheduler& out, bool stereo) {
    auto audio = std::make_shared<OutputAudio>();
    audio->mixer.setNonBlocking(true);
    audio->mixer.setOfflineProxies(true);
    audio->mixer.setAmbisonicBinaural(false);  // (a monitor's speakers)
    audio->stereo = stereo;
    out.setAudioSource([audio](int64_t start, int frames, int channels, float* o) { audio->mix(start, frames, channels, o); });
    return audio;
}

}  // namespace

void PlaybackController::setReferenceOutput(std::shared_ptr<output::Scheduler> out) {
    output_ = std::move(out);
    outputAudio_ = output_ ? outputAudioFor(*output_, false) : nullptr;
    worker_->setSinks({output_, review_});
    syncOutput();
    requestFrame();
}

void PlaybackController::setReviewOutput(std::shared_ptr<output::Scheduler> out) {
    review_ = std::move(out);
    reviewAudio_ = review_ ? outputAudioFor(*review_, true) : nullptr;
    worker_->setSinks({output_, review_});
    syncOutput();
    requestFrame();
}

void PlaybackController::syncOutput() {
    for (const auto& [out, audio] : {std::pair{output_, outputAudio_}, std::pair{review_, reviewAudio_}}) {
        if (!out) continue;
        out->setPosition(position_, speed_);
        if (audio) {
            std::lock_guard lock(audio->m);
            audio->project = heard_;
            audio->sequence = sequenceId_;
            audio->muted = globalMute_;
        }
    }
}

double PlaybackController::renderScale() const {
    // While output is on: the mode's size playing (a UHD sequence on a 1080p mode at half); parked, the whole picture
    // or more, as the viewer and scopes show it.
    // (the reference output's size first: a review stream takes what it renders, fitted)
    const double out = output_ && output_->running() ? output_->renderScale() : review_ && review_->running() ? review_->renderScale() : 0;
    if (out > 0) return isPlaying() ? out : std::max(1.0, out);
    return isPlaying() ? scale_ : 1.0;
}

void PlaybackController::updateHeard() {
    if ((mutedTracks_.empty() && !liveMix_) || !project_ || !project_->findSequence(sequenceId_)) {
        heard_ = project_;
        return;
    }
    auto copy = std::make_shared<Project>(*project_);
    Sequence* s = copy->findSequence(sequenceId_);
    if (liveMix_) liveMix_(*s);
    for (int i : mutedTracks_)
        if (i >= 0 && i < int(s->audioTracks.size())) {
            s->audioTracks[size_t(i)].muted = true;  // (a soloed guide stays soloed, so nothing else comes up)
        }
    heard_ = std::move(copy);
}

void PlaybackController::setLiveMix(const std::function<void(Sequence&)>& apply) {
    if (!apply && !liveMix_) return;
    liveMix_ = apply;
    updateHeard();
    device_->setProject(heard_);
    syncOutput();
}

void PlaybackController::setMutedAudioTracks(const std::vector<int>& tracks) {
    if (tracks == mutedTracks_) return;
    mutedTracks_ = tracks;
    updateHeard();
    device_->setProject(heard_);
    syncOutput();
}

void PlaybackController::setAdrCycle(const std::optional<AdrCycle>& cycle) {
    cycle_ = cycle;
    device_->setCycle(cycle);
}

const Sequence* PlaybackController::sequence() const {
    return project_ ? project_->findSequence(sequenceId_) : nullptr;
}

void PlaybackController::setPreviewScale(double s) {
    scale_ = std::clamp(s, 0.05, 1.0);
    requestFrame();
}

void PlaybackController::setUseProxies(bool on) {
    useProxies_ = on;
    requestFrame();
}

void PlaybackController::setAmbisonicBinaural(bool on) {
    ambisonicBinaural_ = on;
    device_->setAmbisonicBinaural(on);
    scrubMixer_.setAmbisonicBinaural(on);  // scrubbing sounds as playback does
    testMonitor_.reset();
}

void PlaybackController::setShowCaptions(bool on) {
    showCaptions_ = on;
    requestFrame();
}

void PlaybackController::setHdrOutput(bool on) {
    if (hdrOutput_ == on) return;
    hdrOutput_ = on;
    requestFrame();
}

void PlaybackController::setHdrCalibration(std::shared_ptr<const Lut3D> lut, double sdrWhiteNits) {
    if (hdrCalibration_ == lut && hdrWhite_ == sdrWhiteNits) return;
    hdrCalibration_ = std::move(lut);
    hdrWhite_ = sdrWhiteNits;
    if (hdrOutput_) requestFrame();
}

void PlaybackController::setHdrHeadroom(double headroom) {
    if (hdrHeadroom_ == headroom) return;
    hdrHeadroom_ = headroom;
    if (hdrOutput_) requestFrame();
}

void PlaybackController::setGpuRendering(bool on) {
    if (gpu_ == on) return;
    gpu_ = on;
    requestFrame();
}

void PlaybackController::setTrimTarget(const std::string& id) {
    if (trimTarget_ == id) return;
    trimTarget_ = id;
    requestFrame();
}

void PlaybackController::setScopesReadWorking(bool on) {
    if (scopesWorking_ == on) return;
    scopesWorking_ = on;
    requestFrame();
}

void PlaybackController::setStereoView(StereoView v) {
    if (stereoView_ == v) return;
    stereoView_ = v;
    requestFrame();
}

int PlaybackController::playStep() const {
    if (speed_ == 0) return 0;
    int step = int(std::lround(speed_));
    return step == 0 ? (speed_ > 0 ? 1 : -1) : step;
}

FrameTime PlaybackController::clampToSequence(FrameTime t) const { return std::max<FrameTime>(0, t); }

void PlaybackController::requestFrame() {
    if (!sequence()) return;
    // Paused frames render at full quality; playback uses the preview scale (a reference output its own).
    worker_->request({project_, sequenceId_, position_, renderScale(), useProxies_, playStep(), showCaptions_, scale_, stereoView_, hdrOutput_, trimTarget_, gpu_, hdrCalibration_, hdrWhite_, hdrHeadroom_, scopesWorking_});
}

void PlaybackController::seek(FrameTime t) {
    t = clampToSequence(t);
    bool playing = isPlaying();
    if (t == position_ && !playing) return;
    position_ = t;
    if (playing) {
        startFrame_ = t;
        restartClock();
        if (speed_ == 1) startAudio(t);
    }
    requestFrame();
    if (!playing) scrubAudio(t);
    syncOutput();
    emit positionChanged(position_);
}

void PlaybackController::setGlobalMute(bool on) {
    globalMute_ = on;
    device_->setMuted(on);
    syncOutput();
}

std::vector<float> PlaybackController::heard(int64_t start, int frames) {
    std::vector<float> mix(size_t(std::max(0, frames)) * 2, 0.0f);
    const Sequence* s = sequence();
    if (!s || globalMute_ || frames <= 0) return mix;
    scrubMixer_.reset();
    scrubMixer_.setNonBlocking(false);
    scrubMixer_.mix(*heard_, *heard_->findSequence(sequenceId_), start, frames, mix.data());  // its tracks as heard
    return mix;
}

void PlaybackController::scrubAudio(FrameTime t) {
    const Sequence* s = sequence();
    if (!scrubbing_ || !s || s->audioTracks.empty() || globalMute_ || (monitoring_.on && monitoring_.mute)) return;
    if (!scrubSink_) {
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull()) {
            scrubbing_ = false;  // no output device: don't try again
            return;
        }
        QAudioFormat fmt;
        fmt.setSampleRate(s->sampleRate);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(fmt)) {
            scrubbing_ = false;
            return;
        }
        scrubSink_ = new QAudioSink(dev, fmt, this);
        scrubSink_->setBufferSize(fmt.bytesForDuration(150000));
        scrubIo_ = scrubSink_->start();
        scrubRate_ = s->sampleRate;
        if (!scrubIo_) return;
    }
    if (!scrubIo_ || scrubRate_ != s->sampleRate) return;
    // One frame of audio (at least 40 ms), faded at both ends to avoid clicks.
    int frames = std::max(int(s->sampleRate / s->fpsValue()), s->sampleRate / 25);
    if (scrubSink_->bytesFree() < frames * 4) return;  // still playing the previous snippet
    std::vector<float> mix(size_t(frames) * 2);
    int64_t start = int64_t(std::llround(double(t) * s->sampleRate / s->fpsValue()));
    scrubMixer_.reset();
    scrubMixer_.setNonBlocking(true);
    scrubMixer_.mix(*heard_, *heard_->findSequence(sequenceId_), start, frames, mix.data());  // its tracks as heard
    std::vector<int16_t> pcm(mix.size());
    const int fade = std::min(frames / 4, 240);
    // (The monitoring section's level and Dim, though not its speakers: scrubbing is heard in stereo.)
    const float level = monitoring_.on ? float(std::pow(10.0, (monitoring_.levelDb + (monitoring_.dim ? monitoring_.dimDb : 0.0)) / 20)) : 1.0f;
    for (int i = 0; i < frames; ++i) {
        float g = level;
        if (i < fade) g *= float(i) / fade;
        if (i >= frames - fade) g *= float(frames - 1 - i) / fade;
        for (int c = 0; c < 2; ++c)
            pcm[size_t(i) * 2 + size_t(c)] = int16_t(std::lround(std::clamp(mix[size_t(i) * 2 + size_t(c)] * g, -1.0f, 1.0f) * 32767));
    }
    scrubIo_->write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * sizeof(int16_t)));
}

void PlaybackController::step(int frames) {
    if (isPlaying()) pause();
    seek(position_ + frames);
}

void PlaybackController::play() {
    const Sequence* s = sequence();
    if (!s) return;
    if (speed_ == 1) return;
    FrameTime end = loop_ && s->outPoint >= 0 ? s->outPoint + 1 : s->duration();
    if (position_ >= end && end > 0) position_ = loop_ && s->inPoint >= 0 ? s->inPoint : 0;
    speed_ = 1;
    startFrame_ = position_;
    restartClock();
    startAudio(position_);
    timer_.start();
    syncOutput();
    emit playingChanged(true);
}

void PlaybackController::pause() {
    if (speed_ == 0) return;
    speed_ = 0;
    timer_.stop();
    stopAudio();
    requestFrame();  // re-render at full quality
    syncOutput();
    emit playingChanged(false);
}

void PlaybackController::togglePlay() {
    if (isPlaying()) pause();
    else play();
}

void PlaybackController::shuttle(int direction) {
    if (direction == 0) {
        pause();
        return;
    }
    double next;
    if (direction > 0) next = speed_ <= 0 ? 1 : std::min(speed_ * 2, 8.0);
    else next = speed_ >= 0 ? -1 : std::max(speed_ * 2, -8.0);
    bool wasPlaying = isPlaying();
    speed_ = next;
    startFrame_ = position_;
    restartClock();
    if (speed_ == 1) startAudio(position_);
    else stopAudio();
    timer_.start();
    syncOutput();
    if (!wasPlaying) emit playingChanged(true);
}

void PlaybackController::setSpeed(double speed) {
    if (!sequence()) return;
    if (speed == 0) {
        pause();
        return;
    }
    if (speed == speed_) return;
    const bool wasPlaying = isPlaying();
    speed_ = std::clamp(speed, -32.0, 32.0);
    startFrame_ = position_;
    restartClock();
    if (speed_ == 1) startAudio(position_);
    else stopAudio();
    timer_.start();
    syncOutput();
    if (!wasPlaying) emit playingChanged(true);
}

void PlaybackController::restartClock() {
    clock_.restart();
    lastElapsed_ = 0;
}

void PlaybackController::tick() {
    const Sequence* s = sequence();
    if (!s || speed_ == 0) return;
    double elapsed = clock_.nsecsElapsed() / 1e9;
    // While audio plays, follow what the device has actually played (processed
    // minus what is still buffered) so picture stays locked to sound. Backends
    // differ in how they report this, so only trust it when it is close to
    // the wall clock.
    if (speed_ == 1 && sink_ && sink_->state() == QAudio::ActiveState) {
        qint64 buffered = std::max<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
        double played = (sink_->processedUSecs() - sink_->format().durationForBytes(buffered)) / 1e6;
        if (played > 0 && std::fabs(played - elapsed) < 0.25) elapsed = played;
    }
    // (never back: going between the two clocks would step the picture back a frame or two)
    elapsed = lastElapsed_ = std::max(elapsed, lastElapsed_);
    FrameTime t = startFrame_ + FrameTime(std::floor(elapsed * s->fpsValue() * speed_));
    FrameTime end = s->duration();
    FrameTime loopIn = s->inPoint >= 0 ? s->inPoint : 0;
    FrameTime loopOut = s->outPoint >= 0 ? s->outPoint + 1 : end;
    syncAudioLoop();
    if (loop_ && speed_ > 0 && t >= loopOut && loopOut > loopIn) {
        // Round and round on the same clock (the sound wraps itself, sample-exactly), so passes keep their length; the
        // passes are taken off the start, so turning the loop off or moving its points goes on from where it is.
        const FrameTime passes = (t - loopIn) / (loopOut - loopIn);
        startFrame_ -= passes * (loopOut - loopIn);
        t -= passes * (loopOut - loopIn);
    } else if (loop_ && speed_ > 0 && t >= loopOut) {
        t = loopIn;
        startFrame_ = t;
        restartClock();
        if (speed_ == 1) startAudio(t);
    } else if (speed_ > 0 && t >= end) {
        t = end;
        position_ = t;
        emit positionChanged(t);
        pause();
        return;
    } else if (speed_ < 0 && t <= 0) {
        position_ = 0;
        emit positionChanged(0);
        pause();
        return;
    }
    if (t != position_) {
        position_ = t;
        worker_->request({project_, sequenceId_, t, renderScale(), useProxies_, playStep(), showCaptions_, scale_, stereoView_, hdrOutput_, trimTarget_, gpu_, hdrCalibration_, hdrWhite_, hdrHeadroom_, scopesWorking_});
        syncOutput();
        emit positionChanged(t);
    }
}

void PlaybackController::startAudio(FrameTime from, bool idle) {
    if (sink_) {
        sink_->stop();
        sink_->deleteLater();
        sink_ = nullptr;
    }
    const Sequence* s = sequence();
    if (!s) return;
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (monitoring_.on && !audioOutput_.isEmpty())  // (the section's device; without it, the system's as before)
        for (const QAudioDevice& d : QMediaDevices::audioOutputs())
            if (d.description() == audioOutput_) dev = d;
    if (dev.isNull()) return;
    // The channels the monitoring section needs, or two when the device cannot take them.
    const int wanted = monitoring_.on ? monitorOutputs(monitoring_) : 2;
    QAudioFormat fmt;
    fmt.setSampleRate(s->sampleRate);
    fmt.setChannelCount(wanted);
    fmt.setSampleFormat(QAudioFormat::Float);
    bool int16 = false;
    auto supported = [&] {
        fmt.setSampleFormat(QAudioFormat::Float);
        int16 = false;
        if (dev.isFormatSupported(fmt)) return true;
        fmt.setSampleFormat(QAudioFormat::Int16);
        int16 = true;
        return dev.isFormatSupported(fmt);
    };
    if (!supported()) {
        if (wanted == 2) return;
        fmt.setChannelCount(2);
        if (!supported()) return;
        qWarning("The audio output takes %d channels, not the %d the monitoring section needs: only the first two are heard", 2,
                 wanted);
    }
    outputChannels_ = fmt.channelCount();
    device_->setMonitoring(monitoring_, s->sampleRate);
    int64_t startSample = int64_t(std::llround(double(from) * s->sampleRate / s->fpsValue()));
    device_->configure(heard_, sequenceId_, startSample, int16, outputChannels_, idle);
    audioLoop_ = {-1, -1.0};  // (none a loop can be, so the device is always told)
    syncAudioLoop();
    device_->setCalibration(calibration_, monitoring_, s->sampleRate);
    device_->setTalkback(talkback_);
    if (!device_->isOpen()) device_->open(QIODevice::ReadOnly);
    sink_ = new QAudioSink(dev, fmt, this);
    sink_->setBufferSize(fmt.bytesForDuration(80000));  // ~80 ms
    sink_->start(device_);
}

void PlaybackController::syncAudioLoop() {
    const Sequence* s = sequence();
    if (!s || !device_) return;
    std::pair<int64_t, double> want{0, 0.0};
    const FrameTime in = s->inPoint >= 0 ? s->inPoint : 0, out = s->outPoint >= 0 ? s->outPoint + 1 : s->duration();
    if (loop_ && out > in) want = {std::llround(double(in) * s->sampleRate / s->fpsValue()), double(out - in) * s->sampleRate / s->fpsValue()};
    if (want == audioLoop_) return;
    audioLoop_ = want;
    device_->setLoop(want.first, want.second);
}

void PlaybackController::stopAudio(bool force) {
    if (!sink_) return;
    // Calibration noise and talkback go on while stopped.
    if (!force && monitorKeepsAudio()) {
        device_->setIdle(true);
        return;
    }
    sink_->stop();
    sink_->deleteLater();
    sink_ = nullptr;
}

RenderOptions PlaybackController::renderOptions() const {
    RenderOptions o;
    o.scale = scale_;
    o.useProxies = useProxies_;
    o.offlineProxies = true;
    o.captions = showCaptions_;
    o.stereoView = stereoView_;
    o.trimTarget = trimTarget_;
    o.displaySpace = "rec709";
    return o;
}

void PlaybackController::setMonitoring(const MonitorSettings& settings) {
    MonitorSettings m = settings;
    if (m.soloMode == MonitorSolo::Project) m.soloTracks.clear();  // (the mixer's solos are the project's again)
    if (m == monitoring_) return;
    const bool reopen = m.on != monitoring_.on || (m.on && monitorOutputs(m) != monitorOutputs(monitoring_));
    // Calibration stops when the speakers change (the same number may be another speaker now).
    if (!m.on || m.speakers != monitoring_.speakers) calibration_ = -1;
    if (!m.on) talkback_ = false;
    monitoring_ = m;
    const Sequence* s = sequence();
    const int rate = s ? s->sampleRate : 48000;
    device_->setMonitoring(m, rate);
    device_->setCalibration(calibration_, m, rate);
    device_->setTalkback(talkback_);
    testMonitor_.reset();
    refreshAudio(reopen);
    if (!talkback_ && talkSource_) {
        talkSource_->stop();
        talkSource_->deleteLater();
        talkSource_ = nullptr;
        talkIo_ = nullptr;
    }
    emit monitoringChanged();
}

void PlaybackController::refreshAudio(bool reopen) {
    if (speed_ == 1) {
        if (!sink_ || reopen) {
            // (the picture's clock afresh with the sound, as after a seek)
            startFrame_ = position_;
            restartClock();
            startAudio(position_);
        }
    } else if (monitorKeepsAudio()) {
        if (!sink_ || reopen) startAudio(position_, true);
        else device_->setIdle(true);
    } else {
        stopAudio(true);
    }
}

void PlaybackController::setAudioOutput(const QString& description) {
    if (description == audioOutput_) return;
    audioOutput_ = description;
    if (sink_ && monitoring_.on) refreshAudio(true);
    emit monitoringChanged();
}

void PlaybackController::setMonitorCalibration(int speaker) {
    const int n = int(monitorSpeakers(monitoring_.speakers).size());
    const int k = monitoring_.on && speaker >= 0 && speaker < n ? speaker : -1;
    if (k == calibration_) return;
    calibration_ = k;
    const Sequence* s = sequence();
    device_->setCalibration(k, monitoring_, s ? s->sampleRate : 48000);
    if (testMonitor_) testMonitor_->section().setCalibration(k);
    refreshAudio(false);
    emit monitoringChanged();
}

void PlaybackController::setTalkback(bool on) {
    on = on && monitoring_.on;
    if (on == talkback_) return;
    talkback_ = on;
    device_->setTalkback(on);
    if (testMonitor_) testMonitor_->section().setTalkback(on);
    if (on) startTalkbackInput();
    else if (talkSource_) {
        talkSource_->stop();
        talkSource_->deleteLater();
        talkSource_ = nullptr;
        talkIo_ = nullptr;
    }
    refreshAudio(false);
    emit monitoringChanged();
}

void PlaybackController::startTalkbackInput() {
    if (talkSource_) return;
    QAudioDevice dev = QMediaDevices::defaultAudioInput();
    if (!talkbackInput_.isEmpty())
        for (const QAudioDevice& d : QMediaDevices::audioInputs())
            if (d.description() == talkbackInput_) dev = d;
    if (dev.isNull()) return;
    const Sequence* s = sequence();
    QAudioFormat fmt;
    fmt.setSampleRate(s ? s->sampleRate : 48000);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Float);
    if (!dev.isFormatSupported(fmt)) return;  // (talkback needs the microphone at the sequence's rate)
    talkSource_ = new QAudioSource(dev, fmt, this);
    talkIo_ = talkSource_->start();
    if (!talkIo_) return;
    connect(talkIo_, &QIODevice::readyRead, this, [this] {
        if (!talkIo_) return;
        const QByteArray data = talkIo_->readAll();
        feedTalkback(reinterpret_cast<const float*>(data.constData()), int(data.size() / 4));
    });
}

void PlaybackController::feedTalkback(const float* mono, int frames) {
    if (frames <= 0) return;
    device_->feedTalkback(mono, frames);
    if (testMonitor_) testMonitor_->section().feedTalkback(mono, frames);
}

std::vector<float> PlaybackController::monitored(int64_t start, int frames, bool idle) {
    const Sequence* s = sequence();
    if (!monitoring_.on || !s || frames <= 0) return heard(start, std::max(0, frames));
    if (!testMonitor_) {
        testMonitor_ = std::make_unique<Monitor>();
        testMonitor_->setAmbisonicBinaural(ambisonicBinaural_);
        testMonitor_->setSettings(monitoring_, s->sampleRate);
        testMonitor_->section().setCalibration(calibration_);
        testMonitor_->section().setTalkback(talkback_);
    }
    testMonitor_->prepare(heard_, sequenceId_);
    testMonitor_->reset();  // (each call from silence: the delays and noise from their starts)
    std::vector<float> out(size_t(frames) * size_t(testMonitor_->outputs()));
    testMonitor_->render(start, frames, out.data(), nullptr, nullptr, cycle_ ? &*cycle_ : nullptr, !idle);
    if (globalMute_) std::fill(out.begin(), out.end(), 0.0f);
    return out;
}

void PlaybackController::resetLoudness() { device_->resetLoudness(); }

void PlaybackController::setSpectrumKWeighting(bool on) { device_->setSpectrumKWeighting(on); }

}  // namespace montage

#include "PlaybackController.moc"
