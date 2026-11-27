#include "ProjectManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>

#include "ColorSpace.h"
#include "Exporter.h"
#include "core/ClipAnimation.h"
#include "media/ContentCredentials.h"
#include "core/Effects.h"
#include "core/ProjectMerge.h"
#include "audio/DialogueAlign.h"
#include "core/GraphicsTemplate.h"
#include "core/Interpretation.h"
#include "core/EditOps.h"
#include "core/ProjectIO.h"
#include "core/Transcript.h"
#include "media/Decoder.h"
#include "media/ImageSequence.h"
#include "media/Psd.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace montage {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
// With forward slashes on every system, so a collected project's paths read the same everywhere (Windows takes both).
std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// The sequences kept: those chosen and every one they nest, at any depth.
std::set<Id> keptSequences(const Project& p, const std::vector<Id>& chosen) {
    std::set<Id> kept;
    std::vector<Id> todo = chosen;
    if (todo.empty())
        for (const Sequence& s : p.sequences) todo.push_back(s.id);
    while (!todo.empty()) {
        const Id id = todo.back();
        todo.pop_back();
        const Sequence* s = p.findSequence(id);
        if (!s || !kept.insert(id).second) continue;
        for (TrackRef r : allTracks(*s))
            for (const Clip& c : trackAt(*s, r)->clips)
                if (const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr; m && m->kind == MediaKind::Sequence) todo.push_back(m->sequenceId);
    }
    return kept;
}

// A file name not yet used in `taken`, nor by a file there already, unless that file is one of `own` (what an
// earlier run of the same job began: it is written again).
fs::path uniqueIn(const fs::path& dir, const std::string& stem, const std::string& ext, std::set<std::string>& taken,
                  const std::set<std::string>& own = {}) {
    for (int n = 1;; ++n) {
        const std::string name = stem + (n > 1 ? "-" + std::to_string(n) : "") + ext;
        const std::string key = utf8(dir / u8path(name));
        if (!taken.count(key) && (!fs::exists(dir / u8path(name)) || own.count(key))) {
            taken.insert(key);
            return dir / u8path(name);
        }
    }
}

// The pictures graphics show through their published picture fields (core/GraphicsTemplate.h).
std::set<Id> fieldMedia(const Project& p, const Sequence& s) {
    std::set<Id> out;
    for (TrackRef r : allTracks(s))
        for (const Clip& c : trackAt(s, r)->clips) {
            if (c.fields.empty()) continue;
            const Sequence* g = graphicOf(p, c);
            if (!g) continue;
            for (const TemplateField& f : g->fields)
                if (f.kind == "media")
                    if (const auto it = c.fields.find(f.id); it != c.fields.end())
                        if (const Id id = Id(std::strtoull(it->second.c_str(), nullptr, 10)); p.findMedia(id)) out.insert(id);
        }
    return out;
}

}  // namespace

std::vector<Id> usedMedia(const Project& p, const std::vector<Id>& sequences) {
    std::set<Id> used;
    for (Id sid : keptSequences(p, sequences)) {
        const Sequence* s = p.findSequence(sid);
        for (TrackRef r : allTracks(*s)) {
            const Track* tr = trackAt(*s, r);
            auto useClips = [&](const std::vector<Clip>& clips) {
                for (const Clip& c : clips) {
                    if (c.mediaId && p.findMedia(c.mediaId)) used.insert(c.mediaId);
                    for (const Take& t : c.takes)  // an audition's other takes
                        if (p.findMedia(t.mediaId)) used.insert(t.mediaId);
                    for (const CompositeNode& n : c.composite.nodes)  // its node graph's media inputs
                        if (n.media && p.findMedia(n.media)) used.insert(n.media);
                    for (const Effect& e : c.effects)  // pictures in the bin its OpenFX effects read as inputs
                        if (e.type == "ofx")
                            for (const auto& [k, v] : e.strings)
                                if (const Id m = k.rfind("input.", 0) == 0 && v.rfind("media:", 0) == 0 ? Id(std::strtoull(v.c_str() + 6, nullptr, 10)) : 0;
                                    m && p.findMedia(m))
                                    used.insert(m);
                }
            };
            useClips(tr->clips);
            for (const TrackVersion& v : tr->versions) useClips(v.clips);  // its other versions
            for (const TakeLane& l : tr->lanes)                             // and take lanes
                if (p.findMedia(l.mediaId)) used.insert(l.mediaId);
        }
        for (Id m : fieldMedia(p, *s)) used.insert(m);
    }
    return {used.begin(), used.end()};
}

namespace {

// A file's bytes as one number (FNV-1a, 64 bits); false when it cannot be read.
bool fileHash(const fs::path& file, uint64_t& hash) {
    std::FILE* f =
#ifdef _WIN32
        _wfopen(file.c_str(), L"rb");
#else
        std::fopen(file.c_str(), "rb");
#endif
    if (!f) return false;
    hash = 1469598103934665603ull;
    std::vector<unsigned char> buf(size_t(1) << 20);
    size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
        for (size_t i = 0; i < n; ++i) hash = (hash ^ buf[i]) * 1099511628211ull;
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

// Whether `copy` is already the whole of `from`: there, the same size, and the same hash.
bool alreadyWhole(const fs::path& from, const fs::path& copy) {
    std::error_code ec;
    if (!fs::is_regular_file(copy, ec)) return false;
    const auto a = fs::file_size(from, ec);
    if (ec) return false;
    const auto b = fs::file_size(copy, ec);
    if (ec || a != b) return false;
    uint64_t ha = 0, hb = 0;
    return fileHash(from, ha) && fileHash(copy, hb) && ha == hb;
}

// What a video file is coded as, for copying its frames rather than coding them again.
struct Coding {
    AVCodecID codec = AV_CODEC_ID_NONE;
    uint32_t tag = 0;
    int profile = -1;
    std::string pixFmt;
    bool alpha = false;
    int64_t bitrate = 0;
    bool read = false;
};

Coding codingOf(const std::string& path) {
    Coding c;
    AVFormatContext* fmt = nullptr;
    if (openMediaInput(&fmt, path) < 0) return c;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        const int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (v >= 0) {
            const AVCodecParameters* par = fmt->streams[v]->codecpar;
            c.codec = par->codec_id;
            c.tag = par->codec_tag;
            c.profile = par->profile;
            if (const char* n = av_get_pix_fmt_name(AVPixelFormat(par->format))) c.pixFmt = n;
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(par->format));
            c.alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
            c.bitrate = par->bit_rate > 0 ? par->bit_rate : fmt->bit_rate;
            c.read = true;
        }
    }
    avformat_close_input(&fmt);
    return c;
}

// The ProRes flavour a FourCC is, DNxHR's profile names.
std::string proresProfile(uint32_t tag) {
    switch (tag) {
    case MKTAG('a', 'p', 'c', 'o'): return "proxy";
    case MKTAG('a', 'p', 'c', 's'): return "lt";
    case MKTAG('a', 'p', 'c', 'n'): return "standard";
    case MKTAG('a', 'p', '4', 'h'): return "4444";
    case MKTAG('a', 'p', '4', 'x'): return "4444xq";
    default: return "hq";
    }
}
std::string dnxhrProfile(int profile) {
    static const char* const names[] = {"", "dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx", "dnxhr_444"};
    return profile >= 1 && profile <= 5 ? names[profile] : std::string();
}

