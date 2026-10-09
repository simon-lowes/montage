#include "core/AudioChannels.h"

#include <algorithm>
#include <numeric>
#include <set>

namespace montage {

int sourceChannelCount(const MediaItem& m) {
    if (!m.hasAudio || m.kind == MediaKind::Image || m.kind == MediaKind::Sequence) return 0;
    if (!m.audioStreams.empty()) return std::accumulate(m.audioStreams.begin(), m.audioStreams.end(), 0);
    return std::max(1, m.channels);
}

std::vector<std::string> sourceChannelNames(const MediaItem& m) {
    const int n = sourceChannelCount(m);
    std::vector<std::string> names;
    // A field recorder's track names (media/FieldRecorder: "Boom, Lav 1, Track 3").
    if (auto it = m.metadata.find("tracks"); it != m.metadata.end()) {
        size_t from = 0;
        const std::string& list = it->second;
        while (from <= list.size() && int(names.size()) < n) {
            size_t comma = list.find(',', from);
            if (comma == std::string::npos) comma = list.size();
            std::string name = list.substr(from, comma - from);
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            names.push_back(name);
            from = comma + 1;
        }
    }
    names.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        if (names[size_t(i)].empty()) names[size_t(i)] = "Channel " + std::to_string(i + 1);
    return names;
}

std::string channelsLabel(const MediaItem& m, const std::vector<int>& channels) {
    if (channels.empty()) return {};
    const std::vector<std::string> names = sourceChannelNames(m);
    if (channels.size() == 1) {
        const int ch = channels[0];
        if (ch >= 0 && ch < int(names.size()) && names[size_t(ch)] != "Channel " + std::to_string(ch + 1)) return names[size_t(ch)];
        return "Ch " + std::to_string(ch + 1);
    }
    std::string label = "Ch ";
    for (size_t i = 0; i < channels.size(); ++i) label += (i ? "+" : "") + std::to_string(channels[i] + 1);
    return label;
}

namespace edit {

namespace {

// The sound clips a clip stands for: itself on an audio track, else the audio clips linked to it.
std::vector<Id> soundClips(const Sequence& s, Id clip) {
    const auto loc = locate(s, clip);
    if (!loc) return {};
    if (loc->track.kind == TrackKind::Audio) return {clip};
    std::vector<Id> out;
    for (Id id : linkedClips(s, clip))
        if (const auto l = locate(s, id); l && l->track.kind == TrackKind::Audio) out.push_back(id);
    return out;
}

// The clip's name without the channel label an earlier split gave it.
std::string baseName(const Clip& c, const MediaItem& m) {
    const std::string label = channelsLabel(m, c.channels);
    const std::string tail = " - " + label;
    if (!label.empty() && c.name.size() > tail.size() && c.name.compare(c.name.size() - tail.size(), tail.size(), tail) == 0)
        return c.name.substr(0, c.name.size() - tail.size());
    return c.name.empty() ? m.name : c.name;
}

}  // namespace

Result setClipChannels(Project& p, Sequence& s, Id clip, const std::vector<int>& channels) {
    if (!locate(s, clip)) return Result::fail("No such clip");
    const std::vector<Id> targets = soundClips(s, clip);
    if (targets.empty()) return Result::fail("The clip has no sound");
    for (Id id : targets) {
        const Clip* c = clipById(s, id);
        const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
        const int n = m ? sourceChannelCount(*m) : 0;
        if (n == 0) return Result::fail("Only clips of sound files can choose their channels");
        std::set<int> seen;
        for (int ch : channels) {
            if (ch < 0 || ch >= n) return Result::fail(m->name + " has " + std::to_string(n) + (n == 1 ? " channel" : " channels"));
            if (!seen.insert(ch).second) return Result::fail("Channel " + std::to_string(ch + 1) + " is chosen twice");
        }
    }
    bool changed = false;
    for (Id id : targets) {
        Clip* c = clipById(s, id);
        if (c->channels == channels) continue;
        const MediaItem* m = p.findMedia(c->mediaId);
        // A clip named for its channel by a split follows the new ones.
        const std::string before = channelsLabel(*m, c->channels);
        if (!before.empty() && c->name == baseName(*c, *m) + " - " + before) {
            const std::string base = baseName(*c, *m), after = channelsLabel(*m, channels);
            c->name = after.empty() ? base : base + " - " + after;
        }
        c->channels = channels;
        changed = true;
    }
    return changed ? Result{} : Result::fail("");
}

Result splitAudioChannels(Project& p, Sequence& s, Id clip, bool pairs) {
    if (!locate(s, clip)) return Result::fail("No such clip");
    const std::vector<Id> sounds = soundClips(s, clip);
    if (sounds.empty()) return Result::fail("The clip has no sound");
    const Id soundId = sounds.front();
    const auto loc = locate(s, soundId);
    Clip base = *clipById(s, soundId);
    const MediaItem* m = base.mediaId ? p.findMedia(base.mediaId) : nullptr;
    const int n = m ? sourceChannelCount(*m) : 0;
    if (n == 0) return Result::fail("Only clips of sound files can be split into channels");
    std::vector<int> playing = base.channels;
    if (playing.empty()) {
        playing.resize(size_t(n));
        std::iota(playing.begin(), playing.end(), 0);
    }
    std::vector<std::vector<int>> groups;
    for (size_t i = 0; i < playing.size(); i += pairs ? 2 : 1) {
        std::vector<int> g{playing[i]};
        if (pairs && i + 1 < playing.size()) g.push_back(playing[i + 1]);
        groups.push_back(g);
    }
    if (groups.size() < 2) return Result::fail(pairs ? "The clip has no more than two channels" : "The clip plays one channel");
    const std::string name = baseName(base, *m);
    // All of them act as one with the clip's picture.
    Id group = base.linkGroup;
    if (!group) group = p.newId();
    {
        Clip* c = clipById(s, soundId);
        c->linkGroup = group;
        c->channels = groups[0];
        c->name = name + " - " + channelsLabel(*m, groups[0]);
    }
    Result res;
    int below = loc->track.index;
    for (size_t k = 1; k < groups.size(); ++k) {
        // The first audio track below the last one used with room for the clip, or a new one.
        int track = -1;
        for (int t = below + 1; t < int(s.audioTracks.size()) && track < 0; ++t) {
            const Track& tr = s.audioTracks[size_t(t)];
            if (!tr.locked && trackEmpty(tr, base.start, base.end())) track = t;
        }
        if (track < 0) track = addTrack(p, s, TrackKind::Audio).index;
        Clip copy = base;
        copy.id = p.newId();
        copy.linkGroup = group;
        copy.channels = groups[k];
        copy.name = name + " - " + channelsLabel(*m, groups[k]);
        const Result r = overwrite(p, s, {TrackKind::Audio, track}, copy);
        if (!r.ok) return r;
        res.created.push_back(copy.id);
        below = track;
    }
    return res;
}

}  // namespace edit

}  // namespace montage
