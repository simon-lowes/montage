#include "SegmentedExport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "core/Chapters.h"
#include "core/History.h"
#include "core/Surround.h"
#include "media/SpatialAudio.h"
#include "render/BroadcastMxf.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
}

namespace montage {

namespace {

bool fail(std::string* error, const std::string& e) {
    if (error) *error = e;
    return false;
}

std::string averr(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof buf);
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& path) { return lower(QFileInfo(QString::fromStdString(path)).suffix().toStdString()); }

FrameTime rangeIn(const ExportSettings& s) { return s.in >= 0 ? s.in : 0; }
FrameTime rangeOut(const Sequence& seq, const ExportSettings& s) {
    FrameTime out = s.out >= 0 ? s.out : seq.duration();
    if (seq.duration() > 0) out = std::min(out, seq.duration());
    return out;
}

// An image sequence's frames as FFmpeg's image writer names them (the export's: "_%06d" before the extension unless
// the path has its own pattern).
std::string framePattern(const std::string& path) {
    std::string out = path;
    if (out.find('%') == std::string::npos) {
        const size_t slash = out.find_last_of("/\\"), dot = out.find_last_of('.');
        const size_t at = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : out.size();
        out.insert(at, "_%06d");
    }
    return out;
}

// A file's bytes added to `hash`; `framed`, after its length (a frame of several in one hash: bytes moved from one
// frame's file to the next change it).
bool hashFile(const QString& path, QCryptographicHash& hash, int64_t& bytes, const std::atomic<bool>* cancel, bool framed = false) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (framed) {
        char size[8];
        for (int i = 0; i < 8; ++i) size[i] = char((uint64_t(f.size()) >> (8 * i)) & 0xff);
        hash.addData(QByteArrayView(size, 8));
    }
    QByteArray buf;
    while (!f.atEnd()) {
        if (cancel && cancel->load()) return false;
        buf = f.read(4 << 20);
        if (buf.isEmpty() && f.error() != QFileDevice::NoError) return false;
        hash.addData(buf);
        bytes += buf.size();
    }
    return true;
}

struct Input {
    AVFormatContext* fmt = nullptr;
    int v = -1;  // the video stream
    ~Input() { avformat_close_input(&fmt); }
    bool open(const std::string& path, std::string* error) {
        int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
        if (rc < 0) return fail(error, "Cannot read " + path + ": " + averr(rc));
        if ((rc = avformat_find_stream_info(fmt, nullptr)) < 0) return fail(error, "Cannot read " + path + ": " + averr(rc));
        v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        return true;
    }
};

const AVContentLightMetadata* lightLevels(const AVCodecParameters* par) {
    const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    return sd && sd->size >= int(sizeof(AVContentLightMetadata)) ? reinterpret_cast<const AVContentLightMetadata*>(sd->data) : nullptr;
}

// What a stream's decoder is set up with, to compare segments by: its extradata, but of an HEVC configuration record
// only the parameter sets (x265 puts an SEI there naming the computer's processor and threads, which differ between
// nodes and do not change how the video decodes).
QByteArray codingSettings(const AVCodecParameters* par) {
    const QByteArray all = par->extradata ? QByteArray(reinterpret_cast<const char*>(par->extradata), par->extradata_size) : QByteArray();
    if (par->codec_id != AV_CODEC_ID_HEVC || all.size() < 23 || all[0] != 1) return all;
    auto u8 = [&](qsizetype i) { return i < all.size() ? int(uint8_t(all[i])) : -1; };
    QByteArray kept = all.left(22);
    const int arrays = u8(22);
    qsizetype at = 23;
    for (int a = 0; a < arrays; ++a) {
        if (at + 3 > all.size()) return all;
        const int type = u8(at) & 0x3f, count = (u8(at + 1) << 8) | u8(at + 2);
        qsizetype end = at + 3;
        for (int n = 0; n < count; ++n) {
            if (end + 2 > all.size()) return all;
            end += 2 + ((u8(end) << 8) | u8(end + 1));
        }
        if (end > all.size()) return all;
        if (type == 32 || type == 33 || type == 34) kept += all.mid(at, end - at);  // VPS, SPS, PPS
        at = end;
    }
    return kept;
}

// An H.264 decoder configuration: an avcC record's, or Annex B's parameter sets (MPEG-TS and MXF).
struct AvcSets {
    bool annexB = false;
    uint8_t profile = 0, compat = 0, level = 0, lengths = 0;
    std::vector<std::vector<uint8_t>> sps, pps;
    std::vector<uint8_t> tail;  // (an avcC's High profile extension)
};