// How a file's used part is written: the encoder, its settings and the file's extension.
struct Target {
    std::string codec, profile, pixFmt, ext, audio;
    bool alpha = false;
    bool frames = false;   // an image sequence
    int64_t bitrate = 0;
};

Target movieTarget(const std::string& codec, const Coding& src, bool own) {
    Target t;
    t.audio = "pcm_s24le";
    t.ext = ".mov";
    if (codec == "dnxhd") {
        t.codec = "dnxhd";
        t.profile = own && !dnxhrProfile(src.profile).empty() ? dnxhrProfile(src.profile) : std::string("dnxhr_hq");
        if (own && !dnxhrProfile(src.profile).empty()) t.pixFmt = src.pixFmt;
    } else if (codec == "libx264" || codec == "libx265") {
        t.codec = codec;
        t.ext = own ? ".mov" : ".mp4";
        t.audio = own ? "pcm_s24le" : "aac";
    } else if (codec == "ffv1") {
        t.codec = "ffv1";
        t.ext = ".mkv";
    } else {
        t.codec = "prores_ks";
        t.profile = own && src.codec == AV_CODEC_ID_PRORES ? proresProfile(src.tag) : std::string("hq");
        if (own && src.codec == AV_CODEC_ID_PRORES) t.pixFmt = src.pixFmt, t.alpha = src.alpha;
    }
    return t;
}

// The source's own encoder, when Montage has one that writes what it reads ("" when not).
std::string ownEncoder(const Coding& c) {
    switch (c.codec) {
    case AV_CODEC_ID_PRORES: return "prores_ks";
    case AV_CODEC_ID_DNXHD: return dnxhrProfile(c.profile).empty() ? std::string() : std::string("dnxhd");
    case AV_CODEC_ID_H264: return "libx264";
    case AV_CODEC_ID_HEVC: return "libx265";
    case AV_CODEC_ID_FFV1: return "ffv1";
    default: return {};
    }
}

// A duration as "HH:MM:SS:FF" (or fewer fields: "SS:FF", "FF") in frames at `fps`; -1 when it does not read.
int timecodeFrames(const std::string& tc, double fps) {
    std::vector<int> parts;
    for (const QString& f : QString::fromStdString(tc).split(QRegularExpression(QStringLiteral("[:;.]")))) {
        bool ok = false;
        const int v = f.trimmed().toInt(&ok);
        if (!ok || v < 0) return -1;
        parts.push_back(v);
    }
    if (parts.empty() || parts.size() > 4) return -1;
    const int nominal = std::max(1, int(std::lround(fps)));
    const int mult[4] = {1, nominal, nominal * 60, nominal * 3600};
    long long n = 0;
    for (size_t k = 0; k < parts.size(); ++k) n += (long long)parts[parts.size() - 1 - k] * mult[k];
    return n > 100000000 ? -1 : int(n);
}

// A name made safe for a file on every system.
std::string safeFileName(const std::string& in) {
    QString s = QString::fromStdString(in).trimmed();
    s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|%\\x00-\\x1f]")), QStringLiteral("_"));
    while (s.endsWith('.') || s.endsWith(' ')) s.chop(1);
    return s.isEmpty() ? std::string("media") : s.toStdString();
}

// What a pattern gives a media item (ConsolidateOptions::namePattern): its first clip in the kept sequences names
// {sequence} and {shot}. {frame} is left for the caller.
std::string patternName(const std::string& pattern, const Project& q, const MediaItem& m) {
    const Sequence* seq = nullptr;
    const Clip* first = nullptr;
    for (const Sequence& s : q.sequences) {
        for (const Track& t : s.videoTracks)
            for (const Clip& c : t.clips)
                if (c.mediaId == m.id && (!first || c.start < first->start)) first = &c;
        if (!first)
            for (const Track& t : s.audioTracks)
                for (const Clip& c : t.clips)
                    if (c.mediaId == m.id && (!first || c.start < first->start)) first = &c;
        if (first) {
            seq = &s;
            break;
        }
    }
    std::string shot;
    if (seq && first) {
        // (its place among the sequence's footage clips, by start then the lower track first)
        int before = 0;
        std::pair<FrameTime, size_t> mine{first->start, 0};
        std::vector<std::pair<FrameTime, size_t>> order;
        for (size_t t = 0; t < seq->videoTracks.size(); ++t)
            for (const Clip& c : seq->videoTracks[t].clips) {
                if (&c == first) mine = {c.start, t};
                else if (c.mediaId) order.emplace_back(c.start, t);
            }
        for (const auto& x : order) before += x < mine ? 1 : 0;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%03d", (before + 1) * 10);
        shot = buf;
    }
    auto meta = [&](const char* k) {
        const auto it = m.metadata.find(k);
        return it == m.metadata.end() ? QString() : QString::fromStdString(it->second);
    };
    QString out = QString::fromStdString(pattern);
    out.replace(QStringLiteral("{sequence}"), seq ? QString::fromStdString(seq->name) : QString());
    out.replace(QStringLiteral("{shot}"), QString::fromStdString(shot));
    // (an image sequence by its frames' name, without their numbers: "plate" for plate.%04d.exr)
    QString clip = QFileInfo(QString::fromStdString(m.name)).completeBaseName();
    if (ImageSequence frames; parseImageSequencePath(m.path, frames)) {
        const QString file = QFileInfo(QString::fromStdString(frames.pattern)).fileName();
        const qsizetype at = file.indexOf(QLatin1Char('%'));
        clip = at > 0 ? file.left(at) : QString();
        while (!clip.isEmpty() && QStringLiteral(" ._-").contains(clip.back())) clip.chop(1);
        if (clip.isEmpty()) clip = QFileInfo(QString::fromStdString(m.name)).completeBaseName();
    }
    out.replace(QStringLiteral("{clip}"), clip);
    out.replace(QStringLiteral("{reel}"), meta("reel").isEmpty() ? meta("tape") : meta("reel"));
    out.replace(QStringLiteral("{tape}"), meta("tape"));
    out.replace(QStringLiteral("{scene}"), meta("scene"));
    out.replace(QStringLiteral("{take}"), meta("take"));
    // ({frame} marked by a character no name keeps, once the name is safe)
    out.replace(QStringLiteral("{frame}"), QStringLiteral("FRAMExNUMBERx18"));
    return QString::fromStdString(safeFileName(out.toStdString())).replace(QStringLiteral("FRAMExNUMBERx18"), QStringLiteral("\x01")).toStdString();
}

// Whether a file's picture has an alpha channel (kept for the files asked about).
bool hasAlpha(const std::string& path) {
    static std::mutex lock;
    static std::map<std::string, bool> known;
    {
        std::lock_guard<std::mutex> g(lock);
        if (const auto it = known.find(path); it != known.end()) return it->second;
    }
    const Coding c = codingOf(path);
    const bool alpha = !c.read || c.alpha;  // (a file that cannot be read is not taken to cover anything)
    std::lock_guard<std::mutex> g(lock);
    known[path] = alpha;
    return alpha;
}

