#include "Imf.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "ColorSpace.h"
#include "Compositor.h"
#include "DcpMxf.h"
#include "Jpeg2000.h"
#include "PackageFiles.h"
#include "core/Surround.h"
#include "media/SuperScale.h"

namespace montage {

namespace {

QString urn(const dcp::Uuid& u) { return QStringLiteral("urn:uuid:") + QString::fromStdString(dcp::uuidString(u)); }

// "060e2b34040101..." as "urn:smpte:ul:060e2b34.04010101....".
QString ulUrn(const std::string& hex) {
    QString s = QStringLiteral("urn:smpte:ul:");
    for (int i = 0; i < 32; i += 8) s += QString::fromStdString(hex.substr(size_t(i), 8)) + (i < 24 ? "." : "");
    return s;
}
QString ulUrn(const uint8_t* ul) {
    std::string hex;
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) hex += digits[ul[i] >> 4], hex += digits[ul[i] & 15];
    return ulUrn(hex);
}
QString hex(const std::vector<uint8_t>& b) {
    QString s;
    for (uint8_t v : b) s += QStringLiteral("%1").arg(uint(v), 2, 16, QLatin1Char('0'));
    return s;
}

// The colour each master is delivered in: the space frames are converted to, and how the descriptor states it.
struct ImfColourSpec {
    const char* id;
    const char* space;  // ColorSpace.h id
    const char* primaries;
    const char* transfer;
    const char* codingEquations;  // "" for none
    bool hdr;                     // PQ, with mastering display metadata
};
const ImfColourSpec kColours[] = {
    {"rec709", "rec709", "060e2b34040101060401010103030000", "060e2b34040101010401010101020000", "060e2b34040101010401010102020000", false},
    {"p3d65-pq", "p3d65pq", "060e2b340401010d0401010103060000", "060e2b340401010d04010101010a0000", "", true},
    {"rec2020-pq", "rec2100pq", "060e2b340401010d0401010103040000", "060e2b340401010d04010101010a0000", "060e2b340401010d0401010102060000", true},
    {"rec2020-hlg", "rec2100hlg", "060e2b340401010d0401010103040000", "060e2b340401010d04010101010b0000", "060e2b340401010d0401010102060000",
     false},
};
const ImfColourSpec* colourSpec(const std::string& id) {
    for (const auto& c : kColours)
        if (id == c.id) return &c;
    return nullptr;
}

// The content kinds ST 2067-3 lists.
bool validKind(const std::string& k) {
    static const char* kinds[] = {"feature", "short", "trailer", "teaser", "advertisement", "episode", "promotion", "psa", "test",
                                  "transitional", "rating", "policy", "clip"};
    return std::any_of(std::begin(kinds), std::end(kinds), [&](const char* x) { return k == x; });
}

// Frames encoded several at a time, taken back in order (as the DCP's pool: the earliest not taken back is always
// being encoded or next in line, so waiting for it ends).
class ImfEncoderPool {
public:
    ImfEncoderPool(int w, int h, int bits, uint16_t rsiz, size_t maxBytes, int threads) {
        for (int i = 0; i < threads; ++i)
            workers_.emplace_back([this, w, h, bits, rsiz, maxBytes] {
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
                    std::string err;
                    const bool ok = encodeJpeg2000(job.second.data(), w, h, bits, rsiz, maxBytes, cs, &err);
                    std::lock_guard<std::mutex> lock(m_);
                    --busy_;
                    if (!ok) {
                        if (error_.empty()) error_ = err;
                        stop_ = true;
                        work_.notify_all();
                    } else {
                        done_[job.first] = std::move(cs);
                    }
                    ready_.notify_all();
                }
            });
    }
    ~ImfEncoderPool() { finish(); }
    void finish() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread& t : workers_)
            if (t.joinable()) t.join();
    }
    void push(int64_t index, std::vector<uint16_t>&& rgb) {
        std::lock_guard<std::mutex> lock(m_);
        todo_.emplace_back(index, std::move(rgb));
        work_.notify_one();
    }
    size_t pending() {
        std::lock_guard<std::mutex> lock(m_);
        return todo_.size() + size_t(busy_) + done_.size();
    }
    bool take(int64_t index, std::vector<uint8_t>& out, bool wait) {
        std::unique_lock<std::mutex> lock(m_);
        if (wait) ready_.wait(lock, [&] { return !error_.empty() || done_.count(index); });
        auto it = done_.find(index);
        if (it == done_.end()) return false;
        out = std::move(it->second);
        done_.erase(it);
        return true;
    }
    std::string error() {
        std::lock_guard<std::mutex> lock(m_);
        return error_;
    }

private:
    std::mutex m_;
    std::condition_variable work_, ready_;
    std::deque<std::pair<int64_t, std::vector<uint16_t>>> todo_;
    std::map<int64_t, std::vector<uint8_t>> done_;
    std::vector<std::thread> workers_;
    std::string error_;
    int busy_ = 0;
    bool stop_ = false;
};

// A file name's worth of a title: letters, digits and underscores.
QString safeTitle(const std::string& title) {
    QString out;
    for (QChar c : QString::fromStdString(title)) out += c.isLetterOrNumber() ? c : QChar('_');
    while (out.contains("__")) out.replace("__", "_");
    out = out.left(40);
    while (out.startsWith('_')) out.remove(0, 1);
    while (out.endsWith('_')) out.chop(1);
    return out.isEmpty() ? QStringLiteral("Untitled") : out;
}