bool parseAvcSets(const uint8_t* d, size_t n, AvcSets& r) {
    r = {};
    if (!d || n < 4) return false;
    if (d[0] != 1) {
        // Annex B: NAL units after start codes; its sequence and picture parameter sets kept (an encoder's SEI, naming
        // its version and options, says nothing of how the video decodes).
        r.annexB = true;
        size_t i = 0;
        auto start = [&](size_t at) { return at + 3 <= n && d[at] == 0 && d[at + 1] == 0 && d[at + 2] == 1; };
        while (i < n && !start(i)) ++i;
        while (i < n) {
            const size_t from = i + 3;
            size_t end = from;
            while (end < n && !start(end)) ++end;
            size_t last = end;
            while (last > from && d[last - 1] == 0) --last;  // (a four-byte start code's leading zero; a NAL unit never ends in one)
            if (last > from) {
                const int type = d[from] & 0x1f;
                if (type == 7) r.sps.emplace_back(d + from, d + last);
                else if (type == 8) r.pps.emplace_back(d + from, d + last);
            }
            i = end;
        }
        if (r.sps.empty() || r.pps.empty() || r.sps.front().size() < 4) return false;
        r.profile = r.sps.front()[1], r.compat = r.sps.front()[2], r.level = r.sps.front()[3];
        return true;
    }
    if (n < 7) return false;
    r.profile = d[1], r.compat = d[2], r.level = d[3], r.lengths = d[4] & 3;
    size_t i = 6;
    auto sets = [&](int count, std::vector<std::vector<uint8_t>>& out) {
        for (int k = 0; k < count; ++k) {
            if (i + 2 > n) return false;
            const size_t len = size_t(d[i]) << 8 | d[i + 1];
            i += 2;
            if (len < 2 || i + len > n) return false;
            out.emplace_back(d + i, d + i + len);
            i += len;
        }
        return true;
    };
    if (!sets(d[5] & 31, r.sps) || i >= n) return false;
    const int ppsCount = d[i++];
    if (!sets(ppsCount, r.pps)) return false;
    r.tail.assign(d + i, d + n);
    return true;
}

// H.264 segments coded at different rates (two passes) have parameter sets of their own, each segment's under its own
// id (ExportSettings::parameterSetId): `next`'s merged into `merged` (the first segment's to begin with), its level the
// highest; in an avcC record they then sit side by side, in Annex B each segment's go with its pictures. False when
// they cannot share one decoder (another profile or length size, a parameter set's id held differently).
bool mergeAvcSets(AvcSets& merged, const AVCodecParameters* next) {
    // A parameter set's id: the first Exp-Golomb number after `skip` bytes (the NAL header, and an SPS's profile,
    // constraint flags and level; never two zero bytes in a row, so no emulation prevention comes before it).
    auto id = [](const std::vector<uint8_t>& nal, size_t skip) {
        size_t bit = skip * 8;
        auto at = [&](size_t b) { return b / 8 < nal.size() ? (nal[b / 8] >> (7 - b % 8)) & 1 : 1; };
        int zeros = 0;
        while (!at(bit) && zeros < 32) ++zeros, ++bit;
        ++bit;
        int value = 0;
        for (int k = 0; k < zeros; ++k) value = value << 1 | at(bit++);
        return (1 << zeros) - 1 + value;
    };
    AvcSets& a = merged;
    AvcSets b;
    if (!parseAvcSets(next->extradata, size_t(std::max(0, next->extradata_size)), b)) return false;
    if (a.annexB != b.annexB || a.profile != b.profile || a.lengths != b.lengths || a.tail != b.tail) return false;
    auto add = [&](std::vector<std::vector<uint8_t>>& have, const std::vector<std::vector<uint8_t>>& adding, size_t skip) {
        for (const auto& s : adding) {
            bool known = false;
            for (const auto& h : have) {
                if (id(h, skip) != id(s, skip)) continue;
                if (h != s) return false;
                known = true;
            }
            if (!known) have.push_back(s);
        }
        return true;
    };
    if (!add(a.sps, b.sps, 4) || !add(a.pps, b.pps, 1) || a.sps.size() > 31) return false;
    a.compat &= b.compat;
    a.level = std::max(a.level, b.level);
    return true;
}

// An avcC record of `a`'s parameter sets.
std::vector<uint8_t> avccRecord(const AvcSets& a) {
    std::vector<uint8_t> o{1, a.profile, a.compat, a.level, uint8_t(0xfc | a.lengths), uint8_t(0xe0 | a.sps.size())};
    auto put = [&](const std::vector<std::vector<uint8_t>>& sets) {
        for (const auto& s : sets) {
            o.push_back(uint8_t(s.size() >> 8)), o.push_back(uint8_t(s.size()));
            o.insert(o.end(), s.begin(), s.end());
        }
    };
    put(a.sps);
    o.push_back(uint8_t(a.pps.size()));
    put(a.pps);
    o.insert(o.end(), a.tail.begin(), a.tail.end());
    return o;
}

}  // namespace

bool exportIsImageSequence(const ExportSettings& s) {
    return s.videoCodec == "png" || s.videoCodec == "tiff" || s.videoCodec == "exr" || s.videoCodec == "dpx" || s.videoCodec == "jxl";
}

bool exportSegmentsLongGop(const ExportSettings& s) {
    return (s.videoCodec == "libx264" || s.videoCodec == "libx265" || s.videoCodec == "libsvtav1" || s.videoCodec == "mpeg2video" ||
            s.videoCodec == "hw_h264" || s.videoCodec == "hw_hevc") &&
           s.gop != 1;
}

