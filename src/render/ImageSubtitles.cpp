#include "ImageSubtitles.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include "core/SpellCheck.h"
#include "media/TextReader.h"
#include "render/Compositor.h"

namespace montage {

namespace {

std::string averr(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof buf);
    return buf;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& path) { return "." + QFileInfo(QString::fromStdString(path)).suffix().toLower().toStdString(); }

double luminance(uint32_t argb) {
    return (0.2126 * double((argb >> 16) & 0xff) + 0.7152 * double((argb >> 8) & 0xff) + 0.0722 * double(argb & 0xff)) / 255.0;
}

int64_t ticksOf(FrameTime frame, Rational fps) {
    // 90 kHz ticks of a frame's start.
    return int64_t(std::llround(double(frame) * 90000.0 * double(fps.den) / double(fps.num)));
}

}  // namespace

// ---- Names ------------------------------------------------------------------------------------------------------

const char* imageSubtitleFormatId(ImageSubtitleFormat f) {
    switch (f) {
        case ImageSubtitleFormat::Pgs: return "pgs";
        case ImageSubtitleFormat::Dvb: return "dvb";
        case ImageSubtitleFormat::Vobsub: return "vobsub";
        case ImageSubtitleFormat::ImscImage: return "imsc_image";
    }
    return "pgs";
}

bool imageSubtitleFormatFromId(const std::string& id, ImageSubtitleFormat& out) {
    for (ImageSubtitleFormat f : {ImageSubtitleFormat::Pgs, ImageSubtitleFormat::Dvb, ImageSubtitleFormat::Vobsub, ImageSubtitleFormat::ImscImage})
        if (id == imageSubtitleFormatId(f)) {
            out = f;
            return true;
        }
    if (id == "dvd") {
        out = ImageSubtitleFormat::Vobsub;
        return true;
    }
    return false;
}

bool imageSubtitleFormatFor(const std::string& extension, ImageSubtitleFormat& out) {
    std::string e = lower(extension);
    if (!e.empty() && e[0] != '.') e = "." + e;
    if (e == ".sup") out = ImageSubtitleFormat::Pgs;
    else if (e == ".idx" || e == ".sub") out = ImageSubtitleFormat::Vobsub;
    else if (e == ".ts" || e == ".m2ts" || e == ".mts") out = ImageSubtitleFormat::Dvb;
    else return false;
    return true;
}

std::string languageFromTag(const std::string& tag) {
    const std::string t = lower(tag.substr(0, tag.find_first_of("-_")));
    if (t.size() == 2) return t;
    // ISO 639-2, terminological and bibliographic.
    static const std::pair<const char*, const char*> table[] = {
        {"eng", "en"}, {"spa", "es"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"}, {"ger", "de"}, {"ita", "it"},
        {"por", "pt"}, {"nld", "nl"}, {"dut", "nl"}, {"pol", "pl"}, {"swe", "sv"}, {"dan", "da"}, {"nor", "no"},
        {"nob", "nb"}, {"nno", "nn"}, {"fin", "fi"}, {"ces", "cs"}, {"cze", "cs"}, {"slk", "sk"}, {"slo", "sk"},
        {"ell", "el"}, {"gre", "el"}, {"tur", "tr"}, {"rus", "ru"}, {"ukr", "uk"}, {"ara", "ar"}, {"heb", "he"},
        {"hin", "hi"}, {"ind", "id"}, {"vie", "vi"}, {"tha", "th"}, {"jpn", "ja"}, {"kor", "ko"}, {"zho", "zh"},
        {"chi", "zh"}, {"cym", "cy"}, {"wel", "cy"}, {"gle", "ga"}, {"hun", "hu"}, {"ron", "ro"}, {"rum", "ro"},
        {"hrv", "hr"}, {"srp", "sr"}, {"slv", "sl"}, {"bul", "bg"}, {"est", "et"}, {"lav", "lv"}, {"lit", "lt"},
        {"isl", "is"}, {"ice", "is"}, {"cat", "ca"}, {"eus", "eu"}, {"baq", "eu"}, {"glg", "gl"}, {"msa", "ms"},
        {"may", "ms"}, {"fas", "fa"}, {"per", "fa"}};
    for (const auto& [three, two] : table)
        if (t == three) return two;
    return {};
}

// ---- Decoding ---------------------------------------------------------------------------------------------------

std::vector<ImageSubtitleStream> imageSubtitleStreams(const std::string& path) {
    std::vector<ImageSubtitleStream> out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return out;
    // (MPEG program and transport streams find their streams as their packets come.)
    if (fmt->ctx_flags & AVFMTCTX_NOHEADER) avformat_find_stream_info(fmt, nullptr);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecID id = st->codecpar->codec_id;
        const char* codec = id == AV_CODEC_ID_HDMV_PGS_SUBTITLE ? "pgs" : id == AV_CODEC_ID_DVB_SUBTITLE ? "dvb"
                            : id == AV_CODEC_ID_DVD_SUBTITLE    ? "dvd"
                                                                : nullptr;
        if (!codec) continue;
        ImageSubtitleStream s;
        s.index = int(i);
        s.codec = codec;
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0)) s.language = languageFromTag(e->value);
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "title", nullptr, 0)) s.title = e->value;
        out.push_back(std::move(s));
    }
    avformat_close_input(&fmt);
    return out;
}

namespace {

// Several pictures shown at once (the rects of one subtitle) as one, on their bounding box.
SubtitlePicture joinRects(const AVSubtitle& sub) {
    SubtitlePicture p;
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* r = sub.rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0) continue;
        x0 = std::min(x0, r->x), y0 = std::min(y0, r->y);
        x1 = std::max(x1, r->x + r->w), y1 = std::max(y1, r->y + r->h);
    }
    if (x1 <= x0 || y1 <= y0) return p;
    p.x = x0, p.y = y0;
    p.image = QImage(x1 - x0, y1 - y0, QImage::Format_ARGB32);
    p.image.fill(0);
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* r = sub.rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1]) continue;
        const uint32_t* palette = reinterpret_cast<const uint32_t*>(r->data[1]);
        for (int y = 0; y < r->h; ++y) {
            const uint8_t* src = r->data[0] + size_t(y) * size_t(r->linesize[0]);
            auto* dst = reinterpret_cast<uint32_t*>(p.image.scanLine(r->y - y0 + y)) + (r->x - x0);
            for (int x = 0; x < r->w; ++x)
                if (src[x] < r->nb_colors && (palette[src[x]] >> 24)) dst[x] = palette[src[x]];
        }
    }
    return p;
}

}  // namespace

