#include "AafExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

#include "core/Aaf.h"
#include "core/Interpretation.h"
#include "core/EditOps.h"
#include "media/Decoder.h"
#include "media/ImageSequence.h"
#include "media/Psd.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr int kRate = 48000;

// Classes (AUIDs from the AAF object model).
const char* const kHeader = "0d010101-0101-2f00-060e-2b3402060101";
const char* const kContentStorage = "0d010101-0101-1800-060e-2b3402060101";
const char* const kDictionary = "0d010101-0101-2200-060e-2b3402060101";
const char* const kIdentification = "0d010101-0101-3000-060e-2b3402060101";
const char* const kCompositionMob = "0d010101-0101-3500-060e-2b3402060101";
const char* const kMasterMob = "0d010101-0101-3600-060e-2b3402060101";
const char* const kSourceMob = "0d010101-0101-3700-060e-2b3402060101";
const char* const kTimelineMobSlot = "0d010101-0101-3b00-060e-2b3402060101";
const char* const kSequence = "0d010101-0101-0f00-060e-2b3402060101";
const char* const kSourceClip = "0d010101-0101-1100-060e-2b3402060101";
const char* const kFiller = "0d010101-0101-0900-060e-2b3402060101";
const char* const kTimecode = "0d010101-0101-1400-060e-2b3402060101";
const char* const kTransition = "0d010101-0101-1700-060e-2b3402060101";
const char* const kOperationGroup = "0d010101-0101-0a00-060e-2b3402060101";
const char* const kConstantValue = "0d010101-0101-3d00-060e-2b3402060101";
const char* const kVaryingValue = "0d010101-0101-3e00-060e-2b3402060101";
const char* const kControlPoint = "0d010101-0101-1900-060e-2b3402060101";
const char* const kWaveDescriptor = "0d010101-0101-2c00-060e-2b3402060101";
const char* const kCdciDescriptor = "0d010101-0101-2800-060e-2b3402060101";
const char* const kImportDescriptor = "0d010101-0101-4a00-060e-2b3402060101";
const char* const kNetworkLocator = "0d010101-0101-3200-060e-2b3402060101";
const char* const kDataDefinition = "0d010101-0101-1b00-060e-2b3402060101";
const char* const kContainerDefinition = "0d010101-0101-2000-060e-2b3402060101";
const char* const kOperationDefinition = "0d010101-0101-1c00-060e-2b3402060101";
const char* const kParameterDefinition = "0d010101-0101-1d00-060e-2b3402060101";
const char* const kInterpolationDefinition = "0d010101-0101-2100-060e-2b3402060101";

// Definitions.
const char* const kSound = "01030202-0200-0000-060e-2b3404010101";
const char* const kPicture = "01030202-0100-0000-060e-2b3404010101";
const char* const kTimecodeData = "01030201-0100-0000-060e-2b3404010101";
const char* const kContainerAAF = "4313b571-d8ba-11d2-809b-006008143e6f";
const char* const kContainerExternal = "4313b572-d8ba-11d2-809b-006008143e6f";
const char* const kVideoDissolve = "0c3bea40-fc05-11d2-8a29-0050040ef7d2";  // OperationDef_VideoDissolve
const char* const kSpeedControl = "9d2ea890-0968-11d3-8a38-0050040ef7d2";   // OperationDef_VideoSpeedControl
const char* const kSpeedRatio = "72559a80-24d7-11d3-8a50-0050040ef7d2";     // ParameterDef_SpeedRatio
const char* const kAudioGain = "9d2ea894-0968-11d3-8a38-0050040ef7d2";      // OperationDef_MonoAudioGain
const char* const kAudioDissolve = "0c3bea41-fc05-11d2-8a29-0050040ef7d2";  // OperationDef_MonoAudioDissolve
const char* const kAmplitude = "e4962321-2267-11d3-8a4c-0050040ef7d2";      // ParameterDef_Amplitude
const char* const kLinear = "5b6c85a4-0ede-11d3-80a9-006008143e6f";         // InterpolationDef_Linear
const char* const kRationalType = "03010100-0000-0000-060e-2b3401040101";
const char* const kTopLevel = "0d010102-0101-0700-060e-2b3404010101";        // Usage_TopLevel
const char* const kOpPattern = "0d011201-0100-0000-060e-2b3404010105";
const char* const kMontageProduct = "6d6f6e74-6167-4500-8000-000000000001";

using Obj = std::unique_ptr<AafObject>;
Obj make(const char* cls) { return std::make_unique<AafObject>(cls); }

Obj definition(const char* cls, const char* id, const std::string& name, const std::string& description) {
    Obj d = make(cls);
    d->text(0x1b02, name).text(0x1b03, description).bytes(0x1b01, aafAuid(id));
    return d;
}

Obj component(const char* cls, int64_t length, const char* dataDef) {
    Obj c = make(cls);
    c->weak(0x0201, AafRefTable::DataDefinitions, 0x1b01, aafAuid(dataDef)).i64(0x0202, length);
    return c;
}
Obj soundComponent(const char* cls, int64_t length) { return component(cls, length, kSound); }

Obj sourceClip(int64_t length, int64_t start, const std::string& mobId, uint32_t slot, const char* dataDef = kSound) {
    Obj c = component(kSourceClip, length, dataDef);
    c->i64(0x1201, start).bytes(0x1101, mobId).u32(0x1102, slot);
    return c;
}

