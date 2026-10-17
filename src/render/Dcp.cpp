#include "Dcp.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include "ColorSpace.h"
#include "Compositor.h"
#include "DcpMxf.h"
#include "DcpSecurity.h"
#include "Jpeg2000.h"
#include "PackageFiles.h"
#include "core/Captions.h"
#include "core/Surround.h"
#include "media/SuperScale.h"

namespace montage {

namespace {

constexpr int kSampleRate = 48000;

QString urn(const dcp::Uuid& u) { return QStringLiteral("urn:uuid:") + QString::fromStdString(dcp::uuidString(u)); }

QString isoNow() { return packageTimestamp(); }

QString fileHash(const QString& path, qint64* size = nullptr, const std::function<bool(qint64)>& read = {}) {
    return packageFileHash(path, size, read);
}

// ---- Picture: sequence colour to DCI X'Y'Z' -------------------------------------------------------------------------

struct XyzConverter {
    std::vector<float> toLinear;  // code value (65536 steps over 0..1) to display light
    double m[9];                  // linear RGB to CIE XYZ (D65 white kept)
    explicit XyzConverter(const ColorSpace& cs) {
        toLinear.resize(65536);
        for (int i = 0; i < 65536; ++i) toLinear[size_t(i)] = float(std::clamp(montage::toLinear(cs.transfer, i / 65535.0), 0.0, 1.0));
        primariesToXyz(cs.primaries, m);
    }
    // DCI: 48 cd/m^2 white scaled to 52.37 so equal-energy white fits, a 2.6 power, 12 bits.
    static uint16_t code(double v) {
        const double n = std::clamp(v * (48.0 / 52.37), 0.0, 1.0);
        return uint16_t(std::lround(4095.0 * std::pow(n, 1.0 / 2.6)));
    }
    void convert(const Image& img, std::vector<uint16_t>& out) const {
        out.resize(size_t(img.width) * size_t(img.height) * 3);
        for (int y = 0; y < img.height; ++y) {
            const float* s = img.row(y);
            uint16_t* d = out.data() + size_t(y) * size_t(img.width) * 3;
            for (int x = 0; x < img.width; ++x, s += 4, d += 3) {
                auto lin = [&](float v) { return double(toLinear[size_t(std::clamp(int(std::lround(double(v) * 65535.0)), 0, 65535))]); };
                const double r = lin(s[0]), g = lin(s[1]), b = lin(s[2]);
                d[0] = code(m[0] * r + m[1] * g + m[2] * b);
                d[1] = code(m[3] * r + m[4] * g + m[5] * b);
                d[2] = code(m[6] * r + m[7] * g + m[8] * b);
            }
        }
    }
};

// ---- JPEG 2000 ----------------------------------------------------------------------------------------------------

struct J2kEncoder {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    bool openjpeg = false;
    // OpenJPEG itself when Montage is built with it (render/Jpeg2000.h), else FFmpeg's libopenjpeg wrapper or, failing
    // that, FFmpeg's own encoder (not in the DCI profile).
    bool direct = false;
    int w_ = 0, h_ = 0, fps_ = 24;
    bool stereo_ = false;  // two codestreams to a frame, sharing its 250 Mbit/s
    bool fourK() const { return w_ > 2048; }  // the 4K containers are wider than any 2K one
    ~J2kEncoder() {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
    }
    bool open(int w, int h, int fps, bool stereo, std::string* error) {
        w_ = w, h_ = h, fps_ = fps, stereo_ = stereo;
        if (openJpegAvailable()) {
            direct = openjpeg = true;
            return true;
        }
        const AVCodec* codec = avcodec_find_encoder_by_name("libopenjpeg");
        openjpeg = codec != nullptr;
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_JPEG2000);
        if (!codec) {
            if (error) *error = "This FFmpeg has no JPEG 2000 encoder";
            return false;
        }
        ctx = avcodec_alloc_context3(codec);
        ctx->width = w;
        ctx->height = h;
        ctx->time_base = AVRational{1, fps};
        ctx->framerate = AVRational{fps, 1};
        ctx->thread_count = 1;  // frames are encoded side by side instead
        // X'Y'Z' as it is, when the encoder takes it; else as RGB (the numbers are the same).
        ctx->pix_fmt = AV_PIX_FMT_RGB48LE;
        const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        const void* cfg = nullptr;
        if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, nullptr) >= 0)
            formats = static_cast<const AVPixelFormat*>(cfg);
#else
        formats = codec->pix_fmts;
#endif
        if (formats)
            for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
                if (*f == AV_PIX_FMT_XYZ12LE) ctx->pix_fmt = AV_PIX_FMT_XYZ12LE;
        av_opt_set(ctx->priv_data, "format", "j2k", 0);  // a bare codestream, not a JP2 file
        if (openjpeg) {
            // The DCI 2K or 4K profile; the cinema mode keeps each frame within the 250 Mbit/s limit (the 48 fps
            // one, half the size, for every rate above 24; 4K has only the one).
            if (fourK()) {
                if (av_opt_set(ctx->priv_data, "profile", "cinema4k", 0) < 0) ctx->profile = 4;  // FF_PROFILE_JPEG2000_DCINEMA_4K
                av_opt_set(ctx->priv_data, "cinema_mode", "4k_24", 0);
            } else {
                if (av_opt_set(ctx->priv_data, "profile", "cinema2k", 0) < 0) ctx->profile = 3;  // FF_PROFILE_JPEG2000_DCINEMA_2K
                av_opt_set(ctx->priv_data, "cinema_mode", fps <= 24 && !stereo ? "2k_24" : "2k_48", 0);
            }
            av_opt_set(ctx->priv_data, "prog_order", "cprl", 0);
        }
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            if (error) *error = "Cannot start the JPEG 2000 encoder";
            return false;
        }
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        frame->format = ctx->pix_fmt;
        frame->width = w;
        frame->height = h;
        if (av_frame_get_buffer(frame, 0) < 0) {
            if (error) *error = "Out of memory";
            return false;
        }
        return true;
    }
    bool encode(const std::vector<uint16_t>& xyz, int64_t index, std::vector<uint8_t>& out, std::string* error) {
        // The DCI 2K or 4K profile, each frame within 250 Mbit/s (the 48 fps limits for every rate above 24), a 3D
        // frame's two eyes within it together.
        if (direct)
            return encodeJpeg2000(xyz.data(), w_, h_, 12, fourK() ? 0x0004 : 0x0003, (fps_ <= 24 ? 1302083 : 651041) / (stereo_ ? 2 : 1), out, error);
        if (av_frame_make_writable(frame) < 0) return false;
        for (int y = 0; y < ctx->height; ++y) {
            auto* d = reinterpret_cast<uint8_t*>(frame->data[0] + ptrdiff_t(y) * frame->linesize[0]);
            const uint16_t* s = xyz.data() + size_t(y) * size_t(ctx->width) * 3;
            for (int i = 0; i < ctx->width * 3; ++i) {
                const uint16_t v = uint16_t(s[i] << 4);  // 12 bits in the top of 16
                d[2 * i] = uint8_t(v);
                d[2 * i + 1] = uint8_t(v >> 8);
            }
        }
        frame->pts = index;
        out.clear();
        if (avcodec_send_frame(ctx, frame) < 0) {
            if (error) *error = "The JPEG 2000 encoder refused a frame";
            return false;
        }
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            out.insert(out.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
        if (out.empty()) {
            if (error) *error = "The JPEG 2000 encoder gave nothing back";
            return false;
        }
        return true;
    }
};

// Frames waiting to be encoded and codestreams waiting to be written, in order.
class EncoderPool {
public:
    EncoderPool(int w, int h, int fps, bool stereo, int threads) {
        for (int i = 0; i < threads; ++i)
            workers_.emplace_back([this, w, h, fps, stereo] {
                J2kEncoder enc;
                std::string err;
                const bool ok = enc.open(w, h, fps, stereo, &err);
                {
                    std::lock_guard<std::mutex> lock(m_);
                    if (!ok && error_.empty()) error_ = err;
                    if (ok && !enc.openjpeg) openjpeg_ = false;
                }
                for (;;) {
                    std::pair<int64_t, std::vector<uint16_t>> job;
                    {
                        std::unique_lock<std::mutex> lock(m_);
                        work_.wait(lock, [&] { return stop_ || !todo_.empty(); });
                        if (todo_.empty()) return;
                        job = std::move(todo_.front());
                        todo_.pop_front();
                        ++busy_;
                    }
                    std::vector<uint8_t> cs;
                    if (!ok || !enc.encode(job.second, job.first, cs, &err)) {
                        std::lock_guard<std::mutex> lock(m_);
                        if (error_.empty()) error_ = err.empty() ? "JPEG 2000 encoding failed" : err;
                        stop_ = true;
                        work_.notify_all();
                        done_.notify_all();
                        return;
                    }
                    std::lock_guard<std::mutex> lock(m_);
                    --busy_;
                    done_map_[job.first] = std::move(cs);
                    done_.notify_all();
                }
            });
    }
    ~EncoderPool() { finish(); }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread& t : workers_)
            if (t.joinable()) t.join();
    }
    // Frames are taken in the order they are pushed, so the earliest not yet taken back is always being worked on or
    // next in line: waiting for it always ends. The caller keeps the frames in hand (pending()) bounded by taking
    // that one before pushing more.
    void push(int64_t index, std::vector<uint16_t>&& xyz) {
        std::lock_guard<std::mutex> lock(m_);
        todo_.emplace_back(index, std::move(xyz));
        work_.notify_one();
    }
    size_t pending() {
        std::lock_guard<std::mutex> lock(m_);
        return todo_.size() + size_t(busy_) + done_map_.size();
    }
    // The codestream for `index` when it is ready (false after a failure).
    bool take(int64_t index, std::vector<uint8_t>& out, bool wait) {
        std::unique_lock<std::mutex> lock(m_);
        if (wait) done_.wait(lock, [&] { return !error_.empty() || done_map_.count(index); });
        auto it = done_map_.find(index);
        if (it == done_map_.end()) return false;
        out = std::move(it->second);
        done_map_.erase(it);
        done_.notify_all();
        return true;
    }
    bool ready(int64_t index) {
        std::lock_guard<std::mutex> lock(m_);
        return done_map_.count(index) > 0;
    }
    std::string error() {
        std::lock_guard<std::mutex> lock(m_);
        return error_;
    }
    bool openjpeg() {
        std::lock_guard<std::mutex> lock(m_);
        return openjpeg_;
    }