bool exportSplits(const ExportSettings& s, std::string* why) {
    static const char* const intra[] = {"prores_ks", "dnxhd", "ffv1", "cfhd", "v210", "mjpeg"};
    static const char* const longGop[] = {"libx264", "libx265", "libsvtav1", "mpeg2video", "hw_h264", "hw_hevc"};
    const bool image = exportIsImageSequence(s);
    const bool known = image || std::find_if(std::begin(intra), std::end(intra), [&](const char* c) { return s.videoCodec == c; }) != std::end(intra) ||
                       std::find_if(std::begin(longGop), std::end(longGop), [&](const char* c) { return s.videoCodec == c; }) != std::end(longGop);
    if (s.videoCodec == "none" || s.videoCodec.empty()) return fail(why, "an export without video is made in one go");
    if (!known) return fail(why, s.videoCodec + " is not joined segment by segment (ProRes, DNxHR, FFV1, CineForm, uncompressed, Motion JPEG, "
                                                "H.264, H.265 (software or hardware), AV1, MPEG-2 and image sequences are)");
    const std::string ext = image ? std::string() : extensionOf(s.path);
    const bool mxf = ext == "mxf", ts = ext == "ts" || ext == "m2ts";
    if (!image && ext != "mov" && ext != "mp4" && ext != "m4v" && ext != "mkv" && !mxf && !ts)
        return fail(why, "segments are joined into MOV, MP4, MKV, MXF or MPEG-TS files");
    if (!s.format.empty()) return fail(why, "segments are joined into files of the kind their name says (MOV, MP4, MKV, MXF or MPEG-TS)");
    // (Matroska moves a sound-only file's timestamps to start at 0, losing the encoder's delay the picture keeps.)
    if (ext == "mkv" && !s.audioCodec.empty() && s.audioCodec != "none")
        return fail(why, "segments with sound are joined into MOV or MP4 files (MKV without sound)");
    if (!s.mxfDelivery.empty() && !mxf) return fail(why, "broadcast MXF deliveries are .mxf files");
    if ((mxf || ts) && s.embedCaptions) return fail(why, "captions inside MXF or MPEG-TS are made in one go");
    // (x265 picks each segment's level by its share of the bits, and segments at different levels cannot be joined.)
    if (s.twoPass && (s.videoCodec != "libx264" || s.videoBitrate <= 0))
        return fail(why, "two passes across segments are for H.264 (software) at a bitrate; H.265 takes two passes in one go");
    if (s.stereo == "mv") return fail(why, "spatial video is made in one go");
    if (!s.rateChange.empty()) return fail(why, "a frame rate conversion is made in one go");
    if (s.hdr10Plus || s.adaptiveHdr) return fail(why, "dynamic HDR metadata is measured over the whole range in one go");
    if (s.cea608) return fail(why, "CEA-608 captions inside the video are made in one go");
    if (!s.credentialCertificate.empty()) return fail(why, "Content Credentials sign the file made in one go");
    if (s.exrLayers) return fail(why, "OpenEXR layers from a VFX pull are made in one go");
    if (s.embedCaptions && (s.audioCodec.empty() || s.audioCodec == "none"))
        return fail(why, "embedded captions are joined with the sound: choose an audio codec, or burn the captions in");
    return true;
}

std::vector<ExportSegment> planExportSegments(const Sequence& seq, const ExportSettings& s, int n) {
    const FrameTime in = rangeIn(s), out = rangeOut(seq, s);
    std::vector<ExportSegment> parts;
    if (out <= in || n < 1) return parts;
    const FrameTime len = out - in;
    n = int(std::min<FrameTime>(n, len));
    std::vector<FrameTime> cuts;
    for (const Track& t : seq.videoTracks)
        for (const Clip& c : t.clips)
            for (const FrameTime f : {c.start, c.start + c.duration})
                if (f > in && f < out) cuts.push_back(f);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    const double share = double(len) / n;
    const FrameTime reach = FrameTime(share / 8);
    // MPEG-2's segments each a whole number of its fixed GOPs (XDCAM's 12 or 15), so the join's GOPs are all as long as
    // a single render's, but for the last.
    const FrameTime gop = s.videoCodec == "mpeg2video" && s.gop != 1 ? exportMpeg2Gop(s, seq.fps) : 1;
    FrameTime from = in;
    for (int k = 1; k <= n && from < out; ++k) {
        FrameTime to = k == n ? out : in + FrameTime(std::llround(share * k));
        if (k < n) {
            // The cut nearest the share's frame, when near enough (and leaving every segment a frame at least).
            FrameTime best = to, nearest = reach + 1;
            for (const FrameTime c : cuts)
                if (const FrameTime d = std::llabs(c - to); d < nearest) best = c, nearest = d;
            to = std::clamp(best, from + 1, out - FrameTime(n - k));
            if (gop > 1) {
                to = std::max(from + gop, in + (to - in + gop / 2) / gop * gop);
                if (to >= out) to = out;
            }
        }
        parts.push_back({from, to});
        from = to;
    }
    return parts;
}