Obj timelineSlot(uint32_t id, const std::string& name, uint32_t physical, int32_t num, int32_t den, Obj segment) {
    Obj s = make(kTimelineMobSlot);
    s->u32(0x4801, id).text(0x4802, name);
    if (physical) s->u32(0x4804, physical);
    s->i64(0x4b02, 0).rational(0x4b01, num, den).strong(0x4803, "Segment", std::move(segment));
    return s;
}

Obj mob(const char* cls, const std::string& name, const std::string& id) {
    Obj m = make(cls);
    m->text(0x4402, name).bytes(0x4401, id).now(0x4405).now(0x4404);
    return m;
}

// A RIFF header with the "fmt " chunk: the WAVE descriptor's summary of the file.
std::string waveSummary(int channels, int bits) {
    std::string s = "RIFF";
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xff);
    };
    auto u16 = [&](uint16_t v) {
        s += char(v & 0xff);
        s += char(v >> 8);
    };
    u32(4 + 8 + 16);
    s += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(uint16_t(channels));
    u32(kRate);
    u32(uint32_t(kRate * channels * bits / 8));
    u16(uint16_t(channels * bits / 8));
    u16(uint16_t(bits));
    return s;
}

bool writeMono24(const std::string& path, const AudioBuffer& buf, int channel, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    const int64_t n = buf.frames();
    const uint32_t dataBytes = uint32_t(n * 3);
    std::string h = waveSummary(1, 24);
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) h[at + size_t(i)] = char((v >> (8 * i)) & 0xff);
    };
    put32(4, 36 + dataBytes);
    h += "data";
    h += std::string(4, '\0');
    put32(40, dataBytes);
    f.write(h.data(), qint64(h.size()));
    std::string chunk;
    chunk.reserve(3 * 65536);
    for (int64_t i = 0; i < n; ++i) {
        const float v = std::clamp(buf.samples[size_t(i) * 2 + size_t(channel)], -1.0f, 1.0f);
        const int32_t s = int32_t(std::lround(double(v) * 8388607.0));
        chunk += char(s & 0xff);
        chunk += char((s >> 8) & 0xff);
        chunk += char((s >> 16) & 0xff);
        if (chunk.size() >= 3 * 65536) {
            f.write(chunk.data(), qint64(chunk.size()));
            chunk.clear();
        }
    }
    f.write(chunk.data(), qint64(chunk.size()));
    return f.error() == QFile::NoError;
}

std::string safeName(std::string s) {
    for (char& c : s)
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';
    return s.empty() ? std::string("audio") : s;
}

// One source of sound: a media file, or a clip rendered to a file of its own.
struct Source {
    std::string name;
    std::vector<std::string> files;     // a mono WAV per channel
    std::vector<std::string> fileMobs;  // their source mobs
    std::string masterMob;
    int64_t samples = 0;
    double frames = 0;  // length in sequence frames
};

// A picture file the composition links to: the camera original where it is, not copied.
struct Picture {
    std::string file, name, masterMob, fileMob;
    Rational rate{25, 1};  // its frame rate (the sequence's for a still)
    double seconds = 0;    // its length; 0 for a still (as long as wanted)
    double used = 0;       // seconds of it the composition reaches (a still's length)
    int width = 0, height = 0;
    double par = 1;           // pixel aspect Interpret Footage gives it (the width above is as shown, par included)
    double timecode = -1;     // its start timecode in seconds, -1 = none
    std::string tapeMob;      // the source it was recorded as, carrying that timecode
};

// A video clip as placed: its extent with the handles a dissolve reaches into, and where in the file it starts.
struct PlacedPicture {
    const Clip* clip = nullptr;
    Picture* picture = nullptr;
    int64_t start = 0, end = 0;  // timeline frames
    double sourceStart = 0;      // the file's time at `start`, in sequence frames
    double ratio = 1;            // file frames per timeline frame (speed, and a conformed rate)
    int64_t dissolveCut = -1;    // with the clip before: the edit point, when they dissolve
    int64_t headUse = 0;         // frames of its own length the dissolve into it covers
    int64_t own = 0;             // its own length on the timeline, before any handles
};

// A clip as placed in the AAF sequence: its extent with the handles fades and crossfades reach into.
struct Placed {
    const Clip* clip = nullptr;
    Source* source = nullptr;
    int64_t start = 0, end = 0;  // timeline frames
    int64_t sourceStart = 0;     // source frame at `start`
    int64_t fadeIn = 0, fadeOut = 0;
    bool powerIn = true, powerOut = true;
    int64_t crossfadeCut = -1;   // with the clip before: the edit point, when they cross-fade
};