// Whether clip `k` shows its footage opaque over the whole frame, untouched: enabled, blended normally, with no effect,
// animation or compositing, its transform at its defaults (and its picture the frame's shape or fitted to fill it).
bool opaqueCover(const Project& p, const Sequence& s, const Track& t, const Clip& k) {
    if (!k.enabled || k.blendMode != "normal" || !k.effects.empty() || !k.composite.nodes.empty() || hasClipAnimation(k) || !k.layer.empty())
        return false;
    const MediaItem* m = k.mediaId ? p.findMedia(k.mediaId) : nullptr;
    if (!m || !m->hasVideo || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || !m->stereo.empty() || m->path.empty()) return false;
    for (const Transition& tr : t.transitions)
        if (tr.clipA == k.id || tr.clipB == k.id) return false;
    const EffectInfo* info = findEffectInfo(k.motion.empty() ? "transform" : k.motion.type);
    if (!info) return false;
    for (const auto& [key, param] : k.motion.params) {
        if (param.animated()) return false;
        const size_t dot = key.rfind('.');
        const std::string base = dot == std::string::npos ? key : key.substr(0, dot);
        const auto pi = std::find_if(info->params.begin(), info->params.end(), [&](const ParamInfo& x) { return x.name == key || x.name == base; });
        if (pi == info->params.end()) return false;
        if (pi->name == "fit") {
            // (fit 1 fills the frame; any fit is 1:1 when the picture is the frame's shape)
            const bool sameShape = m->width > 0 && m->height > 0 && std::llabs((long long)m->width * s.height - (long long)m->height * s.width) <= std::max(m->width, s.width);
            if (int(std::lround(param.value)) != 1 && !sameShape) return false;
            continue;
        }
        const double def = pi->kind == ParamKind::Color && key.size() > 2 && key.compare(key.size() - 2, 2, ".g") == 0   ? pi->defG
                           : pi->kind == ParamKind::Color && key.size() > 2 && key.compare(key.size() - 2, 2, ".b") == 0 ? pi->defB
                                                                                                                        : pi->def;
        if (std::fabs(param.value - def) > 1e-9) return false;
    }
    if (const auto fit = k.motion.params.find("fit"); fit == k.motion.params.end()) {
        const bool sameShape = m->width > 0 && m->height > 0 && std::llabs((long long)m->width * s.height - (long long)m->height * s.width) <= std::max(m->width, s.width);
        if (!sameShape) return false;
    }
    return !hasAlpha(mediaFileOnDisk(m->path));
}

}  // namespace

std::vector<std::pair<Id, std::string>> hiddenClips(const Project& p, const Sequence& s) {
    std::vector<std::pair<Id, std::string>> out;
    for (size_t ti = 0; ti < s.videoTracks.size(); ++ti)
        for (const Clip& c : s.videoTracks[ti].clips) {
            if (!c.mediaId) continue;
            if (!c.enabled) {
                out.emplace_back(c.id, "disabled");
                continue;
            }
            // Covered, frame by frame from its start, by opaque clips on tracks above (shown ones: a track whose output
            // is off covers nothing).
            FrameTime at = c.start;
            std::string by;
            bool progress = true;
            while (at < c.end() && progress) {
                progress = false;
                for (size_t up = ti + 1; up < s.videoTracks.size() && !progress; ++up) {
                    const Track& t = s.videoTracks[up];
                    if (t.muted) continue;
                    for (const Clip& k : t.clips)
                        if (k.start <= at && k.end() > at && opaqueCover(p, s, t, k)) {
                            if (by.empty()) by = k.name.empty() && p.findMedia(k.mediaId) ? p.findMedia(k.mediaId)->name : k.name;
                            at = k.end();
                            progress = true;
                            break;
                        }
                }
            }
            if (at >= c.end()) out.emplace_back(c.id, "hidden under " + by);
        }
    return out;
}

