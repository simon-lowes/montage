#include "Dcp.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
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
#include "core/Surround.h"
#include "media/SuperScale.h"

namespace montage {

namespace {

constexpr int kSampleRate = 48000;

QString urn(const dcp::Uuid& u) { return QStringLiteral("urn:uuid:") + QString::fromStdString(dcp::uuidString(u)); }

QString isoNow() { return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) + QStringLiteral("+00:00"); }

// SHA-1 of a file, base64, as packing lists give it.
QString fileHash(const QString& path, qint64* size = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (size) *size = f.size();
    QCryptographicHash h(QCryptographicHash::Sha1);
    while (!f.atEnd()) h.addData(f.read(1 << 20));
    return QString::fromLatin1(h.result().toBase64());
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
    ~J2kEncoder() {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
    }
    bool open(int w, int h, int fps, std::string* error) {
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
        if (codec->pix_fmts)
            for (const AVPixelFormat* f = codec->pix_fmts; *f != AV_PIX_FMT_NONE; ++f)
                if (*f == AV_PIX_FMT_XYZ12LE) ctx->pix_fmt = AV_PIX_FMT_XYZ12LE;
        av_opt_set(ctx->priv_data, "format", "j2k", 0);  // a bare codestream, not a JP2 file
        if (openjpeg) {
            // The DCI 2K profile; the cinema mode keeps each frame within the 250 Mbit/s limit (the 48 fps one,
            // half the size, for every rate above 24).
            if (av_opt_set(ctx->priv_data, "profile", "cinema2k", 0) < 0) ctx->profile = 3;  // FF_PROFILE_JPEG2000_DCINEMA_2K
            av_opt_set(ctx->priv_data, "cinema_mode", fps <= 24 ? "2k_24" : "2k_48", 0);
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
    EncoderPool(int w, int h, int fps, int threads) {
        for (int i = 0; i < threads; ++i)
            workers_.emplace_back([this, w, h, fps] {
                J2kEncoder enc;
                std::string err;
                const bool ok = enc.open(w, h, fps, &err);
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
    // Waits while `limit` frames are in hand.
    void push(int64_t index, std::vector<uint16_t>&& xyz, size_t limit) {
        std::unique_lock<std::mutex> lock(m_);
        done_.wait(lock, [&] { return !error_.empty() || todo_.size() + done_map_.size() < limit; });
        todo_.emplace_back(index, std::move(xyz));
        work_.notify_one();
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

bool dcpContainer(const std::string& container, int& width, int& height) {
    if (container == "flat") width = 1998, height = 1080;
    else if (container == "scope") width = 2048, height = 858;
    else if (container == "full") width = 2048, height = 1080;
    else return false;
    return true;
}

std::string defaultDcpContainer(const Sequence& s) { return s.height > 0 && double(s.width) / s.height >= 2.0 ? "scope" : "flat"; }

int dcpFrameRate(const Sequence& s, int requested) {
    static const int rates[] = {24, 25, 30, 48};
    for (int r : rates)
        if (r == requested) return r;
    const double fps = s.fpsValue();
    int best = 24;
    for (int r : rates)
        if (std::fabs(r - fps) < std::fabs(best - fps)) best = r;
    return best;
}

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
    QStringList parts = {titleForName(settings.title), kind != kinds.end() ? kind->second : QStringLiteral("FTR"), aspect, language + "-XX",
                         territory, channels == 8 ? QStringLiteral("71") : QStringLiteral("51"), QStringLiteral("2K")};
    if (!code(settings.studio).isEmpty()) parts << code(settings.studio);
    parts << QString::fromStdString(date);
    if (!code(settings.facility).isEmpty()) parts << code(settings.facility);
    parts << QStringLiteral("SMPTE") << QStringLiteral("OV");
    return parts.join('_').toStdString();
}

bool exportDcp(const Project& p, const Sequence& s, const DcpSettings& settings, const std::string& parent, DcpResult* result,
               const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    int cw = 0, ch = 0;
    if (!dcpContainer(settings.container, cw, ch)) return fail("The container is flat, scope or full");
    if (s.width <= 0 || s.height <= 0) return fail("The sequence has no picture size");
    const int fps = dcpFrameRate(s, settings.fps);
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint > s.inPoint) first = s.inPoint, end = s.outPoint;
    if (end <= first) return fail("The sequence is empty");
    const int64_t frames = end - first;
    if (frames < fps) return fail("A DCP must last at least a second (cinema servers refuse shorter reels)");
    const int seqChannels = layoutChannels(s.audioLayout);
    const int channels = seqChannels == 8 ? 8 : 6;

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

    const dcp::Uuid pictureId = dcp::newUuid(), soundId = dcp::newUuid();
    const QString pictureFile = QStringLiteral("j2c_%1.mxf").arg(QString::fromStdString(dcp::uuidString(pictureId)));
    const QString soundFile = QStringLiteral("pcm_%1.mxf").arg(QString::fromStdString(dcp::uuidString(soundId)));

    // Sound first (quick): the mix at 48 kHz, a picture frame's worth at a time. A 23.976 sequence plays at 24, so its
    // sound is mixed at the rate that makes each sequence frame 2000 samples, and plays 0.1 % faster.
    {
        dcp::SoundMxfWriter sound;
        std::string err;
        if (!sound.open(QDir(folder).filePath(soundFile).toStdString(), soundId, fps, channels, settings.language, &err)) return fail(err);
        Sequence mixSeq = s;
        const int perFrame = kSampleRate / fps;
        mixSeq.sampleRate = int(std::lround(perFrame * s.fpsValue()));
        AudioMixer mixer;
        std::vector<float> mix(size_t(perFrame) * size_t(std::max(2, seqChannels))), out(size_t(perFrame) * size_t(channels));
        // Where each of the sequence's channels goes: stereo to L and R; 5.1 as it is; 7.1 (L R C LFE Lb Rb Ls Rs, as
        // FFmpeg orders it) to 7.1 DS (L R C LFE Lss Rss Lrs Rrs).
        std::vector<int> route;
        if (seqChannels == 8) route = {0, 1, 2, 3, 6, 7, 4, 5};
        else if (seqChannels == 6) route = {0, 1, 2, 3, 4, 5};
        else route = {0, 1};
        for (int64_t k = 0; k < frames; ++k) {
            const int64_t start = (first + k) * perFrame;
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

    // The picture, fitted inside the container on black, several frames encoding at once.
    const double scale = std::min(double(cw) / s.width, double(ch) / s.height);
    const int fw = std::min(cw, int(std::lround(s.width * scale / 2)) * 2), fh = std::min(ch, int(std::lround(s.height * scale / 2)) * 2);
    const int ox = (cw - fw) / 2, oy = (ch - fh) / 2;
    const ColorSpace* space = &sequenceColorSpace(s);
    if (space->hdr()) space = findColorSpace("p3d65");  // tone mapped into the cinema's range, its gamut kept
    const XyzConverter xyz(*space);
    const int threads = settings.threads > 0 ? settings.threads : std::max(1, int(std::thread::hardware_concurrency()));
    bool cinema = true;
    {
        dcp::PictureMxfWriter picture;
        std::string err;
        if (!picture.open(QDir(folder).filePath(pictureFile).toStdString(), pictureId, fps, &err)) return fail(err);
        EncoderPool pool(cw, ch, fps, threads);
        RenderOptions o;
        o.scale = scale;
        o.highQuality = true;
        int64_t written = 0;
        std::vector<uint8_t> cs;
        auto writeReady = [&](bool wait) {
            while (written < frames && pool.take(written, cs, wait)) {
                if (written == 0) {
                    dcp::J2kHeader h;
                    if (dcp::parseJ2kHeader(cs.data(), cs.size(), h)) cinema = h.rsiz == 3 && pool.openjpeg();
                }
                if (!picture.write(cs.data(), cs.size(), &err)) return false;
                ++written;
                wait = false;
            }
            return pool.error().empty();
        };
        for (int64_t k = 0; k < frames; ++k) {
            Image frame = renderProgramFrame(p, s, first + k, o);
            if (frame.width != fw || frame.height != fh) frame = resizeImage(frame, fw, fh);
            if (space != &sequenceColorSpace(s)) convertColor(frame, sequenceColorSpace(s), *space, s.hdrPeakNits);
            Image boxed(cw, ch);  // black around it
            for (int y = 0; y < fh; ++y) std::copy(frame.row(y), frame.row(y) + size_t(fw) * 4, boxed.row(y + oy) + size_t(ox) * 4);
            std::vector<uint16_t> codes;
            xyz.convert(boxed, codes);
            pool.push(k, std::move(codes), size_t(threads) * 2);
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
    const QString issued = isoNow();
    const QString title = QString::fromStdString(settings.title.empty() ? "Untitled" : settings.title);
    const QString issuer = QString::fromStdString(settings.issuer.empty() ? "Montage" : settings.issuer);
    const QString rate = QStringLiteral("%1 1").arg(fps);
    const dcp::Uuid cplId = dcp::newUuid(), pklId = dcp::newUuid(), amId = dcp::newUuid();
    const QString cplFile = QStringLiteral("CPL_%1.xml").arg(QString::fromStdString(dcp::uuidString(cplId)));
    const QString pklFile = QStringLiteral("PKL_%1.xml").arg(QString::fromStdString(dcp::uuidString(pklId)));
    qint64 pictureSize = 0, soundSize = 0;
    const QString pictureHash = fileHash(QDir(folder).filePath(pictureFile), &pictureSize);
    const QString soundHash = fileHash(QDir(folder).filePath(soundFile), &soundSize);
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
    const char* kMeta = "http://www.smpte-ra.org/schemas/429-16/2014/CPL-Metadata";
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
        x.writeStartElement("MainPicture");
        writeText(x, "Id", urn(pictureId));
        writeText(x, "AnnotationText", pictureFile);
        writeText(x, "EditRate", rate);
        writeText(x, "IntrinsicDuration", QString::number(frames));
        writeText(x, "EntryPoint", "0");
        writeText(x, "Duration", QString::number(frames));
        writeText(x, "Hash", pictureHash);
        writeText(x, "FrameRate", rate);
        writeText(x, "ScreenAspectRatio", QStringLiteral("%1 %2").arg(cw).arg(ch));
        x.writeEndElement();
        x.writeStartElement("MainSound");
        writeText(x, "Id", urn(soundId));
        writeText(x, "AnnotationText", soundFile);
        writeText(x, "EditRate", rate);
        writeText(x, "IntrinsicDuration", QString::number(frames));
        writeText(x, "EntryPoint", "0");
        writeText(x, "Duration", QString::number(frames));
        writeText(x, "Hash", soundHash);
        x.writeEndElement();
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
    if (!cplOk) return fail("Cannot write the composition playlist");
    qint64 cplSize = 0;
    const QString cplHash = fileHash(QDir(folder).filePath(cplFile), &cplSize);
    const std::vector<Asset> assets = {{cplId, cplFile, "text/xml", title},
                                       {pictureId, pictureFile, "application/mxf", pictureFile},
                                       {soundId, soundFile, "application/mxf", soundFile}};
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
            qint64 size = 0;
            const QString hash = a.file == cplFile ? cplHash : a.file == pictureFile ? pictureHash : soundHash;
            size = a.file == cplFile ? cplSize : a.file == pictureFile ? pictureSize : soundSize;
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
    if (!pklOk) return fail("Cannot write the packing list");
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
    }
    return true;
}

// ---- Checking -----------------------------------------------------------------------------------------------------

namespace {

struct XmlAsset {
    QString id, path, hash, type;
    qint64 size = -1, length = -1;
    bool packingList = false;
};

// Flattens the elements of an XML file into (path of element names, text) pairs, enough to read DCP documents.
std::vector<std::pair<QString, QString>> xmlItems(const QString& file, QString* root = nullptr) {
    std::vector<std::pair<QString, QString>> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QXmlStreamReader r(&f);
    QStringList stack;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            if (stack.isEmpty() && root) *root = r.name().toString();
            stack << r.name().toString();
            out.push_back({stack.join('/'), {}});
        } else if (r.isCharacters() && !r.isWhitespace() && !out.empty()) {
            out.back().second += r.text().toString().trimmed();
        } else if (r.isEndElement()) {
            if (!stack.isEmpty()) stack.removeLast();
        }
    }
    if (r.hasError()) out.clear();
    return out;
}

QString bareId(QString id) { return id.remove(QStringLiteral("urn:uuid:")).toLower(); }

struct TrackInfo {
    bool ok = false;
    std::string codec, pixfmt;
    int width = 0, height = 0, sampleRate = 0, channels = 0, bits = 0;
    int64_t packets = 0, largest = 0;
};

TrackInfo probeTrack(const std::string& path) {
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
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == 0) {
                ++t.packets;
                t.largest = std::max<int64_t>(t.largest, pkt->size);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        t.ok = true;
    }
    avformat_close_input(&fmt);
    return t;
}

}  // namespace

std::vector<std::string> verifyDcp(const std::string& folderPath) {
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
    for (auto& [id, a] : assets) {
        if (a.packingList || a.hash.isEmpty()) continue;
        qint64 size = 0;
        const QString hash = fileHash(dir.filePath(a.path), &size);
        if (hash != a.hash) issue(QStringLiteral("%1 does not match its hash in the packing list (damaged or changed)").arg(a.path));
        if (a.size >= 0 && size != a.size) issue(QStringLiteral("%1 is %2 bytes; the packing list says %3").arg(a.path).arg(size).arg(a.size));
    }
    // Compositions: their assets, durations and track files.
    int compositions = 0;
    for (const QString& cplId : cpls) {
        const XmlAsset& c = assets[cplId];
        const auto cpl = xmlItems(dir.filePath(c.path), &root);
        if (root != "CompositionPlaylist") continue;
        ++compositions;
        struct Ref {
            QString kind, id;
            int64_t intrinsic = -1, entry = 0, duration = -1;
            int rate = 0;
        };
        std::vector<Ref> refs;
        for (const auto& [path, text] : cpl) {
            const QStringList parts = path.split('/');
            if (parts.size() < 6 || parts[4] == "CompositionMetadataAsset") continue;
            const QString kind = parts[4], field = parts[5];
            if (parts.size() != 6) continue;
            if (field == "Id") refs.push_back({kind, bareId(text)});
            if (refs.empty()) continue;
            if (field == "IntrinsicDuration") refs.back().intrinsic = text.toLongLong();
            if (field == "EntryPoint") refs.back().entry = text.toLongLong();
            if (field == "Duration") refs.back().duration = text.toLongLong();
            if (field == "EditRate") refs.back().rate = text.section(' ', 0, 0).toInt();
        }
        bool picture = false;
        for (const Ref& r : refs) {
            if (!assets.count(r.id) || assets[r.id].hash.isEmpty()) {
                issue(QStringLiteral("%1 %2 in %3 is not in the package").arg(r.kind, r.id, c.path));
                continue;
            }
            const int64_t duration = r.duration >= 0 ? r.duration : r.intrinsic - r.entry;
            if (r.intrinsic >= 0 && r.entry + duration > r.intrinsic) issue(QStringLiteral("%1 %2 plays past its end").arg(r.kind, r.id));
            const std::string file = dir.filePath(assets[r.id].path).toStdString();
            const TrackInfo t = probeTrack(file);
            if (r.kind == "MainPicture") {
                picture = true;
                const bool dci = (t.width == 1998 && t.height == 1080) || (t.width == 2048 && t.height == 858) || (t.width == 2048 && t.height == 1080) ||
                                 (t.width == 3996 && t.height == 2160) || (t.width == 4096 && t.height == 1716) || (t.width == 4096 && t.height == 2160);
                if (!t.ok || t.codec != "jpeg2000") issue(QStringLiteral("%1 is not JPEG 2000 pictures").arg(assets[r.id].path));
                else {
                    if (t.pixfmt.find("xyz12") == std::string::npos) issue(QStringLiteral("%1 is not X'Y'Z' 12-bit").arg(assets[r.id].path));
                    if (!dci) issue(QStringLiteral("%1 is %2 x %3, not a DCI size").arg(assets[r.id].path).arg(t.width).arg(t.height));
                    if (r.intrinsic >= 0 && t.packets != r.intrinsic)
                        issue(QStringLiteral("%1 has %2 frames; the composition says %3").arg(assets[r.id].path).arg(t.packets).arg(r.intrinsic));
                    if (r.rate > 0 && double(t.largest) * 8 * r.rate > 250e6)
                        issue(QStringLiteral("%1 goes over 250 Mbit/s (a frame of %2 bytes)").arg(assets[r.id].path).arg(t.largest));
                }
            } else if (r.kind == "MainSound") {
                if (!t.ok || t.codec.rfind("pcm_s24", 0) != 0 || (t.sampleRate != 48000 && t.sampleRate != 96000))
                    issue(QStringLiteral("%1 is not 24-bit 48 or 96 kHz sound").arg(assets[r.id].path));
            }
        }
        if (!picture) issue(c.path + " has no picture");
    }
    if (compositions == 0) issue("The package holds no composition playlist");
    return issues;
}

bool readDcpFrame(const std::string& mxf, int index, std::vector<uint16_t>& xyz, int& width, int& height, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, mxf.c_str(), nullptr, nullptr) < 0) return fail("Cannot open " + mxf);
    struct Close {
        AVFormatContext*& f;
        ~Close() { avformat_close_input(&f); }
    } closer{fmt};
    if (avformat_find_stream_info(fmt, nullptr) < 0 || fmt->nb_streams == 0) return fail("No picture in " + mxf);
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[0]->codecpar->codec_id);
    if (!codec) return fail("No decoder for " + mxf);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[0]->codecpar);
    struct Free {
        AVCodecContext*& c;
        ~Free() { avcodec_free_context(&c); }
    } freer{ctx};
    if (avcodec_open2(ctx, codec, nullptr) < 0) return fail("Cannot decode " + mxf);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int n = -1;
    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == 0 && ++n == index && avcodec_send_packet(ctx, pkt) >= 0 && avcodec_receive_frame(ctx, frame) == 0) got = true;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (!got || (frame->format != AV_PIX_FMT_XYZ12LE && frame->format != AV_PIX_FMT_XYZ12BE)) {
        av_frame_free(&frame);
        return fail("Frame " + std::to_string(index) + " is not X'Y'Z' 12-bit");
    }
    width = frame->width, height = frame->height;
    xyz.resize(size_t(width) * size_t(height) * 3);
    const bool le = frame->format == AV_PIX_FMT_XYZ12LE;
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
