#include "DualSystem.h"

#include <cmath>
#include <cstdio>

#include "AudioSync.h"
#include "MediaPool.h"

namespace montage {

namespace {
constexpr int kSyncRate = 16000;  // enough for the loudness envelopes the match uses
constexpr double kMinConfidence = 0.25;
}  // namespace

SoundSync syncSound(const Project& p, Id video, Id sound, SyncBy by) {
    SoundSync out;
    if (by != SyncBy::Waveform) {
        double d = 0;
        if (timecodeOffset(p, video, sound, d)) {
            out.offset = d;
            out.found = out.byTimecode = true;
            out.confidence = 1;
            return out;
        }
        if (by == SyncBy::Timecode) return out;
    }
    const MediaItem* v = p.findMedia(video);
    const MediaItem* a = p.findMedia(sound);
    if (!v || !a || !v->hasAudio || !a->hasAudio || v->path.empty() || a->path.empty()) return out;
    AudioBufferPtr ref = MediaPool::instance().audio(v->path, kSyncRate);
    AudioBufferPtr other = MediaPool::instance().audio(a->path, kSyncRate);
    if (!ref || !other || ref->samples.empty() || other->samples.empty()) return out;
    const double lag = std::max(30.0, std::max(v->duration, a->duration));
    const SyncResult r = findAudioOffset(*ref, *other, lag);
    if (r.found && r.confidence >= kMinConfidence) {
        out.offset = r.offset;
        out.found = true;
        out.confidence = r.confidence;
    }
    return out;
}

std::vector<DailiesMatch> matchDailies(const Project& p, const std::vector<Id>& media, std::vector<std::string>* report) {
    std::vector<Id> videos, sounds;
    for (Id id : media)
        if (const MediaItem* m = p.findMedia(id)) {
            if (m->kind == MediaKind::Video && m->hasVideo) videos.push_back(id);
            else if (m->kind == MediaKind::Audio && m->hasAudio) sounds.push_back(id);
        }
    std::vector<DailiesMatch> out;
    for (Id v : videos) {
        DailiesMatch best{v, 0, {}};
        // Timecode first: the sound whose span overlaps the picture most.
        for (const SoundMatch& m : matchByTimecode(p, {v}, sounds)) {
            best.sound = m.sound;
            best.sync.offset = m.offset;
            best.sync.found = best.sync.byTimecode = true;
            best.sync.confidence = 1;
        }
        if (!best.sound)
            for (Id a : sounds) {
                const SoundSync s = syncSound(p, v, a, SyncBy::Waveform);
                if (s.found && s.confidence > best.sync.confidence) best.sound = a, best.sync = s;
            }
        if (best.sound) out.push_back(best);
        else if (report) report->push_back(p.findMedia(v)->name + ": no sound file matches it");
    }
    return out;
}

std::vector<Id> mergeDailies(Project& p, const std::vector<DailiesMatch>& matches, bool keepCameraAudio, std::vector<std::string>* report) {
    std::vector<Id> made;
    for (const DailiesMatch& m : matches) {
        const MediaItem* v = p.findMedia(m.video);
        const MediaItem* a = p.findMedia(m.sound);
        if (!v || !a) continue;
        const std::string name = v->name, soundName = a->name;
        MergeOptions o;
        o.keepCameraAudio = keepCameraAudio;
        std::string why;
        if (const Id id = mergeClips(p, m.video, {m.sound}, {m.sync.offset}, o, &why)) {
            made.push_back(id);
            if (report) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%+.3f s", m.sync.offset);
                report->push_back(name + " + " + soundName + " (" + (m.sync.byTimecode ? "timecode" : "waveform") + ", " + buf + ")");
            }
        } else if (report) {
            report->push_back(name + ": " + why);
        }
    }
    return made;
}

std::vector<Id> syncDailies(Project& p, const std::vector<Id>& media, bool keepCameraAudio, std::vector<std::string>* report) {
    return mergeDailies(p, matchDailies(p, media, report), keepCameraAudio, report);
}

}  // namespace montage
