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
#include "core/Surround.h"
#include "media/SpatialAudio.h"

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

bool hashFile(const QString& path, QCryptographicHash& hash, int64_t& bytes, const std::atomic<bool>* cancel) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
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

}  // namespace

bool exportIsImageSequence(const ExportSettings& s) {
    return s.videoCodec == "png" || s.videoCodec == "tiff" || s.videoCodec == "exr" || s.videoCodec == "dpx";
}

bool exportSegmentsLongGop(const ExportSettings& s) {
    return (s.videoCodec == "libx264" || s.videoCodec == "libx265" || s.videoCodec == "libsvtav1") && s.gop != 1;
}

bool exportSplits(const ExportSettings& s, std::string* why) {
    static const char* const intra[] = {"prores_ks", "dnxhd", "ffv1", "cfhd", "v210", "mjpeg"};
    static const char* const longGop[] = {"libx264", "libx265", "libsvtav1"};
    const bool image = exportIsImageSequence(s);
    const bool known = image || std::find_if(std::begin(intra), std::end(intra), [&](const char* c) { return s.videoCodec == c; }) != std::end(intra) ||
                       std::find_if(std::begin(longGop), std::end(longGop), [&](const char* c) { return s.videoCodec == c; }) != std::end(longGop);
    if (s.videoCodec == "none" || s.videoCodec.empty()) return fail(why, "an export without video is made in one go");
    if (s.videoCodec == "hw_h264" || s.videoCodec == "hw_hevc")
        return fail(why, "each computer's hardware encoder codes differently: choose H.264 or H.265 (software)");
    if (!known) return fail(why, s.videoCodec + " is not joined segment by segment (ProRes, DNxHR, FFV1, CineForm, uncompressed, Motion JPEG, "
                                                "H.264, H.265, AV1 and image sequences are)");
    if (!image) {
        const std::string ext = extensionOf(s.path);
        if (ext != "mov" && ext != "mp4" && ext != "m4v" && ext != "mkv") return fail(why, "segments are joined into MOV, MP4 or MKV files");
    }
    if (!s.format.empty()) return fail(why, "segments are joined into MOV, MP4 or MKV files");
    // (Matroska moves a sound-only file's timestamps to start at 0, losing the encoder's delay the picture keeps.)
    if (!image && extensionOf(s.path) == "mkv" && !s.audioCodec.empty() && s.audioCodec != "none")
        return fail(why, "segments with sound are joined into MOV or MP4 files (MKV without sound)");
    if (!s.mxfDelivery.empty()) return fail(why, "broadcast MXF is made in one go");
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
    FrameTime from = in;
    for (int k = 1; k <= n; ++k) {
        FrameTime to = k == n ? out : in + FrameTime(std::llround(share * k));
        if (k < n) {
            // The cut nearest the share's frame, when near enough (and leaving every segment a frame at least).
            FrameTime best = to, nearest = reach + 1;
            for (const FrameTime c : cuts)
                if (const FrameTime d = std::llabs(c - to); d < nearest) best = c, nearest = d;
            to = std::clamp(best, from + 1, out - FrameTime(n - k));
        }
        parts.push_back({from, to});
        from = to;
    }
    return parts;
}

ExportSettings segmentExportSettings(const ExportSettings& s, const ExportSegment& segment, const std::string& path) {
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
    if (exportIsImageSequence(s)) x.startNumber = s.startNumber + int(segment.in - rangeIn(s));
    else x.path = path;
    return x;
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
            if (!hashFile(QString::fromUtf8(name), hash, out.bytes, cancel))
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

bool joinExportSegments(const Project& p, const Sequence& seq, const ExportSettings& s, int stems, const std::vector<std::string>& files,
                        const std::vector<ExportSegment>& parts, const ExportProgress& progress, const std::atomic<bool>* cancel,
                        std::string* error) {
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
        for (size_t k = 0; k < files.size(); ++k) {
            auto inp = std::make_unique<Input>();
            if (!inp->open(files[k], error)) return false;
            const int v = inp->v;
            if (v < 0) return fail(error, "Segment " + std::to_string(k + 1) + " has no video");
            const AVCodecParameters* par = inp->fmt->streams[v]->codecpar;
            if (k > 0) {
                const AVCodecParameters* first = segs[0]->fmt->streams[segs[0]->v]->codecpar;
                const bool same = par->codec_id == first->codec_id && par->width == first->width && par->height == first->height &&
                                  par->format == first->format && codingSettings(par) == codingSettings(first);
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
        int rc = avformat_alloc_output_context2(&oc, nullptr, nullptr, s.path.c_str());
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
            if (written) QFile::remove(QString::fromStdString(s.path));
            return fail(error, e);
        };
        const AVStream* firstVideo = segs[0]->fmt->streams[segs[0]->v];
        AVStream* vst = avformat_new_stream(oc, nullptr);
        if (!vst || avcodec_parameters_copy(vst->codecpar, firstVideo->codecpar) < 0) return drop("Out of memory");
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
        if ((rc = avio_open(&oc->pb, s.path.c_str(), AVIO_FLAG_WRITE)) < 0) return drop("Cannot write " + s.path + ": " + averr(rc));
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
        const double videoFrom = soundShare, videoShare = 1 - stemShare - soundShare;
        for (;;) {
            if (cancel && cancel->load()) return drop("Cancelled");
            if (!haveV) haveV = nextVideo();
            if (!haveA && !endA) endA = !(haveA = nextSound());
            if (!haveV && !haveA) break;
            const bool video = haveV && (!haveA || av_compare_ts(when(vpkt), vst->time_base, when(apkt), oc->streams[apkt->stream_index]->time_base) <= 0);
            if (video) {
                haveV = false;
                ++copied;
                if ((rc = av_interleaved_write_frame(oc, vpkt)) < 0) return drop("Writing the joined video failed: " + averr(rc));
                if (progress && copied % 25 == 0) progress(videoFrom + videoShare * double(copied) / double(total), in + copied);
            } else {
                haveA = false;
                if ((rc = av_interleaved_write_frame(oc, apkt)) < 0) return drop("Writing the sound failed: " + averr(rc));
            }
        }
        if (copied != total)
            return drop("The segments hold " + std::to_string(copied) + " frames, not the " + std::to_string(total) + " of the range");
        if ((rc = av_write_trailer(oc)) < 0) return drop("Cannot finalise file: " + averr(rc));
        if (oc->pb->error < 0) return drop("Writing the file failed: " + averr(oc->pb->error));
        avio_closep(&oc->pb);
        close.opened = false;
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
    if (progress) progress(1, out);
    return true;
}

}  // namespace montage