private:
    std::mutex m_;
    std::condition_variable work_, done_;
    std::deque<std::pair<int64_t, std::vector<uint16_t>>> todo_;
    std::map<int64_t, std::vector<uint8_t>> done_map_;
    std::vector<std::thread> workers_;
    std::string error_;
    int busy_ = 0;
    bool stop_ = false;
    bool openjpeg_ = true;
};

// ---- Packing ------------------------------------------------------------------------------------------------------

struct Asset {
    dcp::Uuid id;
    QString file;  // in the DCP folder
    QString type;  // "application/mxf", "text/xml"
    QString annotation;
};

void writeText(QXmlStreamWriter& x, const QString& name, const QString& value) { x.writeTextElement(name, value); }

QString titleForName(const std::string& title) {
    // CamelCase, letters and digits only, at most 14 characters.
    QString out;
    bool upper = true;
    for (QChar c : QString::fromStdString(title)) {
        if (c.isLetterOrNumber() && c.unicode() < 128) {
            out += upper ? c.toUpper() : c;
            upper = false;
        } else {
            upper = true;
        }
    }
    if (out.isEmpty()) out = QStringLiteral("Untitled");
    return out.left(14);
}

}  // namespace

bool dcpContainer(const std::string& container, int& width, int& height, bool fourK) {
    if (container == "flat") width = 1998, height = 1080;
    else if (container == "scope") width = 2048, height = 858;
    else if (container == "full") width = 2048, height = 1080;
    else return false;
    if (fourK) width *= 2, height *= 2;
    return true;
}

std::string defaultDcpContainer(const Sequence& s) { return s.height > 0 && double(s.width) / s.height >= 2.0 ? "scope" : "flat"; }

int dcpFrameRate(const Sequence& s, int requested) {
    static const int rates[] = {24, 25, 30, 48};
    for (int r : rates)
        if (r == requested) return r;
    // 50, 59.94 and 60 play at half the rate, every other frame (so at their own speed), as 2K cinema runs at 24 to 48.
    double fps = s.fpsValue();
    if (fps > 49 && std::fabs(fps - 48) > 1) fps /= 2;
    int best = 24;
    for (int r : rates)
        if (std::fabs(r - fps) < std::fabs(best - fps)) best = r;
    return best;
}

int dcpFrameStep(const Sequence& s, int rate) { return rate > 0 ? std::max(1, int(std::lround(s.fpsValue() / rate))) : 1; }

std::string dcpName(const DcpSettings& settings, int channels, const std::string& date) {
    static const std::map<std::string, QString> kinds = {{"feature", "FTR"}, {"short", "SHR"},     {"trailer", "TLR"},      {"teaser", "TSR"},
                                                         {"advertisement", "ADV"}, {"test", "TST"}, {"transitional", "XSN"}, {"rating", "RTG"},
                                                         {"psa", "PSA"},   {"policy", "POL"}};
    const auto kind = kinds.find(settings.kind);
    const QString aspect = settings.container == "scope" ? "S" : settings.container == "full" ? "C" : "F";
    QString language = QString::fromStdString(settings.language).section('-', 0, 0).toUpper();
    if (language.isEmpty()) language = "XX";
    QString territory = QString::fromStdString(settings.territory).toUpper();
    if (territory.isEmpty()) territory = "XX";
    auto code = [](const std::string& c) {
        QString out;
        for (QChar ch : QString::fromStdString(c).toUpper())
            if (ch.isLetterOrNumber() && ch.unicode() < 128) out += ch;
        return out;
    };
    // Subtitles after the audio's language (in lower case when burned in), "-CCAP" for closed captions; "-3D" after
    // the content kind.
    QString subtitle = QStringLiteral("XX");
    if (settings.subtitles && !settings.closedCaptions) {
        subtitle = QString::fromStdString(settings.subtitleLanguage).section('-', 0, 0).toUpper();
        if (subtitle.isEmpty()) subtitle = QStringLiteral("XX");
        if (settings.burnIn) subtitle = subtitle.toLower();
    }
    if (settings.subtitles && settings.closedCaptions) subtitle += QStringLiteral("-CCAP");
    QString content = kind != kinds.end() ? kind->second : QStringLiteral("FTR");
    if (settings.stereo) content += QStringLiteral("-3D");
    QStringList parts = {titleForName(settings.title), content, aspect, language + "-" + subtitle,
                         territory, channels == 8 ? QStringLiteral("71") : QStringLiteral("51"),
                         settings.resolution == "4k" ? QStringLiteral("4K") : QStringLiteral("2K")};
    if (!code(settings.studio).isEmpty()) parts << code(settings.studio);
    parts << QString::fromStdString(date);
    if (!code(settings.facility).isEmpty()) parts << code(settings.facility);
    parts << QStringLiteral("SMPTE") << QStringLiteral("OV");
    return parts.join('_').toStdString();
}