bool decodeImscImage(const std::string& xml, const std::function<QByteArray(const std::string&)>& image, DecodedSubtitles& out,
                     std::string* error) {
    out = {};
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    double rate = 30, multiplier = 1;
    struct Region {
        int x = 0, y = 0;
    };
    std::map<QString, Region> regions;
    auto px = [](const QString& v, int& a, int& b) {
        const QStringList parts = v.simplified().split(' ');
        if (parts.size() != 2 || !parts[0].endsWith("px") || !parts[1].endsWith("px")) return false;
        a = int(std::lround(parts[0].chopped(2).toDouble()));
        b = int(std::lround(parts[1].chopped(2).toDouble()));
        return true;
    };
    auto attr = [](const QXmlStreamAttributes& as, const QString& name) {
        for (const QXmlStreamAttribute& a : as)
            if (a.name() == name) return a.value().toString();
        return QString();
    };
    auto time = [&](const QString& text) -> double {
        static const QRegularExpression clock(QStringLiteral("^(\\d+):(\\d{2}):(\\d{2})(?:(\\.\\d+)|:(\\d+))?$"));
        const auto m = clock.match(text.trimmed());
        if (!m.hasMatch()) return -1;
        double t = m.captured(1).toDouble() * 3600 + m.captured(2).toDouble() * 60 + m.captured(3).toDouble();
        if (!m.captured(4).isEmpty()) t += m.captured(4).toDouble();
        if (!m.captured(5).isEmpty()) t += m.captured(5).toDouble() / (rate * multiplier);
        return t;
    };
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        const QXmlStreamAttributes as = r.attributes();
        if (r.name() == QLatin1String("tt")) {
            px(attr(as, "extent"), out.width, out.height);
            if (const QString fr = attr(as, "frameRate"); !fr.isEmpty()) rate = fr.toDouble();
            if (const QStringList m = attr(as, "frameRateMultiplier").split(' '); m.size() == 2 && m[1].toDouble() > 0)
                multiplier = m[0].toDouble() / m[1].toDouble();
            if (const QString l = attr(as, "lang"); !l.isEmpty()) out.language = languageFromTag(l.toStdString());
        } else if (r.name() == QLatin1String("region")) {
            Region g;
            px(attr(as, "origin"), g.x, g.y);
            regions[attr(as, "id")] = g;
        } else if (r.name() == QLatin1String("div")) {
            const QString name = attr(as, "backgroundImage");
            if (name.isEmpty()) continue;
            SubtitlePicture p;
            p.start = time(attr(as, "begin"));
            p.end = time(attr(as, "end"));
            if (p.start < 0 || p.end <= p.start) continue;
            const auto g = regions.find(attr(as, "region"));
            if (g != regions.end()) p.x = g->second.x, p.y = g->second.y;
            QImage png;
            png.loadFromData(image ? image(name.toStdString()) : QByteArray(), "PNG");
            p.image = png.convertToFormat(QImage::Format_ARGB32);
            if (p.image.isNull()) {
                if (error) *error = "Cannot read the subtitle picture " + name.toStdString();
                return false;
            }
            out.pictures.push_back(std::move(p));
        }
    }
    if (r.hasError()) {
        if (error) *error = "Not an IMSC document: " + r.errorString().toStdString();
        return false;
    }
    out.codec = "imsc";
    std::sort(out.pictures.begin(), out.pictures.end(), [](const SubtitlePicture& a, const SubtitlePicture& b) { return a.start < b.start; });
    return true;
}

bool isImscImage(const std::string& xml) {
    return xml.find("profile/imsc1.1/image") != std::string::npos || xml.find("profile/imsc1/image") != std::string::npos ||
           xml.find("backgroundImage") != std::string::npos;
}

bool decodeImageSubtitles(const std::string& path, DecodedSubtitles& out, int stream, std::string* error, const std::atomic<bool>* cancel) {
    out = {};
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string ext = extensionOf(path);
    if (ext == ".ttml" || ext == ".xml" || ext == ".dfxp") {
        // The document, and the PNGs it names beside it.
        QFile f(QString::fromStdString(path));
        if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + path);
        const QDir dir = QFileInfo(f).absoluteDir();
        return decodeImscImage(f.readAll().toStdString(), [&](const std::string& name) {
            QFile png(dir.filePath(QString::fromStdString(name)));
            return png.open(QIODevice::ReadOnly) ? png.readAll() : QByteArray();
        }, out, error);
    }
    AVFormatContext* fmt = nullptr;
    if (int rc = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr); rc < 0) return fail("Cannot open " + path + ": " + averr(rc));
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmtOwner(fmt, [](AVFormatContext* f) { avformat_close_input(&f); });
    avformat_find_stream_info(fmt, nullptr);
    int index = -1;
    int videoW = 0, videoH = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !videoW) videoW = par->width, videoH = par->height;
        const bool image = par->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE || par->codec_id == AV_CODEC_ID_DVB_SUBTITLE ||
                           par->codec_id == AV_CODEC_ID_DVD_SUBTITLE;
        if (image && (stream < 0 ? index < 0 : int(i) == stream)) index = int(i);
    }
    if (index < 0) return fail(stream < 0 ? "The file holds no PGS, DVB or DVD subtitles" : "That stream is not PGS, DVB or DVD subtitles");
    AVStream* st = fmt->streams[index];
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != index) fmt->streams[i]->discard = AVDISCARD_ALL;
    const AVCodecID id = st->codecpar->codec_id;
    out.codec = id == AV_CODEC_ID_HDMV_PGS_SUBTITLE ? "pgs" : id == AV_CODEC_ID_DVB_SUBTITLE ? "dvb" : "dvd";
    if (const AVDictionaryEntry* e = av_dict_get(st->metadata, "language", nullptr, 0)) out.language = languageFromTag(e->value);
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) return fail("No " + out.codec + " subtitle decoder in this FFmpeg");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> ctxOwner(ctx, [](AVCodecContext* c) { avcodec_free_context(&c); });
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (int rc = avcodec_open2(ctx, codec, nullptr); rc < 0) return fail("Cannot open the subtitle decoder: " + averr(rc));
    AVPacket* pkt = av_packet_alloc();
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pktOwner(pkt, [](AVPacket* p) { av_packet_free(&p); });
    // Times from the start of the file's picture (or sound), as its clips play it; a file of subtitles alone keeps
    // their own times.
    int64_t origin = 0;  // AV_TIME_BASE units
    for (unsigned i = 0; i < fmt->nb_streams && !origin; ++i) {
        const AVStream* s = fmt->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s->start_time != AV_NOPTS_VALUE)
            origin = av_rescale_q(s->start_time, s->time_base, AV_TIME_BASE_Q);
    }
    if (!origin)
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && fmt->start_time != AV_NOPTS_VALUE) origin = fmt->start_time;
    bool open = false;  // the last picture's end is not known yet
    auto take = [&](AVSubtitle& sub, int64_t pktPts) {
        const double base = double(av_rescale_q(pktPts, st->time_base, AV_TIME_BASE_Q) - origin) / AV_TIME_BASE;
        const double at = base + sub.start_display_time / 1000.0;
        if (open && !out.pictures.empty()) out.pictures.back().end = std::max(out.pictures.back().start, at);
        else if (!out.pictures.empty()) out.pictures.back().end = std::min(out.pictures.back().end, std::max(out.pictures.back().start, at));
        open = false;
        SubtitlePicture p = joinRects(sub);
        if (p.image.isNull()) return;  // a clearing subtitle
        p.start = at;
        // An end the decoder knows (DVD's, DVB's time-out), else until the next.
        const bool stated = sub.end_display_time != UINT32_MAX && sub.end_display_time > sub.start_display_time;
        p.end = stated ? base + sub.end_display_time / 1000.0 : at + 3600;
        open = !stated || id == AV_CODEC_ID_DVB_SUBTITLE;
        out.pictures.push_back(std::move(p));
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (cancel && *cancel) return fail("Cancelled");
        if (pkt->stream_index == index) {
            AVSubtitle sub{};
            int got = 0;
            const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts != AV_NOPTS_VALUE ? pkt->dts : 0;
            if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) >= 0 && got) {
                take(sub, pts);
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);
    }
    if (open && !out.pictures.empty()) {
        // Shown to the end of the file, else for five seconds.
        const double fileEnd = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0;
        SubtitlePicture& last = out.pictures.back();
        last.end = fileEnd > last.start ? std::min(last.end, fileEnd) : std::min(last.end, last.start + 5);
    }
    out.width = ctx->width > 0 ? ctx->width : videoW;
    out.height = ctx->height > 0 ? ctx->height : videoH;
    if (out.width <= 0 || out.height <= 0) {
        // The smallest usual frame that holds them all.
        int w = 0, h = 0;
        for (const SubtitlePicture& p : out.pictures) w = std::max(w, p.x + p.image.width()), h = std::max(h, p.y + p.image.height());
        out.width = w <= 720 && h <= 576 ? 720 : w <= 1920 && h <= 1080 ? 1920 : 3840;
        out.height = out.width == 720 ? (h <= 480 ? 480 : 576) : out.width == 1920 ? 1080 : 2160;
    }
    return true;
}

// ---- Reading by OCR ---------------------------------------------------------------------------------------------