Obj gainParameter(const Clip& c, int64_t offset, int64_t length) {
    auto it = c.audio.params.find("gain_db");
    if (it == c.audio.params.end()) return nullptr;
    const Param& gp = it->second;
    auto amp = [](double db) { return int32_t(std::lround(std::pow(10.0, db / 20) * 1000000)); };
    if (!gp.animated()) {
        if (std::fabs(gp.value) < 0.01) return nullptr;
        Obj v = make(kConstantValue);
        v->bytes(0x4c01, aafAuid(kAmplitude)).indirectRational(0x4d01, amp(gp.value), 1000000);
        return v;
    }
    // Keyframes: times as fractions of the operation's length, linear between them.
    Obj v = make(kVaryingValue);
    v->bytes(0x4c01, aafAuid(kAmplitude)).weak(0x4e01, AafRefTable::InterpolationDefinitions, 0x1b01, aafAuid(kLinear));
    for (const Keyframe& k : gp.keys) {
        const int64_t at = std::clamp<int64_t>(k.t + offset, 0, length);
        Obj cp = make(kControlPoint);
        cp->rational(0x1a03, int32_t(at), int32_t(std::max<int64_t>(1, length))).indirectRational(0x1a02, amp(k.v), 1000000);
        v->add(0x4e02, "PointList", std::move(cp));
    }
    return v;
}

}  // namespace