bool exportDcp(const Project& p, const Sequence& s, const DcpSettings& requested, const std::string& parent, DcpResult* result,
               const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    DcpSettings settings = requested;
    int cw = 0, ch = 0;
    if (settings.resolution != "2k" && settings.resolution != "4k") return fail("The resolution is 2k or 4k");
    const bool fourK = settings.resolution == "4k";
    if (!dcpContainer(settings.container, cw, ch, fourK)) return fail("The container is flat, scope or full");
    if (s.width <= 0 || s.height <= 0) return fail("The sequence has no picture size");
    const int fps = dcpFrameRate(s, settings.fps);
    if (fourK && fps > 30) return fail("4K DCPs play at 24, 25 or 30 frames a second");
    const bool stereo = settings.stereo;
    if (stereo && !s.stereo3d) return fail("The sequence is not stereoscopic (Sequence Settings: Stereoscopic 3D)");
    if (stereo && fourK) return fail("3D DCPs are 2K");
    if ((settings.encrypt || settings.sign) && !dcp::securityAvailable()) {
        if (settings.encrypt) return fail("This build cannot encrypt (it has no OpenSSL)");
        settings.sign = false;
    }
    if (settings.encrypt) settings.sign = true;  // an encrypted package is always signed
    const CaptionTrack* subtitles = nullptr;
    if (settings.subtitles) {
        for (const CaptionTrack& t : s.captionTracks)
            if (t.id == settings.subtitles) subtitles = &t;
        if (!subtitles) return fail("The sequence has no such caption track");
        if (settings.subtitleLanguage.empty()) settings.subtitleLanguage = subtitles->language.empty() ? "en" : subtitles->language;
    }
    const bool subtitleTrack = subtitles && !settings.burnIn;
    const int step = dcpFrameStep(s, fps);  // sequence frames to a DCP frame
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint >= s.inPoint) first = s.inPoint, end = s.outPoint + 1;  // both included
    if (end <= first) return fail("The sequence is empty");
    const int64_t frames = (end - first) / step;
    if (frames < fps) return fail("A DCP must last at least a second (cinema servers refuse shorter reels)");
    // Immersive mixes fold their heights down; an ambisonic field is heard through two virtual cardioids (stereo).
    const int seqChannels = ambisonicLayout(s.audioLayout) ? 2 : layoutChannels(earLevelLayout(s.audioLayout));
    const int channels = seqChannels == 8 ? 8 : 6;
    // The captions in DCP frames from the reel's start, cut to it.
    std::vector<Caption> captions;
    if (subtitles)
        for (Caption c : subtitles->captions) {
            const int64_t a = std::max<int64_t>(0, (c.start - first + step - 1) / step), b = std::min<int64_t>(frames, (c.end - first + step - 1) / step);
            if (c.end <= first || b <= a) continue;
            c.start = a, c.end = b;
            captions.push_back(std::move(c));
        }

    // The folder.
    const std::string date = QDate::currentDate().toString(QStringLiteral("yyyyMMdd")).toStdString();
    const std::string name = dcpName(settings, channels, date);
    const QString folder = QDir(QString::fromStdString(parent)).filePath(QString::fromStdString(name));
    if (QFileInfo::exists(folder)) return fail("There is already a folder called " + name + " there");
    if (!QDir().mkpath(folder)) return fail("Cannot make the folder " + folder.toStdString());
    bool finished = false;
    struct Cleanup {
        const QString& folder;
        bool& finished;
        ~Cleanup() {
            if (!finished) QDir(folder).removeRecursively();
        }
    } cleanup{folder, finished};

    // The signer, and a content key for each track file.
    dcp::CertificateChain chain;
    if (settings.sign) {
        std::string err;
        if (!dcp::signerCertificateChain(chain, &err, settings.certificates)) return fail("Cannot sign the package: " + err);
    }
    dcp::ContentKey pictureKey, soundKey, subtitleKey;
    if (settings.encrypt) {
        bool a = false, b = false, c = false;
        pictureKey = dcp::newContentKey("MDIK", &a), soundKey = dcp::newContentKey("MDAK", &b), subtitleKey = dcp::newContentKey("MDSK", &c);
        if (!a || !b || !c) return fail("Cannot make the content keys (the system's random source failed)");
    }
    auto encryption = [](const dcp::ContentKey& k) {
        dcp::TrackEncryption e;
        e.key = k.key;
        e.keyId = k.id;
        return e;
    };

    const dcp::Uuid pictureId = dcp::newUuid(), soundId = dcp::newUuid(), subtitleId = dcp::newUuid();
    const QString pictureFile = QStringLiteral("j2c_%1.mxf").arg(QString::fromStdString(dcp::uuidString(pictureId)));
    const QString soundFile = QStringLiteral("pcm_%1.mxf").arg(QString::fromStdString(dcp::uuidString(soundId)));
    const QString subtitleFile = QStringLiteral("sub_%1.mxf").arg(QString::fromStdString(dcp::uuidString(subtitleId)));

    // Sound first (quick): the mix at 48 kHz, a picture frame's worth at a time. A 23.976 sequence plays at 24, so its
    // sound is mixed at the rate that makes each sequence frame 2000 samples, and plays 0.1 % faster (a 59.94 one, at
    // 30 from every other frame, 1600 samples to two frames).
    {
        dcp::SoundMxfWriter sound;
        if (settings.encrypt) sound.encryptWith(encryption(soundKey));
        std::string err;
        if (!sound.open(QDir(folder).filePath(soundFile).toStdString(), soundId, fps, channels, settings.language, &err)) return fail(err);
        Sequence mixSeq = s;
        mixSeq.audioLayout = earLevelLayout(s.audioLayout);
        const int perFrame = kSampleRate / fps;
        mixSeq.sampleRate = int(std::lround(perFrame * s.fpsValue() / step));
        const int64_t firstSample = int64_t(std::llround(double(first) * perFrame / step));
        AudioMixer mixer;
        std::vector<float> mix(size_t(perFrame) * size_t(std::max(2, seqChannels))), out(size_t(perFrame) * size_t(channels));
        // Where each of the sequence's channels goes: stereo to L and R; 5.1 as it is; 7.1 (L R C LFE Lb Rb Ls Rs, as
        // FFmpeg orders it) to 7.1 DS (L R C LFE Lss Rss Lrs Rrs).
        std::vector<int> route;
        if (seqChannels == 8) route = {0, 1, 2, 3, 6, 7, 4, 5};
        else if (seqChannels == 6) route = {0, 1, 2, 3, 4, 5};
        else route = {0, 1};
        for (int64_t k = 0; k < frames; ++k) {
            const int64_t start = firstSample + k * perFrame;
            if (seqChannels > 2) mixer.mixLayout(p, mixSeq, start, perFrame, mix.data());
            else mixer.mix(p, mixSeq, start, perFrame, mix.data());
            std::fill(out.begin(), out.end(), 0.0f);
            const int in = std::max(2, seqChannels);
            for (int i = 0; i < perFrame; ++i)
                for (size_t c = 0; c < route.size(); ++c) out[size_t(i) * size_t(channels) + size_t(route[c])] = mix[size_t(i) * size_t(in) + c];
            if (!sound.write(out.data(), &err)) return fail(err);
            if (progress && k % 48 == 0 && !progress(0.05 * double(k) / double(frames))) return fail("Stopped");
        }
        if (!sound.close(&err)) return fail(err);
    }

    // The subtitles: ST 428-7 XML with the font it loads, in a timed text track file.
    const QString issued = isoNow();
    if (subtitleTrack) {
        QFile fontFile(QStringLiteral(":/montage/fonts/LiberationSans-Regular.ttf"));
        if (!fontFile.open(QIODevice::ReadOnly)) return fail("The subtitle font is missing from this build");
        const QByteArray font = fontFile.readAll();
        dcp::TimedTextResource resource;
        resource.id = dcp::newUuid();
        resource.mime = "application/x-font-opentype";
        resource.data.assign(font.begin(), font.end());
        const dcp::Uuid xmlId = dcp::newUuid();  // the document's own id, not the track file's (Bv2.1)
        DcstInfo info;
        info.id = dcp::uuidString(xmlId);
        info.title = name;
        info.issueDate = issued.toStdString();
        info.language = settings.subtitleLanguage;
        info.editRate = fps;
        info.fontUuid = dcp::uuidString(resource.id);
        const std::string xml = captionsToDcst(captions, subtitles->style, info);
        dcp::TimedTextMxfWriter track;
        if (settings.encrypt) track.encryptWith(encryption(subtitleKey));
        std::string err;
        if (!track.open(QDir(folder).filePath(subtitleFile).toStdString(), subtitleId, fps, &err) ||
            !track.write(xml, xmlId, frames, settings.subtitleLanguage, {resource}, &err) || !track.close(&err))
            return fail(err);
    }

    // The picture, fitted inside the container on black, several frames encoding at once (a 3D frame's two eyes one
    // after the other).
    const double scale = std::min(double(cw) / s.width, double(ch) / s.height);
    const int fw = std::min(cw, int(std::lround(s.width * scale / 2)) * 2), fh = std::min(ch, int(std::lround(s.height * scale / 2)) * 2);
    const int ox = (cw - fw) / 2, oy = (ch - fh) / 2;
    const ColorSpace* space = &sequenceColorSpace(s);
    if (space->hdr()) space = findColorSpace("p3d65");  // tone mapped into the cinema's range, its gamut kept
    const XyzConverter xyz(*space);
    const int threads = settings.threads > 0 ? settings.threads : std::max(1, int(std::thread::hardware_concurrency()));
    bool cinema = true;
    {
        // Burned in: the chosen track is the one drawn.
        Sequence burned;
        const Sequence* shown = &s;
        if (subtitles && settings.burnIn) {
            burned = s;
            for (CaptionTrack& t : burned.captionTracks) t.visible = t.id == subtitles->id;
            shown = &burned;
        }
        dcp::PictureMxfWriter picture;
        if (settings.encrypt) picture.encryptWith(encryption(pictureKey));
        std::string err;
        if (!picture.open(QDir(folder).filePath(pictureFile).toStdString(), pictureId, fps, stereo, &err)) return fail(err);
        EncoderPool pool(cw, ch, fps, stereo, threads);
        RenderOptions o;
        o.scale = scale;
        o.highQuality = true;
        o.captions = subtitles && settings.burnIn;
        const int eyes = stereo ? 2 : 1;
        const int64_t jobs = frames * eyes;
        int64_t written = 0;
        std::vector<uint8_t> cs, right;
        auto writeReady = [&](bool wait) {
            while (written < frames) {
                const int64_t job = written * eyes;
                if (!wait && !(pool.ready(job) && (!stereo || pool.ready(job + 1)))) break;
                if (!pool.take(job, cs, true) || (stereo && !pool.take(job + 1, right, true))) break;
                if (written == 0) {
                    dcp::J2kHeader h;
                    if (dcp::parseJ2kHeader(cs.data(), cs.size(), h)) cinema = h.rsiz == (fourK ? 4 : 3) && pool.openjpeg();
                }
                if (stereo ? !picture.writeStereo(cs.data(), cs.size(), right.data(), right.size(), &err) : !picture.write(cs.data(), cs.size(), &err))
                    return false;
                ++written;
                wait = false;
            }
            return pool.error().empty();
        };
        const size_t inHand = size_t(threads) * 2;
        for (int64_t j = 0; j < jobs; ++j) {
            const int64_t k = j / eyes;
            if (stereo) o.stereoView = j % 2 ? StereoView::Right : StereoView::Left;
            Image frame = renderProgramFrame(p, *shown, first + k * step, o);
            if (frame.width != fw || frame.height != fh) frame = resizeImage(frame, fw, fh);
            if (space != &sequenceColorSpace(s)) convertColor(frame, sequenceColorSpace(s), *space, s.hdrPeakNits);
            Image boxed(cw, ch);  // black around it
            for (int y = 0; y < fh; ++y) std::copy(frame.row(y), frame.row(y) + size_t(fw) * 4, boxed.row(y + oy) + size_t(ox) * 4);
            std::vector<uint16_t> codes;
            xyz.convert(boxed, codes);
            // Room for it: the earliest frame written first when enough are in hand.
            while (pool.pending() >= inHand && written * eyes + eyes <= j)
                if (!writeReady(true)) return fail(err.empty() ? pool.error() : err);
            pool.push(j, std::move(codes));
            if (!writeReady(false)) return fail(err.empty() ? pool.error() : err);
            if (progress && !progress(0.05 + 0.9 * double(written) / double(frames))) return fail("Stopped");
        }
        while (written < frames) {
            if (!writeReady(true)) return fail(err.empty() ? pool.error() : err);
            if (progress && !progress(0.05 + 0.9 * double(written) / double(frames))) return fail("Stopped");
        }
        pool.finish();
        if (!picture.close(&err)) return fail(err);
    }

    // The composition, packing list, asset map and volume index.
    const QString title = QString::fromStdString(settings.title.empty() ? "Untitled" : settings.title);
    const QString issuer = QString::fromStdString(settings.issuer.empty() ? "Montage" : settings.issuer);
    const QString rate = QStringLiteral("%1 1").arg(fps);
    const dcp::Uuid cplId = dcp::newUuid(), pklId = dcp::newUuid(), amId = dcp::newUuid();
    const QString cplFile = QStringLiteral("CPL_%1.xml").arg(QString::fromStdString(dcp::uuidString(cplId)));
    const QString pklFile = QStringLiteral("PKL_%1.xml").arg(QString::fromStdString(dcp::uuidString(pklId)));
    struct Track {
        dcp::Uuid id;
        QString file, hash;
        qint64 size = 0;
    };
    std::vector<Track> tracks = {{pictureId, pictureFile}, {soundId, soundFile}};
    if (subtitleTrack) tracks.push_back({subtitleId, subtitleFile});
    qint64 toHash = 1;
    for (const Track& t : tracks) toHash += QFileInfo(QDir(folder).filePath(t.file)).size();
    qint64 hashed = 0;
    bool stopped = false;
    auto hashing = [&](qint64 n) {
        hashed += n;
        stopped = progress && !progress(0.95 + 0.05 * double(hashed) / double(toHash));
        return !stopped;
    };
    for (Track& t : tracks) {
        t.hash = fileHash(QDir(folder).filePath(t.file), &t.size, hashing);
        if (t.hash.isEmpty()) return fail(stopped ? "Stopped" : "Cannot read the track files back");
    }
    auto writeXml = [&](const QString& file, const std::function<void(QXmlStreamWriter&)>& body) {
        QFile f(QDir(folder).filePath(file));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        QXmlStreamWriter x(&f);
        x.setAutoFormatting(true);
        x.setAutoFormattingIndent(2);
        x.writeStartDocument();
        body(x);
        x.writeEndDocument();
        return f.error() == QFile::NoError;
    };
    // Signed in place: the signer and an enveloped signature at the end of the root.
    auto sign = [&](const QString& file) {
        if (!settings.sign) return true;
        QFile f(QDir(folder).filePath(file));
        if (!f.open(QIODevice::ReadOnly)) return false;
        std::string xmlText = f.readAll().toStdString();
        f.close();
        std::string err;
        return dcp::signXml(xmlText, chain, &err) && dcp::writeWholeFile(f.fileName().toStdString(), xmlText);
    };
    auto keyId = [&](QXmlStreamWriter& x, const dcp::ContentKey& k) {
        if (settings.encrypt) writeText(x, "KeyId", urn(k.id));
    };
    const char* kMeta = "http://www.smpte-ra.org/schemas/429-16/2014/CPL-Metadata";
    const char* kStereoNs = "http://www.smpte-ra.org/schemas/429-10/2008/Main-Stereo-Picture-CPL";
    const char* kCaptionNs = "http://www.smpte-ra.org/schemas/429-12/2008/TT";
    const bool cplOk = writeXml(cplFile, [&](QXmlStreamWriter& x) {
        x.writeDefaultNamespace(QStringLiteral("http://www.smpte-ra.org/schemas/429-7/2006/CPL"));
        x.writeStartElement(QStringLiteral("CompositionPlaylist"));
        writeText(x, "Id", urn(cplId));
        writeText(x, "AnnotationText", QString::fromStdString(name));  // as servers list it: the same as the title text
        writeText(x, "IssueDate", issued);
        writeText(x, "Issuer", issuer);
        writeText(x, "Creator", QStringLiteral("Montage"));
        writeText(x, "ContentTitleText", QString::fromStdString(name));
        writeText(x, "ContentKind", QString::fromStdString(settings.kind.empty() ? "feature" : settings.kind));
        x.writeStartElement("ContentVersion");
        const dcp::Uuid version = dcp::newUuid();
        writeText(x, "Id", urn(version));
        writeText(x, "LabelText", QString::fromStdString(dcp::uuidString(version)) + "_" + issued);
        x.writeEndElement();
        x.writeEmptyElement("RatingList");
        x.writeStartElement("ReelList");
        x.writeStartElement("Reel");
        writeText(x, "Id", urn(dcp::newUuid()));
        x.writeStartElement("AssetList");
        // A 2D picture leads the reel; a 3D one is an extension element, so after the standard ones (as ST 429-7's schema
        // orders them).
        auto writePicture = [&] {
            if (stereo) {
                x.writeNamespace(QString::fromLatin1(kStereoNs), QStringLiteral("msp-cpl"));
                x.writeStartElement(QString::fromLatin1(kStereoNs), QStringLiteral("MainStereoscopicPicture"));
            } else {
                x.writeStartElement("MainPicture");
            }
            writeText(x, "Id", urn(pictureId));
            writeText(x, "AnnotationText", pictureFile);
            writeText(x, "EditRate", rate);
            writeText(x, "IntrinsicDuration", QString::number(frames));
            writeText(x, "EntryPoint", "0");
            writeText(x, "Duration", QString::number(frames));
            keyId(x, pictureKey);
            writeText(x, "Hash", tracks[0].hash);
            writeText(x, "FrameRate", QStringLiteral("%1 1").arg(stereo ? 2 * fps : fps));  // a 3D one's codestreams
            writeText(x, "ScreenAspectRatio", QStringLiteral("%1 %2").arg(cw).arg(ch));
            x.writeEndElement();
        };
        if (!stereo) writePicture();
        x.writeStartElement("MainSound");
        writeText(x, "Id", urn(soundId));
        writeText(x, "AnnotationText", soundFile);
        writeText(x, "EditRate", rate);
        writeText(x, "IntrinsicDuration", QString::number(frames));
        writeText(x, "EntryPoint", "0");
        writeText(x, "Duration", QString::number(frames));
        keyId(x, soundKey);
        writeText(x, "Hash", tracks[1].hash);
        x.writeEndElement();
        if (subtitleTrack) {
            if (settings.closedCaptions) {
                x.writeNamespace(QString::fromLatin1(kCaptionNs), QStringLiteral("tt"));
                x.writeStartElement(QString::fromLatin1(kCaptionNs), QStringLiteral("ClosedCaption"));
            } else {
                x.writeStartElement("MainSubtitle");
            }
            writeText(x, "Id", urn(subtitleId));
            writeText(x, "AnnotationText", subtitleFile);
            writeText(x, "EditRate", rate);
            writeText(x, "IntrinsicDuration", QString::number(frames));
            writeText(x, "EntryPoint", "0");
            writeText(x, "Duration", QString::number(frames));
            keyId(x, subtitleKey);
            writeText(x, "Hash", tracks[2].hash);
            if (settings.closedCaptions) x.writeTextElement(QString::fromLatin1(kCaptionNs), QStringLiteral("Language"), QString::fromStdString(settings.subtitleLanguage));
            else writeText(x, "Language", QString::fromStdString(settings.subtitleLanguage));
            x.writeEndElement();
        }
        if (stereo) writePicture();
        // ST 429-16 composition metadata (asked for by the SMPTE Bv2.1 profile).
        x.writeNamespace(QString::fromLatin1(kMeta), QStringLiteral("meta"));
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("CompositionMetadataAsset"));
        writeText(x, "Id", urn(dcp::newUuid()));
        writeText(x, "EditRate", rate);
        writeText(x, "IntrinsicDuration", QString::number(frames));
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("FullContentTitleText"), title);
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("ReleaseTerritory"),
                           QString::fromStdString(settings.territory.empty() ? "XX" : settings.territory).toUpper());
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("VersionNumber"));
        x.writeAttribute(QStringLiteral("status"), QStringLiteral("final"));
        x.writeCharacters(QStringLiteral("1"));
        x.writeEndElement();
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("Luminance"));
        x.writeAttribute(QStringLiteral("units"), QStringLiteral("foot-lambert"));
        x.writeCharacters(QStringLiteral("14"));
        x.writeEndElement();
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("MainSoundConfiguration"),
                           channels == 8 ? QStringLiteral("71/L,R,C,LFE,Lss,Rss,Lrs,Rrs") : QStringLiteral("51/L,R,C,LFE,Ls,Rs"));
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("MainSoundSampleRate"), QStringLiteral("48000 1"));
        for (const char* area : {"MainPictureStoredArea", "MainPictureActiveArea"}) {
            x.writeStartElement(QString::fromLatin1(kMeta), QString::fromLatin1(area));
            const bool active = std::string(area) == "MainPictureActiveArea";
            x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("Width"), QString::number(active ? fw : cw));
            x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("Height"), QString::number(active ? fh : ch));
            x.writeEndElement();
        }
        if (subtitleTrack && !settings.closedCaptions)
            x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("MainSubtitleLanguageList"), QString::fromStdString(settings.subtitleLanguage));
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("ExtensionMetadataList"));
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("ExtensionMetadata"));
        x.writeAttribute(QStringLiteral("scope"), QStringLiteral("http://isdcf.com/ns/cplmd/app"));
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("Name"), QStringLiteral("Application"));
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("PropertyList"));
        x.writeStartElement(QString::fromLatin1(kMeta), QStringLiteral("Property"));
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("Name"), QStringLiteral("DCP Constraints Profile"));
        x.writeTextElement(QString::fromLatin1(kMeta), QStringLiteral("Value"), QStringLiteral("SMPTE-RDD-52:2020-Bv2.1"));
        x.writeEndElement();  // Property
        x.writeEndElement();  // PropertyList
        x.writeEndElement();  // ExtensionMetadata
        x.writeEndElement();  // ExtensionMetadataList
        x.writeEndElement();  // CompositionMetadataAsset
        x.writeEndElement();  // AssetList
        x.writeEndElement();  // Reel
        x.writeEndElement();  // ReelList
        x.writeEndElement();  // CompositionPlaylist
    });
    if (!cplOk || !sign(cplFile)) return fail("Cannot write the composition playlist");
    qint64 cplSize = 0;
    const QString cplHash = fileHash(QDir(folder).filePath(cplFile), &cplSize);
    std::vector<Asset> assets = {{cplId, cplFile, "text/xml", title},
                                 {pictureId, pictureFile, "application/mxf", pictureFile},
                                 {soundId, soundFile, "application/mxf", soundFile}};
    if (subtitleTrack) assets.push_back({subtitleId, subtitleFile, "application/mxf", subtitleFile});
    const bool pklOk = writeXml(pklFile, [&](QXmlStreamWriter& x) {
        x.writeDefaultNamespace(QStringLiteral("http://www.smpte-ra.org/schemas/429-8/2007/PKL"));
        x.writeStartElement("PackingList");
        writeText(x, "Id", urn(pklId));
        writeText(x, "AnnotationText", QString::fromStdString(name));
        writeText(x, "IssueDate", issued);
        writeText(x, "Issuer", issuer);
        writeText(x, "Creator", QStringLiteral("Montage"));
        x.writeStartElement("AssetList");
        for (const Asset& a : assets) {
            QString hash = cplHash;
            qint64 size = cplSize;
            for (const Track& t : tracks)
                if (t.file == a.file) hash = t.hash, size = t.size;
            x.writeStartElement("Asset");
            writeText(x, "Id", urn(a.id));
            writeText(x, "AnnotationText", a.annotation);
            writeText(x, "Hash", hash);
            writeText(x, "Size", QString::number(size));
            writeText(x, "Type", a.type);
            writeText(x, "OriginalFileName", a.file);
            x.writeEndElement();
        }
        x.writeEndElement();
        x.writeEndElement();
    });
    if (!pklOk || !sign(pklFile)) return fail("Cannot write the packing list");
    std::vector<Asset> mapped = assets;
    mapped.insert(mapped.begin(), Asset{pklId, pklFile, "text/xml", {}});
    const bool amOk = writeXml(QStringLiteral("ASSETMAP.xml"), [&](QXmlStreamWriter& x) {
        x.writeDefaultNamespace(QStringLiteral("http://www.smpte-ra.org/schemas/429-9/2007/AM"));
        x.writeStartElement("AssetMap");
        writeText(x, "Id", urn(amId));
        writeText(x, "AnnotationText", QString::fromStdString(name));
        writeText(x, "Creator", QStringLiteral("Montage"));
        writeText(x, "VolumeCount", "1");
        writeText(x, "IssueDate", issued);
        writeText(x, "Issuer", issuer);
        x.writeStartElement("AssetList");
        for (const Asset& a : mapped) {
            x.writeStartElement("Asset");
            writeText(x, "Id", urn(a.id));
            if (a.file == pklFile) writeText(x, "PackingList", "true");
            x.writeStartElement("ChunkList");
            x.writeStartElement("Chunk");
            writeText(x, "Path", a.file);
            writeText(x, "VolumeIndex", "1");
            writeText(x, "Offset", "0");
            writeText(x, "Length", QString::number(QFileInfo(QDir(folder).filePath(a.file)).size()));
            x.writeEndElement();
            x.writeEndElement();
            x.writeEndElement();
        }
        x.writeEndElement();
        x.writeEndElement();
    });
    const bool volOk = writeXml(QStringLiteral("VOLINDEX.xml"), [&](QXmlStreamWriter& x) {
        x.writeDefaultNamespace(QStringLiteral("http://www.smpte-ra.org/schemas/429-9/2007/AM"));
        x.writeStartElement("VolumeIndex");
        writeText(x, "Index", "1");
        x.writeEndElement();
    });
    if (!amOk || !volOk) return fail("Cannot write the asset map");

    // The keys, kept only in a DKDM to Montage's own certificate: KDMs for cinemas are made from it (makeKdms).
    std::string dkdmPath;
    if (settings.encrypt) {
        dcp::CertificateInfo leaf;
        dcp::readCertificate(chain.certificates.back(), leaf);
        const QDateTime until = QDateTime::fromString(QString::fromStdString(leaf.notAfter), Qt::ISODate);
        // KDMs can only be made inside the DKDM's window, which ends with the signing certificate.
        if (!until.isValid() || until.toSecsSinceEpoch() < QDateTime::currentSecsSinceEpoch() + 30 * 86400LL)
            return fail("The signing certificate ends within 30 days (" + leaf.notAfter + "): the package's keys could not be given out after that");
        dcp::KdmSettings k;
        k.cplId = dcp::uuidString(cplId);
        k.contentTitle = name;
        k.annotation = "DKDM for " + name;
        const int64_t now = QDateTime::currentSecsSinceEpoch();
        k.notValidBefore = dcp::kdmTime(now - 60);
        k.notValidAfter = dcp::kdmTime(until.toSecsSinceEpoch() - 86400);
        k.keys = {pictureKey, soundKey};
        if (subtitleTrack) k.keys.push_back(subtitleKey);
        k.recipient = chain.certificates.back();
        std::string xmlText, err;
        if (!dcp::makeKdm(k, chain, xmlText, &err)) return fail("Cannot keep the keys: " + err);
        const QDir keep(QString::fromStdString(settings.dkdms.empty() ? dcp::dkdmFolder() : settings.dkdms));
        if (!QDir().mkpath(keep.path())) return fail("Cannot make " + keep.path().toStdString());
        dkdmPath = keep.filePath(QStringLiteral("DKDM_%1_%2.xml").arg(QString::fromStdString(name), QString::fromStdString(dcp::uuidString(cplId)))).toStdString();
        if (!dcp::writeWholeFile(dkdmPath, xmlText))
            return fail("Cannot write the DKDM to " + dkdmPath + ": without it the package can never be opened");
    }
    if (progress) progress(1.0);
    finished = true;
    if (result) {
        result->folder = folder.toStdString();
        result->name = name;
        result->cpl = QDir(folder).filePath(cplFile).toStdString();
        result->frames = frames;
        result->fps = fps;
        result->width = cw;
        result->height = ch;
        result->channels = channels;
        result->cinemaProfile = cinema;
        result->stereo = stereo;
        result->encrypted = settings.encrypt;
        result->signedPackage = settings.sign;
        result->subtitleCount = int(captions.size());
        result->cplId = dcp::uuidString(cplId);
        result->dkdm = dkdmPath;
    }
    return true;
}