// RegXML (SMPTE ST 2001-1) of the descriptors, as the composition playlist repeats them.
const QString kGroups = QStringLiteral("http://www.smpte-ra.org/reg/395/2014/13/1/aaf");
const QString kElements = QStringLiteral("http://www.smpte-ra.org/reg/335/2012");
const QString kTypes = QStringLiteral("http://www.smpte-ra.org/reg/2003/2012");

void rgbaComponents(QXmlStreamWriter& x, const QString& element, int bits) {
    x.writeStartElement("r1:" + element);
    const char* codes[] = {"CompRed", "CompGreen", "CompBlue"};
    for (int i = 0; i < 8; ++i) {
        x.writeStartElement("r2:RGBAComponent");
        x.writeTextElement("r2:Code", i < 3 ? codes[i] : "CompNull");
        x.writeTextElement("r2:ComponentSize", QString::number(i < 3 ? bits : 0));
        x.writeEndElement();
    }
    x.writeEndElement();
}

void pictureDescriptor(QXmlStreamWriter& x, const dcp::ImfPictureWriter& w, int64_t duration) {
    const dcp::J2kHeader& j = w.j2k();
    x.writeStartElement("r0:RGBADescriptor");
    x.writeNamespace(kGroups, "r0");
    x.writeNamespace(kElements, "r1");
    x.writeNamespace(kTypes, "r2");
    x.writeTextElement("r1:InstanceID", urn(w.descriptorId()));
    x.writeStartElement("r1:SubDescriptors");
    x.writeStartElement("r0:JPEG2000SubDescriptor");
    x.writeTextElement("r1:InstanceID", urn(w.subDescriptorId()));
    x.writeTextElement("r1:Rsiz", QString::number(j.rsiz));
    x.writeTextElement("r1:Xsiz", QString::number(j.xsiz));
    x.writeTextElement("r1:Ysiz", QString::number(j.ysiz));
    x.writeTextElement("r1:XOsiz", QString::number(j.xosiz));
    x.writeTextElement("r1:YOsiz", QString::number(j.yosiz));
    x.writeTextElement("r1:XTsiz", QString::number(j.xtsiz));
    x.writeTextElement("r1:YTsiz", QString::number(j.ytsiz));
    x.writeTextElement("r1:XTOsiz", QString::number(j.xtosiz));
    x.writeTextElement("r1:YTOsiz", QString::number(j.ytosiz));
    x.writeTextElement("r1:Csiz", QString::number(j.csiz));
    x.writeStartElement("r1:PictureComponentSizing");
    for (const auto& c : j.components) {
        x.writeStartElement("r2:J2KComponentSizing");
        x.writeTextElement("r2:Ssiz", QString::number(c[0]));
        x.writeTextElement("r2:XRSiz", QString::number(c[1]));
        x.writeTextElement("r2:YRSiz", QString::number(c[2]));
        x.writeEndElement();
    }
    x.writeEndElement();
    x.writeTextElement("r1:CodingStyleDefault", hex(j.cod));
    x.writeTextElement("r1:QuantizationDefault", hex(j.qcd));
    rgbaComponents(x, "J2CLayout", w.bits());
    x.writeEndElement();  // JPEG2000SubDescriptor
    x.writeEndElement();  // SubDescriptors
    const dcp::EditRate r = w.rate();
    x.writeTextElement("r1:LinkedTrackID", "1");
    x.writeTextElement("r1:SampleRate", QStringLiteral("%1/%2").arg(r.num).arg(r.den));
    x.writeTextElement("r1:EssenceLength", QString::number(duration));
    x.writeTextElement("r1:ContainerFormat", ulUrn(std::string("060e2b340401010d0d010301020c0600")));
    x.writeTextElement("r1:FrameLayout", "FullFrame");
    x.writeTextElement("r1:StoredWidth", QString::number(j.xsiz - j.xosiz));
    x.writeTextElement("r1:StoredHeight", QString::number(j.ysiz - j.yosiz));
    x.writeTextElement("r1:ImageAspectRatio", QStringLiteral("%1/%2").arg(w.aspectNum()).arg(w.aspectDen()));
    const dcp::ImfColour& c = w.colour();
    x.writeTextElement("r1:TransferCharacteristic", ulUrn(c.transfer));
    uint8_t coding[16];
    imfPictureCoding(j.rsiz, coding);
    x.writeTextElement("r1:PictureCompression", ulUrn(coding));
    if (!c.codingEquations.empty()) x.writeTextElement("r1:CodingEquations", ulUrn(c.codingEquations));
    x.writeTextElement("r1:ColorPrimaries", ulUrn(c.primaries));
    x.writeStartElement("r1:VideoLineMap");
    x.writeTextElement("r2:Int32", "0");
    x.writeTextElement("r2:Int32", "0");
    x.writeEndElement();
    if (c.hdr) {
        auto xy = [](double v) { return QString::number(std::clamp(std::lround(v / 0.00002), 0L, 50000L)); };
        x.writeStartElement("r1:MasteringDisplayPrimaries");
        for (int i = 0; i < 3; ++i) {
            x.writeStartElement("r2:ColorPrimary");
            x.writeTextElement("r2:X", xy(c.display[2 * i]));
            x.writeTextElement("r2:Y", xy(c.display[2 * i + 1]));
            x.writeEndElement();
        }
        x.writeEndElement();
        x.writeStartElement("r1:MasteringDisplayWhitePointChromaticity");
        x.writeTextElement("r2:X", xy(c.display[6]));
        x.writeTextElement("r2:Y", xy(c.display[7]));
        x.writeEndElement();
        x.writeTextElement("r1:MasteringDisplayMaximumLuminance", QString::number(std::llround(c.maxLuminance * 10000)));
        x.writeTextElement("r1:MasteringDisplayMinimumLuminance", QString::number(std::llround(c.minLuminance * 10000)));
    }
    x.writeTextElement("r1:ComponentMaxRef", QString::number((1 << w.bits()) - 1));
    x.writeTextElement("r1:ComponentMinRef", "0");
    x.writeTextElement("r1:ScanningDirection", "ScanningDirection_LeftToRightTopToBottom");
    rgbaComponents(x, "PixelLayout", w.bits());
    x.writeEndElement();  // RGBADescriptor
}

