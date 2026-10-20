#include "AvidMedia.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include "core/Aaf.h"
#include "media/Decoder.h"

namespace montage {

const std::vector<AvidCodec>& avidCodecs() {
    static const std::vector<AvidCodec> v = {
        {"dnxhr_lb", "DNxHR LB (offline)"},
        {"dnxhr_sq", "DNxHR SQ"},
        {"dnxhr_hq", "DNxHR HQ"},
        {"dnxhr_hqx", "DNxHR HQX (10-bit)"},
        {"dnxhr_444", "DNxHR 444 (10-bit 4:4:4)"},
        {"dnxhd_lb", "DNxHD 36 (offline, 1080 only)"},
        {"dnxhd_sq", "DNxHD 115 (1080 only)"},
        {"dnxhd_hq", "DNxHD 175 (1080 only)"},
    };
    return v;
}

namespace {

// DNxHD's bit rates (Mb/s) at 1920 x 1080 for each rate it takes: the offline, standard and high tiers.
struct DnxhdRates {
    double fps;
    int lb, sq, hq;
};
const DnxhdRates* dnxhdRates(double fps) {
    static const DnxhdRates table[] = {{24000.0 / 1001, 36, 115, 175}, {24, 36, 115, 175},   {25, 36, 120, 185},
                                       {30000.0 / 1001, 45, 145, 220}, {30, 45, 145, 220},   {50, 75, 240, 365},
                                       {60000.0 / 1001, 90, 290, 440}, {60, 90, 290, 440}};
    for (const DnxhdRates& r : table)
        if (std::fabs(r.fps - fps) < 0.01) return &r;
    return nullptr;
}

std::string tag(AVDictionary* d, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(d, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

std::string averr(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(rc, buf, sizeof buf);
    return buf;
}

}  // namespace

std::string avidCodecProblem(const std::string& codec, int width, int height, double fps) {
    const bool known = std::any_of(avidCodecs().begin(), avidCodecs().end(), [&](const AvidCodec& c) { return codec == c.id; });
    if (!known) return "Unknown coding " + codec;
    if (codec.rfind("dnxhd", 0) == 0) {
        if (width != 1920 || height != 1080) return "DNxHD is 1920 x 1080 only: choose DNxHR for a " + std::to_string(width) + " x " +
                                                    std::to_string(height) + " sequence";
        if (!dnxhdRates(fps)) return "DNxHD takes 23.976, 24, 25, 29.97, 30, 50, 59.94 or 60 frames a second: choose DNxHR";
    } else if (width < 256 || height < 120) {
        return "DNxHR needs a picture of at least 256 x 120";
    }
    return {};
}

void avidCodecSettings(const std::string& codec, double fps, std::string& profile, std::string& pixFmt, int64_t& bitrate) {
    bitrate = 0;
    if (codec.rfind("dnxhd", 0) == 0) {
        profile = "dnxhd";
        pixFmt = "yuv422p";
        if (const DnxhdRates* r = dnxhdRates(fps))
            bitrate = int64_t(codec == "dnxhd_lb" ? r->lb : codec == "dnxhd_hq" ? r->hq : r->sq) * 1000000;
        return;
    }
    profile = codec;
    pixFmt = codec == "dnxhr_444" ? "yuv444p10le" : codec == "dnxhr_hqx" ? "yuv422p10le" : "yuv422p";
}

std::string avidMediaFolder(const std::string& drive, std::string* error, std::vector<std::string>* made) {
    const QFileInfo root(QString::fromStdString(drive));
    if (!root.isDir()) {
        if (error) *error = "No such drive or folder: " + drive;
        return {};
    }
    const QString path = root.absoluteFilePath(), name = root.fileName(), parent = QFileInfo(path).dir().dirName();
    QString mxf;
    if (name.compare(QLatin1String("MXF"), Qt::CaseInsensitive) == 0 && parent.compare(QLatin1String("Avid MediaFiles"), Qt::CaseInsensitive) == 0)
        mxf = path;
    else if (name.compare(QLatin1String("Avid MediaFiles"), Qt::CaseInsensitive) == 0)
        mxf = path + "/MXF";
    else
        mxf = path + "/Avid MediaFiles/MXF";
    for (const QString& dir : {QFileInfo(mxf).absolutePath(), mxf})
        if (!QFileInfo::exists(dir)) {
            if (!QDir().mkdir(dir)) break;
            if (made) made->push_back(dir.toStdString());
        }
    // The first number free, claimed by making it (which fails if another export made it first).
    const QDir in(mxf);
    for (int n = 1; n < 100000 && in.exists(); ++n) {
        if (QFileInfo::exists(in.filePath(QString::number(n)))) continue;
        if (in.mkdir(QString::number(n))) return in.filePath(QString::number(n)).toStdString();
    }
    if (error) *error = "Cannot make a folder in " + mxf.toStdString();
    return {};
}

bool readOpAtom(const std::string& path, OpAtomFile& out, std::string* error) {
    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (error) *error = "Cannot open " + path + ": " + averr(rc);
        return false;
    }
    OpAtomFile f;
    bool ok = std::string(fmt->iformat->name) == "mxf" && fmt->nb_streams >= 1 && avformat_find_stream_info(fmt, nullptr) >= 0;
    if (ok) {
        AVStream* st = fmt->streams[0];
        const std::string pattern = tag(fmt->metadata, "operational_pattern_ul");
        f.opAtom = pattern.find("0d010201.10") != std::string::npos;
        f.picture = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        f.codec = avcodec_get_name(st->codecpar->codec_id);
        if (const char* prof = avcodec_profile_name(st->codecpar->codec_id, st->codecpar->profile)) f.profile = prof;
        f.materialPackage = aafMobIdFromUmid(tag(fmt->metadata, "material_package_umid"));
        f.filePackage = aafMobIdFromUmid(tag(st->metadata, "file_package_umid"));
        if (f.picture) {
            const AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            f.editRate = {r.num, r.den};
            f.width = st->codecpar->width;
            f.height = st->codecpar->height;
        } else {
            f.editRate = {st->codecpar->sample_rate, 1};
        }
        if (st->duration > 0 && st->time_base.num > 0 && f.editRate.num > 0)
            f.length = av_rescale_q(st->duration, st->time_base, AVRational{int(f.editRate.den), int(f.editRate.num)});
        f.timecode = tag(fmt->metadata, "timecode");
        if (f.timecode.empty()) f.timecode = tag(st->metadata, "timecode");
        f.name = tag(fmt->metadata, "material_package_name");
        f.reel = tag(st->metadata, "reel_name");
        f.tapePackage = aafMobIdFromUmid(tag(st->metadata, "reel_umid"));
    }
    avformat_close_input(&fmt);
    if (!ok) {
        if (error) *error = path + " is not an MXF file";
        return false;
    }
    out = f;
    return true;
}

bool writeOpAtomSound(const std::string& path, const AudioBuffer& buf, int channel, Rational rate, const std::string& timecode,
                      const std::string& clipName, const std::string& reel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (channel < 0 || channel >= buf.channels) return fail("No such channel");
    AVFormatContext* oc = nullptr;
    int rc = avformat_alloc_output_context2(&oc, nullptr, "mxf_opatom", path.c_str());
    if (rc < 0 || !oc) return fail("This FFmpeg cannot write MXF OP-Atom");
    struct Closer {
        AVFormatContext* oc;
        bool open = false;
        ~Closer() {
            if (open) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    } closer{oc};
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) return fail("Cannot add a stream");
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_PCM_S24LE;
    st->codecpar->sample_rate = buf.sampleRate;
    av_channel_layout_default(&st->codecpar->ch_layout, 1);
    st->codecpar->bits_per_coded_sample = 24;
    st->codecpar->block_align = 3;
    st->codecpar->bit_rate = int64_t(buf.sampleRate) * 24;
    st->time_base = {1, buf.sampleRate};
    av_opt_set_q(oc->priv_data, "mxf_audio_edit_rate", AVRational{int(rate.num), int(rate.den)}, 0);
    av_dict_set(&oc->metadata, "timecode", timecode.empty() ? "00:00:00:00" : timecode.c_str(), 0);
    if (!clipName.empty()) av_dict_set(&oc->metadata, "material_package_name", clipName.c_str(), 0);
    if (!reel.empty()) av_dict_set(&oc->metadata, "reel_name", reel.c_str(), 0);
    if ((rc = avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0) return fail("Cannot write " + path + ": " + averr(rc));
    closer.open = true;
    if ((rc = avformat_write_header(oc, nullptr)) < 0) return fail("Cannot write " + path + ": " + averr(rc));
    AVPacket* pkt = av_packet_alloc();
    const int64_t frames = buf.frames();
    const int64_t block = 4800;
    for (int64_t at = 0; at < frames && rc >= 0; at += block) {
        const int64_t n = std::min(block, frames - at);
        if ((rc = av_new_packet(pkt, int(n * 3))) < 0) break;
        for (int64_t i = 0; i < n; ++i) {
            const float v = std::clamp(buf.samples[size_t((at + i) * buf.channels + channel)], -1.0f, 1.0f);
            const int32_t s = int32_t(std::lround(double(v) * 8388607.0));
            pkt->data[3 * i] = uint8_t(s & 0xff);
            pkt->data[3 * i + 1] = uint8_t((s >> 8) & 0xff);
            pkt->data[3 * i + 2] = uint8_t((s >> 16) & 0xff);
        }
        pkt->stream_index = 0;
        pkt->pts = pkt->dts = at;
        pkt->duration = n;
        rc = av_interleaved_write_frame(oc, pkt);
    }
    av_packet_free(&pkt);
    if (rc < 0) return fail("Cannot write " + path + ": " + averr(rc));
    if ((rc = av_write_trailer(oc)) < 0) return fail("Cannot finish " + path + ": " + averr(rc));
    return true;
}

}  // namespace montage