bool makeKdms(const KdmRequest& r, std::vector<std::string>* files, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    dcp::CertificateChain chain;
    std::string err;
    if (!dcp::signerCertificateChain(chain, &err, r.certificates)) return fail(err);
    QFile f(QString::fromStdString(r.dkdm));
    if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read the DKDM " + r.dkdm);
    dcp::KdmContents dkdm;
    if (!dcp::readKdm(f.readAll().toStdString(), chain.privateKey, dkdm, &err)) return fail("The DKDM cannot be opened with this signer's key: " + err);
    // Montage's own: signed by its leaf too (anyone can encrypt to that leaf, whose certificate every signed CPL carries).
    dcp::CertificateInfo own;
    if (!dcp::readCertificate(chain.certificates.back(), own) || dkdm.signerThumbprint != own.thumbprint)
        return fail("The DKDM was not made by this signer");
    if (r.notAfter <= r.notBefore) return fail("The KDM's window ends before it starts");
    if (r.recipients.empty()) return fail("No cinema server certificates");
    // The window must sit inside the DKDM's (the keys are not to be given out for longer than they were kept for).
    const QDateTime from = QDateTime::fromString(QString::fromStdString(dkdm.notValidBefore), Qt::ISODate);
    const QDateTime to = QDateTime::fromString(QString::fromStdString(dkdm.notValidAfter), Qt::ISODate);
    if (to.isValid() && r.notAfter > to.toSecsSinceEpoch()) return fail("The window ends after the DKDM's (" + dkdm.notValidAfter + ")");
    if (from.isValid() && r.notBefore < from.toSecsSinceEpoch() - 86400) return fail("The window starts before the DKDM's (" + dkdm.notValidBefore + ")");
    // Each server's leaf first, so a bad certificate makes no KDMs at all.
    std::vector<std::pair<std::string, dcp::CertificateInfo>> servers;
    for (const std::string& given : r.recipients) {
        std::string pem = given;
        if (pem.find("-----BEGIN") == std::string::npos) {
            QFile c(QString::fromStdString(given));
            if (!c.open(QIODevice::ReadOnly)) return fail("Cannot read the certificate " + given);
            pem = c.readAll().toStdString();
        }
        // A server's chain file: its leaf is the one that is not a certificate authority.
        std::string leafPem;
        dcp::CertificateInfo leaf;
        for (const std::string& c : dcp::splitCertificates(pem)) {
            dcp::CertificateInfo info;
            if (dcp::readCertificate(c, info) && !info.ca) leafPem = c, leaf = info;
        }
        if (leafPem.empty()) return fail("No server certificate in " + (given.size() < 200 ? given : std::string("the text given")));
        const QDateTime certStart = QDateTime::fromString(QString::fromStdString(leaf.notBefore), Qt::ISODate);
        const QDateTime certEnd = QDateTime::fromString(QString::fromStdString(leaf.notAfter), Qt::ISODate);
        if (!certStart.isValid() || !certEnd.isValid()) return fail("The certificate of " + leaf.commonName + " has no validity dates");
        if (r.notAfter > certEnd.toSecsSinceEpoch()) return fail("The window ends after the certificate of " + leaf.commonName + " does");
        if (r.notBefore < certStart.toSecsSinceEpoch()) return fail("The window starts before the certificate of " + leaf.commonName + " does");
        servers.emplace_back(leafPem, leaf);
    }
    if (!QDir().mkpath(QString::fromStdString(r.folder))) return fail("Cannot make " + r.folder);
    auto safe = [](const std::string& text) {
        QString n = QString::fromStdString(text);
        n.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
        n.replace(QRegularExpression(QStringLiteral("^[._]+")), QString());  // (no "..", no hidden files)
        return n.left(60).isEmpty() ? QStringLiteral("KDM") : n.left(60);
    };
    std::vector<std::string> made;
    for (const auto& [leafPem, leaf] : servers) {
        dcp::KdmSettings k;
        k.cplId = dkdm.cplId;
        k.contentTitle = dkdm.contentTitle;
        k.annotation = r.annotation.empty() ? dkdm.contentTitle : r.annotation;
        k.notValidBefore = dcp::kdmTime(r.notBefore, r.offsetMinutes);
        k.notValidAfter = dcp::kdmTime(r.notAfter, r.offsetMinutes);
        k.keys = dkdm.keys;
        k.recipient = leafPem;
        std::string xml;
        if (!dcp::makeKdm(k, chain, xml, &err)) return fail(err);
        // Named by the title, the server and the day; two servers of one name told apart by their certificates.
        const QString stem = QStringLiteral("KDM_%1_%2_%3").arg(safe(dkdm.contentTitle), safe(leaf.commonName),
                                                               QDateTime::fromSecsSinceEpoch(r.notBefore).toUTC().toString("yyyyMMdd"));
        QString file = QDir(QString::fromStdString(r.folder)).filePath(stem + ".xml");
        if (std::find(made.begin(), made.end(), file.toStdString()) != made.end())
            file = QDir(QString::fromStdString(r.folder)).filePath(stem + "_" + safe(leaf.serial) + ".xml");
        if (!dcp::writeWholeFile(file.toStdString(), xml)) return fail("Cannot write " + file.toStdString());
        made.push_back(file.toStdString());
    }
    if (files) *files = made;
    return true;
}