namespace {

// The colour of a picture's text and of a box behind it (0 when there is none), from its commonest colours.
struct PictureLook {
    uint32_t text = 0xffffffff;
    uint32_t box = 0;  // ARGB; alpha 0 = no box
};
PictureLook lookOf(const QImage& img) {
    std::unordered_map<uint32_t, int> count;
    int covered = 0;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const uint32_t*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t a = p[x] >> 24;
            if (a < 40) continue;
            ++covered;
            // Grouped by 5 bits a channel (antialiasing apart), opacity by eighths.
            ++count[(p[x] & 0xe0f8f8f8u)];
        }
    }
    PictureLook look;
    std::vector<std::pair<int, uint32_t>> ranked;
    for (const auto& [c, n] : count) ranked.push_back({n, c});
    std::sort(ranked.rbegin(), ranked.rend());
    if (ranked.empty()) return look;
    const double area = double(img.width()) * img.height();
    // A box: one colour over most of the picture.
    size_t first = 0;
    if (covered > 0.6 * area && ranked[0].first > 0.4 * covered) {
        look.box = ranked[0].second | 0x07070707u;
        first = 1;
    }
    // The text: of the commonest opaque colours left, the one standing out most from what is behind it.
    std::vector<uint32_t> opaque;
    for (size_t i = first; i < ranked.size() && opaque.size() < 2; ++i)
        if ((ranked[i].second >> 24) >= 0xc0) {
            if (!opaque.empty() && ranked[i].first * 3 < ranked[first].first) break;
            opaque.push_back(ranked[i].second | 0xff070707u);
        }
    if (opaque.empty()) return look;
    const double behind = look.box >> 24 ? luminance(look.box) : 0.0;
    look.text = opaque[0];
    if (opaque.size() > 1 && std::fabs(luminance(opaque[1]) - behind) > std::fabs(luminance(opaque[0]) - behind)) look.text = opaque[1];
    // Light text on nothing reads against the dark outline: keep the brighter.
    if (!(look.box >> 24) && opaque.size() > 1 && luminance(opaque[1]) > luminance(opaque[0])) look.text = opaque[1];
    return look;
}