bool exportAaf(const Project& p, const Sequence& seq, const std::string& path, AafExportResult* result,
               const ExportProgress& progress, const std::atomic<bool>* cancel, std::string* error, const AafExportOptions& options) {
    AafExportResult res;
    const double fps = seq.fpsValue();
    const QFileInfo out(QString::fromStdString(path));
    const QString mediaDir = out.absolutePath() + "/" + out.completeBaseName() + " Media";
    if (!QDir().mkpath(mediaDir)) {
        if (error) *error = "Cannot make " + mediaDir.toStdString();
        return false;
    }
    auto cancelled = [&] {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return true;
        }
        return false;
    };

    // The clips, and the sources they need.
    std::map<std::string, Source> sources;
    std::map<Id, std::string> sourceOf;  // clip -> source key
    std::vector<const Track*> tracks;
    for (const Track& t : seq.audioTracks) {
        if (t.muted) continue;
        bool any = false;
        for (const Clip& c : t.clips) {
            const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
            if (!c.enabled || !m || (!m->hasAudio && m->kind != MediaKind::Sequence)) continue;
            const bool render = m->kind == MediaKind::Sequence || std::fabs(c.speed - 1) > 1e-9 || c.reverse || c.ramped();
            const std::string key = render ? "clip:" + std::to_string(c.id) : audioKey(m->path, c.channels);
            sourceOf[c.id] = key;
            Source& s = sources[key];
            if (s.name.empty()) {
                s.name = render ? (c.name.empty() ? "Clip" : c.name) + " (rendered)" : QFileInfo(QString::fromStdString(uninterpretedPath(m->path))).completeBaseName().toStdString();
                if (!render && !c.channels.empty()) {
                    s.name += " ch";
                    for (size_t i = 0; i < c.channels.size(); ++i) s.name += (i ? "+" : " ") + std::to_string(c.channels[i] + 1);
                }
            }
            any = true;
        }
        if (any) tracks.push_back(&t);
    }

    // The picture: each video clip linked to the file it plays, where it is.
    std::map<std::string, Picture> pictures;  // by file
    struct PictureTrack {
        const Track* track = nullptr;
        std::vector<PlacedPicture> clips;
    };
    std::vector<PictureTrack> pictureTracks;
    if (options.picture) {
        int skipped = 0, motion = 0, notDissolve = 0, fades = 0, shortened = 0;
        for (const Track& t : seq.videoTracks) {
            if (t.muted) continue;
            PictureTrack pt;
            pt.track = &t;
            std::map<Id, size_t> at;  // clip -> its place in pt.clips
            for (const Clip& c : t.clips) {
                if (!c.enabled) continue;
                const MediaItem* m = c.mediaId && !c.isGenerator() ? p.findMedia(c.mediaId) : nullptr;
                const std::string file = m ? uninterpretedPath(m->path) : std::string();
                std::string psd;
                int layer = 0;
                if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || file.empty() || isImageSequencePath(file) ||
                    parsePsdLayerPath(file, psd, layer)) {
                    ++skipped;
                    continue;
                }
                const Interpretation in = interpretationOf(*m);
                const double ts = in.timeScale();  // seconds of the file per second of media time
                Picture& pic = pictures[file];
                if (pic.file.empty()) {
                    pic.file = file;
                    pic.name = m->name.empty() ? QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString() : m->name;
                    const Rational own = in.conformed() ? in.fileFps : m->fps;
                    const bool still = m->kind == MediaKind::Image || m->duration <= 0;
                    pic.rate = still || !own.valid() ? (seq.fps.valid() ? seq.fps : Rational{30, 1}) : own;
                    pic.seconds = still ? 0 : m->duration * ts;
                    pic.width = m->width > 0 ? m->width : seq.width;
                    pic.height = m->height > 0 ? m->height : seq.height;
                    pic.par = in.par > 0 ? in.par : 1;
                    pic.timecode = still ? -1 : m->timecode;
                    pic.masterMob = aafNewMobId();
                    pic.fileMob = aafNewMobId();
                    if (pic.timecode >= 0) pic.tapeMob = aafNewMobId();
                }
                PlacedPicture pc;
                pc.clip = &c;
                pc.picture = &pic;
                pc.start = c.start;
                pc.end = c.end();
                // AAF carries a constant speed; a ramp plays at its average and a reversed clip forwards, from its
                // first frame in the file (its source In).
                const auto timing = c.timing.params.find("speed");
                if (c.reverse || (timing != c.timing.params.end() && timing->second.animated())) ++motion;
                const double speed = c.ramped() ? c.sourceExtent() / std::max<double>(1, double(c.duration)) : c.speed;
                pc.ratio = std::max(1e-6, std::fabs(speed)) * ts;
                pc.sourceStart = std::max(0.0, c.sourceIn) * ts;
                if (pic.seconds <= 0) pc.sourceStart += 60 * fps;  // a still: any frame is the picture, so room for handles
                // Never past the end of the file.
                if (pic.seconds > 0) {
                    const double room = (pic.seconds * fps - pc.sourceStart) / pc.ratio;
                    if (double(pc.end - pc.start) > room + 1e-6) pc.end = pc.start + std::max<int64_t>(1, int64_t(std::floor(room + 1e-6)));
                }
                pc.own = pc.end - pc.start;
                at[c.id] = pt.clips.size();
                pt.clips.push_back(pc);
            }
            // Dissolves between two carried clips that meet: each reaches into the other's handles as far as the
            // media allows, around the edit, and no further into a clip than the dissolve at its other end leaves
            // (AAF's transitions may not overlap). In timeline order, so each clip's head is settled before its tail.
            std::vector<const Transition*> order;
            for (const Transition& tr : t.transitions) order.push_back(&tr);
            auto edge = [&](const Transition* tr) {
                FrameTime a = 0, b = 0;
                edit::transitionRange(t, *tr, a, b);
                return a;
            };
            std::sort(order.begin(), order.end(), [&](const Transition* x, const Transition* y) { return edge(x) < edge(y); });
            for (const Transition* trp : order) {
                const Transition& tr = *trp;
                FrameTime a, b;
                if (!edit::transitionRange(t, tr, a, b)) continue;
                if (!tr.clipA || !tr.clipB) {
                    ++fades;
                    continue;
                }
                const auto ia = at.find(tr.clipA), ib = at.find(tr.clipB);
                if (ia == at.end() || ib == at.end()) continue;
                PlacedPicture& pa = pt.clips[ia->second];
                PlacedPicture& pb = pt.clips[ib->second];
                if (pa.end != pb.start) continue;
                const FrameTime cut = pb.start;
                const int64_t preroll = int64_t(std::floor(pb.sourceStart / pb.ratio + 1e-6));
                int64_t postroll = std::numeric_limits<int64_t>::max();
                if (pa.picture->seconds > 0) {
                    const double used = pa.sourceStart + double(pa.end - pa.start) * pa.ratio;
                    postroll = int64_t(std::floor((pa.picture->seconds * fps - used) / pa.ratio + 1e-6));
                }
                const int64_t before = std::max<int64_t>(0, std::min<int64_t>({cut - a, preroll, pa.own - pa.headUse}));
                const int64_t after = std::max<int64_t>(0, std::min<int64_t>({b - cut, postroll, pb.own}));
                if (before < cut - a || after < b - cut) ++shortened;
                if (before + after <= 0) continue;
                if (tr.type != "cross_dissolve") ++notDissolve;
                pa.end += after;
                pb.start -= before;
                pb.sourceStart -= double(before) * pb.ratio;
                pb.dissolveCut = cut;
                pb.headUse = after;
            }
            std::sort(pt.clips.begin(), pt.clips.end(), [](const PlacedPicture& x, const PlacedPicture& y) { return x.start < y.start; });
            if (!pt.clips.empty()) pictureTracks.push_back(std::move(pt));
        }
        if (skipped) res.warnings.push_back(std::to_string(skipped) + " video clip(s) are not linked: titles, generated clips, nested sequences, image sequences and Photoshop layers leave gaps in the picture");
        if (motion) res.warnings.push_back(std::to_string(motion) + " video clip(s) with speed ramps or reversed play at a constant speed forwards in the AAF");
        if (notDissolve) res.warnings.push_back(std::to_string(notDissolve) + " video transition(s) became dissolves");
        if (fades) res.warnings.push_back(std::to_string(fades) + " fade(s) to or from black are not carried");
        if (shortened) res.warnings.push_back(std::to_string(shortened) + " dissolve(s) were shortened or cut: not enough media beyond the edit");
    }
    if (tracks.empty() && pictureTracks.empty()) {
        QDir().rmdir(mediaDir);
        if (error) *error = "There is nothing on the timeline to export";
        return false;
    }

    // Each source as mono 24-bit WAVs.
    std::map<std::string, int> usedNames;
    size_t done = 0;
    for (auto& [key, s] : sources) {
        if (cancelled()) return false;
        std::vector<int> picked;  // the channels a clip plays, if it chose some
        std::string from = key.rfind("clip:", 0) == 0 ? key : audioKeyFile(key, &picked);
        std::string temp;
        if (key.rfind("clip:", 0) == 0) {
            const Id clip = Id(std::stoull(key.substr(5)));
            temp = (mediaDir + "/.render-" + QString::number(clip) + ".wav").toStdString();
            if (!renderClipAudio(p, seq, clip, temp, error)) return false;
            from = temp;
        }
        AudioBufferPtr buf = decodeAudio(from, kRate, error, nullptr, picked);
        if (!temp.empty()) QFile::remove(QString::fromStdString(temp));
        if (!buf) return false;
        MediaItem probe;
        int channels = 2;
        if (picked.size() == 1 || (picked.empty() && temp.empty() && probeMedia(from, probe) && probe.channels == 1)) channels = 1;
        s.samples = buf->frames();
        s.frames = double(s.samples) * fps / kRate;
        std::string base = safeName(s.name);
        if (usedNames[base]++) base += " " + std::to_string(usedNames[base]);
        for (int ch = 0; ch < channels; ++ch) {
            const std::string file =
                (mediaDir + "/" + QString::fromStdString(base) + (channels == 1 ? "" : ch == 0 ? " L" : " R") + ".wav").toStdString();
            if (!writeMono24(file, *buf, ch, error)) return false;
            s.files.push_back(file);
            res.mediaFiles.push_back(file);
        }
        ++done;
        if (progress) progress(0.9 * double(done) / double(sources.size()), 0);
    }

    // Dictionary.
    Obj dict = make(kDictionary);
    dict->addToSet(0x2605, "DataDefinitions", 0x1b01, aafAuid(kSound), definition(kDataDefinition, kSound, "DataDef_Sound", "Sound data"));
    dict->addToSet(0x2605, "DataDefinitions", 0x1b01, aafAuid(kTimecodeData),
                   definition(kDataDefinition, kTimecodeData, "DataDef_Timecode", "Timecode data"));
    dict->addToSet(0x2608, "ContainerDefinitions", 0x1b01, aafAuid(kContainerAAF), definition(kContainerDefinition, kContainerAAF, "ContainerDef_AAF", ""));
    if (!pictureTracks.empty()) {
        dict->addToSet(0x2605, "DataDefinitions", 0x1b01, aafAuid(kPicture), definition(kDataDefinition, kPicture, "DataDef_Picture", "Picture data"));
        dict->addToSet(0x2608, "ContainerDefinitions", 0x1b01, aafAuid(kContainerExternal),
                       definition(kContainerDefinition, kContainerExternal, "ContainerDef_External", "Essence in a file of its own"));
        Obj dissolve = definition(kOperationDefinition, kVideoDissolve, "Video Dissolve", "Video dissolve");
        dissolve->weak(0x1e01, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kPicture)).u8(0x1e02, 0).u32(0x1e07, 2);
        dict->addToSet(0x2603, "OperationDefinitions", 0x1b01, aafAuid(kVideoDissolve), std::move(dissolve));
        Obj speed = definition(kOperationDefinition, kSpeedControl, "Motion Control", "Video speed control");
        speed->weak(0x1e01, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kPicture)).u8(0x1e02, 1).u32(0x1e07, 1);
        speed->weakSet(0x1e09, "ParametersDefined", AafRefTable::ParameterDefinitions, 0x1b01, {aafAuid(kSpeedRatio)});
        dict->addToSet(0x2603, "OperationDefinitions", 0x1b01, aafAuid(kSpeedControl), std::move(speed));
        Obj ratio = definition(kParameterDefinition, kSpeedRatio, "Speed Ratio", "Speed ratio");
        ratio->weak(0x1f01, AafRefTable::TypeDefinitions, 0x0005, aafAuid(kRationalType));
        dict->addToSet(0x2604, "ParameterDefinitions", 0x1b01, aafAuid(kSpeedRatio), std::move(ratio));
    }
    {
        Obj gain = definition(kOperationDefinition, kAudioGain, "Audio Gain", "Mono audio gain");
        gain->weak(0x1e01, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kSound)).u8(0x1e02, 0).u32(0x1e07, 1);
        gain->weakSet(0x1e09, "ParametersDefined", AafRefTable::ParameterDefinitions, 0x1b01, {aafAuid(kAmplitude)});
        dict->addToSet(0x2603, "OperationDefinitions", 0x1b01, aafAuid(kAudioGain), std::move(gain));
        Obj dissolve = definition(kOperationDefinition, kAudioDissolve, "Audio Dissolve", "Mono audio dissolve");
        dissolve->weak(0x1e01, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kSound)).u8(0x1e02, 0).u32(0x1e07, 2);
        dict->addToSet(0x2603, "OperationDefinitions", 0x1b01, aafAuid(kAudioDissolve), std::move(dissolve));
        Obj amplitude = definition(kParameterDefinition, kAmplitude, "Amplitude", "level");
        amplitude->weak(0x1f01, AafRefTable::TypeDefinitions, 0x0005, aafAuid(kRationalType));
        dict->addToSet(0x2604, "ParameterDefinitions", 0x1b01, aafAuid(kAmplitude), std::move(amplitude));
        dict->addToSet(0x2609, "InterpolationDefinitions", 0x1b01, aafAuid(kLinear),
                       definition(kInterpolationDefinition, kLinear, "LinearInterp", "Linear keyframe interpolation"));
    }

    // Mobs: for each source a master mob, and a file source mob per channel.
    Obj content = make(kContentStorage);
    for (auto& [key, s] : sources) {
        s.masterMob = aafNewMobId();
        Obj master = mob(kMasterMob, s.name, s.masterMob);
        for (size_t ch = 0; ch < s.files.size(); ++ch) {
            const std::string fileMob = aafNewMobId();
            s.fileMobs.push_back(fileMob);
            Obj file = mob(kSourceMob, QFileInfo(QString::fromStdString(s.files[ch])).fileName().toStdString(), fileMob);
            file->add(0x4403, "Slots", timelineSlot(1, "", 1, kRate, 1, sourceClip(s.samples, 0, std::string(32, '\0'), 0)));
            Obj wave = make(kWaveDescriptor);
            Obj locator = make(kNetworkLocator);
            locator->text(0x4001, QUrl::fromLocalFile(QString::fromStdString(s.files[ch])).toString(QUrl::FullyEncoded).toStdString());
            wave->rational(0x3001, kRate, 1)
                .bytes(0x3801, waveSummary(1, 24))
                .i64(0x3002, s.samples)
                .weak(0x3004, AafRefTable::ContainerDefinitions, 0x1b01, aafAuid(kContainerAAF))
                .add(0x2f01, "Locator", std::move(locator));
            file->strong(0x4701, "EssenceDescription", std::move(wave));
            content->addToSet(0x1901, "Mobs", 0x4401, fileMob, std::move(file));
            master->add(0x4403, "Slots",
                        timelineSlot(uint32_t(ch + 1), "", uint32_t(ch + 1), kRate, 1, sourceClip(s.samples, 0, fileMob, 1)));
        }
        content->addToSet(0x1901, "Mobs", 0x4401, s.masterMob, std::move(master));
    }

    // The composition: a timecode track, then each audio track as one mono slot per channel.
    const std::string compId = aafNewMobId();
    Obj comp = mob(kCompositionMob, seq.name, compId);
    comp->bytes(0x4408, aafAuid(kTopLevel));
    const int32_t num = seq.fps.valid() ? int32_t(seq.fps.num) : 30, den = seq.fps.valid() ? int32_t(seq.fps.den) : 1;
    {
        Obj tc = make(kTimecode);
        tc->weak(0x0201, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kTimecodeData))
            .i64(0x0202, std::max<FrameTime>(1, seq.duration()))
            .i64(0x1501, 0)
            .u16(0x1502, uint16_t(std::lround(fps)))
            .u8(0x1503, 0);
        comp->add(0x4403, "Slots", timelineSlot(1, "TC1", 1, num, den, std::move(tc)));
    }
    uint32_t slotId = 2, physical = 1;
    // Picture tracks: V1 up, each clip a source clip into its file's master mob (inside a Motion Control operation
    // when it plays at another speed), dissolves as transitions at their edit points.
    {
        uint32_t picturePhysical = 1;
        for (const PictureTrack& pt : pictureTracks) {
            Obj sequence = component(kSequence, 0, kPicture);
            int64_t cursor = 0;
            const PlacedPicture* prev = nullptr;
            for (const PlacedPicture& pc : pt.clips) {
                int64_t start = pc.start;
                double sourceStart = pc.sourceStart;
                const int64_t overlap = prev ? prev->end - start : 0;
                if (prev && pc.dissolveCut >= 0 && overlap > 0) {
                    Obj op = component(kOperationGroup, overlap, kPicture);
                    op->weak(0x0b01, AafRefTable::OperationDefinitions, 0x1b01, aafAuid(kVideoDissolve));
                    Obj tr = component(kTransition, overlap, kPicture);
                    tr->i64(0x1802, std::clamp<int64_t>(pc.dissolveCut - start, 0, overlap)).strong(0x1801, "OperationGroup", std::move(op));
                    sequence->add(0x1001, "Components", std::move(tr));
                    cursor -= overlap;
                    ++res.videoTransitions;
                } else {
                    if (start < cursor) {  // overlapping the clip before (no dissolve): start where it ends
                        sourceStart += double(cursor - start) * pc.ratio;
                        start = cursor;
                    }
                    if (start > cursor) sequence->add(0x1001, "Components", component(kFiller, start - cursor, kPicture));
                }
                const int64_t length = pc.end - start;
                if (length <= 0) continue;
                const int64_t in = std::max<int64_t>(0, std::llround(sourceStart));
                const double needed = sourceStart + double(length) * pc.ratio;  // sequence frames into the file
                pc.picture->used = std::max(pc.picture->used, needed / fps);
                if (std::fabs(pc.ratio - 1) < 1e-9) {
                    sequence->add(0x1001, "Components", sourceClip(length, in, pc.picture->masterMob, 1, kPicture));
                } else {
                    const int64_t used = std::max<int64_t>(1, int64_t(std::ceil(double(length) * pc.ratio - 1e-9)));  // every frame it shows
                    Obj op = component(kOperationGroup, length, kPicture);
                    Obj ratio = make(kConstantValue);
                    ratio->bytes(0x4c01, aafAuid(kSpeedRatio)).indirectRational(0x4d01, int32_t(std::llround(pc.ratio * 100000)), 100000);
                    op->weak(0x0b01, AafRefTable::OperationDefinitions, 0x1b01, aafAuid(kSpeedControl))
                        .add(0x0b02, "InputSegments", sourceClip(used, in, pc.picture->masterMob, 1, kPicture))
                        .addToSet(0x0b03, "Parameters", 0x1b01, aafAuid(kSpeedRatio), std::move(ratio));
                    sequence->add(0x1001, "Components", std::move(op));
                }
                cursor = start + length;
                prev = &pc;
                ++res.videoClips;
            }
            sequence->i64(0x0202, cursor);
            const std::string name = pt.track->name.empty() ? "V" + std::to_string(picturePhysical) : pt.track->name;
            comp->add(0x4403, "Slots", timelineSlot(slotId++, name, picturePhysical++, num, den, std::move(sequence)));
            ++res.videoTracks;
        }
    }
    for (const Track* t : tracks) {
        // The clips, extended into their handles where fades and crossfades reach.
        std::vector<Placed> placed;
        for (const Clip& c : t->clips) {
            auto key = sourceOf.find(c.id);
            if (key == sourceOf.end()) continue;
            Placed pc;
            pc.clip = &c;
            pc.source = &sources[key->second];
            const bool rendered = key->second.rfind("clip:", 0) == 0;
            pc.start = c.start;
            pc.end = c.end();
            pc.sourceStart = rendered ? 0 : int64_t(std::llround(c.sourceIn));
            for (const Transition& tr : t->transitions) {
                FrameTime a, b;
                if (!edit::transitionRange(*t, tr, a, b)) continue;
                if (tr.clipB == c.id) {
                    pc.sourceStart -= pc.start - a;
                    pc.start = a;
                    pc.fadeIn = b - a;
                    pc.powerIn = tr.type != "crossfade_linear";
                    if (tr.clipA) pc.crossfadeCut = c.start;
                }
                if (tr.clipA == c.id) {
                    pc.end = b;
                    pc.fadeOut = b - a;
                    pc.powerOut = tr.type != "crossfade_linear";
                }
            }
            // Only as far as the source goes.
            if (pc.sourceStart < 0) {
                pc.fadeIn = std::max<int64_t>(0, pc.fadeIn + pc.sourceStart);
                pc.start -= pc.sourceStart;
                pc.sourceStart = 0;
                if (pc.crossfadeCut >= 0) res.warnings.push_back("\"" + c.name + "\": not enough media before it for its crossfade");
            }
            const int64_t room = int64_t(std::floor(pc.source->frames + 1e-6)) - pc.sourceStart;
            if (pc.end - pc.start > room) pc.end = pc.start + std::max<int64_t>(1, room);
            placed.push_back(pc);
        }
        const int channels = std::any_of(placed.begin(), placed.end(), [](const Placed& x) { return x.source->files.size() > 1; }) ? 2 : 1;
        for (int ch = 0; ch < channels; ++ch) {
            Obj sequence = soundComponent(kSequence, 0);
            int64_t cursor = 0;
            const Placed* prev = nullptr;
            for (const Placed& pc : placed) {
                int64_t start = pc.start, sourceStart = pc.sourceStart, fadeIn = pc.fadeIn;
                const int64_t overlap = prev ? prev->end - start : 0;
                const bool crossfade = prev && pc.crossfadeCut >= 0 && overlap > 0 && prev->fadeOut > 0;
                if (crossfade) {
                    Obj op = soundComponent(kOperationGroup, overlap);
                    op->weak(0x0b01, AafRefTable::OperationDefinitions, 0x1b01, aafAuid(kAudioDissolve));
                    Obj tr = soundComponent(kTransition, overlap);
                    tr->i64(0x1802, std::clamp<int64_t>(pc.crossfadeCut - start, 0, overlap)).strong(0x1801, "OperationGroup", std::move(op));
                    sequence->add(0x1001, "Components", std::move(tr));
                    cursor -= overlap;
                    fadeIn = 0;
                    ++res.transitions;
                } else {
                    if (start < cursor) {  // reaching back over the clip before: start where it ends
                        const int64_t cut = cursor - start;
                        sourceStart += cut;
                        fadeIn = std::max<int64_t>(0, fadeIn - cut);
                        start = cursor;
                    }
                    if (start > cursor) sequence->add(0x1001, "Components", soundComponent(kFiller, start - cursor));
                }
                const int64_t length = pc.end - start;
                if (length <= 0) continue;
                const Placed* next = nullptr;
                for (const Placed& n : placed)
                    if (n.crossfadeCut >= 0 && n.crossfadeCut == pc.clip->end()) next = &n;
                const uint32_t slot = uint32_t(std::min<size_t>(size_t(ch), pc.source->files.size() - 1) + 1);
                Obj clip = sourceClip(length, sourceStart, pc.source->masterMob, slot);
                if (fadeIn > 0) clip->i64(0x1202, fadeIn).u8(0x1203, pc.powerIn ? 2 : 1);
                if (pc.fadeOut > 0 && !next) clip->i64(0x1204, pc.fadeOut).u8(0x1205, pc.powerOut ? 2 : 1);
                Obj gain = gainParameter(*pc.clip, pc.clip->start - start, length);
                if (gain) {
                    Obj op = soundComponent(kOperationGroup, length);
                    op->weak(0x0b01, AafRefTable::OperationDefinitions, 0x1b01, aafAuid(kAudioGain))
                        .add(0x0b02, "InputSegments", std::move(clip))
                        .addToSet(0x0b03, "Parameters", 0x1b01, aafAuid(kAmplitude), std::move(gain));
                    sequence->add(0x1001, "Components", std::move(op));
                } else {
                    sequence->add(0x1001, "Components", std::move(clip));
                }
                cursor = start + length;
                prev = &pc;
                ++res.clips;
            }
            sequence->i64(0x0202, cursor);
            const std::string name = t->name.empty() ? "A" + std::to_string(physical) : t->name;
            comp->add(0x4403, "Slots", timelineSlot(slotId++, channels == 1 ? name : name + (ch == 0 ? " L" : " R"), physical++, num, den,
                                                    std::move(sequence)));
            ++res.audioTracks;
        }
    }
    content->addToSet(0x1901, "Mobs", 0x4401, compId, std::move(comp));

    // Each picture file: a master mob, and a file source mob whose descriptor (CDCI) and locator name the file.
    for (auto& [file, pic] : pictures) {
        if (pic.masterMob.empty()) continue;
        const double rate = pic.rate.toDouble();
        const int64_t frames = std::max<int64_t>(1, std::llround((pic.seconds > 0 ? pic.seconds : pic.used) * rate));
        // A file with a start timecode was recorded as a source carrying it (pyaaf2's and Media Composer's "tape" mob):
        // the file's first frame sits at that timecode in it, which is how other editors match the file to the camera
        // original.
        const int64_t tcFrames = pic.tapeMob.empty() ? 0 : std::llround(pic.timecode * rate);
        if (!pic.tapeMob.empty()) {
            const int64_t length = tcFrames + frames;
            Obj tape = mob(kSourceMob, QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString(), pic.tapeMob);
            tape->add(0x4403, "Slots", timelineSlot(1, "", 1, pic.rate.num, pic.rate.den, sourceClip(length, 0, std::string(32, '\0'), 0, kPicture)));
            Obj tc = make(kTimecode);
            tc->weak(0x0201, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kTimecodeData))
                .i64(0x0202, length)
                .i64(0x1501, 0)
                .u16(0x1502, uint16_t(std::max<long>(1, std::lround(rate))))
                .u8(0x1503, 0);
            tape->add(0x4403, "Slots", timelineSlot(2, "TC1", 1, pic.rate.num, pic.rate.den, std::move(tc)));
            tape->strong(0x4701, "EssenceDescription", make(kImportDescriptor));
            content->addToSet(0x1901, "Mobs", 0x4401, pic.tapeMob, std::move(tape));
        }
        Obj fileMob = mob(kSourceMob, QFileInfo(QString::fromStdString(file)).fileName().toStdString(), pic.fileMob);
        fileMob->add(0x4403, "Slots", timelineSlot(1, "", 1, pic.rate.num, pic.rate.den,
                                                   pic.tapeMob.empty() ? sourceClip(frames, 0, std::string(32, '\0'), 0, kPicture)
                                                                       : sourceClip(frames, tcFrames, pic.tapeMob, 1, kPicture)));
        Obj desc = make(kCdciDescriptor);
        Obj locator = make(kNetworkLocator);
        locator->text(0x4001, QUrl::fromLocalFile(QString::fromStdString(file)).toString(QUrl::FullyEncoded).toStdString());
        // The picture's shape as shown (anamorphic footage stretched out) and its stored size.
        const int w = std::max(1, pic.width), h = std::max(1, pic.height);
        const int stored = std::max(1, int(std::lround(double(w) / pic.par)));
        const int64_t aw = w, ah = h;
        const int64_t g = std::gcd(aw, ah);
        std::string lineMap(8, '\0');
        desc->rational(0x3001, pic.rate.num, pic.rate.den)
            .i64(0x3002, frames)
            .weak(0x3004, AafRefTable::ContainerDefinitions, 0x1b01, aafAuid(kContainerExternal))
            .u32(0x3202, uint32_t(h))
            .u32(0x3203, uint32_t(stored))
            .u8(0x320c, 0)  // full frame
            .bytes(0x320d, lineMap)
            .rational(0x320e, int32_t(aw / g), int32_t(ah / g))
            .u32(0x3301, 8)
            .u32(0x3302, 2)
            .u32(0x3308, 1)
            .add(0x2f01, "Locator", std::move(locator));
        fileMob->strong(0x4701, "EssenceDescription", std::move(desc));
        content->addToSet(0x1901, "Mobs", 0x4401, pic.fileMob, std::move(fileMob));
        Obj master = mob(kMasterMob, pic.name, pic.masterMob);
        master->add(0x4403, "Slots", timelineSlot(1, "", 1, pic.rate.num, pic.rate.den, sourceClip(frames, 0, pic.fileMob, 1, kPicture)));
        content->addToSet(0x1901, "Mobs", 0x4401, pic.masterMob, std::move(master));
    }

    // Header.
    AafObject header(kHeader);
    header.strong(0x3b04, "Dictionary", std::move(dict)).strong(0x3b03, "Content", std::move(content));
    header.bytes(0x3b09, aafAuid(kOpPattern)).u32(0x3b07, 1).bytes(0x3b05, std::string("\x01\x02", 2));
    {
        Obj id = make(kIdentification);
        id->text(0x3c01, "Montage").text(0x3c02, "Montage").text(0x3c04, "0.1").bytes(0x3c05, aafAuid(kMontageProduct)).now(0x3c06).text(0x3c08, "Montage");
        id->bytes(0x3c09, aafNewMobId().substr(16));
        header.add(0x3b06, "IdentificationList", std::move(id));
    }
    header.now(0x3b02).u16(0x3b01, 0x4949);
    if (cancelled()) return false;
    if (!writeAafFile(path, header, error)) return false;
    if (tracks.empty()) QDir().rmdir(mediaDir);  // no sound files: no folder for them
    if (progress) progress(1.0, 0);
    if (result) *result = res;
    return true;
}

}  // namespace montage
