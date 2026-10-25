#include "Imf.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "ColorSpace.h"
#include "Compositor.h"
#include "DcpMxf.h"
#include "ImageSubtitles.h"
#include "Jpeg2000.h"
#include "PackageFiles.h"
#include "RateConversion.h"
#include "core/AudioDescription.h"
#include "core/Captions.h"
#include "core/EditOps.h"
#include "core/ImportBuilder.h"
#include "core/Surround.h"
#include "core/TimelineCompare.h"
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
        // (an HTJ2K Rsiz codes through OpenJPH, the rest through OpenJPEG)
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
                    bool ok = false;
                    try {
                        ok = rsiz == kHtj2kRsiz ? encodeHtj2k(job.second.data(), w, h, bits, cs, &err)
                                                : encodeJpeg2000(job.second.data(), w, h, bits, rsiz, maxBytes, cs, &err);
                    } catch (const std::bad_alloc&) {
                        err = "Out of memory coding the pictures: try fewer encoders at once";
                    } catch (const std::exception& e) {
                        err = e.what();
                    }
                    job.second = {};
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
    if (j.pcap) {
        // (HTJ2K) its CAP marker: the parts it needs and their capabilities.
        x.writeStartElement("r1:J2KExtendedCapabilities");
        x.writeTextElement("r2:Pcap", QString::number(j.pcap));
        x.writeStartElement("r2:Ccapi");
        for (uint16_t c : j.ccap) x.writeTextElement("r2:UInt16", QString::number(c));
        x.writeEndElement();
        x.writeEndElement();
    }
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
    x.writeTextElement("r1:MCAAudioContentKind", QString::fromStdString(w.contentKind()));
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

bool imfFrameRateAllowed(Rational fps, int width, int height, Rational* canonical) {
    static const Rational rates[] = {{24, 1}, {24000, 1001}, {25, 1}, {30, 1}, {30000, 1001}, {50, 1}, {60, 1}, {60000, 1001}, {120, 1}};
    for (const Rational& r : rates)
        if (int64_t(fps.num) * r.den == int64_t(r.num) * fps.den) {
            if (canonical) *canonical = r;
            return r.num != 120 || width > 1920 || height > 1080;
        }
    return false;
}

bool imfFrameRateAllowed(const Sequence& s) { return imfFrameRateAllowed(s.fps, s.width, s.height); }

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

namespace {

bool validLanguage(const std::string& l) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*$"));
    return re.match(QString::fromStdString(l)).hasMatch();
}

// The virtual track a caption track becomes.
QString textSequence(const std::string& kind) {
    if (kind == "subtitles") return QStringLiteral("SubtitlesSequence");
    if (kind == "captions") return QStringLiteral("HearingImpairedCaptionsSequence");
    if (kind == "forced") return QStringLiteral("ForcedNarrativeSequence");
    return {};
}

// Whether a virtual track kind is timed text (the three Montage writes and the others ST 2067-2 has).
bool isTextSequence(const QString& kind) {
    return kind == "SubtitlesSequence" || kind == "HearingImpairedCaptionsSequence" || kind == "ForcedNarrativeSequence" ||
           kind == "VisuallyImpairedTextSequence" || kind == "CommentarySequence" || kind == "KaraokeSequence";
}

// The first sample of frame `frame` at 48 kHz.
int64_t sampleAt(int64_t frame, dcp::EditRate r) { return frame * 48000 * int64_t(r.den) / int64_t(r.num); }

// A stretch of a virtual track: from the earlier packages (`at` an edit unit of the earlier composition's track), or
// from this package's own track file (`at` an edit unit in it).
struct Run {
    bool old = false;
    int64_t at = 0, length = 0;
};
void addRun(std::vector<Run>& runs, bool old, int64_t at, int64_t n) {
    if (!runs.empty() && runs.back().old == old && runs.back().at + runs.back().length == at) runs.back().length += n;
    else runs.push_back({old, at, n});
}

// The resources of `t` that play its edit units [at, at + n), each cut to that stretch.
std::vector<ImfResource> slice(const ImfVirtualTrack& t, int64_t at, int64_t n) {
    std::vector<ImfResource> out;
    int64_t pos = 0;
    for (const ImfResource& r : t.resources) {
        const int64_t repeat = std::max<int64_t>(1, r.repeat);
        if (r.duration <= 0) continue;
        // (only the repeats that fall in the stretch)
        int64_t k = at > pos ? (at - pos) / r.duration : 0;
        for (int64_t start = pos + k * r.duration; k < repeat && start < at + n; ++k, start += r.duration) {
            const int64_t a = std::max(at, start), b = std::min(at + n, start + r.duration);
            if (a >= b) continue;
            ImfResource c = r;
            c.entry = r.entry + (a - start);
            c.duration = b - a;
            c.repeat = 1;
            out.push_back(c);
        }
        pos += r.duration * repeat;
        if (pos >= at + n) break;
    }
    return out;
}

// Track files by id, each opened once.
class TrackFiles {
public:
    explicit TrackFiles(std::map<std::string, std::string> paths) : paths_(std::move(paths)) {}
    const dcp::ImfTrackReader* get(const std::string& id, std::string* error = nullptr) {
        if (auto it = open_.find(id); it != open_.end()) return &it->second;
        const auto path = paths_.find(id);
        if (path == paths_.end()) {
            if (error) *error = "Track file " + id + " is neither in the earlier package nor in those beside it";
            return nullptr;
        }
        dcp::ImfTrackReader r;
        if (!r.open(path->second, error)) return nullptr;
        return &open_.emplace(id, std::move(r)).first->second;
    }

private:
    std::map<std::string, std::string> paths_;
    std::map<std::string, dcp::ImfTrackReader> open_;
};

// The earlier composition's sound, through its resources, read a window at a time.
class ComposedPcm {
public:
    ComposedPcm(const ImfVirtualTrack& t, TrackFiles& files) : t_(t), files_(files), length_(t.length()) {}
    // `n` sample frames from the composition's sample `at` (interleaved), or nullptr past the end or when a file
    // cannot be read.
    // Two windows, so that trying two offsets in turn (the one used last and the one the picture moved by) does not
    // read the same stretch again and again.
    const int32_t* read(int64_t at, int64_t n) {
        if (at < 0 || n < 0 || at > length_ - n || t_.channels <= 0) return nullptr;
        ++clock_;
        for (Window& w : windows_)
            if (at >= w.from && at + n <= w.from + w.have) {
                w.used = clock_;
                return w.samples.data() + (at - w.from) * t_.channels;
            }
        Window& w = windows_[0].used <= windows_[1].used ? windows_[0] : windows_[1];
        w.used = clock_;
        w.from = at;
        w.have = std::min<int64_t>(std::max<int64_t>(n, 480000), length_ - at);
        w.samples.resize(size_t(w.have * t_.channels));
        int64_t put = 0;
        for (const ImfResource& r : slice(t_, w.from, w.have)) {
            const dcp::ImfTrackReader* f = files_.get(r.trackFile);
            if (!f || f->channels() != t_.channels || !f->pcm(r.entry, r.duration, part_)) {
                w.have = 0;
                return nullptr;
            }
            std::copy(part_.begin(), part_.end(), w.samples.begin() + ptrdiff_t(put * t_.channels));
            put += r.duration;
        }
        return w.samples.data();
    }

private:
    struct Window {
        int64_t from = 0, have = 0, used = 0;
        std::vector<int32_t> samples;
    };
    const ImfVirtualTrack& t_;
    TrackFiles& files_;
    int64_t length_ = 0, clock_ = 0;
    Window windows_[2];
    std::vector<int32_t> part_;
};

// The sequence a sound version is mixed from: the roles it hears.
Sequence soundVersion(const Sequence& s, const ImfSound& v) {
    Sequence out = s;
    if (!v.roles.empty()) {
        out.mutedRoles.clear();
        for (const std::string& r : edit::sequenceRoles(s))
            if (std::find(v.roles.begin(), v.roles.end(), r) == v.roles.end()) out.mutedRoles.push_back(r);
        return out;
    }
    edit::setRoleMuted(out, kDescriptionRole, v.kind != "DV");
    if (v.kind == "ME") edit::setRoleMuted(out, "Dialogue", true);
    return out;
}

// A descriptor's RegXML as text, to go into the composition playlist as it is.
QString descriptorXml(const std::function<void(QXmlStreamWriter&)>& write) {
    QString out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.setAutoFormattingIndent(2);
    write(x);
    return out.trimmed();
}

// An IMSC Image document's images ("urn:uuid:..." resources) from its track file.
std::function<QByteArray(const std::string&)> imscResource(const dcp::ImfTrackReader& reader) {
    return [&reader](const std::string& name) {
        dcp::Uuid id;
        std::vector<uint8_t> data;
        if (name.rfind("urn:uuid:", 0) != 0 || !dcp::parseUuid(name.substr(9), id) || !reader.resource(id, data)) return QByteArray();
        return QByteArray(reinterpret_cast<const char*>(data.data()), qsizetype(data.size()));
    };
}