// The picture as the recogniser reads best: dark text on white, small pictures doubled, with a margin; `scale` and
// `margin` say how it maps back.
struct Page {
    QImage image;
    double scale = 1;
    int margin = 0;
};
Page forReading(const QImage& img, const PictureLook& look) {
    // Over black the text is light (however outlined or boxed); a light box would read the other way round.
    const bool lightBox = (look.box >> 24) && luminance(look.box) > 0.5;
    QImage flat(img.size(), QImage::Format_RGB32);
    flat.fill(lightBox ? Qt::white : Qt::black);
    {
        QPainter pa(&flat);
        pa.drawImage(0, 0, img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    QImage grey = flat.convertToFormat(QImage::Format_Grayscale8);
    if (!lightBox) grey.invertPixels();
    // Text drawn as dark as the box (dark text in a dark box: rare) still differs from it; stretch what there is.
    int lo = 255, hi = 0;
    for (int y = 0; y < grey.height(); ++y) {
        const uchar* p = grey.constScanLine(y);
        for (int x = 0; x < grey.width(); ++x) lo = std::min<int>(lo, p[x]), hi = std::max<int>(hi, p[x]);
    }
    if (hi - lo > 16 && hi - lo < 200)
        for (int y = 0; y < grey.height(); ++y) {
            uchar* p = grey.scanLine(y);
            for (int x = 0; x < grey.width(); ++x) p[x] = uchar(std::clamp((p[x] - lo) * 255 / (hi - lo), 0, 255));
        }
    const double scale = img.height() < 60 ? 2.0 : 1.0;
    if (scale != 1.0) grey = grey.scaled(int(grey.width() * scale), int(grey.height() * scale), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int margin = std::max(16, grey.height() / 4);
    QImage out(grey.width() + 2 * margin, grey.height() + 2 * margin, QImage::Format_Grayscale8);
    out.fill(255);
    {
        QPainter pa(&out);
        pa.drawImage(margin, margin, grey);
    }
    return {out.convertToFormat(QImage::Format_RGB32), scale, margin};
}

}  // namespace

bool readImageSubtitles(const std::string& path, Rational fps, ImageSubtitleImport& out, int stream, const std::string& language,
                        const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    out = {};
    DecodedSubtitles d;
    if (!decodeImageSubtitles(path, d, stream, error, cancel)) return false;
    if (!readSubtitlePictures(d, fps, out, language, progress, cancel, error)) return false;
    out.track.name = QFileInfo(QString::fromStdString(path)).completeBaseName().toStdString();
    return true;
}

bool readSubtitlePictures(const DecodedSubtitles& d, Rational fps, ImageSubtitleImport& out, const std::string& language,
                          const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    out = {};
    if (d.pictures.empty()) {
        if (error) *error = "The subtitles hold no pictures";
        return false;
    }
    std::string lang = !language.empty() ? language : !d.language.empty() ? d.language : "en";
    std::string err;
    const std::shared_ptr<TextReader> reader = TextReader::load(lang == "en" ? "en" : lang, &err);
    if (!reader) {
        if (error) *error = err.empty() ? "The OCR model is missing" : err;
        return false;
    }
    const SpellChecker* spelling = SpellChecker::forLanguage(lang);
    out.codec = d.codec;
    out.pictures = int(d.pictures.size());
    CaptionTrack& t = out.track;
    t.name = "Subtitles";
    t.language = lang;
    const double rate = fps.toDouble();
    std::map<uint32_t, int> textVotes, boxVotes;
    std::vector<double> lineHeights, bottoms;
    struct Line {
        FrameTime start;  // its caption's
        std::string text;
        float confidence;
    };
    std::vector<Line> unsure;
    for (size_t i = 0; i < d.pictures.size(); ++i) {
        if (cancel && *cancel) {
            if (error) *error = "Cancelled";
            return false;
        }
        if (progress) progress(double(i) / double(d.pictures.size()));
        const SubtitlePicture& p = d.pictures[i];
        const PictureLook look = lookOf(p.image);
        const Page page = forReading(p.image, look);
        const std::vector<TextLine> lines = reader->read(page.image, {}, &err);
        Caption c;
        c.start = FrameTime(std::llround(p.start * rate));
        c.end = std::max(c.start + 1, FrameTime(std::llround(p.end * rate)));
        std::string text;
        std::vector<std::pair<std::string, float>> read;
        for (const TextLine& l : lines) {
            if (l.text.empty() || l.confidence < 0.3f) continue;
            if (!text.empty()) text += '\n';
            text += l.text;
            read.push_back({l.text, l.confidence});
            lineHeights.push_back((l.y1 - l.y0) * page.image.height() / page.scale);
        }
        if (text.empty()) {
            ++out.unread;
            continue;
        }
        c.text = text;
        // Where it is: by its middle, in thirds of the frame, and across.
        const double cy = (p.y + p.image.height() / 2.0) / std::max(1, d.height);
        c.vertical = cy < 1.0 / 3 ? kCaptionTop : cy > 2.0 / 3 ? kCaptionBottom : kCaptionMiddle;
        const double cx = (p.x + p.image.width() / 2.0) / std::max(1, d.width);
        c.align = std::fabs(cx - 0.5) < 0.12 ? kCaptionCentre : cx < 0.5 ? kCaptionLeft : kCaptionRight;
        if (c.vertical == kCaptionBottom) bottoms.push_back(double(p.y + p.image.height()) / std::max(1, d.height));
        ++textVotes[look.text & 0xfff0f0f0u];
        ++boxVotes[look.box >> 24 ? (look.box & 0xf0f0f0f0u) : 0u];
        for (const auto& [line, confidence] : read)
            if (confidence < 0.9f) unsure.push_back({c.start, line, confidence});
        t.captions.push_back(std::move(c));
    }
    if (progress) progress(1);
    if (t.captions.empty()) {
        if (error) *error = "No text was read in the subtitles' pictures";
        return false;
    }
    normalizeCaptions(t.captions);
    // The track looks as most of its pictures do.
    auto commonest = [](const std::map<uint32_t, int>& votes) {
        uint32_t best = 0;
        int n = -1;
        for (const auto& [c, v] : votes)
            if (v > n) best = c, n = v;
        return best;
    };
    const uint32_t text = commonest(textVotes) | 0x0f0f0f, box = commonest(boxVotes);
    t.style.textR = ((text >> 16) & 0xff) / 255.0, t.style.textG = ((text >> 8) & 0xff) / 255.0, t.style.textB = (text & 0xff) / 255.0;
    if (box >> 24) {
        t.style.boxR = ((box >> 16) & 0xff) / 255.0, t.style.boxG = ((box >> 8) & 0xff) / 255.0, t.style.boxB = (box & 0xff) / 255.0;
        t.style.boxOpacity = std::clamp(((box >> 24) | 0x0f) / 255.0, 0.1, 1.0);
    } else {
        t.style.boxOpacity = 0;
        t.style.outline = 0.08;
    }
    if (!lineHeights.empty()) {
        std::nth_element(lineHeights.begin(), lineHeights.begin() + long(lineHeights.size() / 2), lineHeights.end());
        t.style.size = std::clamp(lineHeights[lineHeights.size() / 2] / std::max(1, d.height) / 1.1, 0.02, 0.2);
    }
    if (!bottoms.empty()) {
        std::nth_element(bottoms.begin(), bottoms.begin() + long(bottoms.size() / 2), bottoms.end());
        t.style.position = std::clamp(bottoms[bottoms.size() / 2] - 0.01, 0.5, 0.98);
    }
    // The words read with little confidence that the dictionary does not know, for review.
    if (spelling)
        for (const Line& l : unsure) {
            const size_t caption = std::min(captionIndexAt(t, l.start), t.captions.size() - 1);
            for (const Misspelling& m : spelling->check(l.text, {}, true))
                out.review.push_back({caption, m.word, m.suggestions, l.confidence});
        }
    return true;
}

// ---- Drawing and colours ----------------------------------------------------------------------------------------

std::vector<CaptionPicture> captionPictures(const CaptionTrack& track, int width, int height) {
    std::vector<CaptionPicture> out;
    CaptionTrack one = track;
    one.style.animation = 0;
    for (const Caption& c : track.captions) {
        if (c.end <= c.start || c.text.empty()) continue;
        one.captions = {c};
        QPoint at;
        const QImage drawn = captionImage(one, c.start, width, height, &at);
        if (drawn.isNull()) continue;
        const QImage img = drawn.convertToFormat(QImage::Format_ARGB32);
        // Cut to what is drawn.
        int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < img.height(); ++y) {
            const auto* p = reinterpret_cast<const uint32_t*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x)
                if (p[x] >> 24) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
        if (x1 < x0) continue;
        CaptionPicture p;
        p.start = c.start, p.end = c.end;
        p.x = at.x() + x0, p.y = at.y() + y0;
        p.image = img.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        out.push_back(std::move(p));
    }
    return out;
}

IndexedPicture reduceColours(const QImage& source, int colours, int alphaSteps) {
    const QImage img = source.convertToFormat(QImage::Format_ARGB32);
    IndexedPicture out;
    out.width = img.width(), out.height = img.height();
    out.pixels.assign(size_t(out.width) * size_t(out.height), 0);
    out.palette = {0};
    colours = std::clamp(colours, 2, 256);
    auto snap = [&](uint32_t c) {
        uint32_t a = c >> 24;
        if (alphaSteps > 1) a = uint32_t(std::lround(std::round(a / 255.0 * (alphaSteps - 1)) * 255.0 / (alphaSteps - 1)));
        return (a << 24) | (c & 0xffffff);
    };
    std::unordered_map<uint32_t, int> count;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const uint32_t*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t c = snap(p[x]);
            if (c >> 24) ++count[c];
        }
    }
    struct Entry {
        std::array<int, 4> v;  // a, r, g, b
        int n;
    };
    std::vector<Entry> entries;
    entries.reserve(count.size());
    for (const auto& [c, n] : count)
        entries.push_back({{int(c >> 24), int((c >> 16) & 0xff), int((c >> 8) & 0xff), int(c & 0xff)}, n});
    // Median cut: the box with the widest spread (weighted by how much of the picture it holds) is halved at its
    // median along that channel, until there are enough boxes.
    struct Box {
        size_t begin, end;
    };
    std::vector<Box> boxes;
    if (!entries.empty()) boxes.push_back({0, entries.size()});
    auto spread = [&](const Box& b, int& channel) {
        double best = -1;
        int64_t weight = 0;
        for (size_t i = b.begin; i < b.end; ++i) weight += entries[i].n;
        for (int ch = 0; ch < 4; ++ch) {
            int lo = 255, hi = 0;
            for (size_t i = b.begin; i < b.end; ++i) lo = std::min(lo, entries[i].v[size_t(ch)]), hi = std::max(hi, entries[i].v[size_t(ch)]);
            const double s = double(hi - lo) * std::sqrt(double(weight));
            if (s > best) best = s, channel = ch;
        }
        return b.end - b.begin > 1 ? best : -1.0;
    };
    while (int(boxes.size()) < colours - 1) {
        int which = -1, channel = 0;
        double best = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            int ch = 0;
            const double s = spread(boxes[i], ch);
            if (s > best) best = s, which = int(i), channel = ch;
        }
        if (which < 0) break;
        Box& b = boxes[size_t(which)];
        std::sort(entries.begin() + long(b.begin), entries.begin() + long(b.end),
                  [&](const Entry& x, const Entry& y) { return x.v[size_t(channel)] < y.v[size_t(channel)]; });
        int64_t total = 0, run = 0;
        for (size_t i = b.begin; i < b.end; ++i) total += entries[i].n;
        size_t cut = b.begin + 1;
        for (size_t i = b.begin; i < b.end - 1; ++i) {
            run += entries[i].n;
            cut = i + 1;
            if (run * 2 >= total) break;
        }
        const Box second{cut, b.end};
        b.end = cut;
        boxes.push_back(second);
    }
    // Each box stands for its commonest colour, not its mean: a caption's text and box keep their exact colours
    // however many edge blends fall in with them (fonts are smoothed differently on each system).
    for (const Box& b : boxes) {
        size_t most = b.begin;
        for (size_t i = b.begin; i < b.end; ++i)
            if (entries[i].n > entries[most].n) most = i;
        uint32_t c = 0;
        for (int ch = 0; ch < 4; ++ch) c = (c << 8) | uint32_t(entries[most].v[size_t(ch)]);
        c = snap(c);
        if (!(c >> 24)) c |= 0x01000000;  // (still not the transparent entry)
        out.palette.push_back(c);
    }
    std::unordered_map<uint32_t, uint8_t> nearest;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const uint32_t*>(img.constScanLine(y));
        uint8_t* q = out.pixels.data() + size_t(y) * size_t(out.width);
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t c = snap(p[x]);
            if (!(c >> 24)) continue;
            auto it = nearest.find(c);
            if (it == nearest.end()) {
                int best = 1;
                int64_t bestD = INT64_MAX;
                for (size_t i = 1; i < out.palette.size(); ++i) {
                    int64_t d = 0;
                    for (int sh = 0; sh < 32; sh += 8) {
                        const int64_t e = int64_t((c >> sh) & 0xff) - int64_t((out.palette[i] >> sh) & 0xff);
                        d += e * e;
                    }
                    if (d < bestD) bestD = d, best = int(i);
                }
                it = nearest.emplace(c, uint8_t(best)).first;
            }
            q[x] = it->second;
        }
    }
    return out;
}

// ---- PGS --------------------------------------------------------------------------------------------------------