ExportSettings segmentExportSettings(const ExportSettings& s, const ExportSegment& segment, const std::string& path, Rational fps) {
    ExportSettings x = s;
    x.in = segment.in;
    x.out = segment.out;
    // (The sound, embedded captions, chapters, timecode and reports are the join's, made for the whole range.)
    x.audioCodec = "none";
    x.extraAudio.clear();
    x.describedStream = false;
    x.monoAudioTracks = 0;
    x.loudnessTarget = 0;
    x.loudnessReport = false;
    x.embedCaptions = false;
    x.extraCaptions.clear();
    x.chapters = false;
    x.startTimecode.clear();
    // MPEG-2 states a timecode in every GOP's header: a segment's counts on from where it falls in the whole.
    if (s.videoCodec == "mpeg2video" && fps.num > 0) {
        FrameTime start = 0;
        const bool drop = s.startTimecode.find(';') != std::string::npos;
        if (!s.startTimecode.empty() && !parseTimecode(s.startTimecode, fps, start)) start = 0;
        x.startTimecode = formatTimecode(start + segment.in - rangeIn(s), fps, drop);
    }
    // A broadcast MXF delivery's segments are plain MXF, rewrapped once joined (as, interlaced, the whole one's render).
    if (!s.mxfDelivery.empty()) {
        x.mxfDelivery.clear();
        if (s.mxfDelivery == "as11-dpp") x.interlaced = true;
    }
    // (A segment's GOPs copied from the footage bring the footage's parameter sets, which the one decoder configuration
    // the join keeps would not hold for the others.)
    if (exportSegmentsLongGop(s)) x.smartRender = false;
    // A stated buffer model (or MPEG-2's constant rate): segments after the first start with the buffer lower than an
    // encoder's own start, so the join, which can only fill it (with filler data), meets each where it stands.
    if ((s.hrd == "cbr" || s.hrd == "vbr" || s.videoCodec == "mpeg2video") && segment.in > rangeIn(s) && s.vbvInit <= 0) x.vbvInit = 0.5;
    // (and one HRD for all: the whole's rate, whatever bits two passes give each segment)
    if ((s.hrd == "cbr" || s.hrd == "vbr") && x.vbvMaxrate <= 0) x.vbvMaxrate = s.videoBitrate;
    if (exportIsImageSequence(s)) x.startNumber = s.startNumber + int(segment.in - rangeIn(s));
    else x.path = path;
    return x;
}

std::vector<int64_t> twoPassBitrates(int64_t bitrate, const std::vector<ExportSegment>& parts, const std::vector<int64_t>& measured) {
    std::vector<int64_t> rates(parts.size(), bitrate);
    if (parts.empty() || measured.size() != parts.size()) return rates;
    // Each segment's need a frame (what its first pass took), against the whole's; kept within 4 times either way of the
    // average, then scaled so the frames' bits add up to the whole's budget.
    double frames = 0, need = 0;
    for (size_t k = 0; k < parts.size(); ++k) {
        frames += double(parts[k].out - parts[k].in);
        need += double(std::max<int64_t>(measured[k], 1));
    }
    if (frames <= 0) return rates;
    std::vector<double> weight(parts.size());
    double spent = 0;
    for (size_t k = 0; k < parts.size(); ++k) {
        const double len = std::max<double>(double(parts[k].out - parts[k].in), 1);
        weight[k] = std::clamp((double(std::max<int64_t>(measured[k], 1)) / len) / (need / frames), 0.25, 4.0);
        spent += weight[k] * len;
    }
    for (size_t k = 0; k < parts.size(); ++k) rates[k] = std::max<int64_t>(int64_t(std::llround(double(bitrate) * weight[k] * frames / spent)), 1000);
    return rates;
}

std::string codestreamFile(const std::string& folder, int64_t frame, bool right) {
    char name[40];
    std::snprintf(name, sizeof name, "frame-%08lld%s.j2c", static_cast<long long>(frame), right ? "-R" : "");
    return QDir(QString::fromStdString(folder)).filePath(QString::fromLatin1(name)).toStdString();
}

std::string segmentFileName(const ExportSettings& s, int index) {
    char name[32];
    std::snprintf(name, sizeof name, "segment-%03d", index);
    const std::string ext = QFileInfo(QString::fromStdString(s.path)).suffix().toStdString();
    return std::string(name) + (ext.empty() ? std::string() : "." + ext);
}