// ---- Checking -----------------------------------------------------------------------------------------------------

namespace {

struct XmlAsset {
    QString id, path, hash, type;
    qint64 size = -1, length = -1;
    bool packingList = false;
};

std::vector<std::pair<QString, QString>> xmlItems(const QString& file, QString* root = nullptr) { return packageXmlItems(file, root); }

QString bareId(QString id) { return packageBareId(std::move(id)); }

struct TrackInfo {
    bool ok = false;
    std::string codec, pixfmt;
    int width = 0, height = 0, sampleRate = 0, channels = 0, bits = 0;
    int64_t packets = 0, largest = 0;
};

TrackInfo probeTrack(const std::string& path, const std::function<bool(qint64)>& read = {}) {
    TrackInfo t;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return t;
    if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->nb_streams > 0) {
        const AVCodecParameters* cp = fmt->streams[0]->codecpar;
        t.codec = avcodec_get_name(cp->codec_id);
        if (const char* n = av_get_pix_fmt_name(AVPixelFormat(cp->format)); n && cp->codec_type == AVMEDIA_TYPE_VIDEO) t.pixfmt = n;
        t.width = cp->width, t.height = cp->height;
        t.sampleRate = cp->sample_rate;
        t.channels = cp->ch_layout.nb_channels;
        t.bits = cp->bits_per_raw_sample ? cp->bits_per_raw_sample : cp->bits_per_coded_sample;
        AVPacket* pkt = av_packet_alloc();
        qint64 since = 0;
        bool going = true;
        while (going && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == 0) {
                ++t.packets;
                t.largest = std::max<int64_t>(t.largest, pkt->size);
            }
            since += pkt->size;
            if (read && since >= (1 << 20)) going = read(since), since = 0;
            av_packet_unref(pkt);
        }
        if (read && going && since) read(since);
        av_packet_free(&pkt);
        t.ok = true;
    }
    avformat_close_input(&fmt);
    return t;
}

}  // namespace