namespace {

void put16(std::vector<uint8_t>& b, unsigned v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

void segment(std::vector<uint8_t>& out, uint8_t type, const std::vector<uint8_t>& body) {
    out.push_back(type);
    put16(out, unsigned(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

// A line of PGS run-length code: colour runs, a lone non-zero pixel as itself, and 0 0 at the end.
void pgsLine(std::vector<uint8_t>& out, const uint8_t* p, int w) {
    for (int x = 0; x < w;) {
        const uint8_t c = p[x];
        int run = 1;
        while (x + run < w && p[x + run] == c && run < 16383) ++run;
        if (c && run <= 2) {
            for (int i = 0; i < run; ++i) out.push_back(c);
        } else {
            out.push_back(0);
            if (!c) {
                if (run < 64) out.push_back(uint8_t(run));
                else out.push_back(uint8_t(0x40 | (run >> 8))), out.push_back(uint8_t(run));
            } else {
                if (run < 64) out.push_back(uint8_t(0x80 | run));
                else out.push_back(uint8_t(0xc0 | (run >> 8))), out.push_back(uint8_t(run));
                out.push_back(c);
            }
        }
        x += run;
    }
    out.push_back(0);
    out.push_back(0);
}

// Y, Cr, Cb as video levels (BT.709 for HD, BT.601 at 576 lines and under, as players read them).
std::array<uint8_t, 3> yCrCb(uint32_t argb, bool hd) {
    const double r = ((argb >> 16) & 0xff) / 255.0, g = ((argb >> 8) & 0xff) / 255.0, b = (argb & 0xff) / 255.0;
    const double kr = hd ? 0.2126 : 0.299, kb = hd ? 0.0722 : 0.114;
    const double y = kr * r + (1 - kr - kb) * g + kb * b;
    const double cb = (b - y) / (2 * (1 - kb)), cr = (r - y) / (2 * (1 - kr));
    auto q = [](double v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); };
    return {q(16 + 219 * y), q(128 + 224 * cr), q(128 + 224 * cb)};
}

// One caption's display set (presentation, window, palette, object, end), and the set clearing it.
std::vector<uint8_t> pgsDisplaySet(const CaptionPicture& p, const IndexedPicture& ix, int width, int height, unsigned number) {
    std::vector<uint8_t> out, b;
    // Presentation: an epoch of its own, one object in one window.
    put16(b, unsigned(width)), put16(b, unsigned(height));
    b.push_back(0x10);  // frame rate (ignored by players)
    put16(b, number);
    b.push_back(0x80);  // epoch start
    b.push_back(0x00);  // no palette-only update
    b.push_back(0);     // palette 0
    b.push_back(1);     // objects
    put16(b, 0), b.push_back(0), b.push_back(0x00);
    put16(b, unsigned(p.x)), put16(b, unsigned(p.y));
    segment(out, 0x16, b);
    b.clear();
    b.push_back(1);
    b.push_back(0);
    put16(b, unsigned(p.x)), put16(b, unsigned(p.y)), put16(b, unsigned(ix.width)), put16(b, unsigned(ix.height));
    segment(out, 0x17, b);
    b.clear();
    b.push_back(0), b.push_back(0);
    for (size_t i = 0; i < ix.palette.size(); ++i) {
        const std::array<uint8_t, 3> v = yCrCb(ix.palette[i], height > 576);
        b.push_back(uint8_t(i));
        b.insert(b.end(), v.begin(), v.end());
        b.push_back(uint8_t(ix.palette[i] >> 24));
    }
    segment(out, 0x14, b);
    // The object, in fragments when longer than a segment holds.
    std::vector<uint8_t> rle;
    for (int y = 0; y < ix.height; ++y) pgsLine(rle, ix.pixels.data() + size_t(y) * size_t(ix.width), ix.width);
    const size_t total = rle.size() + 4;
    size_t at = 0;
    bool first = true;
    while (first || at < rle.size()) {
        b.clear();
        put16(b, 0);
        b.push_back(0);
        const size_t room = 65535 - 4 - (first ? 7 : 0);
        const size_t n = std::min(room, rle.size() - at);
        const bool last = at + n >= rle.size();
        b.push_back(uint8_t((first ? 0x80 : 0) | (last ? 0x40 : 0)));
        if (first) {
            b.push_back(uint8_t(total >> 16)), b.push_back(uint8_t(total >> 8)), b.push_back(uint8_t(total));
            put16(b, unsigned(ix.width)), put16(b, unsigned(ix.height));
        }
        b.insert(b.end(), rle.begin() + long(at), rle.begin() + long(at + n));
        segment(out, 0x15, b);
        at += n;
        first = false;
    }
    segment(out, 0x80, {});
    return out;
}

std::vector<uint8_t> pgsClear(const CaptionPicture& p, const IndexedPicture& ix, int width, int height, unsigned number) {
    std::vector<uint8_t> out, b;
    put16(b, unsigned(width)), put16(b, unsigned(height));
    b.push_back(0x10);
    put16(b, number);
    b.push_back(0x00);  // normal: the epoch goes on, with nothing shown
    b.push_back(0x00);
    b.push_back(0);
    b.push_back(0);
    segment(out, 0x16, b);
    b.clear();
    b.push_back(1);
    b.push_back(0);
    put16(b, unsigned(p.x)), put16(b, unsigned(p.y)), put16(b, unsigned(ix.width)), put16(b, unsigned(ix.height));
    segment(out, 0x17, b);
    segment(out, 0x80, {});
    return out;
}

struct Coder {
    AVCodecContext* ctx = nullptr;
    ~Coder() { avcodec_free_context(&ctx); }
};

// A DVB or DVD encoder for the frame.
bool openCoder(Coder& c, AVCodecID id, int width, int height, const std::string& palette, std::string* error) {
    const AVCodec* codec = avcodec_find_encoder(id);
    if (!codec) {
        if (error) *error = std::string("No ") + (id == AV_CODEC_ID_DVB_SUBTITLE ? "DVB" : "DVD") + " subtitle encoder in this FFmpeg";
        return false;
    }
    c.ctx = avcodec_alloc_context3(codec);
    c.ctx->width = width;
    c.ctx->height = height;
    c.ctx->time_base = AVRational{1, 90000};
    AVDictionary* opts = nullptr;
    if (!palette.empty()) av_dict_set(&opts, "palette", palette.c_str(), 0);
    const int rc = avcodec_open2(c.ctx, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        if (error) *error = "Cannot open the subtitle encoder: " + averr(rc);
        return false;
    }
    return true;
}

bool encodeOne(Coder& c, const IndexedPicture* ix, const CaptionPicture* p, int64_t durationMs, std::vector<uint8_t>& out, std::string* error) {
    AVSubtitle sub{};
    sub.format = 0;
    sub.start_display_time = 0;
    sub.end_display_time = uint32_t(std::max<int64_t>(durationMs, 0));
    sub.pts = 0;
    if (ix && p) {
        sub.num_rects = 1;
        sub.rects = static_cast<AVSubtitleRect**>(av_mallocz(sizeof(AVSubtitleRect*)));
        AVSubtitleRect* r = sub.rects[0] = static_cast<AVSubtitleRect*>(av_mallocz(sizeof(AVSubtitleRect)));
        r->type = SUBTITLE_BITMAP;
        r->x = p->x, r->y = p->y, r->w = ix->width, r->h = ix->height;
        r->nb_colors = int(ix->palette.size());
        r->data[0] = static_cast<uint8_t*>(av_malloc(ix->pixels.size()));
        std::copy(ix->pixels.begin(), ix->pixels.end(), r->data[0]);
        r->linesize[0] = ix->width;
        r->data[1] = static_cast<uint8_t*>(av_mallocz(256 * 4));
        std::copy(ix->palette.begin(), ix->palette.end(), reinterpret_cast<uint32_t*>(r->data[1]));
        r->linesize[1] = 4 * 256;
    }
    std::vector<uint8_t> buf(std::max<size_t>(1 << 20, ix ? ix->pixels.size() * 2 + 4096 : 4096));
    const int n = avcodec_encode_subtitle(c.ctx, buf.data(), int(buf.size()), &sub);
    avsubtitle_free(&sub);
    if (n < 0) {
        if (error) *error = "Cannot encode a subtitle: " + averr(n);
        return false;
    }
    out.assign(buf.begin(), buf.begin() + n);
    return true;
}

}  // namespace

bool encodeImageSubtitles(const CaptionTrack& source, Rational fps, ImageSubtitleFormat format, int width, int height,
                          std::vector<SubtitlePacket>& out, std::vector<uint8_t>* extradata, std::string* error, FrameTime from, FrameTime to) {
    out.clear();
    if (format == ImageSubtitleFormat::ImscImage) {
        if (error) *error = "IMSC 1.1 Image is a document and pictures, not a stream";
        return false;
    }
    if (width < 16 || height < 16 || fps.num <= 0 || fps.den <= 0) {
        if (error) *error = "No frame size for the subtitles";
        return false;
    }
    CaptionTrack track = source;
    track.captions.clear();
    for (Caption c : source.captions) {
        if (to >= 0) c.end = std::min(c.end, to);
        c.start = std::max(c.start, from);
        if (c.end <= c.start) continue;
        c.start -= from, c.end -= from;
        track.captions.push_back(std::move(c));
    }
    const std::vector<CaptionPicture> pictures = captionPictures(track, width, height);
    const int colours = format == ImageSubtitleFormat::Pgs ? 256 : format == ImageSubtitleFormat::Dvb ? 16 : 4;
    std::vector<IndexedPicture> reduced;
    for (const CaptionPicture& p : pictures)
        reduced.push_back(reduceColours(p.image, colours, format == ImageSubtitleFormat::Vobsub ? 3 : 0));
    if (format == ImageSubtitleFormat::Pgs) {
        unsigned number = 0;
        for (size_t i = 0; i < pictures.size(); ++i) {
            const CaptionPicture& p = pictures[i];
            const int64_t a = ticksOf(p.start, fps), b = ticksOf(p.end, fps);
            out.push_back({a, b - a, pgsDisplaySet(p, reduced[i], width, height, number++)});
            // Cleared at its end, unless the next one replaces it then.
            if (i + 1 < pictures.size() && pictures[i + 1].start <= p.end) continue;
            out.push_back({b, 0, pgsClear(p, reduced[i], width, height, number++)});
        }
        if (extradata) extradata->clear();
        return true;
    }
    Coder coder;
    std::string palette;
    if (format == ImageSubtitleFormat::Vobsub) {
        // The disc's 16 colours: those the pictures use (reduced again when they come to more).
        std::map<uint32_t, int> used;
        for (const IndexedPicture& ix : reduced)
            for (size_t i = 1; i < ix.palette.size(); ++i) used[ix.palette[i] & 0xffffff]++;
        std::vector<uint32_t> global;
        if (used.size() <= 16) {
            for (const auto& [c, n] : used) global.push_back(c);
        } else {
            QImage swatch(int(used.size()), 1, QImage::Format_ARGB32);
            int x = 0;
            for (const auto& [c, n] : used) swatch.setPixel(x++, 0, 0xff000000u | c);
            const IndexedPicture g = reduceColours(swatch, 17);
            for (size_t i = 1; i < g.palette.size(); ++i) global.push_back(g.palette[i] & 0xffffff);
            for (IndexedPicture& ix : reduced)
                for (size_t i = 1; i < ix.palette.size(); ++i) {
                    uint32_t best = global.front();
                    int64_t bestD = INT64_MAX;
                    for (uint32_t c : global) {
                        int64_t d = 0;
                        for (int sh = 0; sh < 24; sh += 8) {
                            const int64_t e = int64_t((c >> sh) & 0xff) - int64_t((ix.palette[i] >> sh) & 0xff);
                            d += e * e;
                        }
                        if (d < bestD) bestD = d, best = c;
                    }
                    ix.palette[i] = (ix.palette[i] & 0xff000000u) | best;
                }
        }
        while (global.size() < 16) global.push_back(global.empty() ? 0 : global.back());
        char hex[8];
        for (size_t i = 0; i < global.size(); ++i) {
            std::snprintf(hex, sizeof hex, "%06x", unsigned(global[i]));
            palette += (i ? ", " : "") + std::string(hex);
        }
    }
    if (!openCoder(coder, format == ImageSubtitleFormat::Dvb ? AV_CODEC_ID_DVB_SUBTITLE : AV_CODEC_ID_DVD_SUBTITLE, width, height, palette, error))
        return false;
    for (size_t i = 0; i < pictures.size(); ++i) {
        const CaptionPicture& p = pictures[i];
        const int64_t a = ticksOf(p.start, fps), b = ticksOf(p.end, fps);
        SubtitlePacket pk;
        pk.pts = a, pk.duration = b - a;
        if (!encodeOne(coder, &reduced[i], &p, (b - a) / 90, pk.data, error)) return false;
        out.push_back(std::move(pk));
        // DVB pages stay up until another comes: an empty one clears it at its end.
        if (format == ImageSubtitleFormat::Dvb && !(i + 1 < pictures.size() && pictures[i + 1].start <= p.end)) {
            SubtitlePacket clear;
            clear.pts = b;
            if (!encodeOne(coder, nullptr, nullptr, 0, clear.data, error)) return false;
            out.push_back(std::move(clear));
        }
    }
    if (extradata) {
        if (format == ImageSubtitleFormat::Dvb) *extradata = {0x00, 0x01, 0x00, 0x01, 0x10};  // composition and ancillary page 1, normal subtitles
        else extradata->assign(coder.ctx->extradata, coder.ctx->extradata + coder.ctx->extradata_size);
    }
    return true;
}

// ---- IMSC 1.1 Image ---------------------------------------------------------------------------------------------

namespace {

std::string clockOf(FrameTime frame, Rational fps) {
    const int64_t ms = std::llround(double(frame) * 1000.0 * double(fps.den) / double(fps.num));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld.%03lld", static_cast<long long>(ms / 3600000),
                  static_cast<long long>(ms / 60000 % 60), static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
    return buf;
}

std::string xmlEscaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

}  // namespace

std::string imscImageDocument(const std::vector<CaptionPicture>& pictures, const std::vector<std::string>& names, Rational fps,
                              int width, int height, const std::string& language) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
           "xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\" "
           "xmlns:smpte=\"http://www.smpte-ra.org/schemas/2052-1/2010/smpte-tt\" "
           "ttp:timeBase=\"media\" ttp:contentProfiles=\"http://www.w3.org/ns/ttml/profile/imsc1.1/image\" "
           "tts:extent=\"" + std::to_string(width) + "px " + std::to_string(height) + "px\" xml:lang=\"" +
           xmlEscaped(language.empty() ? "en" : language) + "\">\n";
    out += "  <head>\n    <layout>\n";
    for (size_t i = 0; i < pictures.size(); ++i) {
        const CaptionPicture& p = pictures[i];
        out += "      <region xml:id=\"r" + std::to_string(i + 1) + "\" tts:origin=\"" + std::to_string(p.x) + "px " + std::to_string(p.y) +
               "px\" tts:extent=\"" + std::to_string(p.image.width()) + "px " + std::to_string(p.image.height()) +
               "px\" tts:showBackground=\"whenActive\"/>\n";
    }
    out += "    </layout>\n  </head>\n  <body>\n";
    for (size_t i = 0; i < pictures.size(); ++i)
        out += "    <div region=\"r" + std::to_string(i + 1) + "\" begin=\"" + clockOf(pictures[i].start, fps) + "\" end=\"" +
               clockOf(pictures[i].end, fps) + "\" smpte:backgroundImage=\"" + xmlEscaped(i < names.size() ? names[i] : std::string()) + "\"/>\n";
    out += "  </body>\n</tt>\n";
    return out;
}

std::vector<std::string> checkImscImage(const std::string& xml, const std::function<QByteArray(const std::string&)>& image, int width, int height) {
    std::vector<std::string> problems;
    QXmlStreamReader r(QByteArray::fromStdString(xml));
    bool profile = false, root = false;
    int rootW = 0, rootH = 0;
    struct Region {
        int x = 0, y = 0, w = 0, h = 0;
    };
    std::map<QString, Region> regions;
    auto pair = [](const QString& v, int& a, int& b) {
        const QStringList parts = v.simplified().split(' ');
        if (parts.size() != 2 || !parts[0].endsWith("px") || !parts[1].endsWith("px")) return false;
        bool ok1 = false, ok2 = false;
        a = parts[0].chopped(2).toInt(&ok1);
        b = parts[1].chopped(2).toInt(&ok2);
        return ok1 && ok2;
    };
    auto attr = [](const QXmlStreamAttributes& as, const char* name) {
        for (const QXmlStreamAttribute& a : as)
            if (a.name() == QLatin1String(name)) return a.value().toString();
        return QString();
    };
    int divs = 0;
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        const QXmlStreamAttributes as = r.attributes();
        const QString name = r.name().toString();
        if (name == "tt") {
            root = true;
            profile = attr(as, "contentProfiles").contains("http://www.w3.org/ns/ttml/profile/imsc1.1/image") ||
                      attr(as, "profile").contains("profile/imsc1.1/image");
            if (!pair(attr(as, "extent"), rootW, rootH)) problems.push_back("The document's extent is not given in pixels");
            else if (width > 0 && (rootW != width || rootH != height))
                problems.push_back("The document is " + std::to_string(rootW) + " x " + std::to_string(rootH) + ", the picture " +
                                   std::to_string(width) + " x " + std::to_string(height));
        } else if (name == "p" || name == "span") {
            problems.push_back("Text in an Image profile document");
        } else if (name == "region") {
            Region g;
            const QString id = attr(as, "id");
            if (!pair(attr(as, "origin"), g.x, g.y) || !pair(attr(as, "extent"), g.w, g.h)) {
                problems.push_back("Region " + id.toStdString() + " is not placed in pixels");
                continue;
            }
            if (g.x < 0 || g.y < 0 || g.w <= 0 || g.h <= 0 || g.x + g.w > rootW || g.y + g.h > rootH)
                problems.push_back("Region " + id.toStdString() + " is not on the frame");
            regions[id] = g;
        } else if (name == "div") {
            ++divs;
            const QString img = attr(as, "backgroundImage"), region = attr(as, "region");
            const std::string where = "Subtitle " + std::to_string(divs);
            if (attr(as, "begin").isEmpty() || attr(as, "end").isEmpty()) problems.push_back(where + " is not timed");
            if (img.isEmpty()) {
                problems.push_back(where + " has no image");
                continue;
            }
            const auto g = regions.find(region);
            if (g == regions.end()) {
                problems.push_back(where + " is in no region");
                continue;
            }
            QImage png;
            const QByteArray data = image ? image(img.toStdString()) : QByteArray();
            if (data.isEmpty() || !data.startsWith("\x89PNG") || !png.loadFromData(data, "PNG")) {
                problems.push_back(where + "'s image " + img.toStdString() + " is missing or not a PNG");
                continue;
            }
            if (png.width() > g->second.w || png.height() > g->second.h) problems.push_back(where + "'s image is larger than its region");
        }
    }
    if (r.hasError()) problems.push_back("Not well-formed XML: " + r.errorString().toStdString());
    if (!root) problems.push_back("Not a TTML document");
    else if (!profile) problems.push_back("The document does not name the IMSC 1.1 Image profile");
    return problems;
}

// ---- Files ------------------------------------------------------------------------------------------------------

namespace {

// The packets as one subtitle stream in a container (Matroska or MPEG-TS).
bool muxSubtitles(const std::string& path, const char* container, AVCodecID id, const std::vector<SubtitlePacket>& packets,
                  const std::vector<uint8_t>& extradata, const CaptionTrack& track, int width, int height, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    AVFormatContext* oc = nullptr;
    if (int rc = avformat_alloc_output_context2(&oc, nullptr, container, path.c_str()); rc < 0 || !oc)
        return fail("Cannot make " + path + ": " + averr(rc));
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owner(oc, [](AVFormatContext* c) {
        if (c->pb && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
        avformat_free_context(c);
    });
    AVStream* st = avformat_new_stream(oc, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id = id;
    st->codecpar->width = width;
    st->codecpar->height = height;
    if (!extradata.empty()) {
        st->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        std::copy(extradata.begin(), extradata.end(), st->codecpar->extradata);
        st->codecpar->extradata_size = int(extradata.size());
    }
    st->time_base = AVRational{1, 90000};
    std::string lang;
    if (!track.language.empty()) {
        static const std::pair<const char*, const char*> table[] = {
            {"en", "eng"}, {"es", "spa"}, {"fr", "fra"}, {"de", "deu"}, {"it", "ita"}, {"pt", "por"}, {"nl", "nld"},
            {"pl", "pol"}, {"sv", "swe"}, {"da", "dan"}, {"no", "nor"}, {"fi", "fin"}, {"cs", "ces"}, {"el", "ell"},
            {"tr", "tur"}, {"ru", "rus"}, {"uk", "ukr"}, {"ar", "ara"}, {"he", "heb"}, {"hi", "hin"}, {"id", "ind"},
            {"vi", "vie"}, {"th", "tha"}, {"ja", "jpn"}, {"ko", "kor"}, {"zh", "zho"}, {"cy", "cym"}, {"ga", "gle"}};
        for (const auto& [two, three] : table)
            if (track.language == two) lang = three;
        if (lang.empty() && track.language.size() == 3) lang = track.language;
    }
    if (!lang.empty()) av_dict_set(&st->metadata, "language", lang.c_str(), 0);
    if (!track.name.empty()) av_dict_set(&st->metadata, "title", track.name.c_str(), 0);
    av_dict_set(&oc->metadata, "encoder", "Montage", 0);
    if (!(oc->oformat->flags & AVFMT_NOFILE))
        if (int rc = avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE); rc < 0) return fail("Cannot write " + path + ": " + averr(rc));
    // The subtitles' times as they are (MPEG-TS would otherwise start them later).
    AVDictionary* opts = nullptr;
    if (std::string(container) == "mpegts") av_dict_set(&opts, "mpegts_copyts", "1", 0);
    const int rc = avformat_write_header(oc, &opts);
    av_dict_free(&opts);
    if (rc < 0) return fail("Cannot write " + path + ": " + averr(rc));
    AVPacket* pkt = av_packet_alloc();
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pktOwner(pkt, [](AVPacket* p) { av_packet_free(&p); });
    for (const SubtitlePacket& sp : packets) {
        if (av_new_packet(pkt, int(sp.data.size())) < 0) return fail("Out of memory");
        std::copy(sp.data.begin(), sp.data.end(), pkt->data);
        pkt->pts = pkt->dts = av_rescale_q(sp.pts, AVRational{1, 90000}, st->time_base);
        pkt->duration = av_rescale_q(sp.duration, AVRational{1, 90000}, st->time_base);
        pkt->stream_index = st->index;
        pkt->flags |= AV_PKT_FLAG_KEY;
        if (int rc = av_interleaved_write_frame(oc, pkt); rc < 0) return fail("Cannot write " + path + ": " + averr(rc));
    }
    if (int rc = av_write_trailer(oc); rc < 0) return fail("Cannot finish " + path + ": " + averr(rc));
    return true;
}

bool saveFile(const QString& path, const QByteArray& data, std::string* error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error) *error = "Cannot write " + path.toStdString();
        return false;
    }
    return true;
}

// The display sets as a .sup file: each segment headed "PG" with its time.
QByteArray supFile(const std::vector<SubtitlePacket>& packets) {
    QByteArray out;
    for (const SubtitlePacket& p : packets)
        for (size_t at = 0; at + 3 <= p.data.size();) {
            const size_t len = size_t(p.data[at + 1]) << 8 | p.data[at + 2];
            const uint32_t pts = uint32_t(p.pts);
            const char head[10] = {'P', 'G', char(pts >> 24), char(pts >> 16), char(pts >> 8), char(pts), 0, 0, 0, 0};
            out.append(head, 10);
            out.append(reinterpret_cast<const char*>(p.data.data() + at), qsizetype(3 + len));
            at += 3 + len;
        }
    return out;
}

// MPEG-2 program stream packs of 2048 bytes: a subpicture in private stream 1 (substream 0x20), its time in the first.
void vobsubPacks(QByteArray& out, const SubtitlePacket& p) {
    const int64_t scr = p.pts;
    size_t at = 0;
    bool first = true;
    while (at < p.data.size()) {
        QByteArray pack(14, 0);
        auto* h = reinterpret_cast<uint8_t*>(pack.data());
        h[0] = 0, h[1] = 0, h[2] = 1, h[3] = 0xba;
        h[4] = uint8_t(0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03));
        h[5] = uint8_t(scr >> 20);
        h[6] = uint8_t(((scr >> 12) & 0xf8) | 0x04 | ((scr >> 13) & 0x03));
        h[7] = uint8_t(scr >> 5);
        h[8] = uint8_t(((scr << 3) & 0xf8) | 0x04);
        h[9] = 0x01;
        const unsigned muxRate = 25200;  // 10.08 Mbit/s, as DVDs
        h[10] = uint8_t(muxRate >> 14), h[11] = uint8_t(muxRate >> 6), h[12] = uint8_t((muxRate << 2) | 0x03);
        h[13] = 0xf8;
        const size_t header = 6 + 3 + (first ? 5 : 0) + 1;
        size_t room = 2048 - 14 - header;
        const size_t n = std::min(room, p.data.size() - at);
        size_t stuffing = 0, padding = 0;
        const size_t left = room - n;
        if (left >= 6) padding = left;
        else stuffing = left;
        QByteArray pes;
        const size_t pesLength = 3 + (first ? 5 : 0) + stuffing + 1 + n;
        pes.append(char(0)).append(char(0)).append(char(1)).append(char(0xbd));
        pes.append(char(pesLength >> 8)).append(char(pesLength & 0xff));
        pes.append(char(0x81)).append(char(first ? 0x80 : 0x00)).append(char((first ? 5 : 0) + stuffing));
        if (first) {
            const int64_t t = p.pts;
            pes.append(char(0x21 | ((t >> 29) & 0x0e))).append(char(t >> 22)).append(char(((t >> 14) & 0xfe) | 1));
            pes.append(char(t >> 7)).append(char(((t << 1) & 0xfe) | 1));
        }
        pes.append(QByteArray(qsizetype(stuffing), char(0xff)));
        pes.append(char(0x20));
        pes.append(reinterpret_cast<const char*>(p.data.data() + at), qsizetype(n));
        out.append(pack).append(pes);
        if (padding) {
            const size_t len = padding - 6;
            out.append(char(0)).append(char(0)).append(char(1)).append(char(0xbe)).append(char(len >> 8)).append(char(len & 0xff));
            out.append(QByteArray(qsizetype(len), char(0xff)));
        }
        at += n;
        first = false;
    }
}

}  // namespace