void timedTextDescriptor(QXmlStreamWriter& x, const dcp::ImfTimedTextWriter& w) {
    x.writeStartElement("r0:DCTimedTextDescriptor");
    x.writeNamespace(kGroups, "r0");
    x.writeNamespace(kElements, "r1");
    x.writeTextElement("r1:InstanceID", urn(w.descriptorId()));
    if (!w.resources().empty()) {
        // An Image document's pictures.
        x.writeStartElement("r1:SubDescriptors");
        for (size_t i = 0; i < w.resources().size(); ++i) {
            x.writeStartElement("r0:DCTimedTextResourceSubDescriptor");
            x.writeTextElement("r1:InstanceID", urn(w.resourceSets()[i]));
            x.writeTextElement("r1:AncillaryResourceID", urn(w.resources()[i].first));
            x.writeTextElement("r1:MIMEType", QString::fromStdString(w.resources()[i].second));
            x.writeTextElement("r1:EssenceStreamID", QString::number(10 + i));
            x.writeEndElement();
        }
        x.writeEndElement();  // SubDescriptors
    }
    x.writeTextElement("r1:LinkedTrackID", "1");
    x.writeTextElement("r1:SampleRate", QStringLiteral("%1/%2").arg(w.rate().num).arg(w.rate().den));
    x.writeTextElement("r1:EssenceLength", QString::number(w.duration()));
    x.writeTextElement("r1:ContainerFormat", ulUrn(std::string("060e2b340401010a0d01030102130101")));
    x.writeTextElement("r1:DataEssenceCoding", ulUrn(std::string(32, '0')));
    x.writeTextElement("r1:ResourceID", urn(w.resourceId()));
    x.writeTextElement("r1:UCSEncoding", "UTF-8");
    x.writeTextElement("r1:NamespaceURI", w.profile());
    x.writeTextElement("r1:RFC5646LanguageTagList", QString::fromStdString(w.language()));
    x.writeEndElement();
}

}  // namespace