void soundDescriptor(QXmlStreamWriter& x, const dcp::ImfSoundWriter& w) {
    x.writeStartElement("r0:WAVEPCMDescriptor");
    x.writeNamespace(kGroups, "r0");
    x.writeNamespace(kElements, "r1");
    x.writeTextElement("r1:InstanceID", urn(w.descriptorId()));
    x.writeStartElement("r1:SubDescriptors");
    const dcp::McaLabel field = dcp::imfSoundfield(w.channels());
    const QString language = QString::fromStdString(w.language());
    x.writeStartElement("r0:SoundfieldGroupLabelSubDescriptor");
    x.writeTextElement("r1:InstanceID", urn(w.soundfieldId()));
    x.writeTextElement("r1:MCALabelDictionaryID", ulUrn(std::string(field.dictionary)));
    x.writeTextElement("r1:MCALinkID", urn(w.soundfieldLink()));
    x.writeTextElement("r1:MCATagSymbol", field.symbol);
    x.writeTextElement("r1:MCATagName", field.name);
    x.writeTextElement("r1:RFC5646SpokenLanguage", language);
    x.writeTextElement("r1:MCATitle", QString::fromStdString(w.title()));
    x.writeTextElement("r1:MCATitleVersion", QString::fromStdString(w.titleVersion()));
    x.writeTextElement("r1:MCAAudioContentKind", "PRM");
    x.writeTextElement("r1:MCAAudioElementKind", "FCMP");
    x.writeEndElement();
    const std::vector<dcp::McaLabel> labels = dcp::imfChannels(w.channels());
    for (int c = 0; c < w.channels(); ++c) {
        x.writeStartElement("r0:AudioChannelLabelSubDescriptor");
        x.writeTextElement("r1:InstanceID", urn(w.channelId(c)));
        x.writeTextElement("r1:MCALabelDictionaryID", ulUrn(std::string(labels[size_t(c)].dictionary)));
        x.writeTextElement("r1:MCALinkID", urn(w.channelLink(c)));
        x.writeTextElement("r1:MCATagSymbol", labels[size_t(c)].symbol);
        x.writeTextElement("r1:MCATagName", labels[size_t(c)].name);
        x.writeTextElement("r1:MCAChannelID", QString::number(c + 1));
        x.writeTextElement("r1:RFC5646SpokenLanguage", language);
        x.writeTextElement("r1:SoundfieldGroupLinkID", urn(w.soundfieldLink()));
        x.writeEndElement();
    }
    x.writeEndElement();  // SubDescriptors
    x.writeTextElement("r1:LinkedTrackID", "1");
    x.writeTextElement("r1:SampleRate", "48000/1");
    x.writeTextElement("r1:EssenceLength", QString::number(w.duration()));
    x.writeTextElement("r1:ContainerFormat", ulUrn(std::string("060e2b34040101010d01030102060200")));
    x.writeTextElement("r1:AudioSampleRate", "48000/1");
    x.writeTextElement("r1:Locked", "False");
    x.writeTextElement("r1:ChannelCount", QString::number(w.channels()));
    x.writeTextElement("r1:QuantizationBits", "24");
    x.writeTextElement("r1:BlockAlign", QString::number(3 * w.channels()));
    x.writeTextElement("r1:AverageBytesPerSecond", QString::number(48000 * 3 * w.channels()));
    x.writeTextElement("r1:ChannelAssignment", ulUrn(std::string("060e2b340401010d0402021004010000")));
    x.writeEndElement();
}

}  // namespace

std::string defaultImfColour(const Sequence& s) {
    const ColorSpace& cs = sequenceColorSpace(s);
    if (cs.transfer == Transfer::Hlg) return "rec2020-hlg";
    if (cs.transfer == Transfer::Pq) return cs.primaries == Primaries::P3D65 ? "p3d65-pq" : "rec2020-pq";
    return "rec709";
}

bool imfFrameRateAllowed(const Sequence& s) {
    static const Rational rates[] = {{24, 1}, {24000, 1001}, {25, 1}, {30, 1}, {30000, 1001}, {50, 1}, {60, 1}, {60000, 1001}, {120, 1}};
    for (const Rational& r : rates)
        if (int64_t(s.fps.num) * r.den == int64_t(r.num) * s.fps.den) return r.num != 120 || s.width > 1920 || s.height > 1080;
    return false;
}

bool imfPictureSize(const Sequence& s, const std::string& size, int& width, int& height, std::string* error) {
    if (size == "hd") width = 1920, height = 1080;
    else if (size == "uhd") width = 3840, height = 2160;
    else if (size == "4k") width = 4096, height = 2160;
    else if (size.empty() || size == "sequence") width = s.width, height = s.height;
    else {
        if (error) *error = "The size is sequence, hd, uhd or 4k";
        return false;
    }
    if (width <= 0 || height <= 0) {
        if (error) *error = "The sequence has no picture size";
        return false;
    }
    if (width > 4096 || height > 3112) {
        if (error) *error = "IMF Application #2E goes up to 4096 x 3112: choose UHD or 4K";
        return false;
    }
    return true;
}