bool writeImageSubtitles(const CaptionTrack& track, const Sequence& seq, const std::string& path, ImageSubtitleFormat format,
                         std::string* error, int width, int height, ImageSubtitleResult* result) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (width <= 0 || height <= 0) width = seq.width, height = seq.height;
    if (track.captions.empty()) return fail("The caption track is empty");
    ImageSubtitleResult r;
    const QString qpath = QString::fromStdString(path);
    const std::string ext = extensionOf(path);
    const bool matroska = ext == ".mks" || ext == ".mkv" || ext == ".mka";
    if (format == ImageSubtitleFormat::ImscImage) {
        if (ext != ".ttml" && ext != ".xml") return fail("IMSC 1.1 Image goes in a .ttml (or .xml) document");
        const std::vector<CaptionPicture> pictures = captionPictures(track, width, height);
        const QFileInfo info(qpath);
        std::vector<std::string> names;
        for (size_t i = 0; i < pictures.size(); ++i) {
            char num[32];
            std::snprintf(num, sizeof num, "-%04zu.png", i + 1);
            const QString name = info.completeBaseName() + QString::fromLatin1(num);
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            if (!pictures[i].image.save(&buf, "PNG")) return fail("Cannot make a subtitle picture");
            if (!saveFile(info.absoluteDir().filePath(name), png, error)) return false;
            names.push_back(name.toStdString());
            r.files.push_back(info.absoluteDir().filePath(name).toStdString());
            r.colours = std::max(r.colours, int(reduceColours(pictures[i].image, 256).palette.size()));
        }
        if (!saveFile(qpath, QByteArray::fromStdString(imscImageDocument(pictures, names, seq.fps, width, height, track.language)), error))
            return false;
        r.files.insert(r.files.begin(), path);
        r.subtitles = int(pictures.size());
        if (result) *result = r;
        return true;
    }
    std::vector<SubtitlePacket> packets;
    std::vector<uint8_t> extradata;
    if (!encodeImageSubtitles(track, seq.fps, format, width, height, packets, &extradata, error)) return false;
    for (const SubtitlePacket& p : packets) r.subtitles += p.duration > 0;
    r.colours = format == ImageSubtitleFormat::Pgs ? 256 : format == ImageSubtitleFormat::Dvb ? 16 : 4;
    if (format == ImageSubtitleFormat::Pgs && !matroska) {
        if (ext != ".sup") return fail("PGS goes in a .sup file or Matroska (.mks, .mkv)");
        if (!saveFile(qpath, supFile(packets), error)) return false;
        r.files = {path};
    } else if (format == ImageSubtitleFormat::Vobsub && !matroska) {
        if (ext != ".idx" && ext != ".sub") return fail("DVD subtitles go in .idx and .sub files or Matroska (.mks, .mkv)");
        const QFileInfo info(qpath);
        const QString idx = info.absoluteDir().filePath(info.completeBaseName() + ".idx");
        const QString sub = info.absoluteDir().filePath(info.completeBaseName() + ".sub");
        QByteArray body, index;
        index += "# VobSub index file, v7 (do not modify this line!)\n# Made by Montage\n";
        index += QByteArray(reinterpret_cast<const char*>(extradata.data()), qsizetype(extradata.size()));
        if (!index.endsWith('\n')) index += '\n';
        index += "org: 0, 0\nscale: 100%, 100%\nalpha: 100%\nsmooth: OFF\nfadein/out: 0, 0\nalign: OFF at LEFT TOP\n"
                 "time offset: 0\nforced subs: OFF\nlangidx: 0\n";
        index += "id: " + QByteArray::fromStdString(track.language.size() == 2 ? track.language : std::string("en")) + ", index: 0\n";
        for (const SubtitlePacket& p : packets) {
            const int64_t ms = (p.pts + 45) / 90;
            char line[96];
            std::snprintf(line, sizeof line, "timestamp: %02lld:%02lld:%02lld:%03lld, filepos: %09llx\n", static_cast<long long>(ms / 3600000),
                          static_cast<long long>(ms / 60000 % 60), static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000),
                          static_cast<unsigned long long>(body.size()));
            index += line;
            vobsubPacks(body, p);
        }
        if (!saveFile(sub, body, error) || !saveFile(idx, index, error)) return false;
        r.files = {idx.toStdString(), sub.toStdString()};
    } else if (format == ImageSubtitleFormat::Dvb && !matroska) {
        if (ext != ".ts" && ext != ".m2ts" && ext != ".mts") return fail("DVB subtitles go in MPEG-TS (.ts) or Matroska (.mks, .mkv)");
        if (!muxSubtitles(path, "mpegts", AV_CODEC_ID_DVB_SUBTITLE, packets, extradata, track, width, height, error)) return false;
        r.files = {path};
    } else {
        const AVCodecID id = format == ImageSubtitleFormat::Pgs ? AV_CODEC_ID_HDMV_PGS_SUBTITLE
                             : format == ImageSubtitleFormat::Dvb ? AV_CODEC_ID_DVB_SUBTITLE
                                                                  : AV_CODEC_ID_DVD_SUBTITLE;
        if (!muxSubtitles(path, "matroska", id, packets, extradata, track, width, height, error)) return false;
        r.files = {path};
    }
    if (result) *result = r;
    return true;
}

}  // namespace montage