bool consolidateProject(const Project& p, const ConsolidateOptions& o, ConsolidateResult* result, const std::function<void(double)>& progress,
                        const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (o.folder.empty()) return fail("Choose a folder for the project");
    const std::string mode = !o.output.empty() ? o.output : o.trim ? "movie" : "whole";
    // A copy made from a signed source keeps the source's credentials as its parent (when signing is on); a manifest
    // beside a file copied whole goes with it.
    const CredentialSigner signer{o.credentialCertificate, o.credentialKey, {}};
    auto keep = [&](const std::string& source, const std::string& copy, const std::string& what) {
        std::string why;
        return keepCredentialsThroughCopy(source, copy, signer, what, &why) || fail(why);
    };
    auto besideToo = [&](const std::string& source, const std::string& copy) {
        if (!hasCredentialSidecar(source)) return;
        std::error_code e;
        fs::copy_file(u8path(credentialSidecarPath(source)), u8path(credentialSidecarPath(copy)), fs::copy_options::overwrite_existing, e);
    };
    if (mode != "whole" && mode != "trimmed" && mode != "movie" && mode != "frames") return fail("Unknown way of consolidating: " + o.output);
    if (mode == "frames" && o.frames != "exr" && o.frames != "dpx" && o.frames != "tiff") return fail("Image sequences are OpenEXR, DPX or TIFF");
    static const std::set<std::string> movieCodecs{"prores_ks", "dnxhd", "libx264", "libx265", "ffv1"};
    if (mode == "movie" && !(o.output.empty() || o.codec.empty() || movieCodecs.count(o.codec))) return fail("Movies are ProRes, DNxHR, H.264, HEVC or FFV1");
    if (mode == "movie" && o.output.empty() && !movieCodecs.count(o.codec)) return fail("Movies are ProRes, DNxHR, H.264, HEVC or FFV1");
    static const std::set<std::string> units{"seconds", "frames", "timecode", "none", "whole"};
    if (!units.count(o.handleUnit)) return fail("Handles are seconds, frames, a timecode, none or the whole file");
    if (o.handleUnit == "timecode" && timecodeFrames(o.handleTimecode, 25) < 0) return fail("The handles' timecode " + o.handleTimecode + " does not read");
    if (!o.colorSpace.empty() && o.colorSpace != "working" && !findColorSpace(o.colorSpace)) return fail("Unknown colour space " + o.colorSpace);
    ConsolidateResult res;
    const fs::path folder = u8path(o.folder), mediaDir = folder / "Media";
    std::error_code ec;
    fs::create_directories(mediaDir, ec);
    if (ec) return fail("Cannot make " + utf8(mediaDir) + ": " + ec.message());

    const std::set<Id> kept = keptSequences(p, o.sequences);
    if (kept.empty()) return fail("There is no sequence to keep");

    Project q = p;
    std::erase_if(q.sequences, [&](const Sequence& s) { return !kept.count(s.id); });
    if (!kept.count(q.activeSequence)) q.activeSequence = q.sequences.front().id;

    std::set<std::string> taken;
    const std::set<std::string> own(o.replace.begin(), o.replace.end());
    auto begin = [&](const fs::path& file) {
        if (o.began) o.began(utf8(file));
    };
    // Where a source's copy goes: in Media, under as many of its own folders as are kept.
    auto destFolder = [&](const fs::path& src) {
        fs::path dir = mediaDir;
        if (o.folderDepth > 0) {
            std::vector<fs::path> parts;
            for (fs::path d = src.parent_path(); parts.size() < size_t(o.folderDepth) && d.has_relative_path() && d != d.parent_path(); d = d.parent_path())
                parts.push_back(d.filename());
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) dir /= u8path(safeFileName(utf8(*it)));
            fs::create_directories(dir, ec);
        }
        return dir;
    };
    // The working space transcodes are made in when asked for it: the first kept sequence's.
    const Sequence& firstKept = q.sequences.front();
    auto spaceFor = [&](const MediaItem& m, bool exr) -> std::string {
        if (o.colorSpace.empty()) {
            if (!exr) return {};
            const ColorSpace& mine = mediaColorSpace(m);
            return mine.transfer == Transfer::Linear ? mine.id : linearColorSpace(mine.primaries).id;
        }
        const ColorSpace& chosen = o.colorSpace == "working" ? workingColorSpace(firstKept) : *findColorSpace(o.colorSpace);
        if (exr && chosen.transfer != Transfer::Linear) return linearColorSpace(chosen.primaries).id;
        return chosen.id;
    };

    // Clips nothing shows, left out.
    if (o.leaveOutHidden)
        for (Sequence& s : q.sequences) {
            std::vector<std::pair<Id, std::string>> out = hiddenClips(q, s);
            for (const Track& t : s.audioTracks)
                for (const Clip& c : t.clips)
                    if (!c.enabled && c.mediaId) out.emplace_back(c.id, "disabled");
            for (const auto& [id, why] : out) {
                const Clip* c = edit::clipById(s, id);
                const MediaItem* m = c ? q.findMedia(c->mediaId) : nullptr;
                res.leftOut.push_back(s.name + ": " + (c && !c->name.empty() ? c->name : m ? m->name : std::string("clip")) + " (" + why + ")");
            }
            auto gone = [&](Id id) { return std::any_of(out.begin(), out.end(), [&](const auto& x) { return x.first == id; }); };
            for (auto* tracks : {&s.videoTracks, &s.audioTracks})
                for (Track& t : *tracks) {
                    std::erase_if(t.clips, [&](const Clip& c) { return gone(c.id); });
                    std::erase_if(t.transitions, [&](const Transition& tr) { return gone(tr.clipA) || gone(tr.clipB); });
                }
        }

    // Clips baked: each rendered as the sequence shows it, with its handles, into a file its clip then plays plainly.
    std::set<Id> bakedMedia;
    if (o.bake)
        for (Sequence& s : q.sequences)
            for (size_t ti = 0; ti < s.videoTracks.size(); ++ti)
                for (Clip& c : s.videoTracks[ti].clips) {
                    if (cancel && cancel->load()) return fail("Cancelled");
                    const MediaItem* m = c.mediaId ? q.findMedia(c.mediaId) : nullptr;
                    if (!m || m->kind == MediaKind::Sequence || !m->hasVideo || m->path.empty() || bakedMedia.count(m->id)) continue;
                    if (!fs::exists(u8path(mediaFileOnDisk(m->path)), ec)) continue;  // (listed as missing below)
                    const double sfps = s.fpsValue();
                    const double mediaFrames = m->duration > 0 ? m->duration * sfps : 1e18;  // (a still lasts)
                    // The handles in the sequence's frames, as far as the footage goes either side.
                    const double mf = m->fps.valid() ? m->fps.toDouble() : sfps;
                    const double want = o.handleUnit == "none"     ? 0
                                        : o.handleUnit == "frames" ? o.handleFrames * sfps / mf
                                        : o.handleUnit == "timecode" ? timecodeFrames(o.handleTimecode, mf) * sfps / mf
                                        : o.handleUnit == "whole"  ? 1e9
                                                                   : o.handles * sfps;
                    const double sp0 = std::max(1e-6, std::fabs(c.speedAt(0))), sp1 = std::max(1e-6, std::fabs(c.speedAt(double(c.duration))));
                    const double headRoom = c.reverse ? (mediaFrames - (c.sourceIn + double(c.duration) * c.speed)) / c.speed : c.sourceIn / sp0;
                    const double tailRoom = c.reverse ? c.sourceIn / c.speed : (mediaFrames - (c.sourceIn + c.sourceExtent())) / sp1;
                    const FrameTime hh = FrameTime(std::max(0.0, std::floor(std::min(want, headRoom) + 1e-6)));
                    const FrameTime th = FrameTime(std::max(0.0, std::floor(std::min(want, tailRoom) + 1e-6)));
                    Clip e = c;
                    if (!e.reverse) e.sourceIn += e.sourceOffset(-double(hh));
                    e.duration += hh;
                    edit::shiftKeyframes(e, hh);
                    if (e.reverse) e.sourceIn -= double(th) * e.speed;
                    e.duration += th;
                    e.start = 0;
                    Sequence alone = s;
                    for (Track& t : alone.videoTracks) t.clips.clear(), t.transitions.clear();
                    for (Track& t : alone.audioTracks) t.clips.clear(), t.transitions.clear();
                    alone.captionTracks.clear();
                    alone.videoTracks[ti].clips.push_back(e);
                    const bool exr = mode == "frames";
                    const std::string stem = (c.name.empty() ? utf8(u8path(m->name).stem()) : safeFileName(c.name)) + "_baked";
                    ExportSettings st;
                    st.audioCodec = "none";
                    st.smartRender = false;
                    st.alpha = true;
                    st.in = 0;
                    st.out = e.duration;
                    fs::path dest;
                    std::string space;
                    if (exr) {
                        dest = uniqueIn(mediaDir, stem, "", taken, own);
                        begin(dest);
                        fs::create_directories(dest, ec);
                        st.videoCodec = "exr";
                        const ColorSpace& seqSpace = sequenceColorSpace(s);
                        space = seqSpace.transfer == Transfer::Linear ? seqSpace.id : linearColorSpace(seqSpace.primaries).id;
                        st.colorSpace = space;
                        st.path = utf8(dest / u8path(stem + ".%04d.exr"));
                    } else {
                        dest = uniqueIn(mediaDir, stem, ".mov", taken, own);
                        begin(dest);
                        st.videoCodec = "prores_ks";
                        st.profile = "4444";
                        st.path = utf8(dest);
                        space = sequenceColorSpace(s).id;
                    }
                    std::string err;
                    if (!exportSequence(q, alone, st, {}, cancel, &err)) return fail("Baking " + (c.name.empty() ? m->name : c.name) + " failed: " + err);
                    std::string path = st.path;
                    if (exr) {
                        ImageSequence frames;
                        frames.pattern = st.path;
                        frames.first = 0;
                        frames.last = int(e.duration) - 1;
                        frames.fps = s.fps;
                        path = imageSequencePath(frames);
                    }
                    MediaItem baked;
                    if (!probeMedia(path, baked, &err)) return fail("Cannot read the baked " + m->name + ": " + err);
                    if (!keep(m->path, path, "Its clip's grade and effects rendered in by Montage")) return false;
                    baked.id = q.newId();
                    baked.name = utf8(exr ? dest.filename() : dest.filename());
                    baked.colorSpace = space;
                    baked.colorOverride.clear();
                    baked.metadata = m->metadata;
                    baked.timecode = -1;
                    baked.bin = m->bin;
                    // Its markers, where the clip showed them, in the baked file's frames.
                    for (Marker& mk : c.markers) mk.t = FrameTime(std::llround(c.localForSource(double(mk.t)))) + hh;
                    c.mediaId = baked.id;
                    c.sourceIn = double(hh);
                    c.speed = 1;
                    c.reverse = false;
                    c.timing = Effect{};
                    c.effects.clear();
                    c.motion = makeEffect("transform", q.newId());
                    c.grade = GradeGraph{};
                    c.composite = CompositeGraph{};
                    c.animIn = c.animOut = c.animLoop = ClipAnimation{};
                    c.gradeVersions.clear();
                    c.gradeVersion = 0;
                    c.colorGroup = 0;
                    c.takes.clear();
                    c.take = 0;
                    c.layer.clear();
                    c.trims.clear();
                    c.unrendered.clear();
                    bakedMedia.insert(baked.id);
                    std::error_code sz;
                    if (exr)
                        for (const auto& f : fs::directory_iterator(dest, sz)) res.bytes += int64_t(f.file_size(sz));
                    else
                        res.bytes += int64_t(fs::file_size(dest, sz));
                    q.media.push_back(std::move(baked));
                    ++res.baked;
                }

    std::vector<Id> keptIds;
    for (const Sequence& s : q.sequences) keptIds.push_back(s.id);
    const std::vector<Id> usedList = usedMedia(q, keptIds);
    const std::set<Id> used(usedList.begin(), usedList.end());
    std::erase_if(q.media, [&](const MediaItem& m) {
        if (o.keepUnused || used.count(m.id)) return false;
        if (m.kind == MediaKind::Sequence) return !kept.count(m.sequenceId);
        return !(m.subclipOf && used.count(m.subclipOf));  // subclips of kept media stay
    });

    std::map<std::string, std::string> copiedFiles;  // source file -> its copy
    std::vector<MediaItem> added;                    // sound written beside image sequences
    const size_t total = std::max<size_t>(1, q.media.size());
    for (size_t i = 0; i < q.media.size(); ++i) {
        if (cancel && cancel->load()) return fail("Cancelled");
        MediaItem& m = q.media[i];
        if (progress) progress(double(i) / double(total));
        if (m.kind == MediaKind::Sequence || m.subclipOf || m.path.empty() || bakedMedia.count(m.id)) continue;
        ImageSequence frames;
        const bool imageSequence = parseImageSequencePath(m.path, frames);
        const Interpretation reading = interpretationOf(m);  // kept on the copy (Interpret Footage)
        if (!fs::exists(u8path(mediaFileOnDisk(m.path)), ec)) {
            res.missing.push_back(mediaFileOnDisk(m.path));
            continue;
        }
        const fs::path src = u8path(mediaFileOnDisk(m.path));
        const std::string original = m.path;  // (for its credentials)
        const fs::path dir = destFolder(src);
        const std::string named = o.namePattern.empty() ? std::string() : patternName(o.namePattern, q, m);
        auto movieName = [&](const std::string& fallback) {
            if (named.empty()) return fallback;
            std::string n = named;
            std::erase(n, '\x01');
            // (without the separators the frame's number had round it)
            const size_t a = n.find_first_not_of(" ._-"), b = n.find_last_not_of(" ._-");
            n = a == std::string::npos ? std::string() : n.substr(a, b - a + 1);
            return n.empty() ? fallback : n;
        };
        // The span the kept sequences use, in media seconds.
        double first = 1e18, last = -1e18;
        for (const Sequence& s : q.sequences)
            for (TrackRef r : allTracks(s))
                for (const Clip& c : trackAt(s, r)->clips) {
                    if (c.mediaId != m.id) continue;
                    const double fps = s.fpsValue(), a = c.sourceAt(0) / fps, b = c.sourceAt(double(c.duration)) / fps;
                    first = std::min({first, a, b});
                    last = std::max({last, a, b});
                }
        // A take of an audition is copied whole: its clip may be trimmed to any part of it.
        bool aTake = false;
        for (const Sequence& s : q.sequences)
            for (TrackRef r : allTracks(s))
                for (const Clip& c : trackAt(s, r)->clips)
                    for (size_t k = 0; k < c.takes.size(); ++k) aTake |= int(k) != c.take && c.takes[k].mediaId == m.id;
        // So is media a track's other versions or take lanes use (core/TakeLanes.h).
        for (const Sequence& s : q.sequences)
            for (TrackRef r : allTracks(s)) {
                const Track& t = *trackAt(s, r);
                for (const TrackVersion& v : t.versions)
                    for (const Clip& c : v.clips) aTake |= c.mediaId == m.id;
                for (const TakeLane& l : t.lanes) aTake |= l.mediaId == m.id;
            }
        // So is a graphic's picture: it plays over the span of the template's clip, not one of its own.
        for (const Sequence& s : q.sequences) aTake |= fieldMedia(q, s).count(m.id) > 0;
        // The handles, in seconds.
        const double mfps = m.fps.valid() ? m.fps.toDouble() : firstKept.fpsValue();
        const bool wholeFile = o.handleUnit == "whole";
        const double handle = o.handleUnit == "none"       ? 0
                              : o.handleUnit == "frames"   ? double(std::max(0, o.handleFrames)) / mfps
                              : o.handleUnit == "timecode" ? double(std::max(0, timecodeFrames(o.handleTimecode, mfps))) / mfps
                              : o.handleUnit == "whole"    ? 0
                                                           : std::max(0.0, o.handles);
        const bool video = m.kind == MediaKind::Video && m.hasVideo;
        const bool sound = !m.hasVideo && m.hasAudio;
        // Stereo footage is copied whole too: rendered, it would keep one eye.
        const bool part = mode != "whole" && last > first && !aTake && m.stereo.empty();
        double from = 0, to = 0;
        if (part) {
            from = wholeFile ? 0 : std::max(0.0, first - handle);
            to = wholeFile && m.duration > 0 ? m.duration : m.duration > 0 ? std::min(m.duration, last + handle) : last + handle;
        }
        // What was done to the media item's file: its clips, transcript and the rest moved to the new file's time.
        auto rebase = [&](const std::string& path, double shift, double span) -> bool {
            MediaItem fresh;
            std::string err;
            if (!probeMedia(path, fresh, &err)) return fail("Cannot read the consolidated " + m.name + ": " + err);
            m.path = path;
            m.duration = fresh.duration;
            m.videoCodec = fresh.videoCodec;
            m.audioCodec = fresh.audioCodec;
            m.proxyPath.clear();
            m.visual.reset();
            m.sounds.reset();
            m.faces.reset();
            for (double* tc : {&m.timecode, &m.ltcTimecode, &m.fileTimecode})
                if (*tc >= 0) *tc += shift;
            m.ltcChannel = -1;  // the new file's sound is what its clips played, without the LTC
            if (m.transcript) {
                auto shifted = std::make_shared<Transcript>(*m.transcript);
                std::erase_if(shifted->segments, [&](const TranscriptSegment& g) { return g.end <= shift || g.start >= shift + span; });
                for (TranscriptSegment& g : shifted->segments) {
                    g.start -= shift, g.end -= shift;
                    std::erase_if(g.words, [&](const TranscriptWord& w) { return w.end <= 0 || w.start >= span; });
                    for (TranscriptWord& w : g.words) w.start -= shift, w.end -= shift;
                }
                m.transcript = shifted;
            }
            std::erase_if(m.shots, [&](const ShotDescription& d) { return d.end <= shift || d.start >= shift + span; });
            for (ShotDescription& d : m.shots) d.start = std::max(0.0, d.start - shift), d.end = std::min(shift + span, d.end) - shift;
            for (Sequence& s : q.sequences)
                for (TrackRef r : allTracks(s))
                    for (Clip& c : trackAt(s, r)->clips) {
                        if (c.mediaId == m.id) c.sourceIn -= shift * s.fpsValue();
                        // A Dialogue Alignment made for this media: its warp is in the file's seconds too.
                        for (Effect& e : c.effects)
                            if (e.type == "dialogue_align" && e.s("media") == std::to_string(m.id)) {
                                DialogueWarp warp = dialogueWarpFromString(e.s("warp"));
                                for (AlignPoint& pt : warp) pt.u -= shift, pt.w -= shift;
                                e.strings["warp"] = dialogueWarpToString(warp);
                                double a = 0, b = 0;
                                if (std::sscanf(e.s("span").c_str(), "%lf:%lf", &a, &b) == 2) {
                                    char text[64];
                                    std::snprintf(text, sizeof text, "%.3f:%.3f", a - shift * s.fpsValue(), b - shift * s.fpsValue());
                                    e.strings["span"] = text;
                                }
                            }
                    }
            for (MediaItem& sub : q.media)
                if (sub.subclipOf == m.id) {
                    sub.subclipIn = std::max(0.0, sub.subclipIn - shift);
                    sub.subclipOut = std::max(sub.subclipIn, sub.subclipOut - shift);
                }
            res.trimmed++;
            return true;
        };
        if (part && imageSequence && mode == "trimmed") {
            // An image sequence's used frames copied as they are, numbered as they were.
            const int a = std::clamp(frames.first + int(std::floor(from * mfps + 1e-6)), frames.first, frames.last);
            const int b = std::clamp(frames.first + int(std::ceil(to * mfps - 1e-6)) - 1, a, frames.last);
            const fs::path out = uniqueIn(dir, named.empty() ? utf8(src.stem()) : movieName(utf8(src.stem())), "", taken, own);
            begin(out);
            fs::create_directories(out, ec);
            ImageSequence copy = frames;
            const std::string fileName = utf8(u8path(frames.pattern).filename());
            std::string newName = fileName;
            if (!named.empty()) {
                // (the pattern's name, the number where {frame} says or after it, the extension kept)
                const std::string ext = utf8(u8path(frames.pattern).extension());
                const QRegularExpressionMatch num = QRegularExpression(QStringLiteral("%0?\\d*d")).match(QString::fromStdString(fileName));
                const std::string number = num.hasMatch() ? num.captured(0).toStdString() : std::string("%04d");
                newName = named.find('\x01') != std::string::npos ? QString::fromStdString(named).replace(QChar('\x01'), QString::fromStdString(number)).toStdString() + ext
                                                                 : named + "." + number + ext;
            }
            copy.pattern = utf8(out / u8path(newName));
            for (int n = a; n <= b; ++n) {
                if (cancel && cancel->load()) return fail("Cancelled");
                const fs::path from1 = u8path(imageSequenceFrame(frames, n)), to1 = u8path(imageSequenceFrame(copy, n));
                if (!fs::copy_file(from1, to1, fs::copy_options::overwrite_existing, ec)) return fail("Cannot copy " + utf8(from1) + ": " + ec.message());
                res.bytes += int64_t(fs::file_size(to1, ec));
                ++res.framesCopied;
            }
            copy.first = a, copy.last = b;
            const double shift = double(a - frames.first) / mfps;
            if (!rebase(interpretedPath(imageSequencePath(copy), reading), shift, double(b - a + 1) / mfps)) return false;
            if (!keep(original, imageSequencePath(copy), "The frames used, copied by Montage")) return false;
            continue;
        }
        if (imageSequence && !part) {
            // Every frame into a folder of its own, named as they were.
            const fs::path seqFolder = uniqueIn(dir, utf8(src.stem()), "", taken, own);
            begin(seqFolder);
            const bool mine = own.count(utf8(seqFolder)) > 0;
            fs::create_directories(seqFolder, ec);
            bool wrote = false;
            for (int n = frames.first; n <= frames.last; ++n) {
                const fs::path from1 = u8path(imageSequenceFrame(frames, n)), to1 = seqFolder / from1.filename();
                // (a frame the run before had finished stays as it is)
                if (mine && alreadyWhole(from1, to1)) continue;
                if (!fs::copy_file(from1, to1, fs::copy_options::overwrite_existing, ec)) return fail("Cannot copy " + utf8(from1) + ": " + ec.message());
                res.bytes += int64_t(fs::file_size(to1, ec));
                wrote = true;
            }
            frames.pattern = utf8(seqFolder / u8path(frames.pattern).filename());
            besideToo(original, imageSequencePath(frames));
            m.path = interpretedPath(imageSequencePath(frames), reading);
            m.proxyPath.clear();
            // (counted as one file, as it is copied: passed over when the run before had finished every frame)
            ++(wrote || frames.last < frames.first ? res.copied : res.passedOver);
            continue;
        }
        const bool trimSound = part && sound && o.trimSound && !wholeFile && m.kind == MediaKind::Audio;
        if (part && (video || trimSound) && !(mode == "trimmed" && wholeFile)) {
            // Render the span through a sequence of the media's own size and rate.
            Project t = makeDefaultProject();
            Sequence& ts = *t.active();
            if (video) {
                ts.width = m.width, ts.height = m.height;
                ts.fps = m.fps.valid() ? m.fps : Rational{30, 1};
            } else {
                ts.fps = firstKept.fps;
            }
            if (const std::string cs = m.colorOverride.empty() ? m.colorSpace : m.colorOverride; !cs.empty()) ts.colorSpace = cs;
            ts.spherical = !m.projection.empty();  // 360° footage whole, not as a view
            ts.vr180 = m.projection == "vr180";
            MediaItem copy = m;
            copy.id = t.newId();
            copy.transcript.reset(), copy.visual.reset(), copy.sounds.reset(), copy.faces.reset();
            t.media.push_back(copy);
            const double fps = ts.fpsValue();
            if (!edit::placeMedia(t, ts, copy.id, 0, from * fps, to * fps, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok)
                return fail("Cannot place " + m.name + " to consolidate it");
            // How: its own format, cut where it can be without coding again; a movie; or an image sequence.
            Target tg;
            const Coding coding = mode == "trimmed" || (o.output == "movie" && o.codec.empty()) ? codingOf(mediaFileOnDisk(m.path)) : Coding{};
            if (trimSound) {
                tg.codec = "none";
                tg.audio = "pcm_s24le";
                tg.ext = ".wav";
            } else if (mode == "frames") {
                tg.frames = true;
                tg.codec = o.frames;
                tg.ext = o.frames == "tiff" ? ".tif" : "." + o.frames;
                tg.audio = "none";
            } else if (mode == "trimmed") {
                const std::string ownCodec = ownEncoder(coding);
                tg = movieTarget(ownCodec.empty() ? std::string("prores_ks") : ownCodec, coding, !ownCodec.empty());
                if (ownCodec == "libx264" || ownCodec == "libx265") tg.bitrate = coding.bitrate;
            } else if (o.output.empty()) {
                tg = movieTarget(o.codec, coding, false);  // (as Consolidate always made them)
            } else {
                const std::string ownCodec = o.codec.empty() ? ownEncoder(coding) : std::string();
                tg = movieTarget(o.codec.empty() ? (ownCodec.empty() ? std::string("prores_ks") : ownCodec) : o.codec, coding, o.codec.empty() && !ownCodec.empty());
            }
            const std::string space = trimSound ? std::string() : spaceFor(m, tg.frames && o.frames == "exr");
            // (OpenEXR frames of footage in linear light come through a sequence graded in that light, so its values
            // below 0 and over 1 are kept as they are rather than through a display's range)
            if (const ColorSpace& mine = mediaColorSpace(m); tg.frames && o.frames == "exr" && mine.sceneReferred && mine.transfer == Transfer::Linear)
                ts.workingSpace = mine.id;
            ExportSettings st;
            st.videoCodec = tg.codec;
            st.profile = tg.profile;
            st.pixFmt = tg.pixFmt;
            st.alpha = tg.alpha;
            st.crf = 16;
            st.preset = "medium";
            if (tg.bitrate > 0) st.videoBitrate = tg.bitrate;
            st.audioCodec = m.hasAudio && !tg.frames ? tg.audio : "none";
            st.colorSpace = space;
            fs::path dest;
            const std::string stem = utf8(src.stem());
            if (tg.frames) {
                dest = uniqueIn(dir, movieName(stem), "", taken, own);
                begin(dest);
                fs::create_directories(dest, ec);
                const std::string base = named.empty() ? stem : named;
                const std::string file = base.find('\x01') != std::string::npos ? QString::fromStdString(base).replace(QChar('\x01'), QStringLiteral("%04d")).toStdString() + tg.ext
                                                                               : base + ".%04d" + tg.ext;
                st.path = utf8(dest / u8path(file));
                st.startNumber = (imageSequence ? frames.first : 0) + int(std::llround(from * mfps));
            } else {
                dest = uniqueIn(dir, movieName(stem + "_trim"), tg.ext, taken, own);
                begin(dest);
                st.path = utf8(dest);
            }
            std::string err;
            int copiedFrames = 0;
            const size_t index = i;
            if (!exportSequence(
                    t, ts, st, [&](double f, FrameTime) { if (progress) progress((double(index) + f) / double(total)); }, cancel, &err,
                    nullptr, &copiedFrames))
                return fail("Consolidating " + m.name + " failed: " + err);
            const int written = int(std::llround((to - from) * fps));
            if (video) {
                res.framesCopied += copiedFrames;
                res.framesCoded += std::max(0, written - copiedFrames);
            }
            std::string path = st.path;
            if (tg.frames) {
                ImageSequence out;
                out.pattern = st.path;
                out.first = st.startNumber;
                out.last = out.first;
                while (fs::exists(u8path(imageSequenceFrame(out, out.last + 1)), ec)) ++out.last;
                out.fps = m.fps.valid() ? m.fps : ts.fps;
                path = imageSequencePath(out);
                for (int n = out.first; n <= out.last; ++n) res.bytes += int64_t(fs::file_size(u8path(imageSequenceFrame(out, n)), ec));
            } else {
                res.bytes += int64_t(fs::file_size(dest, ec));
            }
            const bool hadSound = m.hasAudio;
            if (!rebase(path, from, to - from)) return false;
            if (!keep(original, path, mode == "trimmed" ? "The part used, cut by Montage" : "The part used, transcoded by Montage")) return false;
            if (!space.empty() && !trimSound) {
                // (video made for a display, written in linear light, holds that display's light: read back as that)
                const ColorSpace* written = findColorSpace(space);
                const bool displayLight = written && written->sceneReferred && !sceneReferredWorking(ts) && !mediaColorSpace(m).sceneReferred;
                m.colorSpace = displayLight ? displayLightOf(*written).id : space;
                m.colorOverride.clear();
            }
            if (tg.frames) {
                m.hasAudio = false;
                m.audioCodec.clear();
                if (hadSound) {
                    // Its sound beside the frames, as WAV, for its sound clips.
                    const fs::path wav = uniqueIn(dir, movieName(stem), ".wav", taken, own);
                    begin(wav);
                    ExportSettings sw;
                    sw.path = utf8(wav);
                    sw.videoCodec = "none";
                    sw.audioCodec = "pcm_s24le";
                    if (!exportSequence(t, ts, sw, {}, cancel, &err)) return fail("Consolidating the sound of " + m.name + " failed: " + err);
                    MediaItem snd;
                    if (!probeMedia(sw.path, snd, &err)) return fail("Cannot read the consolidated sound of " + m.name + ": " + err);
                    snd.id = q.newId();
                    snd.name = utf8(wav.filename());
                    snd.bin = m.bin;
                    snd.metadata = m.metadata;
                    snd.timecode = m.timecode;
                    res.bytes += int64_t(fs::file_size(wav, ec));
                    for (Sequence& s : q.sequences)
                        for (Track& tr : s.audioTracks)
                            for (Clip& c : tr.clips)
                                if (c.mediaId == m.id) c.mediaId = snd.id, c.channels.clear();
                    if (!keep(original, sw.path, "Its sound, written beside its frames by Montage")) return false;
                    added.push_back(std::move(snd));
                }
            }
        } else {
            // A Photoshop layer is its file's: the file is copied once for all its layers.
            std::string file;
            int layer = -1;
            const bool psdLayer = parsePsdLayerPath(m.path, file, layer);
            if (const auto done = copiedFiles.find(utf8(src)); done != copiedFiles.end()) {
                m.path = interpretedPath(psdLayer ? psdLayerPath(done->second, layer) : done->second, reading);
                m.proxyPath.clear();
                continue;
            }
            const std::string ext = utf8(src.extension());
            // (a file copied whole keeps its name: the pattern names what is made from the footage)
            const fs::path dest = uniqueIn(dir, utf8(src.stem()), ext, taken, own);
            begin(dest);
            // (a copy the run before had finished stays as it is; one it had only begun is written over)
            const bool mine = own.count(utf8(dest)) > 0, whole = mine && alreadyWhole(src, dest);
            if (!whole && !fs::copy_file(src, dest, mine ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec))
                return fail("Cannot copy " + m.path + ": " + ec.message());
            copiedFiles[utf8(src)] = utf8(dest);
            besideToo(utf8(src), utf8(dest));
            m.path = interpretedPath(psdLayer ? psdLayerPath(utf8(dest), layer) : utf8(dest), reading);
            m.proxyPath.clear();
            if (whole) {
                res.passedOver++;
            } else {
                res.copied++;
                res.bytes += int64_t(fs::file_size(dest, ec));
            }
        }
    }
    for (MediaItem& m : added) q.media.push_back(std::move(m));
    const fs::path projectPath = folder / u8path(o.name + ".montage");
    std::string err;
    if (mode == "whole" && !o.bake) setMergeBase(q, q);  // (what it starts as, to be merged back; a consolidated copy plays other files)
    if (!saveProject(q, utf8(projectPath), &err)) return fail(err.empty() ? "Cannot save the project" : err);
    res.projectPath = utf8(projectPath);
    if (progress) progress(1.0);
    if (result) *result = res;
    return true;
}