bool exportImf(const Project& p, const Sequence& s, const ImfSettings& settings, const std::string& parent, ImfResult* result,
               const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!openJpegAvailable()) return fail("This build of Montage has no OpenJPEG, which IMF masters are coded with");
    if (!validKind(settings.kind)) return fail("Unknown content kind " + settings.kind);
    if (!imfFrameRateAllowed(s)) return fail("IMF Application #2E takes 23.976, 24, 25, 29.97, 30, 50, 59.94 or 60 fps (120 above HD)");
    int W = 0, H = 0;
    std::string err;
    if (!imfPictureSize(s, settings.size, W, H, &err)) return fail(err);
    const std::string colourId = settings.colour.empty() ? defaultImfColour(s) : settings.colour;
    const ImfColourSpec* spec = colourSpec(colourId);
    if (!spec) return fail("The colour is rec709, p3d65-pq, rec2020-pq or rec2020-hlg");
    const ColorSpace* target = findColorSpace(spec->space);
    if (!target) return fail("Missing colour space " + std::string(spec->space));
    const bool hdrOut = target->hdr();
    const int bits = settings.bits ? settings.bits : (hdrOut ? 12 : 10);
    if (bits != 10 && bits != 12) return fail("IMF pictures here are 10 or 12 bits");
    if (!settings.lossless && !(settings.megabitsPerSecond >= 20)) return fail("A lossy master needs at least 20 Mbit/s");
    const dcp::EditRate rate{uint32_t(s.fps.num), uint32_t(s.fps.den)};
    const double fps = rate.value();
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint >= s.inPoint) first = s.inPoint, end = s.outPoint + 1;
    if (end <= first) return fail("The sequence is empty");
    const int64_t frames = end - first;
    const uint16_t rsiz = imfRsiz(W, H, 3, fps, settings.lossless, settings.megabitsPerSecond);
    if (!rsiz) return fail("No JPEG 2000 IMF profile takes that picture at that rate");
    const size_t maxBytes = settings.lossless ? 0 : size_t(settings.megabitsPerSecond * 1e6 / 8 / fps);
    const int seqChannels = layoutChannels(s.audioLayout);
    const int channels = seqChannels == 8 ? 8 : seqChannels == 6 ? 6 : 2;

    // The folder.
    const QString title = QString::fromStdString(settings.title.empty() ? "Untitled" : settings.title);
    const QString name = safeTitle(title.toStdString()) + "_IMF_" + QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
    const QString folder = QDir(QString::fromStdString(parent)).filePath(name);
    if (QFileInfo::exists(folder)) return fail("There is already a folder called " + name.toStdString() + " there");
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
    const QString pictureFile = QStringLiteral("VIDEO_%1.mxf").arg(QString::fromStdString(dcp::uuidString(pictureId)));
    const QString soundFile = QStringLiteral("AUDIO_%1.mxf").arg(QString::fromStdString(dcp::uuidString(soundId)));

    // Sound first: the mix at 48 kHz from the first frame's time, as many samples as the frames last.
    dcp::ImfSoundWriter sound;
    if (!sound.open(QDir(folder).filePath(soundFile).toStdString(), soundId, channels, settings.language, title.toStdString(), "1", &err))
        return fail(err);
    {
        Sequence mixSeq = s;
        mixSeq.sampleRate = 48000;
        const int64_t firstSample = int64_t(std::llround(double(first) * 48000.0 * rate.den / rate.num));
        const int64_t total = int64_t(std::llround(double(frames) * 48000.0 * rate.den / rate.num));
        const int chunk = 4800;
        const int in = std::max(2, seqChannels);
        std::vector<float> mix(size_t(chunk) * size_t(in)), out(size_t(chunk) * size_t(channels));
        // Stereo as it is; 5.1 as it is; 7.1 (L R C LFE Lb Rb Ls Rs, as FFmpeg orders it) to 7.1 DS (L R C LFE Lss Rss Lrs Rrs).
        std::vector<int> route;
        if (channels == 8) route = {0, 1, 2, 3, 6, 7, 4, 5};
        else if (channels == 6) route = {0, 1, 2, 3, 4, 5};
        else route = {0, 1};
        AudioMixer mixer;
        for (int64_t done = 0; done < total; done += chunk) {
            const int n = int(std::min<int64_t>(chunk, total - done));
            if (seqChannels > 2) mixer.mixLayout(p, mixSeq, firstSample + done, n, mix.data());
            else mixer.mix(p, mixSeq, firstSample + done, n, mix.data());
            for (int i = 0; i < n; ++i)
                for (size_t c = 0; c < route.size(); ++c) out[size_t(i) * size_t(channels) + size_t(route[c])] = mix[size_t(i) * size_t(in) + c];
            if (!sound.write(out.data(), size_t(n), &err)) return fail(err);
            if (progress && (done / chunk) % 20 == 0 && !progress(0.05 * double(done) / double(total))) return fail("Stopped");
        }
        if (!sound.close(&err)) return fail(err);
    }

    // The picture, fitted inside the frame on black, several frames encoding at once.
    const double scale = std::min(double(W) / s.width, double(H) / s.height);
    const int fw = std::min(W, int(std::lround(s.width * scale / 2)) * 2), fh = std::min(H, int(std::lround(s.height * scale / 2)) * 2);
    const int ox = (W - fw) / 2, oy = (H - fh) / 2;
    dcp::ImfColour colour;
    colour.primaries = spec->primaries;
    colour.transfer = spec->transfer;
    colour.codingEquations = spec->codingEquations;
    colour.hdr = spec->hdr;
    if (spec->hdr) {
        // A P3-D65 mastering display (as HDR grading monitors are), at the master's peak.
        const double p3[8] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
        std::copy(std::begin(p3), std::end(p3), colour.display);
        colour.maxLuminance = settings.masteringPeak > 0 ? settings.masteringPeak : std::max(1000.0, s.hdrPeakNits);
        colour.minLuminance = 0.0001;
    }
    const int threads = settings.threads > 0 ? settings.threads : std::max(1, int(std::thread::hardware_concurrency()));
    dcp::ImfPictureWriter picture;
    {
        const int g = std::gcd(W, H);
        if (!picture.open(QDir(folder).filePath(pictureFile).toStdString(), pictureId, rate, bits, colour, uint32_t(W / g), uint32_t(H / g), &err))
            return fail(err);
        ImfEncoderPool pool(W, H, bits, rsiz, maxBytes, threads);
        RenderOptions o;
        o.scale = scale;
        o.highQuality = true;
        const ColorSpace& seqSpace = sequenceColorSpace(s);
        const double maxCode = double((1 << bits) - 1);
        int64_t written = 0;
        std::vector<uint8_t> cs;
        auto writeReady = [&](bool wait) {
            while (written < frames && pool.take(written, cs, wait)) {
                if (written == 0) {
                    dcp::J2kHeader h;
                    if (!dcp::parseJ2kHeader(cs.data(), cs.size(), h) || h.rsiz != rsiz) {
                        err = "OpenJPEG could not code the picture in the JPEG 2000 IMF profile";
                        return false;
                    }
                }
                if (!picture.write(cs.data(), cs.size(), &err)) return false;
                ++written;
                wait = false;
            }
            return pool.error().empty();
        };
        const size_t inHand = size_t(threads) * 2;
        for (int64_t k = 0; k < frames; ++k) {
            Image frame = renderProgramFrame(p, s, first + k, o);
            if (frame.width != fw || frame.height != fh) frame = resizeImage(frame, fw, fh);
            if (target != &seqSpace) convertColor(frame, seqSpace, *target, s.hdrPeakNits);
            std::vector<uint16_t> rgb(size_t(W) * size_t(H) * 3, 0);  // black round it
            for (int y = 0; y < fh; ++y) {
                const float* src = frame.row(y);
                uint16_t* d = rgb.data() + (size_t(y + oy) * size_t(W) + size_t(ox)) * 3;
                for (int x = 0; x < fw; ++x, src += 4, d += 3)
                    for (int c = 0; c < 3; ++c) d[c] = uint16_t(std::lround(std::clamp(double(src[c]), 0.0, 1.0) * maxCode));
            }
            while (pool.pending() >= inHand && written < k)
                if (!writeReady(true)) return fail(err.empty() ? pool.error() : err);
            pool.push(k, std::move(rgb));
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

    // Hashes, then the composition, packing list and asset map.
    const QString picturePath = QDir(folder).filePath(pictureFile), soundPath = QDir(folder).filePath(soundFile);
    const qint64 toHash = std::max<qint64>(1, QFileInfo(picturePath).size() + QFileInfo(soundPath).size());
    qint64 hashed = 0;
    bool stopped = false;
    auto hashing = [&](qint64 n) {
        hashed += n;
        stopped = progress && !progress(0.95 + 0.05 * double(hashed) / double(toHash));
        return !stopped;
    };
    qint64 pictureSize = 0, soundSize = 0;
    const QString pictureHash = packageFileHash(picturePath, &pictureSize, hashing);
    const QString soundHash = pictureHash.isEmpty() ? QString() : packageFileHash(soundPath, &soundSize, hashing);
    if (pictureHash.isEmpty() || soundHash.isEmpty()) return fail(stopped ? "Stopped" : "Cannot read the track files back");
    const QString issued = packageTimestamp();
    const QString issuer = QString::fromStdString(settings.issuer.empty() ? "Montage" : settings.issuer);
    const QString lang = QString::fromStdString(settings.language.empty() ? "en" : settings.language);
    const dcp::Uuid cplId = dcp::newUuid(), pklId = dcp::newUuid(), amId = dcp::newUuid(), versionId = dcp::newUuid();
    const dcp::Uuid segmentId = dcp::newUuid(), imageSeqId = dcp::newUuid(), audioSeqId = dcp::newUuid(), imageTrack = dcp::newUuid(),
                    audioTrack = dcp::newUuid(), imageRes = dcp::newUuid(), audioRes = dcp::newUuid();
    const dcp::Uuid pictureDescId = dcp::newUuid(), soundDescId = dcp::newUuid();
    const QString cplFile = QStringLiteral("CPL_%1.xml").arg(QString::fromStdString(dcp::uuidString(cplId)));
    const QString pklFile = QStringLiteral("PKL_%1.xml").arg(QString::fromStdString(dcp::uuidString(pklId)));
    auto writeXml = [&](const QString& file, const std::function<void(QXmlStreamWriter&)>& body) {
        QFile f(QDir(folder).filePath(file));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        QXmlStreamWriter x(&f);
        x.setAutoFormatting(true);
        x.setAutoFormattingIndent(2);
        x.writeStartDocument(QStringLiteral("1.0"), true);
        body(x);
        x.writeEndDocument();
        return !x.hasError() && f.error() == QFileDevice::NoError;
    };
    auto text = [&](QXmlStreamWriter& x, const QString& el, const QString& value) {
        x.writeStartElement(el);
        x.writeAttribute("language", lang);
        x.writeCharacters(value);
        x.writeEndElement();
    };
    const int64_t runSeconds = int64_t(std::llround(double(frames) / fps));
    const QString running = QStringLiteral("%1:%2:%3")
                                .arg(runSeconds / 3600, 2, 10, QLatin1Char('0'))
                                .arg(runSeconds / 60 % 60, 2, 10, QLatin1Char('0'))
                                .arg(runSeconds % 60, 2, 10, QLatin1Char('0'));
    const QString editRate = QStringLiteral("%1 %2").arg(rate.num).arg(rate.den);
    const bool cplOk = writeXml(cplFile, [&](QXmlStreamWriter& x) {
        x.writeStartElement("CompositionPlaylist");
        x.writeDefaultNamespace("http://www.smpte-ra.org/schemas/2067-3/2016");
        x.writeNamespace("http://www.smpte-ra.org/ns/2067-2/2020", "cc");
        x.writeNamespace("http://www.w3.org/2001/XMLSchema-instance", "xsi");
        x.writeTextElement("Id", urn(cplId));
        text(x, "Annotation", title);
        x.writeTextElement("IssueDate", issued);
        text(x, "Issuer", issuer);
        text(x, "Creator", "Montage");
        text(x, "ContentOriginator", issuer);
        text(x, "ContentTitle", title);
        x.writeStartElement("ContentKind");
        x.writeAttribute("scope", "http://www.smpte-ra.org/schemas/2067-3/2013#content-kind");
        x.writeCharacters(QString::fromStdString(settings.kind));
        x.writeEndElement();
        x.writeStartElement("ContentVersionList");
        x.writeStartElement("ContentVersion");
        x.writeTextElement("Id", urn(versionId));
        text(x, "LabelText", title + " " + issued.left(10));
        x.writeEndElement();
        x.writeEndElement();
        x.writeStartElement("EssenceDescriptorList");
        x.writeStartElement("EssenceDescriptor");
        x.writeTextElement("Id", urn(pictureDescId));
        pictureDescriptor(x, picture, frames);
        x.writeEndElement();
        x.writeStartElement("EssenceDescriptor");
        x.writeTextElement("Id", urn(soundDescId));
        soundDescriptor(x, sound);
        x.writeEndElement();
        x.writeEndElement();
        x.writeTextElement("EditRate", editRate);
        x.writeTextElement("TotalRunningTime", running);
        x.writeStartElement("ExtensionProperties");
        x.writeTextElement("cc:ApplicationIdentification", "http://www.smpte-ra.org/ns/2067-21/2021");
        x.writeEndElement();
        x.writeStartElement("SegmentList");
        x.writeStartElement("Segment");
        x.writeTextElement("Id", urn(segmentId));
        x.writeStartElement("SequenceList");
        auto sequence = [&](const QString& kind, const dcp::Uuid& id, const dcp::Uuid& track, const dcp::Uuid& res, const QString& resRate,
                            int64_t duration, const dcp::Uuid& desc, const dcp::Uuid& file, const QString& hash) {
            x.writeStartElement(kind);
            x.writeTextElement("Id", urn(id));
            x.writeTextElement("TrackId", urn(track));
            x.writeStartElement("ResourceList");
            x.writeStartElement("Resource");
            x.writeAttribute("xsi:type", "TrackFileResourceType");
            x.writeTextElement("Id", urn(res));
            x.writeTextElement("EditRate", resRate);
            x.writeTextElement("IntrinsicDuration", QString::number(duration));
            x.writeTextElement("EntryPoint", "0");
            x.writeTextElement("SourceDuration", QString::number(duration));
            x.writeTextElement("SourceEncoding", urn(desc));
            x.writeTextElement("TrackFileId", urn(file));
            x.writeTextElement("Hash", hash);
            x.writeEmptyElement("HashAlgorithm");
            x.writeAttribute("Algorithm", "http://www.w3.org/2000/09/xmldsig#sha1");
            x.writeEndElement();
            x.writeEndElement();
            x.writeEndElement();
        };
        sequence("cc:MainImageSequence", imageSeqId, imageTrack, imageRes, editRate, frames, pictureDescId, pictureId, pictureHash);
        sequence("cc:MainAudioSequence", audioSeqId, audioTrack, audioRes, "48000 1", sound.duration(), soundDescId, soundId, soundHash);
        x.writeEndElement();  // SequenceList
        x.writeEndElement();  // Segment
        x.writeEndElement();  // SegmentList
        x.writeEndElement();
    });
    if (!cplOk) return fail("Cannot write the composition playlist");
    qint64 cplSize = 0;
    const QString cplHash = packageFileHash(QDir(folder).filePath(cplFile), &cplSize);
    const bool pklOk = writeXml(pklFile, [&](QXmlStreamWriter& x) {
        x.writeStartElement("PackingList");
        x.writeDefaultNamespace("http://www.smpte-ra.org/schemas/2067-2/2016/PKL");
        x.writeTextElement("Id", urn(pklId));
        text(x, "AnnotationText", title);
        x.writeTextElement("IssueDate", issued);
        text(x, "Issuer", issuer);
        text(x, "Creator", "Montage");
        x.writeStartElement("AssetList");
        auto asset = [&](const dcp::Uuid& id, const QString& hash, qint64 size, const QString& type, const QString& file) {
            x.writeStartElement("Asset");
            x.writeTextElement("Id", urn(id));
            text(x, "AnnotationText", file);
            x.writeTextElement("Hash", hash);
            x.writeTextElement("Size", QString::number(size));
            x.writeTextElement("Type", type);
            text(x, "OriginalFileName", file);
            x.writeEmptyElement("HashAlgorithm");
            x.writeAttribute("Algorithm", "http://www.w3.org/2000/09/xmldsig#sha1");
            x.writeEndElement();
        };
        asset(cplId, cplHash, cplSize, "text/xml", cplFile);
        asset(pictureId, pictureHash, pictureSize, "application/mxf", pictureFile);
        asset(soundId, soundHash, soundSize, "application/mxf", soundFile);
        x.writeEndElement();
        x.writeEndElement();
    });
    if (!pklOk) return fail("Cannot write the packing list");
    const qint64 pklSize = QFileInfo(QDir(folder).filePath(pklFile)).size();
    const bool amOk = writeXml("ASSETMAP.xml", [&](QXmlStreamWriter& x) {
        x.writeStartElement("AssetMap");
        x.writeDefaultNamespace("http://www.smpte-ra.org/schemas/429-9/2007/AM");
        x.writeTextElement("Id", urn(amId));
        text(x, "AnnotationText", title);
        text(x, "Creator", "Montage");
        x.writeTextElement("VolumeCount", "1");
        x.writeTextElement("IssueDate", issued);
        text(x, "Issuer", issuer);
        x.writeStartElement("AssetList");
        auto asset = [&](const dcp::Uuid& id, bool pkl, const QString& file, qint64 size) {
            x.writeStartElement("Asset");
            x.writeTextElement("Id", urn(id));
            if (pkl) x.writeTextElement("PackingList", "true");
            x.writeStartElement("ChunkList");
            x.writeStartElement("Chunk");
            x.writeTextElement("Path", file);
            x.writeTextElement("VolumeIndex", "1");
            x.writeTextElement("Offset", "0");
            x.writeTextElement("Length", QString::number(size));
            x.writeEndElement();
            x.writeEndElement();
            x.writeEndElement();
        };
        asset(pklId, true, pklFile, pklSize);
        asset(cplId, false, cplFile, cplSize);
        asset(pictureId, false, pictureFile, pictureSize);
        asset(soundId, false, soundFile, soundSize);
        x.writeEndElement();
        x.writeEndElement();
    });
    if (!amOk) return fail("Cannot write the asset map");
    finished = true;
    if (result) {
        result->folder = folder.toStdString();
        result->cpl = QDir(folder).filePath(cplFile).toStdString();
        result->frames = frames;
        result->rateNum = rate.num;
        result->rateDen = rate.den;
        result->width = W;
        result->height = H;
        result->bits = bits;
        result->channels = channels;
        result->rsiz = picture.j2k().rsiz;
        result->colour = colourId;
    }
    return true;
}

// ---- Checking -----------------------------------------------------------------------------------------------------

namespace {

struct ImfTrack {
    bool ok = false;
    std::string codec;
    int sampleRate = 0, channels = 0, bits = 0;
    int64_t packets = 0, bytes = 0;
    std::vector<uint8_t> firstPacket;
};

ImfTrack probeImfTrack(const std::string& path, const std::function<bool(qint64)>& read) {
    ImfTrack t;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return t;
    if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->nb_streams > 0) {
        const AVCodecParameters* cp = fmt->streams[0]->codecpar;
        t.codec = avcodec_get_name(cp->codec_id);
        t.sampleRate = cp->sample_rate;
        t.channels = cp->ch_layout.nb_channels;
        t.bits = cp->bits_per_coded_sample;
        AVPacket* pkt = av_packet_alloc();
        qint64 since = 0;
        bool going = true;
        while (going && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == 0) {
                if (t.packets == 0) t.firstPacket.assign(pkt->data, pkt->data + pkt->size);
                ++t.packets;
                t.bytes += pkt->size;
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

std::vector<std::string> verifyImf(const std::string& folderPath, const std::function<bool(double)>& progress) {
    std::vector<std::string> issues;
    auto issue = [&](const QString& s) { issues.push_back(s.toStdString()); };
    const QDir dir(QString::fromStdString(folderPath));
    if (!dir.exists("ASSETMAP.xml")) {
        issue("No ASSETMAP.xml");
        return issues;
    }
    QString root;
    const auto am = packageXmlItems(dir.filePath("ASSETMAP.xml"), &root);
    if (root != "AssetMap") {
        issue("ASSETMAP.xml is not an asset map");
        return issues;
    }
    struct Asset {
        QString path, hash, type;
        qint64 size = -1, length = -1;
        bool packingList = false;
    };
    std::map<QString, Asset> assets;
    Asset* cur = nullptr;
    for (const auto& [path, text] : am) {
        if (path == "AssetMap/AssetList/Asset") cur = nullptr;
        if (path == "AssetMap/AssetList/Asset/Id") cur = &assets[packageBareId(text)];
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
    std::vector<QString> cpls;
    int packingLists = 0;
    for (auto& [id, a] : assets) {
        if (!a.packingList) continue;
        ++packingLists;
        const auto pkl = packageXmlItems(dir.filePath(a.path), &root);
        if (root != "PackingList") {
            issue(a.path + " is not a packing list");
            continue;
        }
        QString assetId;
        for (const auto& [path, text] : pkl) {
            if (path == "PackingList/AssetList/Asset/Id") {
                assetId = packageBareId(text);
                if (!assets.count(assetId)) issue(QStringLiteral("Asset %1 is in the packing list but not the asset map").arg(assetId));
            }
            if (assetId.isEmpty() || !assets.count(assetId)) continue;
            Asset& x = assets[assetId];
            if (path.endsWith("Asset/Hash")) x.hash = text;
            if (path.endsWith("Asset/Size")) x.size = text.toLongLong();
            if (path.endsWith("Asset/Type")) {
                x.type = text;
                if (text.contains("xml")) cpls.push_back(assetId);
            }
        }
    }
    if (packingLists == 0) issue("The asset map names no packing list");
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
        const QString hash = packageFileHash(dir.filePath(a.path), &size, reading);
        if (stopped) {
            issue("Stopped");
            return issues;
        }
        if (hash != a.hash) issue(QStringLiteral("%1 does not match its hash in the packing list (damaged or changed)").arg(a.path));
        if (a.size >= 0 && size != a.size) issue(QStringLiteral("%1 is %2 bytes; the packing list says %3").arg(a.path).arg(size).arg(a.size));
    }
    int compositions = 0;
    for (const QString& cplId : cpls) {
        const Asset& c = assets[cplId];
        const auto cpl = packageXmlItems(dir.filePath(c.path), &root);
        if (root != "CompositionPlaylist") continue;
        ++compositions;
        std::set<QString> descriptors;
        bool application = false;
        struct Resource {
            QString kind, file, encoding;
            int64_t intrinsic = -1, entry = 0, duration = -1;
        };
        std::vector<Resource> resources;
        for (const auto& [path, text] : cpl) {
            if (path == "CompositionPlaylist/EssenceDescriptorList/EssenceDescriptor/Id") descriptors.insert(packageBareId(text));
            if (path.endsWith("ExtensionProperties/ApplicationIdentification") && text.startsWith("http://www.smpte-ra.org/ns/2067-21/"))
                application = true;
            // CompositionPlaylist/SegmentList/Segment/SequenceList/<kind>/ResourceList/Resource/<field>
            const QStringList parts = path.split('/');
            if (parts.size() < 7 || parts[3] != "SequenceList" || parts[5] != "ResourceList" || parts[6] != "Resource") continue;
            if (parts.size() == 7) resources.push_back({parts[4], {}, {}});
            if (parts.size() != 8 || resources.empty()) continue;
            Resource& r = resources.back();
            if (parts[7] == "TrackFileId") r.file = packageBareId(text);
            if (parts[7] == "SourceEncoding") r.encoding = packageBareId(text);
            if (parts[7] == "IntrinsicDuration") r.intrinsic = text.toLongLong();
            if (parts[7] == "EntryPoint") r.entry = text.toLongLong();
            if (parts[7] == "SourceDuration") r.duration = text.toLongLong();
        }
        if (!application) issue(c.path + " does not identify IMF Application #2E");
        bool picture = false;
        for (const Resource& r : resources) {
            if (!assets.count(r.file) || assets[r.file].hash.isEmpty()) {
                issue(QStringLiteral("%1 %2 in %3 is not in the package").arg(r.kind, r.file, c.path));
                continue;
            }
            if (r.encoding.isEmpty() || !descriptors.count(r.encoding))
                issue(QStringLiteral("%1 %2 has no essence descriptor in %3").arg(r.kind, r.file, c.path));
            const int64_t duration = r.duration >= 0 ? r.duration : r.intrinsic - r.entry;
            if (r.intrinsic >= 0 && r.entry + duration > r.intrinsic) issue(QStringLiteral("%1 %2 plays past its end").arg(r.kind, r.file));
            const std::string file = dir.filePath(assets[r.file].path).toStdString();
            const ImfTrack t = probeImfTrack(file, reading);
            if (stopped) {
                issue("Stopped");
                return issues;
            }
            if (r.kind == "MainImageSequence") {
                picture = true;
                dcp::J2kHeader h;
                if (!t.ok || t.codec != "jpeg2000") issue(QStringLiteral("%1 is not JPEG 2000 pictures").arg(assets[r.file].path));
                else if (!dcp::parseJ2kHeader(t.firstPacket.data(), t.firstPacket.size(), h) || (h.rsiz & 0xff00) < 0x0400 || (h.rsiz & 0xff00) > 0x0900)
                    issue(QStringLiteral("%1 is not in a JPEG 2000 IMF profile").arg(assets[r.file].path));
                else if (r.intrinsic >= 0 && t.packets != r.intrinsic)
                    issue(QStringLiteral("%1 has %2 frames; the composition says %3").arg(assets[r.file].path).arg(t.packets).arg(r.intrinsic));
            } else if (r.kind == "MainAudioSequence") {
                if (!t.ok || t.codec.rfind("pcm_s24", 0) != 0 || (t.sampleRate != 48000 && t.sampleRate != 96000))
                    issue(QStringLiteral("%1 is not 24-bit 48 or 96 kHz sound").arg(assets[r.file].path));
                else if (r.intrinsic >= 0 && t.channels > 0 && t.bytes / (3 * t.channels) != r.intrinsic)
                    issue(QStringLiteral("%1 has %2 samples; the composition says %3").arg(assets[r.file].path).arg(t.bytes / (3 * t.channels)).arg(r.intrinsic));
            }
        }
        if (!picture) issue(c.path + " has no picture");
    }
    if (compositions == 0) issue("The package holds no composition playlist");
    return issues;
}

bool readImfFrame(const std::string& mxf, int index, std::vector<uint16_t>& rgb, int& width, int& height, int& bits, std::string* error) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, mxf.c_str(), nullptr, nullptr) < 0) {
        if (error) *error = "Cannot open " + mxf;
        return false;
    }
    bool got = false;
    std::vector<uint8_t> data;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        AVPacket* pkt = av_packet_alloc();
        int n = 0;
        while (!got && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == 0 && n++ == index) {
                data.assign(pkt->data, pkt->data + pkt->size);
                got = true;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    if (!got) {
        if (error) *error = "No such frame";
        return false;
    }
    int components = 0;
    if (!decodeJpeg2000(data.data(), data.size(), rgb, width, height, components, bits, error)) return false;
    if (components != 3) {
        if (error) *error = "Not RGB";
        return false;
    }
    return true;
}

}  // namespace montage