bool checkExportSegment(const ExportSettings& segment, SegmentCheck& out, std::string* error, const std::atomic<bool>* cancel,
                        const std::function<void(double)>& progress) {
    out = {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const FrameTime frames = segment.out - segment.in;
    if (exportIsImageSequence(segment)) {
        // Each frame's file, in order: counted while they are there, hashed one after another.
        const std::string pattern = framePattern(segment.path);
        for (FrameTime i = 0; i < frames; ++i) {
            char name[4096];
            if (av_get_frame_filename2(name, sizeof name, pattern.c_str(), segment.startNumber + int(i), 0) < 0)
                return fail(error, "The file name " + segment.path + " has no frame number");
            if (!QFileInfo::exists(QString::fromUtf8(name))) break;
            if (!hashFile(QString::fromUtf8(name), hash, out.bytes, cancel, true))
                return fail(error, cancel && cancel->load() ? std::string("Cancelled") : "Cannot read " + std::string(name));
            ++out.frames;
            if (progress && i % 16 == 0) progress(double(i) / double(frames));
        }
    } else {
        Input in;
        if (!in.open(segment.path, error)) return false;
        const int v = in.v;
        if (v < 0) return fail(error, segment.path + " has no video");
        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(in.fmt, pkt) >= 0) {
            out.frames += pkt->stream_index == v;
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        if (!hashFile(QString::fromStdString(segment.path), hash, out.bytes, cancel))
            return fail(error, cancel && cancel->load() ? std::string("Cancelled") : "Cannot read " + segment.path);
    }
    out.sha256 = hash.result().toHex().toStdString();
    if (progress) progress(1);
    return true;
}

bool checkCodestreams(const std::string& folder, int64_t from, int64_t to, bool stereo, SegmentCheck& out, std::string* error,
                      const std::atomic<bool>* cancel, const std::function<void(double)>& progress) {
    out = {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int64_t k = from; k < to; ++k) {
        for (int eye = 0; eye < (stereo ? 2 : 1); ++eye) {
            const QString file = QString::fromStdString(codestreamFile(folder, k, eye == 1));
            if (!QFileInfo::exists(file)) {
                if (progress) progress(1);
                out.sha256 = hash.result().toHex().toStdString();
                return true;
            }
            if (!hashFile(file, hash, out.bytes, cancel, true))
                return fail(error, cancel && cancel->load() ? std::string("Cancelled") : "Cannot read " + file.toStdString());
        }
        ++out.frames;
        if (progress && (k - from) % 16 == 0) progress(double(k - from) / double(to - from));
    }
    out.sha256 = hash.result().toHex().toStdString();
    if (progress) progress(1);
    return true;
}

bool joinExportSegments(const Project& p, const Sequence& seq, const ExportSettings& s, int stems, const std::vector<std::string>& files,
                        const std::vector<ExportSegment>& parts, const ExportProgress& progress, const std::atomic<bool>* cancel,
                        std::string* error, HrdReport* hrd) {
    if (hrd) *hrd = HrdReport{};
    bool keptHrd = false;
    if (parts.empty()) return fail(error, "No segments to join");
    const FrameTime in = parts.front().in, out = parts.back().out;
    const bool wantAudio = !s.audioCodec.empty() && s.audioCodec != "none";
    const bool image = exportIsImageSequence(s);
    const double stemShare = stems ? 0.3 : 0;
    auto part = [&progress](double from, double share) {
        return ExportProgress([&progress, from, share](double f, FrameTime t) {
            if (progress) progress(from + f * share, t);
        });
    };
    if (!image) {
        if (files.size() != parts.size()) return fail(error, "A segment's file is missing");
        // MXF holds no file of sound alone, and MPEG-TS moves a sound file's start: the sound for those is rendered into
        // MOV (still named for the output). A broadcast MXF delivery is joined into plain OP1a beside it, then rewrapped.
        const std::string ext = extensionOf(s.path);
        const bool soundInMov = ext == "mxf" || ext == "ts" || ext == "m2ts";
        const bool delivery = !s.mxfDelivery.empty();
        BroadcastMxfOptions deliver;
        if (delivery) {
            ExportSettings whole = s;
            whole.in = in;
            whole.out = out;
            if (!broadcastMxfJoinOptions(seq, whole, deliver, error)) return false;
        }
        const std::string target = delivery ? s.path + ".render.mxf" : s.path;
        struct RemoveStaged {
            std::string path;
            ~RemoveStaged() {
                if (!path.empty()) QFile::remove(QString::fromStdString(path));
            }
        } removeStaged{delivery ? target : std::string()};
        // The sound (and embedded captions, the reports beside it) for the whole range, in the output's own kind of
        // file: rendered where the output goes (so what goes beside it is named for it), then put aside for the join.
        std::string soundPath;
        struct Remove {
            std::string& path;
            ~Remove() {
                if (!path.empty()) QFile::remove(QString::fromStdString(path));
            }
        } removeSound{soundPath};
        const double soundShare = wantAudio ? 0.4 * (1 - stemShare) : 0;
        if (wantAudio) {
            ExportSettings a = s;
            a.videoCodec = "none";
            a.in = in;
            a.out = out;
            a.chapters = false;       // (the join's)
            a.startTimecode.clear();  // (stated with the video)
            a.burnIn = {};
            a.mxfDelivery.clear();
            a.twoPass = false;  // (the sound alone: one pass)
            a.pass = 0;
            a.passStats.clear();
            if (soundInMov) a.format = "mov";
            std::string err;
            if (!exportSequence(p, seq, a, part(0, soundShare), cancel, &err)) return fail(error, "The sound: " + err);
            const QFileInfo made(QString::fromStdString(s.path));
            const QString aside = made.dir().filePath(QStringLiteral(".montage-join-sound-") +
                                                     QString::number(QRandomGenerator::global()->generate64(), 16) + "." + made.suffix());
            if (!QFile::rename(made.filePath(), aside)) return fail(error, "Cannot put the sound aside in " + made.path().toStdString());
            soundPath = aside.toStdString();
        }

        // The segments, read before anything is written: each must be coded as the first is (one encoder's, one set of
        // settings) for their video to follow on in one stream.
        std::vector<std::unique_ptr<Input>> segs;
        int maxCll = -1, maxFall = -1;
        AvcSets avc;  // (the first segment's decoder configuration, merged with the others' when H.264's differ)
        bool firstAvc = false, mergedAvcc = false;
        for (size_t k = 0; k < files.size(); ++k) {
            auto inp = std::make_unique<Input>();
            if (!inp->open(files[k], error)) return false;
            const int v = inp->v;
            if (v < 0) return fail(error, "Segment " + std::to_string(k + 1) + " has no video");
            const AVCodecParameters* par = inp->fmt->streams[v]->codecpar;
            if (k == 0 && par->codec_id == AV_CODEC_ID_H264) firstAvc = parseAvcSets(par->extradata, size_t(std::max(0, par->extradata_size)), avc);
            if (k > 0) {
                const AVCodecParameters* first = segs[0]->fmt->streams[segs[0]->v]->codecpar;
                bool same = par->codec_id == first->codec_id && par->width == first->width && par->height == first->height &&
                            par->format == first->format;
                // (H.264 parameter sets that differ, each under its own id: side by side in one avcC; in Annex B (MPEG-TS,
                // MXF) each segment's go with its pictures. One id held differently, or another profile, is another
                // encoder's or settings'.)
                if (same && codingSettings(par) != codingSettings(first)) {
                    same = par->codec_id == AV_CODEC_ID_H264 && firstAvc && mergeAvcSets(avc, par);
                    mergedAvcc |= same && !avc.annexB;
                }
                if (!same)
                    return fail(error, "Segment " + std::to_string(k + 1) +
                                           " was coded differently from the first (another version of the encoder?): it cannot be joined to it");
            }
            if (const AVContentLightMetadata* cl = lightLevels(par)) {
                maxCll = std::max(maxCll, int(cl->MaxCLL));
                maxFall = std::max(maxFall, int(cl->MaxFALL));
            }
            segs.push_back(std::move(inp));
        }
        Input sound;
        if (!soundPath.empty() && !sound.open(soundPath, error)) return false;

        AVFormatContext* oc = nullptr;
        int rc = avformat_alloc_output_context2(&oc, nullptr, delivery ? "mxf" : nullptr, target.c_str());
        if (rc < 0 || !oc) return fail(error, "Unknown output format for " + s.path);
        struct Close {
            AVFormatContext*& oc;
            bool opened = false;
            ~Close() {
                if (opened) avio_closep(&oc->pb);
                avformat_free_context(oc);
            }
        } close{oc};
        bool written = false;
        auto drop = [&](const std::string& e) {
            if (close.opened) {
                avio_closep(&oc->pb);
                close.opened = false;
            }
            if (written) QFile::remove(QString::fromStdString(target));
            return fail(error, e);
        };
        const AVStream* firstVideo = segs[0]->fmt->streams[segs[0]->v];
        AVStream* vst = avformat_new_stream(oc, nullptr);
        if (!vst || avcodec_parameters_copy(vst->codecpar, firstVideo->codecpar) < 0) return drop("Out of memory");
        if (mergedAvcc) {
            const std::vector<uint8_t> avcc = avccRecord(avc);
            av_freep(&vst->codecpar->extradata);
            vst->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!vst->codecpar->extradata) return drop("Out of memory");
            std::copy(avcc.begin(), avcc.end(), vst->codecpar->extradata);
            vst->codecpar->extradata_size = int(avcc.size());
        }
        vst->time_base = firstVideo->time_base;
        // (MOV's demuxer keeps the pixel aspect on the stream; its muxer writes it from the parameters.)
        vst->sample_aspect_ratio = firstVideo->sample_aspect_ratio;
        if (!vst->codecpar->sample_aspect_ratio.num) vst->codecpar->sample_aspect_ratio = firstVideo->sample_aspect_ratio;
        vst->avg_frame_rate = firstVideo->avg_frame_rate;
        vst->r_frame_rate = firstVideo->r_frame_rate;
        vst->disposition = firstVideo->disposition;
        av_dict_copy(&vst->metadata, firstVideo->metadata, 0);
        if (maxCll >= 0)
            if (const AVContentLightMetadata* cl = lightLevels(vst->codecpar)) {
                auto* w = const_cast<AVContentLightMetadata*>(cl);
                w->MaxCLL = unsigned(maxCll);
                w->MaxFALL = unsigned(maxFall);
            }
        // The sound file's streams after the video, as the export orders them (the mix, other streams, captions).
        std::vector<int> soundMap(sound.fmt ? sound.fmt->nb_streams : 0, -1);
        for (unsigned i = 0; sound.fmt && i < sound.fmt->nb_streams; ++i) {
            const AVStream* ist = sound.fmt->streams[i];
            const AVMediaType type = ist->codecpar->codec_type;
            // (A MOV's chapter track is read as chapters and its stream left out: the join writes its own.)
            if (ist->discard == AVDISCARD_ALL || (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_SUBTITLE)) continue;
            AVStream* ost = avformat_new_stream(oc, nullptr);
            if (!ost || avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) return drop("Out of memory");
            ost->time_base = ist->time_base;
            ost->disposition = ist->disposition;
            av_dict_copy(&ost->metadata, ist->metadata, 0);
            soundMap[i] = ost->index;
        }
        av_dict_set(&oc->metadata, "encoder", "Montage", 0);
        if (!s.startTimecode.empty()) av_dict_set(&oc->metadata, "timecode", s.startTimecode.c_str(), 0);
        for (const auto& [key, value] : s.formatMetadata) av_dict_set(&oc->metadata, key.c_str(), value.c_str(), 0);
        if (s.chapters) {
            const std::vector<Chapter> chapters = chaptersOf(seq, in, out);
            if (!chapters.empty()) {
                oc->chapters = static_cast<AVChapter**>(av_calloc(chapters.size(), sizeof(AVChapter*)));
                for (size_t i = 0; oc->chapters && i < chapters.size(); ++i) {
                    auto* ch = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
                    if (!ch) break;
                    ch->id = int64_t(i) + 1;
                    ch->time_base = AVRational{int(seq.fps.den), int(seq.fps.num)};
                    ch->start = chapters[i].start;
                    ch->end = chapters[i].end;
                    av_dict_set(&ch->metadata, "title", chapters[i].title.c_str(), 0);
                    oc->chapters[oc->nb_chapters++] = ch;
                }
            }
        }
        if ((rc = avio_open(&oc->pb, target.c_str(), AVIO_FLAG_WRITE)) < 0) return drop("Cannot write " + target + ": " + averr(rc));
        close.opened = written = true;
        if ((rc = avformat_write_header(oc, nullptr)) < 0) return drop("Cannot write header: " + averr(rc));

        // The video, segment after segment, each moved to where its first frame falls; the sound file's packets between
        // them in time.
        const AVRational frameTb{int(seq.fps.den), int(seq.fps.num)};
        const int64_t total = out - in;
        int64_t copied = 0;
        size_t seg = 0;
        AVPacket* vpkt = av_packet_alloc();
        AVPacket* apkt = av_packet_alloc();
        struct Free {
            AVPacket*& a;
            AVPacket*& b;
            ~Free() {
                av_packet_free(&a);
                av_packet_free(&b);
            }
        } freePackets{vpkt, apkt};
        bool haveV = false, haveA = false, endA = !sound.fmt;
        auto shift = [](int64_t ts, int64_t start, AVRational from, AVRational to, int64_t offset) {
            return ts == AV_NOPTS_VALUE ? ts : av_rescale_q(ts - start, from, to) + offset;
        };
        auto nextVideo = [&]() -> bool {
            while (seg < segs.size()) {
                AVFormatContext* f = segs[seg]->fmt;
                const int v = segs[seg]->v;
                const AVStream* ist = f->streams[v];
                if (av_read_frame(f, vpkt) < 0) {
                    ++seg;
                    continue;
                }
                if (vpkt->stream_index != v) {
                    av_packet_unref(vpkt);
                    continue;
                }
                const int64_t start = ist->start_time != AV_NOPTS_VALUE ? ist->start_time : 0;
                const int64_t offset = av_rescale_q(parts[seg].in - in, frameTb, vst->time_base);
                vpkt->pts = shift(vpkt->pts, start, ist->time_base, vst->time_base, offset);
                vpkt->dts = shift(vpkt->dts, start, ist->time_base, vst->time_base, offset);
                vpkt->duration = av_rescale_q(vpkt->duration, ist->time_base, vst->time_base);
                vpkt->stream_index = vst->index;
                vpkt->pos = -1;
                return true;
            }
            return false;
        };
        auto nextSound = [&]() -> bool {
            while (av_read_frame(sound.fmt, apkt) >= 0) {
                const int to = apkt->stream_index < int(soundMap.size()) ? soundMap[size_t(apkt->stream_index)] : -1;
                if (to < 0) {
                    av_packet_unref(apkt);
                    continue;
                }
                const AVStream* ist = sound.fmt->streams[apkt->stream_index];
                const AVRational tb = oc->streams[to]->time_base;
                apkt->pts = shift(apkt->pts, 0, ist->time_base, tb, 0);
                apkt->dts = shift(apkt->dts, 0, ist->time_base, tb, 0);
                apkt->duration = av_rescale_q(apkt->duration, ist->time_base, tb);
                apkt->stream_index = to;
                apkt->pos = -1;
                return true;
            }
            return false;
        };
        auto when = [](const AVPacket* pkt) { return pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts; };
        // The buffer model, one across the joins: each segment's buffering periods set where the buffer stands, its
        // picture timing counted on, filler before a join to meet what the next segment asks for (a packet held until
        // the next is read).
        HrdSplicer splicer;
        {
            const AVCodecParameters* par = vst->codecpar;
            const double fps = av_q2d(AVRational{int(seq.fps.num), int(seq.fps.den)});
            if (par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC) {
                int lengthSize = 0;
                const auto sets = hrd::parameterSets(par->codec_id, par->extradata, size_t(std::max(0, par->extradata_size)), lengthSize);
                splicer.start(par->codec_id, sets, lengthSize, fps, par->video_delay > 0 ? par->video_delay : 2);
            }
            keptHrd = splicer.active();
        }
        // (MPEG-2, XDCAM HD422 at its constant rate: the rate and buffer its first sequence header states)
        const bool mpeg2 = vst->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO && s.videoCodec == "mpeg2video";
        AVPacket* held = keptHrd || mpeg2 ? av_packet_alloc() : nullptr;
        struct FreeHeld {
            AVPacket*& p;
            ~FreeHeld() { av_packet_free(&p); }
        } freeHeld{held};
        bool holding = false;
        int64_t decoded = 0;
        auto writeVideo = [&](AVPacket* pkt) -> int {
            if (mpeg2 && decoded == 0) {
                double rate = 0, buffer = 0;
                if (hrd::mpeg2Rate(pkt->data, size_t(pkt->size), rate, buffer))
                    keptHrd = splicer.start(AV_CODEC_ID_MPEG2VIDEO, {}, 0, av_q2d(AVRational{int(seq.fps.num), int(seq.fps.den)}), 2, rate, buffer);
            }
            ++decoded;
            if (!keptHrd) return av_interleaved_write_frame(oc, pkt);
            const int64_t display = av_rescale_q(pkt->pts, vst->time_base, frameTb);
            int64_t shown = 0;
            std::vector<uint8_t> before = splicer.push(std::vector<uint8_t>(pkt->data, pkt->data + pkt->size), display, false, true, &shown);
            int r = 0;
            if (holding) {
                AVPacket* outp = av_packet_alloc();
                if (!outp || av_new_packet(outp, int(before.size())) < 0) {
                    av_packet_free(&outp);
                    return AVERROR(ENOMEM);
                }
                std::copy(before.begin(), before.end(), outp->data);
                av_packet_copy_props(outp, held);
                r = av_interleaved_write_frame(oc, outp);
                av_packet_free(&outp);
            }
            av_packet_unref(held);
            av_packet_move_ref(held, pkt);
            holding = true;
            return r;
        };
        const double rewrapShare = delivery ? 0.05 * (1 - stemShare) : 0;
        const double videoFrom = soundShare, videoShare = 1 - stemShare - soundShare - rewrapShare;
        for (;;) {
            if (cancel && cancel->load()) return drop("Cancelled");
            if (!haveV) haveV = nextVideo();
            if (!haveA && !endA) endA = !(haveA = nextSound());
            if (!haveV && !haveA) break;
            const bool video = haveV && (!haveA || av_compare_ts(when(vpkt), vst->time_base, when(apkt), oc->streams[apkt->stream_index]->time_base) <= 0);
            if (video) {
                haveV = false;
                ++copied;
                if ((rc = writeVideo(vpkt)) < 0) return drop("Writing the joined video failed: " + averr(rc));
                if (progress && copied % 25 == 0) progress(videoFrom + videoShare * double(copied) / double(total), in + copied);
            } else {
                haveA = false;
                if ((rc = av_interleaved_write_frame(oc, apkt)) < 0) return drop("Writing the sound failed: " + averr(rc));
            }
        }
        if (holding) {
            int64_t shown = 0;
            std::vector<uint8_t> last = splicer.finish(&shown);
            AVPacket* outp = av_packet_alloc();
            if (!outp || av_new_packet(outp, int(last.size())) < 0) {
                av_packet_free(&outp);
                return drop("Out of memory");
            }
            std::copy(last.begin(), last.end(), outp->data);
            av_packet_copy_props(outp, held);
            rc = av_interleaved_write_frame(oc, outp);
            av_packet_free(&outp);
            if (rc < 0) return drop("Writing the joined video failed: " + averr(rc));
        }
        if (copied != total)
            return drop("The segments hold " + std::to_string(copied) + " frames, not the " + std::to_string(total) + " of the range");
        if ((rc = av_write_trailer(oc)) < 0) return drop("Cannot finalise file: " + averr(rc));
        if (oc->pb->error < 0) return drop("Writing the file failed: " + averr(oc->pb->error));
        avio_closep(&oc->pb);
        close.opened = false;
        if (delivery) {
            std::string why;
            const double from = 1 - stemShare - rewrapShare;
            const bool rewrapped = rewrapBroadcastMxf(target, s.path, deliver, &why, [&](double f) {
                if (progress) progress(from + rewrapShare * f, in + FrameTime(std::lround(f * double(out - in))));
                return !(cancel && cancel->load());
            });
            if (!rewrapped) return fail(error, why);
        }
        // An ambisonic mix: its Spatial Audio box again (the join wrote the stream without it).
        const std::string layout = s.downmixStereo || s.monoAudioTracks > 0 ? seq.audioLayout : exportAudioLayout(seq.audioLayout, s.audioCodec);
        if (wantAudio && ambisonicLayout(layout) && !s.downmixStereo) {
            const std::string ext = extensionOf(s.path);
            std::string why;
            if ((ext == "mp4" || ext == "mov" || ext == "m4v") && !writeSpatialAudioBox(s.path, ambisonicOrder(layout), &why))
                qWarning("Spatial audio metadata not added to %s: %s", s.path.c_str(), why.c_str());
        }
    }
    if (stems) {
        ExportSettings whole = s;
        whole.in = in;
        whole.out = out;
        std::string err;
        if (!exportStems(p, seq, whole, stems, nullptr, part(1 - stemShare, stemShare), cancel, &err)) return fail(error, err);
    }
    // The joined file against its buffer model (warned about, not failed: see exportNeedsHrdCheck).
    if (!image && (keptHrd || exportNeedsHrdCheck(s, 0))) {
        const HrdReport r = checkHrd(s.path, cancel);
        if (r.checked && r.signalled && !r.conforms())
            qWarning("The joined export %s does not keep its buffer model: %s", s.path.c_str(), r.summary().c_str());
        if (hrd) *hrd = r;
    }
    if (progress) progress(1, out);
    return true;
}

}  // namespace montage