// ---- Presets --------------------------------------------------------------------------------------------------------

namespace {

QJsonObject choicesObject(const ConsolidateOptions& o) {
    auto q = [](const std::string& s) { return QString::fromStdString(s); };
    return QJsonObject{{"trim", o.trim},
                       {"output", q(o.output)},
                       {"codec", q(o.codec)},
                       {"frames", q(o.frames)},
                       {"colorSpace", q(o.colorSpace)},
                       {"handleUnit", q(o.handleUnit)},
                       {"handles", o.handles},
                       {"handleFrames", o.handleFrames},
                       {"handleTimecode", q(o.handleTimecode)},
                       {"namePattern", q(o.namePattern)},
                       {"folderDepth", o.folderDepth},
                       {"leaveOutHidden", o.leaveOutHidden},
                       {"bake", o.bake},
                       {"trimSound", o.trimSound},
                       {"keepUnused", o.keepUnused}};
}

// A value as the compare view shows it.
std::string valueText(const QJsonValue& v) {
    if (v.isBool()) return v.toBool() ? "on" : "off";
    if (v.isDouble()) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v.toDouble());
        return buf;
    }
    if (v.isString()) return v.toString().toStdString();
    return {};
}

}  // namespace

std::string consolidateChoicesJson(const ConsolidateOptions& o) {
    return QJsonDocument(choicesObject(o)).toJson(QJsonDocument::Compact).toStdString();
}

