#include "MusicTags.h"

#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <cmath>
#include <initializer_list>

extern "C" {
#include <libavformat/avformat.h>
}

#include "media/Decoder.h"
#include "media/FieldRecorder.h"
#include "media/SoundLibrary.h"
#include "media/StemSeparation.h"

namespace montage {

namespace {

// Every tag of the container and its streams, by lower-case key (the container's first).
std::map<std::string, std::string> fileTags(const std::string& path) {
    std::map<std::string, std::string> tags;
    AVFormatContext* fmt = nullptr;
    // (The headers hold the tags: no need to look into the streams.)
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return tags;
    std::vector<const AVDictionary*> dicts{fmt->metadata};
    for (unsigned i = 0; i < fmt->nb_streams; ++i) dicts.push_back(fmt->streams[i]->metadata);
    for (const AVDictionary* m : dicts) {
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_get(m, "", e, AV_DICT_IGNORE_SUFFIX))) {
            const std::string key = QString::fromUtf8(e->key).toLower().toStdString();
            const std::string value = QString::fromUtf8(e->value).trimmed().toStdString();
            if (!value.empty() && !tags.count(key)) tags[key] = value;
        }
    }
    avformat_close_input(&fmt);
    return tags;
}

}  // namespace

std::map<std::string, std::string> musicTags(const std::string& path) {
    const std::map<std::string, std::string> tags = fileTags(path);
    FieldRecording f;
    readFieldRecording(path, f);
    auto first = [&](std::initializer_list<const char*> tagKeys, std::initializer_list<const char*> userKeys) {
        for (const char* k : userKeys)
            if (auto it = f.user.find(k); it != f.user.end() && !it->second.empty()) return it->second;
        for (const char* k : tagKeys)
            if (auto it = tags.find(k); it != tags.end() && !it->second.empty()) return it->second;
        return std::string();
    };
    std::map<std::string, std::string> out;
    auto put = [&](const char* field, const std::string& v) {
        if (!v.empty()) out[field] = v;
    };
    put("title", first({"title", "tit2"}, {"SONGTITLE", "TRACKTITLE", "TITLE"}));
    put("composer", first({"composer", "tcom", "writer", "songwriter"}, {"COMPOSER", "SONGWRITER"}));
    put("publisher", first({"publisher", "tpub", "organization", "label"}, {"MUSICPUBLISHER", "PUBLISHER"}));
    put("society", first({"society", "pro", "performing_rights_organization"}, {"PRO", "SOCIETY", "PERFORMINGRIGHTSORG"}));
    put("library", first({"library"}, {"LIBRARY", "LIBRARYNAME"}));
    put("catalogue", first({"catalognumber", "catalogue", "catalog", "catalogue_number"}, {"CATALOGUENUMBER", "CATALOGNUMBER", "CATALOGUE", "CATALOG", "CATALOGID"}));
    put("isrc", first({"isrc", "tsrc"}, {"ISRC", "ISRCID"}));
    put("iswc", first({"iswc"}, {"ISWC", "ISWCID"}));
    return out;
}

std::map<std::string, std::string> musicTagsFor(const MediaItem& m) {
    std::map<std::string, std::string> out = musicTags(m.path);
    if (auto lib = SoundLibrary::shared()) {
        if (auto e = lib->entryForPath(m.path)) {
            if (!out.count("title") && !e->fxName.empty()) out["title"] = e->fxName;
            if (!out.count("library") && !e->sourceId.empty()) out["library"] = e->sourceId;
        }
    }
    return out;
}

bool cueIsVocal(const Project& p, const Sequence& seq, const MusicCue& cue, bool& vocal, double* vocalShare, double seconds,
                std::string* error) {
    const MediaItem* m = p.findMedia(cue.mediaId);
    if (!m) {
        if (error) *error = "The cue's media is not in the project";
        return false;
    }
    // The first clip of the cue: the media it plays.
    const Clip* clip = nullptr;
    for (const Track& t : seq.audioTracks)
        for (const Clip& c : t.clips)
            if (c.mediaId == cue.mediaId && c.start >= cue.in && c.start < cue.out && (!clip || c.start < clip->start)) clip = &c;
    if (!clip) {
        if (error) *error = "The cue has no clip";
        return false;
    }
    const double fps = seq.fpsValue();
    const double a = clip->sourceFrameAt(clip->start) / fps, b = clip->sourceFrameAt(clip->end() - 1) / fps;
    const double lo = std::min(a, b), hi = std::max(a, b);
    const double mid = (lo + hi) / 2, half = std::min(seconds, hi - lo) / 2;
    // The music model at hand (vocals come first in each): two stems, else four or five.
    int stems = 2;
    for (int n : {2, 4, 5})
        if (stemModel(n).installed()) {
            stems = n;
            break;
        }
    const int rate = stemRate(stems);
    AudioBufferPtr sound = decodeAudio(m->path, rate, error, nullptr, clip->channels);
    if (!sound) return false;
    const int64_t from = std::clamp<int64_t>(int64_t((mid - half) * rate), 0, sound->frames());
    const int64_t to = std::clamp<int64_t>(int64_t((mid + half) * rate), from, sound->frames());
    if (to - from < rate / 2) {
        if (error) *error = "The cue is too short to tell";
        return false;
    }
    double vocals = 0, rest = 0;
    auto splitter = StemSplitter::create(stems, [&](const std::vector<std::vector<float>>& out) {
        for (size_t s = 0; s < out.size(); ++s)
            for (float v : out[s]) (s == 0 ? vocals : rest) += double(v) * v;
        return true;
    }, error);
    if (!splitter) return false;
    // (Stereo to the model: an ambisonic field's omni W on both sides.)
    std::vector<float> stereo;
    const float* in = sound->samples.data() + size_t(from) * 2;
    if (sound->channels != 2) {
        const size_t ch = size_t(std::max(1, sound->channels));
        stereo.resize(size_t(to - from) * 2);
        for (size_t i = 0; i < size_t(to - from); ++i) stereo[i * 2] = stereo[i * 2 + 1] = sound->samples[(size_t(from) + i) * ch];
        in = stereo.data();
    }
    if (!splitter->push(in, size_t(to - from), error) || !splitter->finish(error)) return false;
    const double share = vocals + rest > 0 ? vocals / (vocals + rest) : 0;
    if (vocalShare) *vocalShare = share;
    vocal = share > 0.1;
    return true;
}

}  // namespace montage