bool exportImf(const Project& p, const Sequence& s, const ImfSettings& settings, const std::string& parent, ImfResult* result,
               const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!openJpegAvailable()) return fail("This build of Montage has no OpenJPEG, which IMF masters are coded with");
    if (!validKind(settings.kind)) return fail("Unknown content kind " + settings.kind);
    std::string err;
    // A supplemental package keeps the earlier composition's picture: its size, colour, depth and coding.
    const bool supplemental = !settings.supplementOf.empty();
    ImfComposition old;
    const ImfVirtualTrack* oldPicture = nullptr;
    std::map<std::string, std::string> oldPaths;
    if (supplemental) {
        const std::string cpl = imfCompositionIn(settings.supplementOf);
        if (cpl.empty()) return fail("There is no IMF package in " + settings.supplementOf);
        if (!readImfComposition(cpl, old, &err)) return fail(err);
        oldPicture = old.picture();
        if (!oldPicture || old.width <= 0 || old.height <= 0 || (old.bits != 10 && old.bits != 12) || !old.rsiz || old.colour.empty())
            return fail("The earlier package's picture is not one Montage makes supplementals of");
        oldPaths = imfTrackFiles(settings.supplementOf);
        // (its composition plays the earlier package's files from the folder beside it)
        const QString earlierParent = QFileInfo(QDir(QString::fromStdString(settings.supplementOf)).absolutePath()).absolutePath();
        if (QFileInfo(earlierParent).canonicalFilePath() != QFileInfo(QString::fromStdString(parent)).canonicalFilePath())
            return fail("A supplemental package goes beside the earlier one: make it in " + earlierParent.toStdString());
    }
    TrackFiles oldFiles(oldPaths);
    int W = 0, H = 0;
    if (supplemental) W = old.width, H = old.height;
    else if (!imfPictureSize(s, settings.size, W, H, &err)) return fail(err);
    Rational canonical;
    if (!imfFrameRateAllowed(s.fps, W, H, &canonical))
        return fail("IMF Application #2E takes 23.976, 24, 25, 29.97, 30, 50, 59.94 or 60 fps (120 above HD)");
    if (supplemental && (uint32_t(canonical.num) != old.rateNum || uint32_t(canonical.den) != old.rateDen))
        return fail("The earlier package runs at another frame rate");
    const std::string colourId = supplemental ? old.colour : settings.colour.empty() ? defaultImfColour(s) : settings.colour;
    const ImfColourSpec* spec = colourSpec(colourId);
    if (!spec) return fail("The colour is rec709, p3d65-pq, rec2020-pq or rec2020-hlg");
    const ColorSpace* target = findColorSpace(spec->space);
    if (!target) return fail("Missing colour space " + std::string(spec->space));
    const bool hdrOut = target->hdr();
    const int bits = supplemental ? old.bits : settings.bits ? settings.bits : (hdrOut ? 12 : 10);
    if (bits != 10 && bits != 12) return fail("IMF pictures here are 10 or 12 bits");
    const bool htj2k = supplemental ? old.rsiz == kHtj2kRsiz : settings.htj2k;
    const bool lossless = supplemental ? htj2k || (old.rsiz & 0xff00) >= 0x0700 : settings.lossless;
    if (!lossless && !supplemental && !(settings.megabitsPerSecond >= 20)) return fail("A lossy master needs at least 20 Mbit/s");
    if (htj2k && !lossless) return fail("HTJ2K masters here are lossless");
    if (htj2k && !htj2kAvailable()) return fail("This build of Montage has no HTJ2K encoder");
    const dcp::EditRate rate{uint32_t(canonical.num), uint32_t(canonical.den)};  // as written, not as the sequence stores it
    const double fps = rate.value();
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint >= s.inPoint) first = s.inPoint, end = s.outPoint + 1;
    if (end <= first) return fail("The sequence is empty");
    // The sound must be a whole number of samples at every frame boundary of the composition: at 29.97 and 59.94 that
    // takes a multiple of 5 frames, so the programme is padded with black and silence to one.
    auto padded = [&](int64_t n) {
        while ((n * 48000 * int64_t(rate.den)) % int64_t(rate.num) != 0) ++n;
        return n;
    };
    const int64_t programme = end - first;
    const int64_t frames = padded(programme);
    // (HTJ2K: the codestream declares no Part 1 profile, but is held to the size and rate one would take)
    const uint16_t profile = supplemental ? old.rsiz : imfRsiz(W, H, 3, fps, lossless, settings.megabitsPerSecond);
    if (!profile) return fail("No JPEG 2000 IMF profile takes that picture at that rate");
    const uint16_t rsiz = htj2k ? kHtj2kRsiz : profile;
    uint8_t coding[16];
    imfPictureCoding(rsiz, coding);
    if (std::all_of(std::begin(coding), std::end(coding), [](uint8_t b) { return b == 0; }))
        return fail("No JPEG 2000 IMF profile takes that picture at that bit rate");
    size_t maxBytes = lossless ? 0 : size_t(settings.megabitsPerSecond * 1e6 / 8 / fps);
    // A lossy supplemental's new pictures at the earlier ones' rate: no larger than its largest (within its level).
    if (supplemental && !lossless) {
        size_t most = 0;
        for (const ImfResource& r : oldPicture->resources)
            if (const dcp::ImfTrackReader* f = oldFiles.get(r.trackFile)) most = std::max(most, size_t(f->largestFrame()));
        if (!most) return fail("The earlier package's pictures are neither in it nor in the packages beside it");
        maxBytes = most;
    }
    // Immersive mixes fold their heights down; an ambisonic field is heard through two virtual cardioids (stereo).
    const int seqChannels = ambisonicLayout(s.audioLayout) ? 2 : layoutChannels(earLevelLayout(s.audioLayout));
    const int channels = seqChannels == 8 ? 8 : seqChannels == 6 ? 6 : 2;

    // The sound versions and the caption tracks, checked before anything is written.
    std::vector<ImfSound> sounds = settings.sounds;
    if (sounds.empty()) sounds.push_back({settings.language.empty() ? "en" : settings.language, "PRM", {}});
    for (size_t i = 0; i < sounds.size(); ++i) {
        const ImfSound& v = sounds[i];
        if (!validLanguage(v.language)) return fail("\"" + v.language + "\" is not a language tag (RFC 5646, such as en or de-DE)");
        if (v.kind != "PRM" && v.kind != "ME" && v.kind != "DV") return fail("A sound version is PRM, ME or DV, not " + v.kind);
        for (size_t j = 0; j < i; ++j)
            if (sounds[j].language == v.language && sounds[j].kind == v.kind) return fail("Two sound versions are " + v.kind + " in " + v.language);
        // (a role mistyped would leave the version silent but for the clips without one)
        const std::vector<std::string> roles = edit::sequenceRoles(s);
        for (const std::string& r : v.roles)
            if (std::find(roles.begin(), roles.end(), r) == roles.end()) return fail("No clip in the sequence has the role \"" + r + "\"");
    }
    struct TextPlan {
        const CaptionTrack* track = nullptr;
        std::string kind;
        bool image = false;
    };
    std::vector<TextPlan> textPlans;
    if (settings.captions) {
        if (settings.texts.empty()) {
            for (const CaptionTrack& t : s.captionTracks)
                if (t.name != kDescriptionTrackName && !t.captions.empty()) textPlans.push_back({&t, "subtitles"});
        } else {
            for (const ImfText& x : settings.texts) {
                const auto it = std::find_if(s.captionTracks.begin(), s.captionTracks.end(), [&](const CaptionTrack& t) { return t.id == x.track; });
                if (it == s.captionTracks.end()) return fail("The sequence has no caption track " + std::to_string(x.track));
                if (textSequence(x.kind).isEmpty()) return fail("A caption track goes in as subtitles, captions or forced, not " + x.kind);
                textPlans.push_back({&*it, x.kind, x.image});
            }
        }
        for (const TextPlan& t : textPlans)
            if (!validLanguage(t.track->language)) return fail("The caption track " + t.track->name + " has no language tag");
    }

    // A supplemental: which pictures the earlier composition already has. Its frame m showing the same as frame k of
    // this one where the old cut (or, with none, this one) shows it; each stretch reused is checked against the old
    // frames, and coded afresh if they differ.
    std::vector<int64_t> same(size_t(frames), -1);
    const ColorSpace& seqSpace = sequenceColorSpace(s);
    const double peakNits = std::clamp(s.hdrPeakNits, 100.0, 10000.0);
    const double scale = std::min(double(W) / s.width, double(H) / s.height);
    const int fw = std::min(W, int(std::lround(s.width * scale / 2)) * 2), fh = std::min(H, int(std::lround(s.height * scale / 2)) * 2);
    const int ox = (W - fw) / 2, oy = (H - fh) / 2;
    RenderOptions o;
    o.scale = scale;
    o.highQuality = true;
    const double maxCode = double((1 << bits) - 1);
    // The picture of composition frame k: fitted inside the frame on black (black after the programme).
    auto pictureAt = [&](int64_t k) {
        std::vector<uint16_t> rgb(size_t(W) * size_t(H) * 3, 0);
        Image frame;
        if (k < programme) {
            frame = renderProgramFrame(p, s, first + k, o);
            if (frame.width != fw || frame.height != fh) frame = resizeImage(frame, fw, fh);
            if (target != &seqSpace) convertColor(frame, seqSpace, *target, peakNits);
        }
        for (int y = 0; y < fh && !frame.empty(); ++y) {
            const float* src = frame.row(y);
            uint16_t* d = rgb.data() + (size_t(y + oy) * size_t(W) + size_t(ox)) * 3;
            for (int x = 0; x < fw; ++x, src += 4, d += 3)
                for (int c = 0; c < 3; ++c) d[c] = uint16_t(std::lround(std::clamp(double(src[c]), 0.0, 1.0) * maxCode));
        }
        return rgb;
    };
    if (supplemental) {
        const Sequence* before = settings.originalSequence ? p.findSequence(settings.originalSequence) : &s;
        if (!before) return fail("The project has no sequence " + std::to_string(settings.originalSequence));
        // Where the earlier composition started in that sequence: its start, or its In (as it was exported).
        const int64_t oldFrames = oldPicture->length();
        FrameTime oldFirst = -1, oldEnd = 0;
        if (padded(before->duration()) == oldFrames) oldFirst = 0, oldEnd = before->duration();
        else if (before->inPoint >= 0 && before->outPoint >= before->inPoint && padded(before->outPoint + 1 - before->inPoint) == oldFrames)
            oldFirst = before->inPoint, oldEnd = before->outPoint + 1;
        if (oldFirst < 0)
            return fail("The earlier package is " + std::to_string(oldFrames) + " frames long and " + before->name + " is " +
                        std::to_string(before->duration()) + ": choose the sequence it was mastered from");
        std::vector<FrameTime> was;
        if (*before == s) {
            for (FrameTime k = 0; k < programme; ++k) was.push_back(first + k);
        } else {
            was = samePictureFrames(p, *before, s, first, programme);
        }
        for (int64_t k = 0; k < programme; ++k)
            if (was[size_t(k)] >= oldFirst && was[size_t(k)] < oldEnd) same[size_t(k)] = was[size_t(k)] - oldFirst;
        // The black after the programme (at 29.97 and 59.94): the earlier package's own, where it has as much.
        for (int64_t k = programme; k < frames; ++k)
            if (oldEnd - oldFirst + (k - programme) < oldFrames) same[size_t(k)] = oldEnd - oldFirst + (k - programme);
        // Every frame taken from the earlier package is checked against it: what the sequence says is the same can still
        // differ (a relinked or re-rendered shot, Interpret Footage, a colour override, a file changed on disk). A stretch
        // taken in one piece, up to an edit, is kept or made again whole (frame by frame, a lossy check could keep the
        // faint start of a change and make its end again). Rendered here, the earlier frames decoded and compared on other threads.
        std::vector<int64_t> stretchOf(size_t(frames), -1);
        int64_t stretches = 0, toCheck = 0, checked = 0;
        for (int64_t k = 0; k < frames; ++k) {
            if (same[size_t(k)] < 0) continue;
            ++toCheck;
            const bool runsOn = k > 0 && same[size_t(k - 1)] >= 0 && same[size_t(k)] == same[size_t(k - 1)] + 1 &&
                                (k >= programme || !editBetween(s, first + k));
            stretchOf[size_t(k)] = runsOn ? stretchOf[size_t(k - 1)] : stretches++;
        }
        std::vector<char> differs(size_t(stretches), 0);
        // Whether two pictures are the same, within the coding's noise.
        auto alikeTo = [W, H, lossless, maxCode](const std::vector<uint16_t>& mine, const std::vector<uint16_t>& theirs) {
            if (theirs.size() != mine.size()) return false;
            if (lossless) {
                for (size_t i = 0; i < mine.size(); ++i)
                    if (std::abs(int(mine[i]) - int(theirs[i])) > 1) return false;
                return true;
            }
            // Lossy: coding noise is small everywhere and averages out; a change (a grade, a word of a title) moves the
            // average or stands out in a block of the picture.
            double mean[3] = {0, 0, 0}, worst = 0;
            const int B = 32;
            for (int by = 0; by < H; by += B)
                for (int bx = 0; bx < W; bx += B) {
                    double sum = 0;
                    size_t n = 0;
                    for (int y = by; y < std::min(H, by + B); ++y)
                        for (int x = bx; x < std::min(W, bx + B); ++x)
                            for (int c = 0; c < 3; ++c, ++n) {
                                const size_t i = (size_t(y) * size_t(W) + size_t(x)) * 3 + size_t(c);
                                const double d = double(int(mine[i]) - int(theirs[i]));
                                sum += d * d;
                                mean[c] += d;
                            }
                    worst = std::max(worst, sum / double(std::max<size_t>(1, n)));
                }
            const double pixels = double(W) * double(H);
            for (double m : mean)
                if (std::fabs(m / pixels) > maxCode / 1000) return false;
            return worst <= 0 || 10 * std::log10(maxCode * maxCode / worst) >= 30;
        };
        // (each check holds a rendered and a decoded frame, 12 bytes a pixel, and the decoder's own: within about 2 GiB)
        const int memoryCap = std::max(1, int(2147483648.0 / (double(W) * double(H) * 40.0)));
        const size_t inHand = size_t(std::min(memoryCap, settings.threads > 0 ? settings.threads : std::max(1, int(std::thread::hardware_concurrency()))));
        std::deque<std::pair<int64_t, std::future<bool>>> checks;
        bool stopped = false;
        auto collect = [&](size_t keep) {
            while (checks.size() > keep) {
                const int64_t k = checks.front().first;
                if (!checks.front().second.get()) differs[size_t(stretchOf[size_t(k)])] = 1;
                checks.pop_front();
                stopped = stopped || (progress && !progress(0.05 * double(++checked) / double(std::max<int64_t>(1, toCheck))));
            }
        };
        for (int64_t k = 0; k < frames && !stopped; ++k) {
            if (same[size_t(k)] < 0) continue;
            if (differs[size_t(stretchOf[size_t(k)])]) {  // (already to be made again)
                stopped = progress && !progress(0.05 * double(++checked) / double(std::max<int64_t>(1, toCheck)));
                continue;
            }
            std::vector<uint16_t> mine = pictureAt(k);
            // (the earlier frame's codestream read here: the files are not shared between threads)
            std::vector<uint8_t> cs;
            const std::vector<ImfResource> r = slice(*oldPicture, same[size_t(k)], 1);
            const dcp::ImfTrackReader* f = r.size() == 1 ? oldFiles.get(r[0].trackFile) : nullptr;
            if (!f || !f->frame(r[0].entry, cs)) cs.clear();
            checks.emplace_back(k, std::async(std::launch::async, [&alikeTo, W, H, mine = std::move(mine), cs = std::move(cs)] {
                try {
                    std::vector<uint16_t> theirs;
                    int w = 0, h = 0, components = 0, bits = 0;
                    return !cs.empty() && decodeJpeg2000(cs.data(), cs.size(), theirs, w, h, components, bits) && components == 3 && w == W &&
                           h == H && alikeTo(mine, theirs);
                } catch (...) {
                    return false;  // (out of memory: made again)
                }
            }));
            collect(inHand);
        }
        collect(0);
        if (stopped) return fail("Stopped");
        for (int64_t k = 0; k < frames; ++k)
            if (same[size_t(k)] >= 0 && differs[size_t(stretchOf[size_t(k)])]) same[size_t(k)] = -1;
    }

    // The folder.
    const QString title = QString::fromStdString(settings.title.empty() ? "Untitled" : settings.title);
    // (a second supplemental of the day gets a number: a chain can grow by several)
    const QString stem = safeTitle(title.toStdString()) + (supplemental ? "_IMF_SUPP_" : "_IMF_") + QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
    QString name = stem;
    if (!supplemental && QFileInfo::exists(QDir(QString::fromStdString(parent)).filePath(name)))
        return fail("There is already a folder called " + name.toStdString() + " there");
    for (int n = 2; QFileInfo::exists(QDir(QString::fromStdString(parent)).filePath(name)); ++n) {
        if (n > 99) return fail("There are already 99 folders called " + stem.toStdString() + " there");
        name = stem + QStringLiteral("_%1").arg(n);
    }
    const QString folder = QDir(QString::fromStdString(parent)).filePath(name);
    if (!QDir().mkpath(folder)) return fail("Cannot make the folder " + folder.toStdString());
    bool finished = false;
    struct Cleanup {
        const QString& folder;
        bool& finished;
        ~Cleanup() {
            if (!finished) QDir(folder).removeRecursively();
        }
    } cleanup{folder, finished};
    // The track files this package holds, with the virtual tracks' stretches.
    struct Own {
        QString file, kind, hash;
        dcp::Uuid id{};
        qint64 size = 0;
        QString descriptor;  // its RegXML
        dcp::Uuid descriptorId = dcp::newUuid();
    };
    std::vector<Own> own;
    auto ownFile = [&](const QString& prefix, const QString& kind) {
        Own f;
        f.id = dcp::newUuid();
        f.kind = kind;
        f.file = QStringLiteral("%1_%2.mxf").arg(prefix, QString::fromStdString(dcp::uuidString(f.id)));
        own.push_back(f);
        return own.size() - 1;
    };
    struct Virtual {
        QString kind, trackId;
        QString editRate;
        const ImfVirtualTrack* old = nullptr;  // the earlier composition's, which its old stretches play
        int ownFile = -1;
        int64_t ownDuration = 0;
        std::vector<Run> runs;
    };
    std::vector<Virtual> virtuals;
    auto trackIdFor = [&](const ImfVirtualTrack* t) { return t ? QStringLiteral("urn:uuid:") + QString::fromStdString(t->id) : urn(dcp::newUuid()); };
    const QString editRate = QStringLiteral("%1 %2").arg(rate.num).arg(rate.den);

    // Sound: each version's mix at 48 kHz from the first frame's time, as many samples as the frames last (a
    // supplemental's only where it differs from the earlier package's version in the same language and kind).
    const int64_t firstSample = int64_t(std::llround(double(first) * 48000.0 * rate.den / rate.num));
    const int64_t total = sampleAt(frames, rate);
    const int64_t heard = int64_t(std::llround(double(programme) * 48000.0 * rate.den / rate.num));  // then silence
    int64_t newSamples = 0;
    for (size_t vi = 0; vi < sounds.size(); ++vi) {
        const ImfSound& v = sounds[vi];
        Virtual vt;
        vt.kind = QStringLiteral("MainAudioSequence");
        vt.editRate = QStringLiteral("48000 1");
        for (const ImfVirtualTrack& t : old.tracks)
            if (t.kind == "MainAudioSequence" && t.language == v.language && (t.contentKind.empty() ? "PRM" : t.contentKind) == v.kind &&
                t.channels == channels && !t.resources.empty() && t.resources.front().rateNum == 48000 && t.resources.front().rateDen == 1)
                vt.old = &t;
        vt.trackId = trackIdFor(vt.old);
        std::optional<ComposedPcm> before;
        if (vt.old) before.emplace(*vt.old, oldFiles);
        dcp::ImfSoundWriter writer;
        size_t fileIndex = 0;
        bool opened = false;
        Sequence mixSeq = soundVersion(s, v);
        mixSeq.audioLayout = earLevelLayout(s.audioLayout);
        mixSeq.sampleRate = 48000;
        const int chunk = 4800;
        const int in = std::max(2, seqChannels);
        std::vector<float> mix(size_t(chunk) * size_t(in));
        // Stereo as it is; 5.1 as it is; 7.1 (L R C LFE Lb Rb Ls Rs, as FFmpeg orders it) to 7.1 DS (L R C LFE Lss Rss Lrs Rrs).
        std::vector<int> route;
        if (channels == 8) route = {0, 1, 2, 3, 6, 7, 4, 5};
        else if (channels == 6) route = {0, 1, 2, 3, 4, 5};
        else route = {0, 1};
        AudioMixer mixer;
        std::vector<int32_t> pending;  // mixed samples not yet decided on, from sample `pendingAt`
        int64_t pendingAt = 0, block = 0, ownSamples = 0;
        int64_t lastOffset = INT64_MIN;
        // Frame by frame: the earlier version's samples where they are the same (at the offset the picture moved by,
        // or the one before), else this package's own.
        auto decide = [&]() -> bool {
            while (block < frames && sampleAt(block + 1, rate) <= pendingAt + int64_t(pending.size()) / channels) {
                const int64_t at = sampleAt(block, rate), n = sampleAt(block + 1, rate) - at;
                const int32_t* mine = pending.data() + (at - pendingAt) * channels;
                bool reused = false;
                if (before) {
                    int64_t offsets[2] = {lastOffset, same[size_t(block)] >= 0 ? sampleAt(same[size_t(block)], rate) - at : INT64_MIN};
                    for (int64_t d : offsets) {
                        if (d == INT64_MIN) continue;
                        const int32_t* theirs = before->read(at + d, n);
                        if (theirs && std::equal(mine, mine + n * channels, theirs)) {
                            addRun(vt.runs, true, at + d, n);
                            lastOffset = d;
                            reused = true;
                            break;
                        }
                    }
                }
                if (!reused) {
                    if (!opened) {
                        fileIndex = ownFile("AUDIO", vt.kind);
                        if (!writer.open(QDir(folder).filePath(own[fileIndex].file).toStdString(), own[fileIndex].id, channels, v.language,
                                         title.toStdString(), "1", &err, v.kind))
                            return false;
                        opened = true;
                    }
                    if (!writer.writePcm(mine, size_t(n), &err)) return false;
                    addRun(vt.runs, false, ownSamples, n);
                    ownSamples += n;
                }
                ++block;
            }
            const int64_t done = sampleAt(block, rate) - pendingAt;
            pending.erase(pending.begin(), pending.begin() + ptrdiff_t(done * channels));
            pendingAt += done;
            return true;
        };
        for (int64_t done = 0; done < total; done += chunk) {
            const int n = int(std::min<int64_t>(chunk, total - done));
            if (seqChannels > 2) mixer.mixLayout(p, mixSeq, firstSample + done, n, mix.data());
            else mixer.mix(p, mixSeq, firstSample + done, n, mix.data());
            const size_t base = pending.size();
            pending.resize(base + size_t(n) * size_t(channels));
            for (int i = 0; i < n; ++i)
                for (size_t c = 0; c < route.size(); ++c) {
                    const float x = done + i < heard ? mix[size_t(i) * size_t(in) + c] : 0.0f;
                    pending[base + size_t(i) * size_t(channels) + size_t(route[c])] =
                        int32_t(std::lround(std::clamp(double(x), -1.0, 1.0) * 8388607.0));
                }
            if (!decide()) return fail(err);
            if (progress && (done / chunk) % 20 == 0 &&
                !progress(0.05 + 0.05 * (double(vi) + double(done) / double(total)) / double(sounds.size())))
                return fail("Stopped");
        }
        if (opened) {
            if (!writer.close(&err)) return fail(err);
            own[fileIndex].descriptor = descriptorXml([&](QXmlStreamWriter& x) { soundDescriptor(x, writer); });
            vt.ownFile = int(fileIndex);
            vt.ownDuration = writer.duration();
            newSamples += writer.duration();
        }
        virtuals.push_back(std::move(vt));
    }

    // Timed text: each caption track's IMSC 1.1 document (times from the composition's start), unless the earlier
    // package has the same document in the same language and kind.
    std::set<std::string> textTracksUsed;
    for (const TextPlan& plan : textPlans) {
        Virtual vt;
        vt.kind = textSequence(plan.kind);
        vt.editRate = editRate;
        std::vector<Caption> captions;
        for (Caption c : plan.track->captions) {
            c.start = std::max<FrameTime>(c.start - first, 0);
            c.end = std::min<FrameTime>(c.end - first, programme);
            if (c.end > c.start) captions.push_back(c);
        }
        // Pictures (IMSC 1.1 Image): each caption drawn on the master's frame, a PNG in the track file named by its id.
        std::vector<dcp::TimedTextResource> images;
        std::string xml;
        if (plan.image) {
            CaptionTrack drawn = *plan.track;
            drawn.captions = captions;
            const std::vector<CaptionPicture> pictures = captionPictures(drawn, W, H);
            std::vector<std::string> names;
            for (const CaptionPicture& pic : pictures) {
                dcp::TimedTextResource r;
                r.mime = "image/png";
                QByteArray png;
                QBuffer buf(&png);
                buf.open(QIODevice::WriteOnly);
                if (!pic.image.save(&buf, "PNG")) return fail("Cannot make a subtitle picture");
                r.data.assign(png.begin(), png.end());
                // Named by what it shows (a name-based UUID), so the same pictures make the same document again.
                const QByteArray hash = QCryptographicHash::hash(png, QCryptographicHash::Md5);
                std::copy(hash.begin(), hash.begin() + 16, r.id.begin());
                r.id[6] = uint8_t((r.id[6] & 0x0f) | 0x30);
                r.id[8] = uint8_t((r.id[8] & 0x3f) | 0x80);
                names.push_back("urn:uuid:" + dcp::uuidString(r.id));
                images.push_back(std::move(r));
            }
            xml = imscImageDocument(pictures, names, s.fps, W, H, plan.track->language);
        } else {
            xml = captionsToTtml(captions, s.fps, plan.track->language, plan.track->style);
        }
        // The earlier version of the same kind and language: its virtual track carried on (a corrected one too), its
        // file played again when the document is the same.
        const ImfVirtualTrack* was = nullptr;
        for (const ImfVirtualTrack& t : old.tracks)
            if (!was && QString::fromStdString(t.kind) == vt.kind && t.language == plan.track->language && !textTracksUsed.count(t.id)) was = &t;
        if (was) {
            textTracksUsed.insert(was->id);
            const std::vector<ImfResource> all = was->length() == frames ? slice(*was, 0, frames) : std::vector<ImfResource>();
            const dcp::ImfTrackReader* f = all.size() == 1 ? oldFiles.get(all[0].trackFile) : nullptr;
            std::string theirs;
            if (f && f->text(theirs) && theirs == xml) vt.old = was;
        }
        vt.trackId = trackIdFor(was);
        if (vt.old) {
            addRun(vt.runs, true, 0, frames);
        } else {
            const size_t fileIndex = ownFile("TEXT", vt.kind);
            dcp::ImfTimedTextWriter writer;
            if (!writer.open(QDir(folder).filePath(own[fileIndex].file).toStdString(), own[fileIndex].id, rate, &err) ||
                !writer.write(xml, frames, plan.track->language, &err, images) || !writer.close(&err))
                return fail(err);
            own[fileIndex].descriptor = descriptorXml([&](QXmlStreamWriter& x) { timedTextDescriptor(x, writer); });
            vt.ownFile = int(fileIndex);
            vt.ownDuration = frames;
            addRun(vt.runs, false, 0, frames);
        }
        virtuals.push_back(std::move(vt));
    }

    // The picture: the frames to code (all, or a supplemental's changed ones), several encoding at once.
    dcp::ImfColour colour;
    colour.primaries = spec->primaries;
    colour.transfer = spec->transfer;
    colour.codingEquations = spec->codingEquations;
    colour.hdr = spec->hdr;
    if (spec->hdr) {
        // A P3-D65 mastering display (as HDR grading monitors are), at the master's peak.
        const double p3[8] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
        std::copy(std::begin(p3), std::end(p3), colour.display);
        const double peak = supplemental && old.masteringPeak > 0 ? old.masteringPeak
                            : settings.masteringPeak > 0          ? settings.masteringPeak
                                                                  : std::max(1000.0, s.hdrPeakNits);
        colour.maxLuminance = std::clamp(peak, 100.0, 10000.0);
        colour.minLuminance = 0.0001;
    }
    std::vector<int64_t> toCode;
    {
        Virtual vt;
        vt.kind = QStringLiteral("MainImageSequence");
        vt.editRate = editRate;
        vt.old = oldPicture;
        vt.trackId = trackIdFor(vt.old);
        for (int64_t k = 0; k < frames; ++k) {
            if (same[size_t(k)] >= 0) {
                addRun(vt.runs, true, same[size_t(k)], 1);
            } else {
                addRun(vt.runs, false, int64_t(toCode.size()), 1);
                toCode.push_back(k);
            }
        }
        virtuals.insert(virtuals.begin(), std::move(vt));
    }
    dcp::ImfPictureWriter picture;
    if (!toCode.empty()) {
        const size_t fileIndex = ownFile("VIDEO", "MainImageSequence");
        // Encoders at once, within about 4 GiB for the frames waiting and being coded (an encode holds roughly 30 bytes
        // a pixel, a waiting frame 6, two waiting per encoder): a dozen at UHD, more at HD.
        const double perEncoder = double(W) * double(H) * (30.0 + 2 * 6.0);
        const int memoryCap = std::max(1, int(4294967296.0 / perEncoder));
        const int threads = std::min(memoryCap, settings.threads > 0 ? settings.threads : std::max(1, int(std::thread::hardware_concurrency())));
        const int g = std::gcd(W, H);
        if (!picture.open(QDir(folder).filePath(own[fileIndex].file).toStdString(), own[fileIndex].id, rate, bits, colour, uint32_t(W / g),
                          uint32_t(H / g), &err))
            return fail(err);
        ImfEncoderPool pool(W, H, bits, rsiz, maxBytes, threads);
        const int64_t count = int64_t(toCode.size());
        int64_t written = 0;
        std::vector<uint8_t> cs;
        auto writeReady = [&](bool wait) {
            while (written < count && pool.take(written, cs, wait)) {
                if (written == 0) {
                    dcp::J2kHeader h;
                    if (!dcp::parseJ2kHeader(cs.data(), cs.size(), h) || h.rsiz != rsiz) {
                        err = rsiz == kHtj2kRsiz ? "OpenJPH could not code the picture as HTJ2K"
                                                 : "OpenJPEG could not code the picture in the JPEG 2000 IMF profile";
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
        auto step = [&] { return 0.1 + 0.85 * double(written) / double(count); };
        for (int64_t j = 0; j < count; ++j) {
            std::vector<uint16_t> rgb = pictureAt(toCode[size_t(j)]);
            while (pool.pending() >= inHand && written < j)
                if (!writeReady(true)) return fail(err.empty() ? pool.error() : err);
            pool.push(j, std::move(rgb));
            if (!writeReady(false)) return fail(err.empty() ? pool.error() : err);
            if (progress && !progress(step())) return fail("Stopped");
        }
        while (written < count) {
            if (!writeReady(true)) return fail(err.empty() ? pool.error() : err);
            if (progress && !progress(step())) return fail("Stopped");
        }
        pool.finish();
        if (!picture.close(&err)) return fail(err);
        own[fileIndex].descriptor = descriptorXml([&](QXmlStreamWriter& x) { pictureDescriptor(x, picture, count); });
        virtuals.front().ownFile = int(fileIndex);
        virtuals.front().ownDuration = count;
    }

    // Hashes, then the composition, packing list and asset map.
    qint64 toHash = 1;
    for (const Own& f : own) toHash += QFileInfo(QDir(folder).filePath(f.file)).size();
    qint64 hashed = 0;
    bool stopped = false;
    auto hashing = [&](qint64 n) {
        hashed += n;
        stopped = progress && !progress(0.95 + 0.05 * double(hashed) / double(toHash));
        return !stopped;
    };
    for (Own& f : own) {
        f.hash = packageFileHash(QDir(folder).filePath(f.file), &f.size, hashing);
        if (f.hash.isEmpty()) return fail(stopped ? "Stopped" : "Cannot read the track files back");
    }
    const QString issued = packageTimestamp();
    const QString issuer = QString::fromStdString(settings.issuer.empty() ? "Montage" : settings.issuer);
    const QString lang = QString::fromStdString(sounds.front().language);
    const dcp::Uuid cplId = dcp::newUuid(), pklId = dcp::newUuid(), amId = dcp::newUuid(), versionId = dcp::newUuid(), segmentId = dcp::newUuid();
    const QString cplFile = QStringLiteral("CPL_%1.xml").arg(QString::fromStdString(dcp::uuidString(cplId)));
    const QString pklFile = QStringLiteral("PKL_%1.xml").arg(QString::fromStdString(dcp::uuidString(pklId)));
    // Every resource of each virtual track, and the descriptors they name (the earlier composition's copied as they are).
    struct Res {
        QString trackFile, encoding, hash, editRate;
        int64_t intrinsic = 0, entry = 0, duration = 0;
    };
    std::vector<std::vector<Res>> resources;
    std::vector<std::pair<QString, QString>> descriptors;  // id, RegXML
    std::set<QString> listed;
    for (const Own& f : own) {
        descriptors.push_back({urn(f.descriptorId), f.descriptor});
        listed.insert(urn(f.descriptorId));
    }
    for (const Virtual& vt : virtuals) {
        std::vector<Res> list;
        for (const Run& r : vt.runs) {
            if (!r.old) {
                const Own& f = own[size_t(vt.ownFile)];
                list.push_back({urn(f.id), urn(f.descriptorId), f.hash, vt.editRate, vt.ownDuration, r.at, r.length});
                continue;
            }
            for (const ImfResource& x : slice(*vt.old, r.at, r.length)) {
                const QString enc = QStringLiteral("urn:uuid:") + QString::fromStdString(x.encoding);
                if (!listed.count(enc)) {
                    const auto d = old.descriptors.find(x.encoding);
                    if (d == old.descriptors.end()) return fail("The earlier composition has no essence descriptor " + x.encoding);
                    descriptors.push_back({enc, QString::fromStdString(d->second)});
                    listed.insert(enc);
                }
                list.push_back({QStringLiteral("urn:uuid:") + QString::fromStdString(x.trackFile), enc, QString::fromStdString(x.hash),
                                QStringLiteral("%1 %2").arg(x.rateNum).arg(x.rateDen), x.intrinsic, x.entry, x.duration});
            }
        }
        resources.push_back(std::move(list));
    }
    // (the descriptors' places, by a mark no copied descriptor can hold)
    const QString marker = QString::fromStdString(dcp::uuidString(dcp::newUuid()));
    auto writeXml = [&](const QString& file, const std::function<void(QXmlStreamWriter&)>& body) {
        QByteArray bytes;
        QXmlStreamWriter x(&bytes);
        x.setAutoFormatting(true);
        x.setAutoFormattingIndent(2);
        x.writeStartDocument(QStringLiteral("1.0"), true);
        body(x);
        x.writeEndDocument();
        if (x.hasError()) return false;
        // The descriptors, written in place of their markers.
        for (size_t i = 0; i < descriptors.size(); ++i)
            bytes.replace(QStringLiteral("<!--montage-descriptor-%1-%2-->").arg(marker).arg(i).toUtf8(), descriptors[i].second.toUtf8());
        QFile f(QDir(folder).filePath(file));
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size() && f.flush();
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
        for (size_t i = 0; i < descriptors.size(); ++i) {
            x.writeStartElement("EssenceDescriptor");
            x.writeTextElement("Id", descriptors[i].first);
            x.writeComment(QStringLiteral("montage-descriptor-%1-%2").arg(marker).arg(i));
            x.writeEndElement();
        }
        x.writeEndElement();
        x.writeTextElement("EditRate", editRate);
        x.writeTextElement("TotalRunningTime", running);
        x.writeStartElement("ExtensionProperties");
        // (HTJ2K pictures came with the 2023 edition)
        x.writeTextElement("cc:ApplicationIdentification", htj2k ? "http://www.smpte-ra.org/ns/2067-21/2023" : "http://www.smpte-ra.org/ns/2067-21/2021");
        x.writeEndElement();
        x.writeStartElement("SegmentList");
        x.writeStartElement("Segment");
        x.writeTextElement("Id", urn(segmentId));
        x.writeStartElement("SequenceList");
        for (size_t i = 0; i < virtuals.size(); ++i) {
            x.writeStartElement("cc:" + virtuals[i].kind);
            x.writeTextElement("Id", urn(dcp::newUuid()));
            x.writeTextElement("TrackId", virtuals[i].trackId);
            x.writeStartElement("ResourceList");
            for (const Res& r : resources[i]) {
                x.writeStartElement("Resource");
                x.writeAttribute("xsi:type", "TrackFileResourceType");
                x.writeTextElement("Id", urn(dcp::newUuid()));
                x.writeTextElement("EditRate", r.editRate);
                x.writeTextElement("IntrinsicDuration", QString::number(r.intrinsic));
                x.writeTextElement("EntryPoint", QString::number(r.entry));
                x.writeTextElement("SourceDuration", QString::number(r.duration));
                x.writeTextElement("SourceEncoding", r.encoding);
                x.writeTextElement("TrackFileId", r.trackFile);
                if (!r.hash.isEmpty()) {
                    x.writeTextElement("Hash", r.hash);
                    x.writeEmptyElement("HashAlgorithm");
                    x.writeAttribute("Algorithm", "http://www.w3.org/2000/09/xmldsig#sha1");
                }
                x.writeEndElement();
            }
            x.writeEndElement();  // ResourceList
            x.writeEndElement();
        }
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
        for (const Own& f : own) asset(f.id, f.hash, f.size, "application/mxf", f.file);
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
        for (const Own& f : own) asset(f.id, false, f.file, f.size);
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
        result->rsiz = rsiz;
        result->colour = colourId;
        result->sounds = int(sounds.size());
        result->texts = int(textPlans.size());
        result->supplemental = supplemental;
        result->codedFrames = int64_t(toCode.size());
        result->newSamples = newSamples;
        result->trackFiles.clear();
        for (const Own& f : own) result->trackFiles.push_back(QDir(folder).filePath(f.file).toStdString());
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
    // Track files a supplemental plays from the packages beside it (checked there; here only that they are what the
    // composition says).
    const std::map<std::string, std::string> beside = imfTrackFiles(folderPath);
    int compositions = 0;
    for (const QString& cplId : cpls) {
        const Asset& c = assets[cplId];
        const auto cplItems = packageXmlItems(dir.filePath(c.path), &root);
        if (root != "CompositionPlaylist") continue;
        ++compositions;
        bool application = false;
        for (const auto& [path, text] : cplItems)
            if (path.endsWith("ExtensionProperties/ApplicationIdentification") && text.startsWith("http://www.smpte-ra.org/ns/2067-21/"))
                application = true;
        if (!application) issue(c.path + " does not identify IMF Application #2E");
        ImfComposition comp;
        std::string err;
        if (!readImfComposition(dir.filePath(c.path).toStdString(), comp, &err)) {
            issue(QString::fromStdString(err));
            continue;
        }
        const ImfVirtualTrack* picture = comp.picture();
        if (!picture) issue(c.path + " has no picture");
        std::set<std::string> seen;
        for (const ImfVirtualTrack& t : comp.tracks) {
            const QString kind = QString::fromStdString(t.kind);
            const bool image = t.kind == "MainImageSequence", sound = t.kind == "MainAudioSequence";
            const bool text = isTextSequence(kind);
            if (!image && !sound && !text) continue;  // (markers, and kinds other tools write that Montage does not read)
            // Every virtual track as long as the picture.
            if (picture && !t.resources.empty()) {
                const ImfResource& r0 = t.resources.front();
                const int64_t want = picture->length() * int64_t(r0.rateNum) * int64_t(comp.rateDen) / (int64_t(r0.rateDen) * int64_t(comp.rateNum));
                if (t.length() != want) issue(QStringLiteral("A %1 lasts %2 edit units; the picture lasts %3").arg(kind).arg(t.length()).arg(want));
            }
            for (const ImfResource& r : t.resources) {
                const QString id = QString::fromStdString(r.trackFile);
                if (r.encoding.empty() || !comp.descriptors.count(r.encoding))
                    issue(QStringLiteral("%1 %2 has no essence descriptor in %3").arg(kind, id, c.path));
                if (r.intrinsic >= 0 && r.entry + r.duration > r.intrinsic) issue(QStringLiteral("%1 %2 plays past its end").arg(kind, id));
                if (!seen.insert(r.trackFile).second) continue;
                const bool inside = assets.count(id) && !assets[id].hash.isEmpty();
                const auto out = beside.find(r.trackFile);
                if (!inside && out == beside.end()) {
                    issue(QStringLiteral("%1 %2 in %3 is neither in the package nor in the packages beside it").arg(kind, id, c.path));
                    continue;
                }
                const QString name = inside ? assets[id].path : QFileInfo(QString::fromStdString(out->second)).fileName();
                const std::string file = inside ? dir.filePath(assets[id].path).toStdString() : out->second;
                // Another package's file: the one the composition names, by its hash.
                if (!inside && !r.hash.empty()) {
                    const QString hash = packageFileHash(QString::fromStdString(file), nullptr, reading);
                    if (stopped) {
                        issue("Stopped");
                        return issues;
                    }
                    if (hash != QString::fromStdString(r.hash)) {
                        issue(QStringLiteral("%1 (beside the package) does not match its hash in %2").arg(QString::fromStdString(file), c.path));
                        continue;
                    }
                }
                if (text || !inside) {
                    // Read as Montage reads track files (those of other packages without reading all their essence).
                    dcp::ImfTrackReader reader;
                    if (!reader.open(file, &err)) {
                        issue(QString::fromStdString(err));
                        continue;
                    }
                    using K = dcp::ImfTrackReader::Kind;
                    const K want = image ? K::Picture : sound ? K::Sound : K::TimedText;
                    const int64_t units = image ? reader.frames() : sound ? reader.samples() : r.intrinsic;
                    std::string xml;
                    std::vector<Caption> captions;
                    if (reader.kind() != want) issue(QStringLiteral("%1 is not the %2 its %3 plays").arg(name, image ? "picture" : sound ? "sound" : "timed text", kind));
                    else if (r.intrinsic >= 0 && units != r.intrinsic)
                        issue(QStringLiteral("%1 has %2 edit units; the composition says %3").arg(name).arg(units).arg(r.intrinsic));
                    else if (text && !reader.text(xml, &err))
                        issue(QStringLiteral("%1 is not timed text Montage reads: %2").arg(name, QString::fromStdString(err)));
                    else if (text && isImscImage(xml)) {
                        // Pictures: the document as the Image profile has it, each image a PNG in the file.
                        for (const std::string& problem : checkImscImage(xml, imscResource(reader), comp.width, comp.height))
                            issue(QStringLiteral("%1: %2").arg(name, QString::fromStdString(problem)));
                        for (const dcp::ImfTrackReader::Resource& res : reader.resources())
                            if (!res.present) issue(QStringLiteral("%1 lists resource %2 but does not carry it").arg(name, QString::fromStdString(dcp::uuidString(res.id))));
                    } else if (text && !parseTtml(xml, Rational{int(comp.rateNum), int(comp.rateDen)}, captions, &err))
                        issue(QStringLiteral("%1 is not timed text Montage reads: %2").arg(name, QString::fromStdString(err)));
                    continue;
                }
                const ImfTrack tr = probeImfTrack(file, reading);
                if (stopped) {
                    issue("Stopped");
                    return issues;
                }
                if (image) {
                    dcp::J2kHeader h;
                    if (!tr.ok || tr.codec != "jpeg2000") issue(QStringLiteral("%1 is not JPEG 2000 pictures").arg(name));
                    else if (!dcp::parseJ2kHeader(tr.firstPacket.data(), tr.firstPacket.size(), h) ||
                             (h.rsiz == kHtj2kRsiz ? !(h.pcap & 0x00020000)  // (HTJ2K: its CAP marker names Part 15)
                                                   : (h.rsiz & 0xff00) < 0x0400 || (h.rsiz & 0xff00) > 0x0900))
                        issue(QStringLiteral("%1 is not in a JPEG 2000 IMF profile").arg(name));
                    else if (r.intrinsic >= 0 && tr.packets != r.intrinsic)
                        issue(QStringLiteral("%1 has %2 frames; the composition says %3").arg(name).arg(tr.packets).arg(r.intrinsic));
                } else if (sound) {
                    if (!tr.ok || tr.codec.rfind("pcm_s24", 0) != 0 || (tr.sampleRate != 48000 && tr.sampleRate != 96000))
                        issue(QStringLiteral("%1 is not 24-bit 48 or 96 kHz sound").arg(name));
                    else if (r.intrinsic >= 0 && tr.channels > 0 && tr.bytes / (3 * tr.channels) != r.intrinsic)
                        issue(QStringLiteral("%1 has %2 samples; the composition says %3").arg(name).arg(tr.bytes / (3 * tr.channels)).arg(r.intrinsic));
                }
            }
        }
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

// ---- Reading compositions -----------------------------------------------------------------------------------------

int64_t ImfVirtualTrack::length() const {
    int64_t n = 0;
    for (const ImfResource& r : resources) n += r.duration * std::max<int64_t>(1, r.repeat);
    return n;
}

const ImfVirtualTrack* ImfComposition::picture() const {
    for (const ImfVirtualTrack& t : tracks)
        if (t.kind == "MainImageSequence") return &t;
    return nullptr;
}

bool readImfComposition(const std::string& cplPath, ImfComposition& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    out = {};
    out.path = cplPath;
    QFile f(QString::fromStdString(cplPath));
    if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + cplPath);
    const QString all = QString::fromUtf8(f.readAll());
    QXmlStreamReader r(all);
    QStringList path;  // the open elements' local names
    std::vector<QXmlStreamNamespaceDeclarations> scopes;
    // What each essence descriptor says, by its id.
    struct Info {
        int width = 0, height = 0, bits = 0, channels = 0;
        uint16_t rsiz = 0;
        QString transfer, primaries, language, contentKind;
        double peak = 0;
    };
    std::map<std::string, Info> info;
    QString descriptorId, inject;
    qint64 descriptorStart = -1;
    std::map<QString, size_t> byTrackId;
    int track = -1;
    QString sequenceKind;
    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType token = r.readNext();
        if (token == QXmlStreamReader::StartElement) {
            path << r.name().toString();
            scopes.push_back(r.namespaceDeclarations());
            const int d = int(path.size());
            const QString& name = path.back();
            if (d == 1 && name != "CompositionPlaylist") return fail(cplPath + " is not a composition playlist");
            QString value;
            bool leaf = false;
            auto take = [&] {
                value = r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                leaf = true;
            };
            if (d == 2 && (name == "Id" || name == "ContentTitle" || name == "EditRate" || name == "IssueDate")) {
                take();
                if (name == "Id") out.id = packageBareId(value).toStdString();
                if (name == "ContentTitle") out.title = value.toStdString();
                if (name == "IssueDate") out.issued = value.toStdString();
                if (name == "EditRate") {
                    const QStringList parts = value.split(' ', Qt::SkipEmptyParts);
                    if (parts.size() == 2) out.rateNum = parts[0].toUInt(), out.rateDen = parts[1].toUInt();
                }
            } else if (d == 4 && path[1] == "EssenceDescriptorList" && path[2] == "EssenceDescriptor") {
                if (name == "Id") {
                    take();
                    descriptorId = packageBareId(value);
                } else if (descriptorStart < 0 && !descriptorId.isEmpty()) {
                    // The descriptor as written, with the namespaces declared above it added to its own element.
                    descriptorStart = all.lastIndexOf(QLatin1Char('<'), r.characterOffset() - 1);
                    std::map<QString, QString> inScope;
                    for (size_t i = 0; i + 1 < scopes.size(); ++i)
                        for (const QXmlStreamNamespaceDeclaration& n : scopes[i]) inScope[n.prefix().toString()] = n.namespaceUri().toString();
                    for (const QXmlStreamNamespaceDeclaration& n : scopes.back()) inScope.erase(n.prefix().toString());
                    inject.clear();
                    for (const auto& [prefix, uri] : inScope)
                        inject += prefix.isEmpty() ? QStringLiteral(" xmlns=\"%1\"").arg(uri.toHtmlEscaped())
                                                   : QStringLiteral(" xmlns:%1=\"%2\"").arg(prefix, uri.toHtmlEscaped());
                }
            } else if (d > 4 && descriptorStart >= 0) {
                static const QStringList fields = {"StoredWidth", "StoredHeight", "Rsiz", "ComponentSize", "TransferCharacteristic", "ColorPrimaries",
                                                   "MasteringDisplayMaximumLuminance", "ChannelCount", "RFC5646SpokenLanguage",
                                                   "MCAAudioContentKind", "RFC5646LanguageTagList"};
                if (fields.contains(name)) {
                    take();
                    Info& x = info[descriptorId.toStdString()];
                    if (name == "StoredWidth") x.width = value.toInt();
                    if (name == "StoredHeight") x.height = value.toInt();
                    if (name == "Rsiz") x.rsiz = uint16_t(value.toUInt());
                    if (name == "ComponentSize" && !x.bits) x.bits = value.toInt();
                    if (name == "TransferCharacteristic") x.transfer = value.toLower();
                    if (name == "ColorPrimaries") x.primaries = value.toLower();
                    if (name == "MasteringDisplayMaximumLuminance") x.peak = value.toDouble() / 10000.0;
                    if (name == "ChannelCount") x.channels = value.toInt();
                    if ((name == "RFC5646SpokenLanguage" || name == "RFC5646LanguageTagList") && x.language.isEmpty())
                        x.language = value.split(',').value(0).trimmed();
                    if (name == "MCAAudioContentKind" && x.contentKind.isEmpty()) x.contentKind = value;
                }
            } else if (d >= 5 && path[1] == "SegmentList" && path[2] == "Segment" && path[3] == "SequenceList") {
                // CompositionPlaylist/SegmentList/Segment/SequenceList/<kind>/TrackId, .../ResourceList/Resource/<field>
                if (d == 5) {
                    sequenceKind = name;
                    track = -1;
                } else if (d == 6 && name == "TrackId") {
                    take();
                    auto it = byTrackId.find(value);
                    if (it == byTrackId.end()) {
                        ImfVirtualTrack t;
                        t.kind = sequenceKind.toStdString();
                        t.id = packageBareId(value).toStdString();
                        out.tracks.push_back(t);
                        it = byTrackId.emplace(value, out.tracks.size() - 1).first;
                    }
                    track = int(it->second);
                } else if (d == 7 && name == "Resource" && track >= 0) {
                    ImfResource res;
                    res.intrinsic = res.duration = -1;
                    res.rateNum = 0;
                    out.tracks[size_t(track)].resources.push_back(res);
                } else if (d == 8 && path[6] == "Resource" && track >= 0 && !out.tracks[size_t(track)].resources.empty()) {
                    ImfResource& res = out.tracks[size_t(track)].resources.back();
                    take();
                    if (name == "EditRate") {
                        const QStringList parts = value.split(' ', Qt::SkipEmptyParts);
                        if (parts.size() == 2) res.rateNum = parts[0].toUInt(), res.rateDen = parts[1].toUInt();
                    }
                    if (name == "IntrinsicDuration") res.intrinsic = value.toLongLong();
                    if (name == "EntryPoint") res.entry = value.toLongLong();
                    if (name == "SourceDuration") res.duration = value.toLongLong();
                    if (name == "RepeatCount") res.repeat = value.toLongLong();
                    if (name == "SourceEncoding") res.encoding = packageBareId(value).toStdString();
                    if (name == "TrackFileId") res.trackFile = packageBareId(value).toStdString();
                    if (name == "Hash") res.hash = value.toStdString();
                }
            }
            if (leaf) {
                path.removeLast();
                scopes.pop_back();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (descriptorStart >= 0 && path.size() == 4) {
                QString xml = all.mid(descriptorStart, r.characterOffset() - descriptorStart).trimmed();
                const qsizetype tagEnd = xml.indexOf(QRegularExpression(QStringLiteral("[\\s/>]")), 1);
                if (tagEnd > 0) xml.insert(tagEnd, inject);
                out.descriptors[descriptorId.toStdString()] = xml.toStdString();
                descriptorStart = -1;
                descriptorId.clear();
            }
            if (!path.isEmpty()) path.removeLast();
            if (!scopes.empty()) scopes.pop_back();
        }
    }
    if (r.hasError()) return fail(cplPath + " does not read: " + r.errorString().toStdString());
    if (!out.rateNum || !out.rateDen) return fail(cplPath + " has no edit rate");
    // (a package from anywhere: numbers that would play for ever, backwards or past what a track can hold refused)
    // Edit rates: whole numbers to a million, at least one unit a second, at most 1000 pictures or 384 kHz of sound.
    auto rateOk = [](uint32_t num, uint32_t den, uint32_t most) {
        return num >= 1 && den >= 1 && num <= 1000000 && den <= 1000000 && num >= den && uint64_t(num) <= uint64_t(most) * den;
    };
    if (!rateOk(out.rateNum, out.rateDen, 1000)) return fail(cplPath + " has an edit rate that does not make sense");
    constexpr int64_t kMaxUnits = int64_t(1) << 40;  // edit units in a virtual track: years of 96 kHz sound
    int64_t plays = 0;                               // resources played, repeats counted (each opens as a clip)
    for (ImfVirtualTrack& t : out.tracks) {
        int64_t length = 0;
        for (ImfResource& res : t.resources) {
            if (!res.rateNum || !res.rateDen) res.rateNum = out.rateNum, res.rateDen = out.rateDen;
            if (!rateOk(res.rateNum, res.rateDen, 384000)) return fail(cplPath + " has a resource with an edit rate that does not make sense");
            // (an intrinsic duration of -1 is none given)
            const bool bad = res.entry < 0 || res.entry > kMaxUnits || res.intrinsic < -1 || res.intrinsic > kMaxUnits;
            if (!bad && res.duration < 0) res.duration = std::max<int64_t>(0, res.intrinsic - res.entry);
            if (bad || res.duration > kMaxUnits || (res.intrinsic >= 0 && res.entry + res.duration > res.intrinsic) || res.repeat < 1 ||
                res.repeat > 100000 || res.duration * res.repeat > kMaxUnits - length || (plays += res.repeat) > 1000000)
                return fail(cplPath + " has a resource that does not make sense (its entry point, duration or repeat count)");
            length += res.duration * res.repeat;
        }
        if (t.resources.empty()) continue;
        const auto it = info.find(t.resources.front().encoding);
        if (it == info.end()) continue;
        t.language = it->second.language.toStdString();
        t.contentKind = it->second.contentKind.toStdString();
        t.channels = it->second.channels;
    }
    if (const ImfVirtualTrack* p = out.picture(); p && !p->resources.empty()) {
        const auto it = info.find(p->resources.front().encoding);
        if (it != info.end()) {
            const Info& x = it->second;
            out.width = x.width;
            out.height = x.height;
            out.bits = x.bits;
            out.rsiz = x.rsiz;
            out.masteringPeak = x.peak;
            for (const ImfColourSpec& c : kColours)
                if (ulUrn(std::string(c.transfer)) == x.transfer && ulUrn(std::string(c.primaries)) == x.primaries) out.colour = c.id;
        }
    }
    return true;
}

namespace {

// An asset map's assets: id and the absolute path of its file.
std::vector<std::pair<std::string, QString>> assetMapPaths(const QDir& dir) {
    std::vector<std::pair<std::string, QString>> out;
    if (!dir.exists("ASSETMAP.xml")) return out;
    QString root, id;
    for (const auto& [path, text] : packageXmlItems(dir.filePath("ASSETMAP.xml"), &root)) {
        if (path == "AssetMap/AssetList/Asset/Id") id = packageBareId(text);
        if (path == "AssetMap/AssetList/Asset/ChunkList/Chunk/Path" && !id.isEmpty()) out.emplace_back(id.toStdString(), dir.absoluteFilePath(text));
    }
    return root == "AssetMap" ? out : decltype(out){};
}

}  // namespace

std::string imfCompositionIn(const std::string& folder) {
    const QDir dir(QString::fromStdString(folder));
    std::string best, bestIssued;
    for (const auto& [id, file] : assetMapPaths(dir)) {
        if (!file.endsWith(".xml", Qt::CaseInsensitive)) continue;
        QString root, issued;
        for (const auto& [path, text] : packageXmlItems(file, &root))
            if (path == "CompositionPlaylist/IssueDate") issued = text;
        if (root == "CompositionPlaylist" && (best.empty() || issued.toStdString() > bestIssued)) best = file.toStdString(), bestIssued = issued.toStdString();
    }
    return best;
}

std::map<std::string, std::string> imfTrackFiles(const std::string& folder) {
    std::map<std::string, std::string> out;
    const QDir own(QString::fromStdString(folder));
    for (const auto& [id, file] : assetMapPaths(own)) out.emplace(id, file.toStdString());
    QDir parent(own.absolutePath());
    if (!parent.cdUp()) return out;
    for (const QString& sub : parent.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir d(parent.filePath(sub));
        if (d.absolutePath() == own.absolutePath()) continue;
        for (const auto& [id, file] : assetMapPaths(d)) out.emplace(id, file.toStdString());  // (the package's own come first)
    }
    return out;
}

// ---- Opening as a sequence ----------------------------------------------------------------------------------------

bool isImfPackage(const std::string& path) {
    const QFileInfo info(QString::fromStdString(path));
    if (info.isDir()) return QFileInfo::exists(QDir(info.filePath()).filePath(QStringLiteral("ASSETMAP.xml")));
    if (info.fileName().compare(QStringLiteral("ASSETMAP.xml"), Qt::CaseInsensitive) == 0) return true;
    if (info.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) != 0) return false;
    QFile head(info.filePath());
    return head.open(QIODevice::ReadOnly) && head.read(4096).contains("CompositionPlaylist");
}

ImportResult importImf(Project& p, const std::string& path, const MediaProber& probe) {
    ImportResult res;
    const QFileInfo fi(QString::fromStdString(path));
    std::string cpl = path;
    if (fi.isDir()) cpl = imfCompositionIn(path);
    else if (fi.fileName().compare("ASSETMAP.xml", Qt::CaseInsensitive) == 0) cpl = imfCompositionIn(fi.absolutePath().toStdString());
    if (cpl.empty()) {
        res.error = "There is no IMF composition in " + path;
        return res;
    }
    ImfComposition c;
    if (!readImfComposition(cpl, c, &res.error)) return res;
    const ImfVirtualTrack* picture = c.picture();
    if (!picture) {
        res.error = "The composition has no picture";
        return res;
    }
    const std::map<std::string, std::string> files = imfTrackFiles(QFileInfo(QString::fromStdString(cpl)).absolutePath().toStdString());
    const Rational fps{int(c.rateNum), int(c.rateDen)};
    TimelineBuilder b(p, c.title.empty() ? "IMF" : c.title, fps, probe);
    std::string space = "rec709";
    if (const ImfColourSpec* spec = colourSpec(c.colour)) space = spec->space;
    {
        Sequence& seq = b.sequence();
        if (c.width > 0 && c.height > 0) seq.width = c.width, seq.height = c.height;
        seq.colorSpace = space;
        if (c.masteringPeak > 0) seq.hdrPeakNits = c.masteringPeak;
    }
    auto media = [&](const ImfResource& r, bool video, double seconds) {
        const auto it = files.find(r.trackFile);
        const Id id = b.media(it == files.end() ? std::string() : it->second, it == files.end() ? r.trackFile : std::string(), video, !video, seconds);
        if (MediaItem* m = p.findMedia(id); m && video && m->colorOverride.empty()) m->colorOverride = space;  // full-range RGB as the descriptor says
        return id;
    };
    FrameTime at = 0;
    for (const ImfResource& r : picture->resources)
        for (int64_t k = 0; k < std::max<int64_t>(1, r.repeat); ++k) {
            b.addClip(TrackKind::Video, 0, media(r, true, double(r.intrinsic > 0 ? r.intrinsic : r.entry + r.duration) / fps.toDouble()), at,
                      r.duration, double(r.entry));
            at += r.duration;
        }
    // Sound: a track for each virtual track, the first programme heard.
    int audio = 0, heard = -1;
    for (const ImfVirtualTrack& t : c.tracks)
        if (t.kind == "MainAudioSequence" && heard < 0 && (t.contentKind.empty() || t.contentKind == "PRM")) heard = audio++;
    audio = 0;
    int channels = 2;
    for (const ImfVirtualTrack& t : c.tracks) {
        if (t.kind != "MainAudioSequence") continue;
        const int index = audio++;
        Track& track = b.track(TrackKind::Audio, index);
        const std::string kind = t.contentKind.empty() ? "PRM" : t.contentKind;
        track.name = (t.language.empty() ? std::string("Sound") : t.language) + " " +
                     (kind == "PRM" ? "programme" : kind == "ME" ? "music and effects" : kind == "DV" ? "described" : kind);
        track.muted = index != (heard < 0 ? 0 : heard);
        if (!track.muted && t.channels > 0) channels = t.channels;
        int64_t pos = 0;
        for (const ImfResource& r : t.resources)
            for (int64_t k = 0; k < std::max<int64_t>(1, r.repeat); ++k) {
                const double perUnit = fps.toDouble() * double(r.rateDen) / double(r.rateNum);  // sequence frames an edit unit
                const FrameTime start = FrameTime(std::llround(double(pos) * perUnit));
                const FrameTime stop = FrameTime(std::llround(double(pos + r.duration) * perUnit));
                b.addClip(TrackKind::Audio, index, media(r, false, double(r.intrinsic > 0 ? r.intrinsic : r.entry + r.duration) * double(r.rateDen) / double(r.rateNum)),
                          start, stop - start, double(r.entry) * perUnit);
                pos += r.duration;
            }
    }
    b.sequence().audioLayout = channels == 8 ? "7.1" : channels == 6 ? "5.1" : "stereo";
    // Timed text: a caption track each, the document's captions placed where its resources play them.
    for (const ImfVirtualTrack& t : c.tracks) {
        const QString kind = QString::fromStdString(t.kind);
        if (!isTextSequence(kind)) continue;
        CaptionTrack track;
        track.id = p.newId();
        track.language = t.language.empty() ? "en" : t.language;
        track.name = (kind == "SubtitlesSequence" ? "Subtitles" : kind == "HearingImpairedCaptionsSequence" ? "Captions"
                      : kind == "ForcedNarrativeSequence"                 ? "Forced narrative"
                                                                          : "Text") +
                     std::string(" (") + track.language + ")";
        FrameTime pos = 0;
        for (const ImfResource& r : t.resources) {
            const auto it = files.find(r.trackFile);
            dcp::ImfTrackReader reader;
            std::string xml, err;
            std::vector<Caption> captions;
            bool read = it != files.end() && reader.open(it->second, &err) && reader.text(xml, &err);
            if (read && isImscImage(xml)) {
                // Pictures, read by OCR (in the track's language).
                DecodedSubtitles pictures;
                ImageSubtitleImport in;
                read = decodeImscImage(xml, imscResource(reader), pictures, &err) &&
                       readSubtitlePictures(pictures, fps, in, track.language, {}, nullptr, &err);
                captions = in.track.captions;
                if (read && !in.track.captions.empty()) track.style = in.track.style;
            } else if (read) {
                read = parseTtml(xml, fps, captions, &err);
            }
            if (!read) {
                b.warn("The timed text " + r.trackFile + " could not be read" + (err.empty() ? std::string() : ": " + err));
                pos += r.duration * std::max<int64_t>(1, r.repeat);
                continue;
            }
            for (int64_t k = 0; k < std::max<int64_t>(1, r.repeat); ++k, pos += r.duration)
                for (Caption cap : captions) {
                    const FrameTime from = std::max<FrameTime>(cap.start, r.entry), to = std::min<FrameTime>(cap.end, r.entry + r.duration);
                    if (to <= from) continue;
                    cap.start = from - r.entry + pos;
                    cap.end = to - r.entry + pos;
                    track.captions.push_back(cap);
                }
        }
        normalizeCaptions(track.captions);
        b.sequence().captionTracks.push_back(std::move(track));
    }
    return b.finish();
}

}  // namespace montage