std::vector<std::string> verifyDcp(const std::string& folderPath, const std::function<bool(double)>& progress, std::vector<std::string>* advice) {
    std::vector<std::string> issues;
    auto issue = [&](const QString& s) { issues.push_back(s.toStdString()); };
    const QDir dir(QString::fromStdString(folderPath));
    const QString map = dir.exists("ASSETMAP.xml") ? dir.filePath("ASSETMAP.xml") : dir.filePath("ASSETMAP");
    if (!QFileInfo::exists(map)) {
        issue("No ASSETMAP.xml");
        return issues;
    }
    if (!dir.exists("VOLINDEX.xml") && !dir.exists("VOLINDEX")) issue("No VOLINDEX.xml");
    QString root;
    const auto am = xmlItems(map, &root);
    if (root != "AssetMap") {
        issue("ASSETMAP.xml is not an asset map");
        return issues;
    }
    std::map<QString, XmlAsset> assets;
    XmlAsset* cur = nullptr;
    for (const auto& [path, text] : am) {
        if (path == "AssetMap/AssetList/Asset") cur = nullptr;
        if (path == "AssetMap/AssetList/Asset/Id") cur = &assets[bareId(text)], cur->id = bareId(text);
        if (!cur) continue;
        if (path.endsWith("/PackingList")) cur->packingList = text == "true";
        if (path.endsWith("/Chunk/Path")) cur->path = text;
        if (path.endsWith("/Chunk/Length")) cur->length = text.toLongLong();
    }
    for (auto& [id, a] : assets) {
        const QFileInfo fi(dir.filePath(a.path));
        if (a.path.isEmpty() || !fi.exists()) issue(QStringLiteral("Missing file for asset %1: %2").arg(id, a.path));
        else if (a.length >= 0 && fi.size() != a.length) issue(QStringLiteral("%1 is %2 bytes; the asset map says %3").arg(a.path).arg(fi.size()).arg(a.length));
    }
    // Packing lists: sizes and hashes.
    std::vector<QString> cpls;
    int packingLists = 0;
    for (auto& [id, a] : assets) {
        if (!a.packingList) continue;
        ++packingLists;
        const auto pkl = xmlItems(dir.filePath(a.path), &root);
        if (root != "PackingList") {
            issue(a.path + " is not a packing list");
            continue;
        }
        QString assetId;
        for (const auto& [path, text] : pkl) {
            if (path == "PackingList/AssetList/Asset/Id") assetId = bareId(text);
            if (assetId.isEmpty() || !assets.count(assetId)) {
                if (path == "PackingList/AssetList/Asset/Id") issue(QStringLiteral("Asset %1 is in the packing list but not the asset map").arg(assetId));
                continue;
            }
            XmlAsset& x = assets[assetId];
            if (path.endsWith("Asset/Hash")) x.hash = text;
            if (path.endsWith("Asset/Size")) x.size = text.toLongLong();
            if (path.endsWith("Asset/Type")) {
                x.type = text;
                if (text.contains("xml")) cpls.push_back(assetId);
            }
        }
    }
    if (packingLists == 0) issue("The asset map names no packing list");
    // Every file is read twice (hashed, then its frames counted): progress by bytes.
    qint64 total = 1, read = 0;
    for (auto& [id, a] : assets)
        if (!a.packingList && !a.hash.isEmpty()) total += 2 * QFileInfo(dir.filePath(a.path)).size();
    bool stopped = false;
    auto reading = [&](qint64 n) {
        read += n;
        if (progress && !progress(std::min(1.0, double(read) / double(total)))) stopped = true;
        return !stopped;
    };
    for (auto& [id, a] : assets) {
        if (a.packingList || a.hash.isEmpty()) continue;
        qint64 size = 0;
        const QString hash = fileHash(dir.filePath(a.path), &size, reading);
        if (stopped) {
            issue("Stopped");
            return issues;
        }
        if (hash != a.hash) issue(QStringLiteral("%1 does not match its hash in the packing list (damaged or changed)").arg(a.path));
        if (a.size >= 0 && size != a.size) issue(QStringLiteral("%1 is %2 bytes; the packing list says %3").arg(a.path).arg(size).arg(a.size));
    }
    // Compositions: their assets, durations and track files.
    int compositions = 0;
    auto signature = [&](const QString& path) {
        QFile f(dir.filePath(path));
        if (!f.open(QIODevice::ReadOnly)) return;
        const std::string text = f.readAll().toStdString();
        if (text.find("xmldsig#") == std::string::npos || text.find("Signature") == std::string::npos) return;
        std::string err;
        dcp::SignatureCheck check;
        if (!dcp::securityAvailable()) return;
        if (!dcp::verifyXmlSignature(text, &err, &check)) issue(QStringLiteral("%1's signature does not hold: %2").arg(path, QString::fromStdString(err)));
        else if (std::find(check.references.begin(), check.references.end(), std::string()) == check.references.end())
            issue(path + "'s signature does not cover the whole document");
    };
    for (auto& [id, a] : assets)
        if (a.packingList) signature(a.path);
    for (const QString& cplId : cpls) {
        const XmlAsset& c = assets[cplId];
        const auto cpl = xmlItems(dir.filePath(c.path), &root);
        if (root != "CompositionPlaylist") continue;
        ++compositions;
        signature(c.path);
        struct Ref {
            QString kind, id, keyId, language;
            int64_t intrinsic = -1, entry = 0, duration = -1;
            int rate = 0;
        };
        std::vector<Ref> refs;
        QString subtitleLanguages;
        bool encryptedAssets = false, signedCpl = false;
        for (const auto& [path, text] : cpl) {
            const QStringList parts = path.split('/');
            if (parts.size() == 2 && parts[1] == "Signature") signedCpl = true;
            if (path.endsWith("CompositionMetadataAsset/MainSubtitleLanguageList")) subtitleLanguages = text;
            if (parts.size() != 6 || parts[1] != "ReelList" || parts[4] == "CompositionMetadataAsset") continue;
            const QString kind = parts[4], field = parts[5];
            if (field == "Id") refs.push_back({kind, bareId(text)});
            if (refs.empty()) continue;
            if (field == "IntrinsicDuration") refs.back().intrinsic = text.toLongLong();
            if (field == "EntryPoint") refs.back().entry = text.toLongLong();
            if (field == "Duration") refs.back().duration = text.toLongLong();
            if (field == "EditRate") refs.back().rate = text.section(' ', 0, 0).toInt();
            if (field == "KeyId") refs.back().keyId = bareId(text), encryptedAssets = true;
            if (field == "Language") refs.back().language = text;
        }
        if (encryptedAssets && !signedCpl) issue(c.path + " has encrypted assets but is not signed (servers refuse it)");
        bool picture = false;
        for (const Ref& r : refs) {
            if (!assets.count(r.id) || assets[r.id].hash.isEmpty()) {
                issue(QStringLiteral("%1 %2 in %3 is not in the package").arg(r.kind, r.id, c.path));
                continue;
            }
            const int64_t duration = r.duration >= 0 ? r.duration : r.intrinsic - r.entry;
            if (r.intrinsic >= 0 && r.entry + duration > r.intrinsic) issue(QStringLiteral("%1 %2 plays past its end").arg(r.kind, r.id));
            const QString& path = assets[r.id].path;
            const std::string file = dir.filePath(path).toStdString();
            dcp::TrackFileReader track;
            std::string err;
            if (!track.open(file, &err)) {
                issue(QStringLiteral("%1 cannot be read: %2").arg(path, QString::fromStdString(err)));
                continue;
            }
            if (track.encrypted() != !r.keyId.isEmpty())
                issue(track.encrypted() ? path + " is encrypted but the composition names no key for it" : path + " is not encrypted but the composition names a key for it");
            else if (track.encrypted() && QString::fromStdString(dcp::uuidString(track.keyId())) != r.keyId)
                issue(QStringLiteral("%1 is encrypted with key %2; the composition says %3").arg(path, QString::fromStdString(dcp::uuidString(track.keyId())), r.keyId));
            if (r.intrinsic >= 0 && track.containerDuration() != r.intrinsic && !track.timedText())
                issue(QStringLiteral("%1 has %2 frames; the composition says %3").arg(path).arg(track.containerDuration()).arg(r.intrinsic));
            if (r.kind == "MainPicture" || r.kind == "MainStereoscopicPicture") {
                picture = true;
                const bool stereo = r.kind == "MainStereoscopicPicture";
                if (track.stereo() != stereo) issue(stereo ? path + " is not stereoscopic" : path + " is stereoscopic; the composition calls it flat");
                if (track.editUnits() != track.containerDuration())
                    issue(QStringLiteral("%1 indexes %2 frames of %3").arg(path).arg(track.editUnits()).arg(track.containerDuration()));
                if (r.rate > 0 && double(track.largestEditUnit()) * 8 * r.rate > 250e6)
                    issue(QStringLiteral("%1 goes over 250 Mbit/s (a frame of %2 bytes%3)").arg(path).arg(track.largestEditUnit()).arg(stereo ? ", both eyes" : ""));
                if (track.encrypted()) {
                    reading(QFileInfo(dir.filePath(path)).size());  // its codestreams cannot be read without the key
                    continue;
                }
                // Every codestream: a DCI size in X'Y'Z' 12-bit, and the Bv2.1 rules (each told once).
                std::set<std::string> told;
                std::vector<uint8_t> cs;
                for (int64_t k = 0; k < track.editUnits() && !stopped; ++k)
                    for (int eye = 0; eye < (stereo && track.stereo() ? 2 : 1); ++eye) {
                        if (!track.read(k, eye, cs, nullptr, &err)) {
                            issue(QStringLiteral("%1: %2").arg(path, QString::fromStdString(err)));
                            k = track.editUnits();
                            break;
                        }
                        reading(qint64(cs.size()));
                        dcp::J2kHeader h;
                        if (!dcp::parseJ2kHeader(cs.data(), cs.size(), h)) {
                            if (told.insert("j2k").second) issue(QStringLiteral("%1 is not JPEG 2000 pictures").arg(path));
                            continue;
                        }
                        const int w = int(h.xsiz - h.xosiz), ht = int(h.ysiz - h.yosiz);
                        const bool dci = (w == 1998 && ht == 1080) || (w == 2048 && ht == 858) || (w == 2048 && ht == 1080) || (w == 3996 && ht == 2160) ||
                                         (w == 4096 && ht == 1716) || (w == 4096 && ht == 2160);
                        if (!dci && told.insert("size").second) issue(QStringLiteral("%1 is %2 x %3, not a DCI size").arg(path).arg(w).arg(ht));
                        bool xyz12 = h.csiz == 3;
                        for (const auto& comp : h.components) xyz12 = xyz12 && comp[0] == 11 && comp[1] == 1 && comp[2] == 1;
                        if (!xyz12 && told.insert("xyz").second) issue(QStringLiteral("%1 is not X'Y'Z' 12-bit").arg(path));
                        for (const std::string& rule : checkDciCodestream(cs.data(), cs.size()))
                            if (told.insert(rule).second) issue(QStringLiteral("%1 (frame %2): %3").arg(path).arg(k).arg(QString::fromStdString(rule)));
                    }
                if (stopped) {
                    issue("Stopped");
                    return issues;
                }
            } else if (r.kind == "MainSound") {
                if (track.encrypted()) {
                    reading(QFileInfo(dir.filePath(path)).size());
                    continue;
                }
                const TrackInfo t = probeTrack(file, reading);
                if (stopped) {
                    issue("Stopped");
                    return issues;
                }
                if (!t.ok || t.codec.rfind("pcm_s24", 0) != 0 || (t.sampleRate != 48000 && t.sampleRate != 96000))
                    issue(QStringLiteral("%1 is not 24-bit 48 or 96 kHz sound").arg(path));
            } else if (r.kind == "MainSubtitle" || r.kind == "ClosedCaption" || r.kind == "MainClosedCaption") {
                const bool closed = r.kind != "MainSubtitle";
                reading(QFileInfo(dir.filePath(path)).size());
                if (!track.timedText()) {
                    issue(path + " is not a timed text track file");
                    continue;
                }
                if (r.language.isEmpty()) issue(QStringLiteral("%1 has no language in the composition").arg(path));
                if (!closed && !subtitleLanguages.split(' ', Qt::SkipEmptyParts).contains(r.language))
                    issue(QStringLiteral("%1's language is not in the composition's subtitle languages").arg(path));
                if (track.encrypted()) continue;
                std::vector<uint8_t> xml;
                if (!track.read(0, 0, xml, nullptr, &err)) {
                    issue(QStringLiteral("%1: %2").arg(path, QString::fromStdString(err)));
                    continue;
                }
                std::vector<Caption> subs;
                DcstInfo info;
                if (!parseDcst(std::string(xml.begin(), xml.end()), Rational{r.rate > 0 ? r.rate : 24, 1}, subs, &err, &info)) {
                    issue(QStringLiteral("%1's subtitles cannot be read: %2").arg(path, QString::fromStdString(err)));
                    continue;
                }
                if (bareId(QString::fromStdString(info.id)) == r.id) issue(path + "'s subtitle document has the track file's own id (Bv2.1 asks for its own)");
                // The fonts it loads are in the file, each within 640 KiB.
                const std::string text(xml.begin(), xml.end());
                for (const auto& [resource, stream] : track.resources()) {
                    std::vector<uint8_t> data;
                    if (!track.readStream(stream, data, nullptr, &err)) issue(QStringLiteral("%1: %2").arg(path, QString::fromStdString(err)));
                    else if (data.size() > 655360) issue(QStringLiteral("%1's font is %2 bytes (Bv2.1 allows 640 KiB)").arg(path).arg(data.size()));
                }
                if (text.find("<LoadFont") != std::string::npos && track.resources().empty()) issue(path + " loads a font it does not carry");
                // Bv2.1's limits on lines (3 at most; 52 characters advised, 79 at most; 32 for closed captions),
                // timing (from 4 seconds in, 15 frames or more each, 2 frames apart) are advice except where a server refuses.
                const int rate = r.rate > 0 ? r.rate : 24;
                FrameTime previous = -1000;
                for (const Caption& sub : subs) {
                    const QStringList lines = QString::fromStdString(sub.text).split('\n');
                    int longest = 0;
                    for (const QString& l : lines) longest = std::max(longest, int(l.size()));
                    if (closed && (lines.size() > 3 || longest > 32))
                        issue(QStringLiteral("%1: a closed caption at %2 s has more than 3 lines or 32 characters to a line").arg(path).arg(double(sub.start) / rate));
                    else if (!closed && longest > 79)
                        issue(QStringLiteral("%1: a subtitle at %2 s has a line over 79 characters").arg(path).arg(double(sub.start) / rate));
                    if (advice) {
                        auto advise = [&](const QString& s) { advice->push_back(s.toStdString()); };
                        if (!closed && (lines.size() > 3 || longest > 52))
                            advise(QStringLiteral("%1: the subtitle at %2 s is over 3 lines or 52 characters to a line").arg(path).arg(double(sub.start) / rate));
                        if (sub.end - sub.start < 15) advise(QStringLiteral("%1: the subtitle at %2 s is on screen for under 15 frames").arg(path).arg(double(sub.start) / rate));
                        if (sub.start - previous < 2 && previous >= 0)
                            advise(QStringLiteral("%1: the subtitle at %2 s follows the one before within 2 frames").arg(path).arg(double(sub.start) / rate));
                    }
                    previous = sub.end;
                }
                if (advice && !subs.empty() && subs.front().start < 4 * rate)
                    advice->push_back(QStringLiteral("%1: the first subtitle is in the reel's first 4 seconds").arg(path).toStdString());
            }
        }
        if (!picture) issue(c.path + " has no picture");
    }
    if (compositions == 0) issue("The package holds no composition playlist");
    return issues;
}