bool applyConsolidateChoices(const std::string& json, ConsolidateOptions& o, std::string* error) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &pe);
    if (!doc.isObject()) {
        if (error) *error = "A preset is a JSON object: " + pe.errorString().toStdString();
        return false;
    }
    const QJsonObject j = doc.object();
    auto str = [&](const char* k, std::string& to) {
        if (j.value(k).isString()) to = j.value(k).toString().toStdString();
    };
    auto flag = [&](const char* k, bool& to) {
        if (j.value(k).isBool()) to = j.value(k).toBool();
    };
    flag("trim", o.trim);
    str("output", o.output);
    str("codec", o.codec);
    str("frames", o.frames);
    str("colorSpace", o.colorSpace);
    str("handleUnit", o.handleUnit);
    if (j.value("handles").isDouble()) o.handles = std::max(0.0, j.value("handles").toDouble());
    if (j.value("handleFrames").isDouble()) o.handleFrames = std::max(0, j.value("handleFrames").toInt());
    str("handleTimecode", o.handleTimecode);
    str("namePattern", o.namePattern);
    if (j.value("folderDepth").isDouble()) o.folderDepth = std::clamp(j.value("folderDepth").toInt(), 0, 64);
    flag("leaveOutHidden", o.leaveOutHidden);
    flag("bake", o.bake);
    flag("trimSound", o.trimSound);
    flag("keepUnused", o.keepUnused);
    return true;
}

