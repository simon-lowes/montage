#include "PitchCurves.h"

#include <chrono>
#include <thread>

#include <QCoreApplication>
#include <QPointer>

#include "audio/DialogueAlign.h"
#include "audio/SpeechCleanup.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr int kRate = 48000;

}  // namespace

// What the pitch is tracked on: the clip's sound, through its source effects up to its pitch edits.
struct PitchCurves::Job {
    std::string sound;
    std::deque<Effect> effects;
    double from = 0, to = 0;  // source seconds
};

namespace {

using Job = PitchCurves::Job;

bool jobFor(const Project& p, const Sequence& s, const Clip& c, Job& job) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio || m->kind == MediaKind::Sequence) return false;
    job.sound = audioKey(m->path, c.channels);
    std::vector<const Effect*> fx;
    for (const Effect& e : c.effects) {
        if (e.type == "pitch_contour") break;
        if (e.enabled && isSourceAudioEffect(e.type) && dialogueAlignmentApplies(e, c) && bandwidthExtensionApplies(e, c)) fx.push_back(&e);
    }
    std::deque<Effect> resolved;
    resolveElasticMembers(p, fx, resolved);
    for (const Effect* e : fx) job.effects.push_back(*e);
    const double fps = s.fpsValue();
    job.from = c.sourceIn / fps;
    job.to = (c.sourceIn + c.sourceExtent()) / fps;
    return job.to - job.from <= PitchCurves::kLongest;
}

std::shared_ptr<const PitchTrack> run(const Job& job, const std::atomic<bool>* cancel = nullptr) {
    AudioBufferPtr sound = MediaPool::instance().audio(job.sound, kRate);
    if (!sound) return std::make_shared<PitchTrack>();
    std::vector<const Effect*> fx;
    for (const Effect& e : job.effects) fx.push_back(&e);
    if (!fx.empty()) {
        // (the cleanup runs on its own and is waited for here a little at a time, so closing the timeline need not wait
        // for it; without `cancel`, it is waited for whole)
        AudioBufferPtr played;
        if (!cancel) played = cleanedAudio(job.sound, sound, fx, true);
        while (cancel && !(played = cleanedAudio(job.sound, sound, fx, false, true))) {
            if (cancel->load()) return std::make_shared<PitchTrack>();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (played) sound = played;
    }
    return std::make_shared<PitchTrack>(trackPitch(*sound, job.from, job.to, 50, 800, cancel));
}

}  // namespace

PitchCurves::PitchCurves(QObject* parent) : QObject(parent) {}

PitchCurves::~PitchCurves() {
    cancel_->store(true);
    if (worker_.joinable()) worker_.join();
}

std::string PitchCurves::keyOf(const Project& p, const Sequence& s, const Clip& c) const {
    Job job;
    if (!jobFor(p, s, c, job)) return {};
    std::string key = job.sound + "|" + std::to_string(job.from) + "|" + std::to_string(job.to);
    for (const Effect& e : job.effects) {
        key += "|" + e.type;
        for (const auto& [name, prm] : e.params) key += ":" + name + "=" + std::to_string(prm.at(0));
        for (const auto& [name, text] : e.strings) key += ":" + name + "=" + text;
    }
    return key;
}

std::shared_ptr<const PitchCurves::Curve> PitchCurves::finish(const std::string& key, const std::shared_ptr<const PitchTrack>& track,
                                                              const Clip& c) {
    const Effect* pitch = pitchContourOf(c);
    const std::string edits = pitch && pitch->enabled ? pitch->s("edits") : std::string();
    const std::string full = key + "#" + edits;
    if (auto it = curves_.find(full); it != curves_.end()) return it->second;
    auto curve = std::make_shared<Curve>();
    curve->track = *track;
    curve->edited = editedPitch(*track, pitchEditsFromString(edits));
    if (curves_.size() > 64) curves_.clear();
    curves_[full] = curve;
    return curve;
}

std::shared_ptr<const PitchCurves::Curve> PitchCurves::curve(const Project& p, const Sequence& s, const Clip& c, bool start) {
    const std::string key = keyOf(p, s, c);
    if (key.empty()) return nullptr;
    if (auto it = tracks_.find(key); it != tracks_.end()) return finish(key, it->second, c);
    if (!start || key == running_) return nullptr;
    auto job = std::make_shared<Job>();
    jobFor(p, s, c, *job);
    // Asked again: to the front of the queue (its back); a few kept, the oldest dropped.
    std::erase_if(pending_, [&](const auto& q) { return q.first == key; });
    pending_.emplace_back(key, job);
    while (pending_.size() > 8) pending_.pop_front();
    startNext();
    return nullptr;
}

void PitchCurves::startNext() {
    if (!running_.empty() || pending_.empty()) return;
    if (worker_.joinable()) worker_.join();  // finished: it has posted its result
    auto [key, job] = pending_.back();
    pending_.pop_back();
    running_ = key;
    QPointer<PitchCurves> self(this);
    worker_ = std::thread([self, key, job, cancel = cancel_] {
        auto track = run(*job, cancel.get());
        if (cancel->load()) return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, key, track] {
            if (!self) return;
            self->running_.clear();
            if (self->tracks_.size() > 32) self->tracks_.clear();
            self->tracks_[key] = track;
            emit self->ready();
            self->startNext();
        }, Qt::QueuedConnection);
    });
}

std::shared_ptr<const PitchCurves::Curve> PitchCurves::curveNow(const Project& p, const Sequence& s, const Clip& c) {
    const std::string key = keyOf(p, s, c);
    if (key.empty()) return nullptr;
    auto it = tracks_.find(key);
    if (it == tracks_.end()) {
        Job job;
        jobFor(p, s, c, job);
        it = tracks_.emplace(key, run(job)).first;
    }
    return finish(key, it->second, c);
}

}  // namespace montage