std::vector<std::string> checkDciCodestream(const uint8_t* d, size_t n) {
    std::vector<std::string> out;
    size_t i = 0;
    auto u8 = [&]() -> int { return i < n ? d[i++] : -1; };
    auto u16 = [&]() -> int {
        if (i + 2 > n) return -1;
        i += 2;
        return d[i - 2] << 8 | d[i - 1];
    };
    auto u32 = [&]() -> int64_t {
        if (i + 4 > n) return -1;
        i += 4;
        return int64_t(d[i - 4]) << 24 | d[i - 3] << 16 | d[i - 2] << 8 | d[i - 1];
    };
    if (u16() != 0xff4f || u16() != 0xff51 || u16() != 47) return {"not a DCI codestream (its first marker is not a 3-component SIZ)"};
    const int rsiz = u16();
    const int64_t width = u32(), height = u32();
    const bool fourK = width > 2048;
    if (rsiz != (fourK ? 4 : 3)) out.push_back(fourK ? "not in the DCI 4K profile" : "not in the DCI 2K profile");
    const int64_t x0 = u32(), y0 = u32(), tw = u32(), th = u32(), tx = u32(), ty = u32();
    if (x0 || y0 || tx || ty || tw != width || th != height) out.push_back("not one tile the size of the picture");
    if (u16() != 3) out.push_back("not three components");
    i += 9;
    int cod = 0, qcd = 0, pocMain = 0, pocAfter = 0;
    bool tlm = false, inMain = true, partsTold = false;
    while (i + 2 <= n) {
        if (d[i] != 0xff) {
            out.push_back("a marker is missing its start");
            break;
        }
        const int marker = d[i + 1];
        i += 2;
        if (marker == 0xd9) break;  // EOC
        if (marker == 0x90) {       // SOT
            if (u16() != 10) {
                out.push_back("a damaged tile part");
                break;
            }
            u16();
            const int64_t length = u32();
            u8();
            const int parts = u8();
            if (parts != (fourK ? 6 : 3) && !partsTold) out.push_back(fourK ? "not 6 tile parts" : "not 3 tile parts"), partsTold = true;
            inMain = false;
            // Straight on to the next tile part.
            if (length < 14) break;
            i = i - 12 + size_t(length);
            continue;
        }
        const int length = u16();
        if (length < 2) {
            out.push_back("a damaged marker");
            break;
        }
        const size_t next = i + size_t(length) - 2;
        if (marker == 0x52) {  // COD
            ++cod;
            const int style = u8(), order = u8(), layers = u16(), mct = u8(), levels = u8(), cbw = u8(), cbh = u8(), modes = u8(), wavelet = u8();
            if (style != 1 || order != 4 || layers != 1 || mct != 1) out.push_back("not CPRL with one layer, precincts and the component transform");
            if (levels != (fourK ? 6 : 5)) out.push_back(fourK ? "not 6 decomposition levels" : "not 5 decomposition levels");
            if (cbw != 3 || cbh != 3) out.push_back("code-blocks not 32 x 32");
            if (modes != 0 || wavelet != 0) out.push_back("not the 9-7 wavelet without coding modes");
            bool precincts = u8() == 0x77;
            for (int k = 0; k < levels; ++k) precincts = precincts && u8() == 0x88;
            if (!precincts) out.push_back("not the DCI precincts");
        } else if (marker == 0x5c) {  // QCD
            ++qcd;
            const int guard = (u8() >> 5) & 7;
            if (guard != (fourK ? 2 : 1)) out.push_back(fourK ? "guard bits not 2 (Bv2.1)" : "guard bits not 1 (Bv2.1)");
        } else if (marker == 0x55) {  // TLM
            tlm = true;
        } else if (marker == 0x5f) {  // POC
            (inMain ? pocMain : pocAfter)++;
        }
        i = next;
    }
    if (cod != 1 || qcd != 1) out.push_back("not one COD and one QCD marker");
    if (pocMain != (fourK ? 1 : 0) || pocAfter) out.push_back(fourK ? "not one progression order change in the main header" : "a progression order change at 2K");
    if (!tlm) out.push_back("no TLM marker (Bv2.1)");
    return out;
}

