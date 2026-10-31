#include "ImmersiveTrackFile.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "DcpMxf.h"
#include "Iab.h"

namespace montage {

namespace {

bool openTrack(const std::string& mxf, dcp::ImfTrackReader& r, ImmersiveTrackInfo& info, std::string* error) {
    if (!r.open(mxf, error, false, true)) return false;
    using K = dcp::ImfTrackReader::Kind;
    const dcp::EditRate rate = r.rate();
    // (rates a file says, kept to ones that make sense: a frame or a sample at a time, 8 to 768 kHz)
    if (rate.num == 0 || rate.den == 0 || rate.value() < 1 || rate.value() > 768000 || r.sampleRate() < 8000 || r.sampleRate() > 768000) {
        if (error) *error = mxf + " has no rate Montage reads";
        return false;
    }
    auto unitsToSamples = [&](int64_t units) {
        const double samples = double(units) * double(r.sampleRate()) * double(rate.den) / double(rate.num);
        return int64_t(std::llround(std::min(samples, 1e13)));
    };
    if (r.kind() == K::Sound && r.adm()) {
        info.kind = ImmersiveTrackInfo::Kind::Adm;
        info.channels = r.channels();
        info.sampleRate = r.sampleRate();
        info.samples = r.samples();
    } else if (r.kind() == K::Mga) {
        info.kind = ImmersiveTrackInfo::Kind::Mga;
        info.channels = r.channels();
        info.sampleRate = r.sampleRate();
        info.samples = unitsToSamples(r.frames());
    } else if (r.kind() == K::Iab) {
        info.kind = ImmersiveTrackInfo::Kind::Iab;
        info.sampleRate = 48000;
        info.samples = unitsToSamples(r.frames());
        std::vector<uint8_t> bytes;
        iab::Frame f;
        if (r.frames() == 0 || !r.frame(0, bytes, error) || !iab::readFrame(bytes.data(), bytes.size(), f, error)) return false;
        info.channels = int(f.bed.size() + f.objects.size());
    } else {
        if (error) *error = mxf + " is not immersive sound";
        return false;
    }
    if (info.channels <= 0) {
        if (error) *error = mxf + " has no channels";
        return false;
    }
    return true;
}

// Where an IAB frame's waveforms go: each bed channel by its ChannelID and each object by its MetaID, as the first frame
// has them.
struct IabLayout {
    std::map<int, int> bed;
    std::map<uint32_t, int> objects;
};
IabLayout iabLayout(const iab::Frame& f) {
    IabLayout l;
    for (size_t i = 0; i < f.bed.size(); ++i) l.bed.emplace(f.bed[i].id, int(i));
    for (size_t k = 0; k < f.objects.size(); ++k) l.objects.emplace(f.objects[k].metaId, int(f.bed.size() + k));
    return l;
}

// A cinema bed's channels as BS.2051 names its speakers.
const char* iabSpeaker(int id) {
    switch (id) {
    case iab::L: return "M+030";
    case iab::R: return "M-030";
    case iab::C: return "M+000";
    case iab::LFE: return "LFE1";
    case iab::Lss: return "M+090";
    case iab::Rss: return "M-090";
    case iab::Lrs: return "M+135";
    case iab::Rrs: return "M-135";
    case iab::Ls: return "M+110";
    case iab::Rs: return "M-110";
    case iab::Lts: return "U+090";
    case iab::Rts: return "U-090";
    default: return "";
    }
}

}  // namespace

bool immersiveTrackInfo(const std::string& mxf, ImmersiveTrackInfo& info, std::string* error) {
    dcp::ImfTrackReader r;
    info = ImmersiveTrackInfo{};
    return openTrack(mxf, r, info, error);
}

bool immersiveTrackScene(const std::string& mxf, AdmScene& out, std::string* error, std::vector<std::string>* warnings) {
    dcp::ImfTrackReader r;
    ImmersiveTrackInfo info;
    if (!openTrack(mxf, r, info, error)) return false;
    AdmScene sc;
    sc.rate = info.sampleRate;
    sc.channels = info.channels;
    if (info.kind == ImmersiveTrackInfo::Kind::Adm) {
        std::string xml;
        std::vector<AdmTrack> tracks;
        for (const dcp::AdmChannelMapping& m : r.admChannels()) if (m.channel >= 1 && m.channel <= 4096) tracks.push_back({int(m.channel) - 1, m.uid, m.trackFormat, m.pack});
        if (!r.admXml(xml, error) || !parseAdm(xml, sc, tracks, error, warnings)) return false;
    } else if (info.kind == ImmersiveTrackInfo::Kind::Mga) {
        // Frame by frame, each frame's blocks after the last.
        std::string sadm;
        for (int64_t k = 0; k < r.frames(); ++k) {
            if (!r.mgaFrame(k, nullptr, &sadm, error)) return false;
            if (sadm.empty()) continue;
            if (!parseAdm(sadm, sc, {}, error, k == 0 ? warnings : nullptr)) return false;
        }
    } else {
        // IAB: the bed's channels by their speakers, each object where its pan sub-blocks put it.
        std::vector<uint8_t> bytes;
        iab::Frame f;
        IabLayout layout;
        size_t bedN = 0;  // the first frame's, which the layout numbers its objects after
        int64_t at = 0;
        for (int64_t k = 0; k < r.frames(); ++k) {
            if (!r.frame(k, bytes, error) || !iab::readFrame(bytes.data(), bytes.size(), f, error)) return false;
            if (k == 0) {
                layout = iabLayout(f);
                bedN = f.bed.size();
                std::map<std::string, int> speakers;
                for (const iab::BedChannel& c : f.bed)
                    if (*iabSpeaker(c.id)) speakers[iabSpeaker(c.id)] = layout.bed[c.id];
                if (!speakers.empty()) admSetBed(sc, speakers, warnings);
                for (size_t o = 0; o < f.objects.size(); ++o)
                    sc.objects.push_back({"Object " + std::to_string(o + 1), int(f.bed.size() + o), {}});
            }
            const int n = iab::frameSamples(f.rate, f.frameIndex);
            for (const iab::Object& o : f.objects) {
                const auto it = layout.objects.find(o.metaId);
                if (it == layout.objects.end() || size_t(it->second) < bedN || size_t(it->second) - bedN >= sc.objects.size()) continue;
                std::vector<AdmBlock>& blocks = sc.objects[size_t(it->second) - bedN].blocks;
                for (size_t sb = 0; sb < o.pan.size(); ++sb) {
                    const int64_t t = at + iab::panSubBlockStart(f.rate, f.frameIndex, int(sb));
                    const AdmPoint p{o.pan[sb].x * 2 - 1, 1 - o.pan[sb].y * 2, o.pan[sb].z};
                    // (gliding to each sub-block's point from the one before; the first there from the start)
                    if (blocks.empty()) blocks.push_back({0, 0, p, true});
                    admAppendBlock(blocks, {blocks.back().to, t, p, false});
                }
            }
            at += n;
        }
        for (AdmScene::Object& o : sc.objects)
            if (!o.blocks.empty()) admAppendBlock(o.blocks, {o.blocks.back().to, at, o.blocks.back().at, false});
        sc.samples = at;
    }
    // Speakers and objects on channels the file doesn't have (a track ID past its last) would be silent clips.
    if (info.kind != ImmersiveTrackInfo::Kind::Iab && sc.channels > info.channels) {
        bool dropped = false;
        for (int& c : sc.bed)
            if (c >= info.channels) c = -1, dropped = true;
        const size_t before = sc.objects.size();
        std::erase_if(sc.objects, [&](const AdmScene::Object& o) { return o.channel >= info.channels; });
        dropped = dropped || sc.objects.size() != before;
        if (dropped && warnings)
            warnings->push_back("The ADM puts sound on channels past the file's " + std::to_string(info.channels) + ": those are not read");
        sc.channels = info.channels;
        if (std::none_of(sc.bed.begin(), sc.bed.end(), [](int c) { return c >= 0; }) && sc.objects.empty()) {
            if (error) *error = "The ADM's speakers and objects are all on channels the file doesn't have";
            return false;
        }
    }
    sc.samples = std::max(sc.samples, info.samples);
    out = std::move(sc);
    return true;
}

bool readImmersiveTrack(const std::string& mxf, int64_t first, const std::function<bool(const int32_t*, size_t)>& sink, std::string* error) {
    dcp::ImfTrackReader r;
    ImmersiveTrackInfo info;
    if (!openTrack(mxf, r, info, error)) return false;
    const size_t channels = size_t(info.channels);
    std::vector<int32_t> pcm;
    if (info.kind == ImmersiveTrackInfo::Kind::Adm) {
        for (int64_t at = std::max<int64_t>(0, first); at < info.samples;) {
            const int64_t n = std::min<int64_t>(48000, info.samples - at);
            if (!r.pcm(at, n, pcm, error)) return false;
            if (!sink(pcm.data(), size_t(n))) return true;
            at += n;
        }
        return true;
    }
    // Frame by frame, from the one holding `first`.
    std::vector<uint8_t> bytes;
    iab::Frame f;
    IabLayout layout;
    int64_t at = 0;
    for (int64_t k = 0; k < r.frames(); ++k) {
        size_t n = 0;
        if (info.kind == ImmersiveTrackInfo::Kind::Mga) {
            if (!r.mgaFrame(k, &pcm, nullptr, error)) return false;
            n = pcm.size() / channels;
            // (a frame without sound is a frame's worth of silence at the edit rate, keeping the rest in time)
            if (n == 0) {
                const dcp::EditRate er = r.rate();
                const auto sampleAt = [&](int64_t f) {
                    return er.num ? int64_t(std::llround(double(f) * info.sampleRate * er.den / er.num)) : 0;
                };
                pcm.assign((n = size_t(std::max<int64_t>(0, sampleAt(k + 1) - sampleAt(k)))) * channels, 0);
            }
        } else {
            if (!r.frame(k, bytes, error) || !iab::readFrame(bytes.data(), bytes.size(), f, error)) return false;
            if (k == 0) layout = iabLayout(f);
            n = size_t(iab::frameSamples(f.rate, f.frameIndex));
            pcm.assign(n * channels, 0);
            auto put = [&](int channel, uint32_t audio) {
                const auto w = f.audio.find(audio);
                if (audio == 0 || w == f.audio.end()) return;
                for (size_t i = 0; i < std::min(n, w->second.size()); ++i) pcm[i * channels + size_t(channel)] = w->second[i];
            };
            for (const iab::BedChannel& c : f.bed)
                if (const auto it = layout.bed.find(c.id); it != layout.bed.end()) put(it->second, c.audio);
            for (const iab::Object& o : f.objects)
                if (const auto it = layout.objects.find(o.metaId); it != layout.objects.end()) put(it->second, o.audio);
        }
        const int64_t end = at + int64_t(n);
        if (end > first) {
            const size_t skip = size_t(std::max<int64_t>(0, first - at));
            if (!sink(pcm.data() + skip * channels, n - skip)) return true;
        }
        at = end;
    }
    return true;
}

}  // namespace montage
