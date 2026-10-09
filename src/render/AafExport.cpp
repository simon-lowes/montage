#include "AafExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <map>

#include "core/Aaf.h"
#include "core/EditOps.h"
#include "media/Decoder.h"
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
const char* const kNetworkLocator = "0d010101-0101-3200-060e-2b3402060101";
const char* const kDataDefinition = "0d010101-0101-1b00-060e-2b3402060101";
const char* const kContainerDefinition = "0d010101-0101-2000-060e-2b3402060101";
const char* const kOperationDefinition = "0d010101-0101-1c00-060e-2b3402060101";
const char* const kParameterDefinition = "0d010101-0101-1d00-060e-2b3402060101";
const char* const kInterpolationDefinition = "0d010101-0101-2100-060e-2b3402060101";

// Definitions.
const char* const kSound = "01030202-0200-0000-060e-2b3404010101";
const char* const kTimecodeData = "01030201-0100-0000-060e-2b3404010101";
const char* const kContainerAAF = "4313b571-d8ba-11d2-809b-006008143e6f";
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

Obj soundComponent(const char* cls, int64_t length) {
    Obj c = make(cls);
    c->weak(0x0201, AafRefTable::DataDefinitions, 0x1b01, aafAuid(kSound)).i64(0x0202, length);
    return c;
}

Obj sourceClip(int64_t length, int64_t start, const std::string& mobId, uint32_t slot) {
    Obj c = soundComponent(kSourceClip, length);
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
               const ExportProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
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
                s.name = render ? (c.name.empty() ? "Clip" : c.name) + " (rendered)" : QFileInfo(QString::fromStdString(m->path)).completeBaseName().toStdString();
                if (!render && !c.channels.empty()) {
                    s.name += " ch";
                    for (size_t i = 0; i < c.channels.size(); ++i) s.name += (i ? "+" : " ") + std::to_string(c.channels[i] + 1);
                }
            }
            any = true;
        }
        if (any) tracks.push_back(&t);
    }
    if (tracks.empty()) {
        if (error) *error = "There is no audio on the timeline to export";
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
    if (progress) progress(1.0, 0);
    if (result) *result = res;
    return true;
}

}  // namespace montage