const char* presetLevelName(PresetLevel level) {
    switch (level) {
    case PresetLevel::Facility: return "facility";
    case PresetLevel::User: return "user";
    case PresetLevel::Project: return "project";
    }
    return "";
}

std::vector<std::string> ConsolidatePresets::names() const {
    std::set<std::string> all;
    for (const auto& level : levels)
        for (const auto& [name, json] : level) all.insert(name);
    return {all.begin(), all.end()};
}

std::string facilityPresetFolder() { return qEnvironmentVariable("MONTAGE_FACILITY_PRESETS").toStdString(); }

std::string userPresetFolder() {
    return (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Consolidate Presets")).toStdString();
}

std::map<std::string, std::string> readPresetFolder(const std::string& folder) {
    std::map<std::string, std::string> out;
    if (folder.empty()) return out;
    const QDir dir(QString::fromStdString(folder));
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile file(f.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) out[f.completeBaseName().toStdString()] = QJsonDocument(doc.object()).toJson(QJsonDocument::Compact).toStdString();
    }
    return out;
}

bool writePreset(const std::string& folder, const std::string& name, const std::string& json, std::string* error) {
    const std::string file = safeFileName(name);
    if (folder.empty() || file != name) {
        if (error) *error = folder.empty() ? "There is no folder for the preset" : "A preset's name cannot hold / \\ : * ? \" < > | or %";
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isObject() || !QDir().mkpath(QString::fromStdString(folder))) {
        if (error) *error = doc.isObject() ? "Cannot make " + folder : std::string("A preset is a JSON object");
        return false;
    }
    QSaveFile f(QDir(QString::fromStdString(folder)).filePath(QString::fromStdString(file) + QStringLiteral(".json")));
    if (!f.open(QIODevice::WriteOnly) || f.write(doc.toJson(QJsonDocument::Indented)) < 0 || !f.commit()) {
        if (error) *error = "Cannot write the preset " + name + " in " + folder;
        return false;
    }
    return true;
}