bool readDcpFrame(const std::string& mxf, int index, std::vector<uint16_t>& xyz, int& width, int& height, std::string* error, const uint8_t* key,
                  int eye) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    dcp::TrackFileReader track;
    std::vector<uint8_t> cs;
    std::string err;
    if (!track.open(mxf, &err) || !track.read(index, eye, cs, key, &err)) return fail(err);
    // OpenJPEG when Montage has it, else FFmpeg's decoder.
    if (openJpegAvailable()) {
        int components = 0, bits = 0;
        if (!decodeJpeg2000(cs.data(), cs.size(), xyz, width, height, components, bits, &err)) return fail(err);
        if (components != 3 || bits != 12) return fail("Frame " + std::to_string(index) + " is not X'Y'Z' 12-bit");
        return true;
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_JPEG2000);
    if (!codec) return fail("No JPEG 2000 decoder");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    struct Free {
        AVCodecContext*& c;
        ~Free() { avcodec_free_context(&c); }
    } freer{ctx};
    if (avcodec_open2(ctx, codec, nullptr) < 0) return fail("Cannot decode " + mxf);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    av_new_packet(pkt, int(cs.size()));
    std::memcpy(pkt->data, cs.data(), cs.size());
    const bool got = avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, frame) == 0;
    av_packet_free(&pkt);
    const bool xyzFormat = frame->format == AV_PIX_FMT_XYZ12LE || frame->format == AV_PIX_FMT_XYZ12BE;
    const bool rgbFormat = frame->format == AV_PIX_FMT_RGB48LE || frame->format == AV_PIX_FMT_RGB48BE;
    if (!got || (!xyzFormat && !rgbFormat)) {
        av_frame_free(&frame);
        return fail("Frame " + std::to_string(index) + " is not X'Y'Z' 12-bit");
    }
    width = frame->width, height = frame->height;
    xyz.resize(size_t(width) * size_t(height) * 3);
    const bool le = frame->format == AV_PIX_FMT_XYZ12LE || frame->format == AV_PIX_FMT_RGB48LE;
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = frame->data[0] + ptrdiff_t(y) * frame->linesize[0];
        for (int i = 0; i < width * 3; ++i) {
            const uint16_t v = le ? uint16_t(s[2 * i] | s[2 * i + 1] << 8) : uint16_t(s[2 * i] << 8 | s[2 * i + 1]);
            xyz[size_t(y) * size_t(width) * 3 + size_t(i)] = uint16_t(v >> 4);
        }
    }
    av_frame_free(&frame);
    return true;
}

}  // namespace montage