ConsolidatePresets loadConsolidatePresets(const Project& p, const std::string& userFolder) {
    ConsolidatePresets out;
    out.levels[int(PresetLevel::Facility)] = readPresetFolder(facilityPresetFolder());
    out.levels[int(PresetLevel::User)] = readPresetFolder(userFolder);
    out.levels[int(PresetLevel::Project)] = p.consolidatePresets;
    return out;
}

bool resolvePreset(const ConsolidatePresets& presets, const std::string& name, ConsolidateOptions& base, std::map<std::string, PresetLevel>* from,
                   std::string* error) {
    bool any = false;
    for (int level = 0; level < 3; ++level) {
        const auto it = presets.levels[level].find(name);
        if (it == presets.levels[level].end()) continue;
        if (!applyConsolidateChoices(it->second, base, error)) return false;
        any = true;
        if (from)
            for (const QString& k : QJsonDocument::fromJson(QByteArray::fromStdString(it->second)).object().keys()) (*from)[k.toStdString()] = PresetLevel(level);
    }
    if (!any && error) *error = "There is no preset " + name;
    return any;
}

std::vector<PresetDifference> comparePreset(const ConsolidatePresets& presets, const std::string& name, PresetLevel level) {
    std::vector<PresetDifference> out;
    const auto mine = presets.levels[int(level)].find(name);
    if (mine == presets.levels[int(level)].end()) return out;
    const QJsonObject here = QJsonDocument::fromJson(QByteArray::fromStdString(mine->second)).object();
    // What the wider levels give without it, and which gives each choice.
    ConsolidateOptions wider;
    std::map<std::string, PresetLevel> from;
    for (int l = 0; l < int(level); ++l)
        if (const auto it = presets.levels[l].find(name); it != presets.levels[l].end()) {
            applyConsolidateChoices(it->second, wider);
            for (const QString& k : QJsonDocument::fromJson(QByteArray::fromStdString(it->second)).object().keys()) from[k.toStdString()] = PresetLevel(l);
        }
    const QJsonObject widerValues = choicesObject(wider);
    for (auto it = here.begin(); it != here.end(); ++it) {
        if (!widerValues.contains(it.key())) continue;  // (not a choice)
        const QJsonValue w = widerValues.value(it.key());
        if (valueText(w) == valueText(it.value())) continue;
        const auto f = from.find(it.key().toStdString());
        out.push_back({it.key().toStdString(), valueText(it.value()), valueText(w), f == from.end() ? std::string("default") : std::string(presetLevelName(f->second))});
    }
    return out;
}

}  // namespace montage
