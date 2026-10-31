#include "Adm.h"

#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "Compositor.h"
#include "core/Automation.h"
#include "core/Surround.h"

namespace montage {

namespace {

constexpr int kRate = 48000;

// BS.2094's channel formats by BS.2051 speaker label.
const std::map<std::string, std::string>& commonChannels() {
    static const std::map<std::string, std::string> m{
        {"M+030", "AC_00010001"}, {"M-030", "AC_00010002"}, {"M+000", "AC_00010003"}, {"LFE1", "AC_00010004"},
        {"M+110", "AC_00010005"}, {"M-110", "AC_00010006"}, {"M+090", "AC_0001000a"}, {"M-090", "AC_0001000b"},
        {"M+135", "AC_0001001c"}, {"M-135", "AC_0001001d"}, {"U+030", "AC_0001000d"}, {"U-030", "AC_0001000f"},
        {"U+110", "AC_00010010"}, {"U-110", "AC_00010012"}, {"U+045", "AC_00010022"}, {"U-045", "AC_00010023"},
        {"U+135", "AC_0001001e"}, {"U-135", "AC_0001001f"}, {"U+090", "AC_00010013"}, {"U-090", "AC_00010014"}};
    return m;
}

constexpr const char* kOwnBedPack = "AP_00011001";  // 7.1.2 as Dolby's bed: 0+7+0 with the pair above at the sides

// hh:mm:ss.fffff, as BS.2076 writes times.
QString admTime(int64_t samples) {
    const int64_t whole = samples / kRate;
    const int64_t frac = (samples % kRate) * 100000 / kRate;
    return QString::asprintf("%02lld:%02lld:%02lld.%05lld", (long long)(whole / 3600), (long long)(whole / 60 % 60), (long long)(whole % 60),
                             (long long)frac);
}

// The same to the nanosecond (BS.2125's longer form), for S-ADM frames whose edges fall between ADM's 10 µs steps.
QString admTimeExact(int64_t samples, int rate) {
    const int64_t whole = samples / rate;
    const int64_t frac = (samples % rate) * 1000000000 / rate;
    return QString::asprintf("%02lld:%02lld:%02lld.%09lld", (long long)(whole / 3600), (long long)(whole / 60 % 60), (long long)(whole % 60),
                             (long long)frac);
}

QString number(double v) { return QString::number(v, 'f', 6); }

void put16(QByteArray& b, uint16_t v) { b.append(char(v & 0xff)).append(char(v >> 8)); }
void put32(QByteArray& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff));
}
void put64(QByteArray& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.append(char((v >> (8 * i)) & 0xff));
}
void putText(QByteArray& b, const std::string& s, size_t n) {
    QByteArray t = QByteArray::fromStdString(s).left(int(n));
    t.append(QByteArray(int(n) - t.size(), '\0'));
    b.append(t);
}

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32); }

// ITU-R BS.2127's polar to cartesian mapping (section 10.1) at ear level, the one ADM renderers apply to cartesian
// objects: the loudspeakers M+000, M-030, M-110, M+110 and M+030 sit on the corners and edges of the square, so a
// position at a speaker's angle lands where a renderer puts that speaker. `azimuth` in degrees, positive to the left as
// ADM counts it; X and Y scaled by `distance`.
std::array<double, 2> polarToCartesian(double azimuth, double distance) {
    constexpr double kDeg = M_PI / 180;
    // The sectors between neighbouring loudspeakers, anticlockwise: azimuth and position on the square.
    struct Corner {
        double az, x, y;
    };
    static const Corner corners[5] = {{0, 0, 1}, {-30, 1, 1}, {-110, 1, -1}, {110, -1, -1}, {30, -1, 1}};
    // An angle unwrapped to lie at or past `base` (by less than a turn).
    // (fmod, not a loop: an angle a file gives can be any size)
    auto from = [](double base, double a) {
        a = base + std::fmod(a - base, 360.0);
        if (a < base) a += 360;
        return a;
    };
    if (!std::isfinite(azimuth)) azimuth = 0;
    for (int i = 0; i < 5; ++i) {
        const Corner& left = corners[i];
        const Corner& right = corners[(i + 1) % 5];
        const double rightAz = right.az, leftAz = from(rightAz, left.az), az = from(rightAz, azimuth);
        if (az > leftAz) continue;
        // Equal-tangent panning between the two corners, then the constant-power gain's angle as a fraction.
        const double mid = (leftAz + rightAz) / 2, half = rightAz - mid;
        const double gainR = 0.5 + 0.5 * std::tan((az - mid) * kDeg) / std::tan(half * kDeg);
        const double t = std::atan2(gainR, 1 - gainR) * 2 / M_PI;
        return {distance * (left.x + (right.x - left.x) * t), distance * (left.y + (right.y - left.y) * t)};
    }
    return {0, distance};  // not reached: the sectors cover the circle
}

// Where an object is in ADM's cartesian coordinates, as the panner places it: its angle and distance round the room
// into the square by BS.2127's mapping, and its height as Z (0 at the ear, 1 overhead, in layouts with overhead
// speakers; `overhead`), which renderers crossfade between the layers at constant power as the mix does, whatever the
// distance.
struct Position {
    double x = 0, y = 1, z = 0;
    double az = 0;  // the panner's angle (degrees, ADM's way round), for how far round a block goes
};
Position admPosition(const SurroundPan& sp, bool overhead) {
    const double az = -std::atan2(sp.x, sp.y) * 180 / M_PI;
    const std::array<double, 2> xy = polarToCartesian(az, std::min(1.0, std::hypot(sp.x, sp.y)));
    return {xy[0], xy[1], overhead ? std::clamp(sp.z, 0.0, 1.0) : 0.0, az};
}

// One audioBlockFormat: from `from` to `to` (samples into the master), moving to `at` over its length.
struct Block {
    int64_t from = 0, to = 0;
    Position at;
};

// An object's blocks. Still: one. Moving (its position lanes play): a millisecond at where it starts, then the
// position every frame, each block gliding to the next point; points on a straight line (within 0.005 in X, Y and Z,
// and no more than 10 degrees round) share a block. Cartesian blocks need no jump behind the listener: the path between
// two points behind stays behind. Boundaries fall on whole samples that hh:mm:ss.fffff writes exactly (every 12 at
// 48 kHz).
std::vector<Block> objectBlocks(const Track& tr, bool overhead, FrameTime first, FrameTime end, double fps, int64_t total) {
    const bool reads = automationReads(trackAutomation(tr));
    if (!reads || !surroundAnimated(tr)) return {{0, total, admPosition(tr.surround, overhead)}};
    auto sampleAt = [&](FrameTime f) {
        const int64_t v = int64_t(std::llround(double(f - first) * kRate / fps));
        return std::min(total, v / 12 * 12);
    };
    std::vector<std::pair<int64_t, Position>> points;  // every frame, merged below
    for (FrameTime f = first; f < end; ++f) points.push_back({sampleAt(f), admPosition(trackSurroundAt(tr, double(f)), overhead)});
    points.push_back({total, admPosition(trackSurroundAt(tr, double(end)), overhead)});
    std::vector<Block> out{{0, std::min<int64_t>(total, kRate / 1000), points[0].second}};
    auto onLine = [](const Position& a, const Position& b, double u, const Position& m) {
        return std::fabs(a.x + (b.x - a.x) * u - m.x) < 0.005 && std::fabs(a.y + (b.y - a.y) * u - m.y) < 0.005 &&
               std::fabs(a.z + (b.z - a.z) * u - m.z) < 0.005;
    };
    size_t i = 1;
    while (i < points.size()) {
        const int64_t from = out.back().to;
        if (points[i].first <= from) {  // a point the lead-in already covers
            ++i;
            continue;
        }
        const Position start = out.back().at;
        // As far along as the points between stay on the straight line (at most 200 points to a block), and no
        // further than 10 degrees round (either way across the back): renderers (BS.2127) fade the speaker gains
        // between blocks, so a long block would play an arc as a crossfade between its ends instead of a sweep through
        // the speakers between (a rise at one angle fades between the same speakers either way).
        size_t j = i;
        while (j + 1 < points.size() && j - i < 200) {
            const size_t k = j + 1;
            if (std::fabs(std::remainder(points[k].second.az - start.az, 360.0)) > 10) break;
            bool straight = true;
            for (size_t m = i; m <= j && straight; ++m) {
                const double u = double(points[m].first - from) / double(points[k].first - from);
                straight = onLine(start, points[k].second, u, points[m].second);
            }
            if (!straight) break;
            j = k;
        }
        out.push_back({from, points[j].first, points[j].second});
        i = j + 1;
    }
    out.back().to = total;
    return out;
}

}  // namespace

std::vector<int> admObjectTracks(const Sequence& s) {
    const bool anySolo = std::any_of(s.audioTracks.begin(), s.audioTracks.end(), [](const Track& t) { return t.solo; });
    std::vector<int> out;
    for (size_t i = 0; i < s.audioTracks.size(); ++i) {
        const Track& t = s.audioTracks[i];
        if (t.surround.object && !t.output && !t.muted && (!anySolo || t.solo)) out.push_back(int(i));
    }
    return out;
}

std::string admBedPack(const std::string& layout) {
    if (layout == "5.1") return "AP_00010003";
    if (layout == "7.1") return "AP_0001000f";
    if (layout == "5.1.2") return "AP_00010004";
    if (layout == "5.1.4") return "AP_00010005";
    if (layout == "7.1.4") return "AP_00010017";
    if (layout == "7.1.2") return kOwnBedPack;
    return "AP_00010002";
}

std::vector<std::string> admBedChannels(const std::string& layout) {
    std::vector<std::string> out;
    for (const Speaker& sp : layoutSpeakers(layout)) {
        auto it = commonChannels().find(sp.label);
        out.push_back(it != commonChannels().end() ? it->second : std::string());
    }
    return out;
}

AdmPoint admSpeakerPoint(const Speaker& sp) {
    const std::array<double, 2> xy = polarToCartesian(-sp.angle, 1);
    return {xy[0], xy[1], sp.elevation > 0 ? 1.0 : 0.0};
}

AdmPoint AdmScene::objectAt(size_t k, double sample) const {
    if (k >= objects.size() || objects[k].blocks.empty()) return {};
    const std::vector<AdmBlock>& b = objects[k].blocks;
    // The block holding the sample (a block's end its own), and where the one before left off.
    size_t i = size_t(std::lower_bound(b.begin(), b.end(), sample, [](const AdmBlock& x, double v) { return double(x.to) < v; }) - b.begin());
    if (i >= b.size()) return b.back().at;
    const AdmBlock& x = b[i];
    if (i == 0 || x.jump || x.to <= x.from) return x.at;
    const AdmPoint& a = b[i - 1].at;
    const double u = std::clamp((sample - double(x.from)) / double(x.to - x.from), 0.0, 1.0);
    return {a.x + (x.at.x - a.x) * u, a.y + (x.at.y - a.y) * u, a.z + (x.at.z - a.z) * u};
}

bool admSceneOf(const Project& p, const Sequence& s, FrameTime first, FrameTime end, const std::string& title, AdmScene& out,
                std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (ambisonicLayout(s.audioLayout))
        return fail("An immersive master is a bed of speakers and objects: an ambisonic mix is delivered as a file of its field (File > Export)");
    (void)p;
    AdmScene sc;
    sc.title = title.empty() ? s.name : title;
    sc.layout = std::find(audioLayouts().begin(), audioLayouts().end(), s.audioLayout) != audioLayouts().end() ? s.audioLayout : "stereo";
    const std::vector<std::string> bedChannels = admBedChannels(sc.layout);
    if (std::any_of(bedChannels.begin(), bedChannels.end(), [](const std::string& c) { return c.empty(); }))
        return fail("The sequence's speakers have no ADM names");
    const int bedN = int(bedChannels.size());
    const std::vector<int> objects = admObjectTracks(s);
    sc.channels = bedN + int(objects.size());
    if (sc.channels > 128) return fail("An ADM master here holds up to 128 channels: the bed and at most " + std::to_string(128 - bedN) + " objects");
    if (end <= first) return fail("The sequence is empty");
    const double fps = s.fpsValue();
    sc.samples = int64_t(std::llround(double(end - first) * kRate / fps));
    sc.bedPack = admBedPack(sc.layout);
    for (int i = 0; i < bedN; ++i) sc.bed.push_back(i);
    const bool overhead = std::any_of(layoutSpeakers(sc.layout).begin(), layoutSpeakers(sc.layout).end(), [](const Speaker& k) { return k.elevation > 0; });
    for (size_t k = 0; k < objects.size(); ++k) {
        const Track& tr = s.audioTracks[size_t(objects[k])];
        AdmScene::Object o;
        o.name = tr.name;
        o.channel = bedN + int(k);
        for (const Block& b : objectBlocks(tr, overhead, first, end, fps, sc.samples)) o.blocks.push_back({b.from, b.to, {b.at.x, b.at.y, b.at.z}, false});
        sc.objects.push_back(std::move(o));
    }
    out = std::move(sc);
    return true;
}

namespace {

QString objectHex(size_t k) { return QString::asprintf("%04X", unsigned(0x1001 + k)); }

// The programme's elements, as admDocument writes them and sadmFrame repeats them in every frame; each object's blocks
// from `blocks`.
void writeProgramme(QXmlStreamWriter& x, const AdmScene& sc, const std::vector<AdmTrack>& tracks,
                    const std::function<void(size_t k, const QString& hex)>& blocks, const std::function<QString(int64_t)>& time) {
    const QString duration = time(sc.samples);
    const size_t bedN = sc.bed.size();
    x.writeStartElement("audioProgramme");
    x.writeAttribute("audioProgrammeID", "APR_1001");
    x.writeAttribute("audioProgrammeName", QString::fromStdString(sc.title));
    x.writeAttribute("start", time(0));
    x.writeAttribute("end", duration);
    x.writeTextElement("audioContentIDRef", "ACO_1001");
    if (!sc.objects.empty()) x.writeTextElement("audioContentIDRef", "ACO_1002");
    x.writeEndElement();
    x.writeStartElement("audioContent");
    x.writeAttribute("audioContentID", "ACO_1001");
    x.writeAttribute("audioContentName", "Bed");
    x.writeTextElement("audioObjectIDRef", "AO_1001");
    x.writeEndElement();
    if (!sc.objects.empty()) {
        x.writeStartElement("audioContent");
        x.writeAttribute("audioContentID", "ACO_1002");
        x.writeAttribute("audioContentName", "Objects");
        for (size_t k = 0; k < sc.objects.size(); ++k) x.writeTextElement("audioObjectIDRef", QString::asprintf("AO_%04X", unsigned(0x1002 + k)));
        x.writeEndElement();
    }
    x.writeStartElement("audioObject");
    x.writeAttribute("audioObjectID", "AO_1001");
    x.writeAttribute("audioObjectName", "Bed");
    x.writeAttribute("start", time(0));
    x.writeAttribute("duration", duration);
    x.writeTextElement("audioPackFormatIDRef", QString::fromStdString(sc.bedPack));
    for (size_t i = 0; i < bedN; ++i) x.writeTextElement("audioTrackUIDRef", QString::fromStdString(tracks[i].uid));
    x.writeEndElement();
    for (size_t k = 0; k < sc.objects.size(); ++k) {
        x.writeStartElement("audioObject");
        x.writeAttribute("audioObjectID", QString::asprintf("AO_%04X", unsigned(0x1002 + k)));
        x.writeAttribute("audioObjectName", QString::fromStdString(sc.objects[k].name));
        x.writeAttribute("start", time(0));
        x.writeAttribute("duration", duration);
        x.writeTextElement("audioPackFormatIDRef", "AP_0003" + objectHex(k));
        x.writeTextElement("audioTrackUIDRef", QString::fromStdString(tracks[bedN + k].uid));
        x.writeEndElement();
    }
    if (sc.bedPack == kOwnBedPack) {
        x.writeStartElement("audioPackFormat");
        x.writeAttribute("audioPackFormatID", kOwnBedPack);
        x.writeAttribute("audioPackFormatName", "7.1.2");
        x.writeAttribute("typeLabel", "0001");
        x.writeAttribute("typeDefinition", "DirectSpeakers");
        for (const std::string& c : admBedChannels(sc.layout)) x.writeTextElement("audioChannelFormatIDRef", QString::fromStdString(c));
        x.writeEndElement();
    }
    for (size_t k = 0; k < sc.objects.size(); ++k) {
        const QString hex = objectHex(k), name = QString::fromStdString(sc.objects[k].name);
        x.writeStartElement("audioPackFormat");
        x.writeAttribute("audioPackFormatID", "AP_0003" + hex);
        x.writeAttribute("audioPackFormatName", name);
        x.writeAttribute("typeLabel", "0003");
        x.writeAttribute("typeDefinition", "Objects");
        x.writeTextElement("audioChannelFormatIDRef", "AC_0003" + hex);
        x.writeEndElement();
        x.writeStartElement("audioChannelFormat");
        x.writeAttribute("audioChannelFormatID", "AC_0003" + hex);
        x.writeAttribute("audioChannelFormatName", name);
        x.writeAttribute("typeLabel", "0003");
        x.writeAttribute("typeDefinition", "Objects");
        blocks(k, hex);
        x.writeEndElement();  // audioChannelFormat
        x.writeStartElement("audioStreamFormat");
        x.writeAttribute("audioStreamFormatID", "AS_0003" + hex);
        x.writeAttribute("audioStreamFormatName", "PCM_" + name);
        x.writeAttribute("formatLabel", "0001");
        x.writeAttribute("formatDefinition", "PCM");
        x.writeTextElement("audioChannelFormatIDRef", "AC_0003" + hex);
        x.writeTextElement("audioTrackFormatIDRef", "AT_0003" + hex + "_01");
        x.writeEndElement();
        x.writeStartElement("audioTrackFormat");
        x.writeAttribute("audioTrackFormatID", "AT_0003" + hex + "_01");
        x.writeAttribute("audioTrackFormatName", "PCM_" + name);
        x.writeAttribute("formatLabel", "0001");
        x.writeAttribute("formatDefinition", "PCM");
        x.writeTextElement("audioStreamFormatIDRef", "AS_0003" + hex);
        x.writeEndElement();
    }
    for (const AdmTrack& c : tracks) {
        x.writeStartElement("audioTrackUID");
        x.writeAttribute("UID", QString::fromStdString(c.uid));
        x.writeAttribute("sampleRate", QString::number(sc.rate));
        x.writeAttribute("bitDepth", "24");
        x.writeTextElement("audioTrackFormatIDRef", QString::fromStdString(c.trackFormat));
        x.writeTextElement("audioPackFormatIDRef", QString::fromStdString(c.pack));
        x.writeEndElement();
    }
}

void writeBlock(QXmlStreamWriter& x, const QString& hex, size_t index, const QString& rtime, const QString& duration, const AdmPoint& at, bool jump) {
    x.writeStartElement("audioBlockFormat");
    x.writeAttribute("audioBlockFormatID", "AB_0003" + hex + QString::asprintf("_%08X", unsigned(index + 1)));
    x.writeAttribute("rtime", rtime);
    x.writeAttribute("duration", duration);
    x.writeTextElement("cartesian", "1");
    for (auto [coord, v] : {std::pair<const char*, double>{"X", at.x}, {"Y", at.y}, {"Z", at.z}}) {
        x.writeStartElement("position");
        x.writeAttribute("coordinate", coord);
        x.writeCharacters(number(std::abs(v) < 5e-7 ? 0.0 : std::clamp(v, -1.0, 1.0)));
        x.writeEndElement();
    }
    if (jump) x.writeTextElement("jumpPosition", "1");
    x.writeEndElement();
}

}  // namespace

std::vector<AdmTrack> admTracks(const AdmScene& sc) {
    std::vector<AdmTrack> out;
    const std::vector<std::string> bedChannels = admBedChannels(sc.layout);
    for (size_t i = 0; i < sc.bed.size() && i < bedChannels.size(); ++i)
        out.push_back({sc.bed[i], "", "AT_" + bedChannels[i].substr(3) + "_01", sc.bedPack});  // BS.2094's
    for (size_t k = 0; k < sc.objects.size(); ++k)
        out.push_back({sc.objects[k].channel, "", ("AT_0003" + objectHex(k) + "_01").toStdString(), ("AP_0003" + objectHex(k)).toStdString()});
    for (size_t i = 0; i < out.size(); ++i) out[i].uid = QString::asprintf("ATU_%08X", unsigned(i + 1)).toStdString();
    return out;
}

std::string admDocument(const AdmScene& sc) {
    const std::vector<AdmTrack> tracks = admTracks(sc);
    QByteArray xml;
    QXmlStreamWriter x(&xml);
    x.setAutoFormatting(true);
    x.writeStartDocument();
    x.writeStartElement("ebuCoreMain");
    x.writeDefaultNamespace("urn:ebu:metadata-schema:ebuCore_2017");
    x.writeNamespace("http://purl.org/dc/elements/1.1/", "dc");
    x.writeAttribute("schema", "EBU_CORE_20171102.xsd");
    x.writeAttribute("xml:lang", "en");
    x.writeStartElement("coreMetadata");
    x.writeStartElement("format");
    x.writeStartElement("audioFormatExtended");
    x.writeAttribute("version", "ITU-R_BS.2076-2");
    writeProgramme(
        x, sc, tracks,
        [&](size_t k, const QString& hex) {
            const std::vector<AdmBlock>& blocks = sc.objects[k].blocks;
            for (size_t b = 0; b < blocks.size(); ++b)
                writeBlock(x, hex, b, admTime(blocks[b].from), blocks.size() == 1 ? admTime(sc.samples) : admTime(blocks[b].to - blocks[b].from),
                           blocks[b].at, blocks[b].jump);
        },
        [](int64_t t) { return admTime(t); });
    x.writeEndElement();  // audioFormatExtended
    x.writeEndElement();  // format
    x.writeEndElement();  // coreMetadata
    x.writeEndElement();  // ebuCoreMain
    x.writeEndDocument();
    return xml.toStdString();
}

std::string sadmFrame(const AdmScene& sc, int64_t index, int64_t start, int64_t length, const std::string& flowId) {
    const std::vector<AdmTrack> tracks = admTracks(sc);
    auto time = [&](int64_t t) { return admTimeExact(t, sc.rate); };
    const int64_t end = start + length;
    QByteArray xml;
    QXmlStreamWriter x(&xml);
    x.writeStartDocument();
    x.writeStartElement("frame");
    x.writeAttribute("version", "ITU-R_BS.2125-1");
    x.writeStartElement("frameHeader");
    x.writeStartElement("frameFormat");
    x.writeAttribute("frameFormatID", QString::asprintf("FF_%08llX", (unsigned long long)index));
    x.writeAttribute("start", time(start));
    x.writeAttribute("duration", time(length));
    x.writeAttribute("type", "full");
    x.writeAttribute("timeReference", "total");
    if (!flowId.empty()) x.writeAttribute("flowID", QString::fromStdString(flowId));
    x.writeAttribute("countToFull", "1");
    x.writeEndElement();
    x.writeStartElement("transportTrackFormat");
    x.writeAttribute("transportID", "TP_0001");
    x.writeAttribute("transportName", "MXF");
    x.writeAttribute("numIDs", QString::number(tracks.size()));
    x.writeAttribute("numTracks", QString::number(sc.channels));
    for (const AdmTrack& t : tracks) {
        x.writeStartElement("audioTrack");
        x.writeAttribute("trackID", QString::number(t.channel + 1));
        x.writeTextElement("audioTrackUIDRef", QString::fromStdString(t.uid));
        x.writeEndElement();
    }
    x.writeEndElement();  // transportTrackFormat
    x.writeEndElement();  // frameHeader
    x.writeStartElement("audioFormatExtended");
    x.writeAttribute("version", "ITU-R_BS.2076-2");
    writeProgramme(
        x, sc, tracks,
        [&](size_t k, const QString& hex) {
            // The blocks over the frame, cut at its edges: each ends where the object is then.
            const std::vector<AdmBlock>& blocks = sc.objects[k].blocks;
            const auto first = std::upper_bound(blocks.begin(), blocks.end(), start, [](int64_t v, const AdmBlock& o) { return v < o.to; });
            for (size_t b = size_t(first - blocks.begin()); b < blocks.size() && blocks[b].from < end; ++b) {
                const int64_t from = std::max(blocks[b].from, start), to = std::min(blocks[b].to, end);
                if (to > from) writeBlock(x, hex, b, time(from), time(to - from), sc.objectAt(k, double(to)), blocks[b].jump && from == blocks[b].from);
            }
        },
        time);
    x.writeEndElement();  // audioFormatExtended
    x.writeEndElement();  // frame
    x.writeEndDocument();
    return xml.toStdString();
}

namespace {

// A time as ADM and S-ADM write them ("hh:mm:ss.fffff", "hh:mm:ss.zzzzzSfffff", "zzzzzSfffff", "ss.fffff", or with a
// date before a T), in samples at `rate`; -1 when it is none of those.
int64_t admSamples(QString t, int rate) {
    t = t.trimmed();
    if (const int T = t.indexOf('T'); T >= 0) t = t.mid(T + 1);
    if (t.endsWith('Z')) t.chop(1);
    const int S = t.indexOf('S');
    QString clock = t, samples;
    int of = 0;
    if (S >= 0) {
        bool ok = false;
        of = t.mid(S + 1).toInt(&ok);
        if (!ok || of <= 0) return -1;
        clock = t.left(S);
    }
    double seconds = 0;
    const QStringList parts = clock.split(':');
    if (parts.size() > 3) return -1;
    if (S >= 0) {
        // The part after the last point (or all of a short form) counts samples at `of`.
        const int dot = clock.lastIndexOf('.');
        samples = dot >= 0 ? clock.mid(dot + 1) : clock;
        clock = dot >= 0 ? clock.left(dot) : QString();
    }
    const QStringList hms = clock.isEmpty() ? QStringList() : clock.split(':');
    for (const QString& part : hms) {
        bool ok = false;
        const double v = part.toDouble(&ok);
        if (!ok || v < 0) return -1;
        seconds = seconds * 60 + v;
    }
    double total = seconds * rate;
    if (S >= 0) {
        bool ok = false;
        const double n = samples.toDouble(&ok);
        if (!ok || n < 0) return -1;
        total += n * rate / of;
    }
    // (no further than a week: a time a file makes up stays one the arithmetic on it can take)
    if (!std::isfinite(total) || total > 7.0 * 86400 * rate) return -1;
    return int64_t(std::llround(total));
}

// A BS.2051 speaker label as written ("M+030", "urn:itu:bs:2051:0:speaker:M+030", "LFE").
std::string speakerLabel(QString l) {
    l = l.trimmed();
    if (const int c = l.lastIndexOf(':'); c >= 0) l = l.mid(c + 1);
    if (l.startsWith("LFE", Qt::CaseInsensitive)) return "LFE1";
    return l.toStdString();
}

struct ParsedBlock {
    int64_t rtime = -1, duration = -1;
    std::map<std::string, double> position;
    bool cartesian = false, jump = false;
    std::string speaker;
};
struct ParsedChannel {
    std::string type;  // typeDefinition, or from the ID's typeLabel
    std::vector<ParsedBlock> blocks;
};
struct ParsedObject {
    std::string name;
    int64_t start = 0;
    std::vector<std::string> packs, uids, objects;
};

}  // namespace

bool parseAdm(const std::string& text, AdmScene& out, const std::vector<AdmTrack>& tracks, std::string* error, std::vector<std::string>* warnings) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const int rate = out.rate > 0 ? out.rate : 48000;
    std::vector<std::pair<std::string, std::vector<std::string>>> programmes;  // name, contents (in order)
    std::map<std::string, int64_t> programmeStart;
    std::map<std::string, std::vector<std::string>> contents, packChannels, streamChannel, trackStream;
    std::map<std::string, std::string> packType;
    std::map<std::string, ParsedObject> objects;
    std::map<std::string, ParsedChannel> channels;
    std::map<std::string, std::pair<std::string, std::string>> uids;  // UID: track (or channel) format, pack
    std::map<std::string, int> transport;                               // UID: channel (S-ADM's transport track format)
    std::string firstProgramme;
    int64_t frameStart = -1, frameLength = -1;
    bool local = false, frame = false;

    QXmlStreamReader x(QByteArray::fromStdString(text));
    std::string programme, content, object, pack, channel, stream, track, uid;
    ParsedBlock* block = nullptr;
    std::string coordinate;
    int transportTrack = -1;
    auto attr = [&](const char* name) { return x.attributes().value(QLatin1String(name)).toString(); };
    while (!x.atEnd()) {
        const auto tok = x.readNext();
        if (tok == QXmlStreamReader::EndElement) {
            const QStringView n = x.name();
            if (n == u"audioBlockFormat") block = nullptr;
            else if (n == u"audioChannelFormat") channel.clear();
            else if (n == u"audioObject") object.clear();
            else if (n == u"audioTrack") transportTrack = -1;
            continue;
        }
        if (tok != QXmlStreamReader::StartElement) continue;
        const QStringView n = x.name();
        if (n == u"frame") {
            frame = true;
        } else if (n == u"frameFormat") {
            frameStart = admSamples(attr("start"), rate);
            frameLength = admSamples(attr("duration"), rate);
            local = attr("timeReference") == "local";
        } else if (n == u"audioTrack") {
            bool ok = false;
            transportTrack = attr("trackID").toInt(&ok) - 1;
            if (!ok || transportTrack < 0 || transportTrack >= 4096) transportTrack = -1;
        } else if (n == u"audioProgramme") {
            programme = attr("audioProgrammeID").toStdString();
            if (firstProgramme.empty()) firstProgramme = programme;
            programmes.push_back({attr("audioProgrammeName").toStdString(), {}});
            programmeStart[programme] = std::max<int64_t>(0, admSamples(attr("start").isEmpty() ? "0S1" : attr("start"), rate));
        } else if (n == u"audioContent") {
            content = attr("audioContentID").toStdString();
        } else if (n == u"audioObject") {
            object = attr("audioObjectID").toStdString();
            ParsedObject& o = objects[object];
            o.name = attr("audioObjectName").toStdString();
            o.start = attr("start").isEmpty() ? 0 : std::max<int64_t>(0, admSamples(attr("start"), rate));
        } else if (n == u"audioPackFormat") {
            pack = attr("audioPackFormatID").toStdString();
            packType[pack] = attr("typeDefinition").toStdString();
        } else if (n == u"audioChannelFormat") {
            channel = attr("audioChannelFormatID").toStdString();
            channels[channel].type = attr("typeDefinition").toStdString();
        } else if (n == u"audioStreamFormat") {
            stream = attr("audioStreamFormatID").toStdString();
        } else if (n == u"audioTrackFormat") {
            track = attr("audioTrackFormatID").toStdString();
        } else if (n == u"audioTrackUID") {
            uid = attr("UID").toStdString();
            uids[uid];
        } else if (n == u"audioBlockFormat" && !channel.empty()) {
            ParsedBlock b;
            const QString rt = local ? attr("lstart") : attr("rtime");
            const QString du = local ? attr("lduration") : attr("duration");
            b.rtime = rt.isEmpty() ? -1 : admSamples(rt, rate);
            b.duration = du.isEmpty() ? -1 : admSamples(du, rate);
            if (local && b.rtime >= 0 && frameStart >= 0) b.rtime += frameStart;
            channels[channel].blocks.push_back(b);
            block = &channels[channel].blocks.back();
        } else if (block && n == u"position") {
            coordinate = attr("coordinate").toStdString();
            bool ok = false;
            const double v = x.readElementText().trimmed().toDouble(&ok);
            if (ok && std::isfinite(v)) block->position[coordinate] = v;
        } else if (block && n == u"cartesian") {
            block->cartesian = x.readElementText().trimmed() == "1";
        } else if (block && n == u"jumpPosition") {
            block->jump = x.readElementText().trimmed() == "1";
        } else if (block && n == u"speakerLabel") {
            if (block->speaker.empty()) block->speaker = speakerLabel(x.readElementText());
        } else {
            // References, by the element they sit in.
            const std::string ref = n.toString().toStdString();
            auto refText = [&] { return x.readElementText().trimmed().toStdString(); };
            if (ref == "audioContentIDRef" && !programme.empty() && !programmes.empty()) programmes.back().second.push_back(refText());
            else if (ref == "audioObjectIDRef" && !object.empty()) objects[object].objects.push_back(refText());
            else if (ref == "audioObjectIDRef" && !content.empty()) contents[content].push_back(refText());
            else if (ref == "audioPackFormatIDRef" && !object.empty()) objects[object].packs.push_back(refText());
            else if (ref == "audioPackFormatIDRef" && !uid.empty()) uids[uid].second = refText();
            else if (ref == "audioTrackUIDRef" && transportTrack >= 0) transport[refText()] = transportTrack;
            else if (ref == "audioTrackUIDRef" && !object.empty()) objects[object].uids.push_back(refText());
            else if (ref == "audioChannelFormatIDRef" && !pack.empty() && channel.empty() && stream.empty()) packChannels[pack].push_back(refText());
            else if (ref == "audioChannelFormatIDRef" && !stream.empty()) streamChannel[stream] = {refText()};
            else if (ref == "audioStreamFormatIDRef" && !track.empty()) trackStream[track] = {refText()};
            else if ((ref == "audioTrackFormatIDRef" || ref == "audioChannelFormatIDRef") && !uid.empty()) uids[uid].first = refText();
        }
        // (an element's scope ends where the next sibling of its kind starts; references only count inside their own)
        if (n == u"audioProgramme" || n == u"audioContent" || n == u"audioObject" || n == u"audioPackFormat" || n == u"audioChannelFormat" ||
            n == u"audioStreamFormat" || n == u"audioTrackFormat" || n == u"audioTrackUID") {
            if (n != u"audioProgramme") programme.clear();
            if (n != u"audioContent") content.clear();
            if (n != u"audioObject") object.clear();
            if (n != u"audioPackFormat") pack.clear();
            if (n != u"audioChannelFormat") channel.clear();
            if (n != u"audioStreamFormat") stream.clear();
            if (n != u"audioTrackFormat") track.clear();
            if (n != u"audioTrackUID") uid.clear();
        }
    }
    if (x.hasError()) return fail("The ADM is not well-formed XML: " + x.errorString().toStdString());
    if (programmes.empty()) return fail("The ADM has no programme");
    if (programmes.size() > 1 && warnings && !frame)
        warnings->push_back("The ADM has " + std::to_string(programmes.size()) + " programmes: the first (" + programmes[0].first + ") is read");

    // Which channel of the sound each track UID is on.
    std::map<std::string, int> channelOf = transport;
    for (const AdmTrack& t : tracks) channelOf[t.uid] = t.channel;
    // A UID's channel format: through its track format and stream format, or named directly (S-ADM may), or by number
    // (BS.2094's common definitions: AT_xxxxxxxx_01 is AC_xxxxxxxx's).
    auto channelFormatOf = [&](const std::string& u) -> std::string {
        auto it = uids.find(u);
        std::string ref = it != uids.end() ? it->second.first : std::string();
        if (ref.empty())
            for (const AdmTrack& t : tracks)
                if (t.uid == u) ref = t.trackFormat;
        if (ref.rfind("AC_", 0) == 0) return ref;
        if (const auto ts = trackStream.find(ref); ts != trackStream.end() && !ts->second.empty())
            if (const auto sc = streamChannel.find(ts->second[0]); sc != streamChannel.end() && !sc->second.empty()) return sc->second[0];
        if (ref.rfind("AT_", 0) == 0 && ref.size() >= 11) return "AC_" + ref.substr(3, 8);
        return {};
    };
    auto typeOf = [&](const std::string& ac) {
        if (const auto c = channels.find(ac); c != channels.end() && !c->second.type.empty()) return c->second.type;
        // (typeLabel: the ID's first four digits)
        const std::string label = ac.size() >= 7 ? ac.substr(3, 4) : std::string();
        return std::string(label == "0001" ? "DirectSpeakers" : label == "0003" ? "Objects" : label == "0002" ? "Matrix" : label == "0004" ? "HOA" : "Binaural");
    };
    // The common definitions' speakers by channel format.
    auto commonLabel = [](const std::string& ac) -> std::string {
        for (const auto& [label, id] : commonChannels())
            if (QString::fromStdString(id).compare(QString::fromStdString(ac), Qt::CaseInsensitive) == 0) return label;
        return {};
    };

    // The first programme's objects, nested ones included (to 16 deep, as BS.2076 nests a few at most), in order.
    std::vector<std::string> wanted;
    std::set<std::string> seen;
    for (const std::string& c : programmes[0].second)
        for (const std::string& top : contents[c]) {
            std::vector<std::pair<std::string, int>> stack{{top, 0}};
            while (!stack.empty() && seen.size() < 4096) {
                const auto [id, depth] = stack.back();
                stack.pop_back();
                if (!seen.insert(id).second) continue;
                const auto o = objects.find(id);
                if (o == objects.end()) continue;
                wanted.push_back(id);
                if (depth < 16)
                    for (auto inner = o->second.objects.rbegin(); inner != o->second.objects.rend(); ++inner) stack.push_back({*inner, depth + 1});
            }
        }
    const int64_t progStart = programmeStart[firstProgramme];
    const int64_t frameEnd = frameStart >= 0 && frameLength >= 0 ? frameStart - progStart + frameLength : -1;

    if (out.title.empty()) out.title = programmes[0].first;
    std::map<std::string, int> speakers;  // label: channel
    std::vector<std::string> skipped;
    std::map<int, size_t> objectOn;  // out.objects by channel
    for (size_t k = 0; k < out.objects.size(); ++k) objectOn[out.objects[k].channel] = k;
    bool tooMany = false;
    for (const std::string& id : wanted) {
        const ParsedObject& o = objects[id];
        for (const std::string& u : o.uids) {
            const auto ch = channelOf.find(u);
            if (ch == channelOf.end() || ch->second < 0) continue;  // (a silent track, or one this sound does not carry)
            const std::string ac = channelFormatOf(u);
            const std::string type = typeOf(ac);
            if (type == "DirectSpeakers") {
                std::string label = commonLabel(ac);
                if (const auto c = channels.find(ac); c != channels.end())
                    for (const ParsedBlock& b : c->second.blocks)
                        if (!b.speaker.empty()) label = b.speaker;
                if (label.empty()) skipped.push_back(o.name + " (" + ac + ")");
                else speakers[label] = ch->second;
            } else if (type == "Objects") {
                AdmScene::Object* obj = nullptr;
                if (const auto e = objectOn.find(ch->second); e != objectOn.end()) obj = &out.objects[e->second];
                if (!obj) {
                    // (as many as an ADM master Montage writes holds)
                    if (out.objects.size() >= 128) {
                        tooMany = true;
                        continue;
                    }
                    objectOn[ch->second] = out.objects.size();
                    out.objects.push_back({o.name.empty() ? ac : o.name, ch->second, {}});
                    obj = &out.objects.back();
                }
                const auto c = channels.find(ac);
                if (c == channels.end()) continue;
                for (const ParsedBlock& b : c->second.blocks) {
                    AdmBlock nb;
                    // (rtime from the object's start, lstart from the frame's; a block without one lasts the whole object)
                    const int64_t from = b.rtime >= 0 ? (local ? b.rtime - progStart : o.start + b.rtime) : o.start;
                    nb.from = std::max<int64_t>(0, from);
                    nb.to = b.duration >= 0 ? nb.from + b.duration : std::max<int64_t>(nb.from, frameEnd >= 0 ? frameEnd : out.samples);
                    nb.jump = b.jump;
                    auto pos = [&](const char* k, double d) {
                        const auto it = b.position.find(k);
                        return it == b.position.end() ? d : it->second;
                    };
                    if (b.cartesian || b.position.count("X")) {
                        nb.at = {std::clamp(pos("X", 0), -1.0, 1.0), std::clamp(pos("Y", 1), -1.0, 1.0), std::clamp(pos("Z", 0), -1.0, 1.0)};
                    } else {
                        // Polar: round the room by BS.2127's mapping, its elevation as height (30° and above overhead).
                        const std::array<double, 2> xy = polarToCartesian(pos("azimuth", 0), std::clamp(pos("distance", 1), 0.0, 1.0));
                        nb.at = {xy[0], xy[1], std::clamp(pos("elevation", 0) / 30.0, -1.0, 1.0)};
                    }
                    admAppendBlock(obj->blocks, nb);
                }
            } else {
                skipped.push_back(o.name + " (" + type + ")");
            }
        }
    }
    if (warnings && tooMany && !frame) warnings->push_back("The ADM has more than 128 objects: the first 128 are read");
    if (warnings && !skipped.empty() && !frame) {
        std::string list;
        for (const std::string& k : skipped) list += (list.empty() ? "" : ", ") + k;
        warnings->push_back("Not read (Montage reads speakers and objects): " + list);
    }

    if (!speakers.empty() && out.bed.empty()) admSetBed(out, speakers, frame ? nullptr : warnings);
    int most = -1;
    for (int c : out.bed) most = std::max(most, c);
    for (const AdmScene::Object& o : out.objects) most = std::max(most, o.channel);
    out.channels = std::max(out.channels, most + 1);
    int64_t end = std::max<int64_t>(0, frameEnd);
    for (const AdmScene::Object& o : out.objects)
        if (!o.blocks.empty()) end = std::max(end, o.blocks.back().to);
    out.samples = std::max(out.samples, end);
    if (out.bed.empty() && out.objects.empty()) return fail("The ADM's programme has no speakers or objects Montage reads");
    return true;
}

void admSetBed(AdmScene& out, const std::map<std::string, int>& speakers, std::vector<std::string>* warnings) {
    std::string best;
    size_t bestHits = 0, bestSize = 0;
    for (const std::string& l : audioLayouts()) {
        if (ambisonicLayout(l)) continue;
        const auto& sp = layoutSpeakers(l);
        size_t hits = 0;
        for (const Speaker& k : sp) hits += speakers.count(k.label);
        if (hits > bestHits || (hits == bestHits && hits > 0 && sp.size() < bestSize)) best = l, bestHits = hits, bestSize = sp.size();
    }
    if (best.empty()) return;
    out.layout = best;
    out.bedPack = admBedPack(best);
    out.bed.clear();
    for (const Speaker& k : layoutSpeakers(best)) {
        const auto it = speakers.find(k.label);
        out.bed.push_back(it == speakers.end() ? -1 : it->second);
    }
    if (warnings && bestHits < speakers.size()) {
        std::string list;
        for (const auto& [label, c] : speakers)
            if (std::none_of(layoutSpeakers(best).begin(), layoutSpeakers(best).end(), [&](const Speaker& k) { return label == k.label; }))
                list += (list.empty() ? "" : ", ") + label;
        warnings->push_back("Speakers not in " + best + ", not read: " + list);
    }
}

void admAppendBlock(std::vector<AdmBlock>& bl, AdmBlock nb) {
    auto same = [](const AdmPoint& a, const AdmPoint& b) { return std::fabs(a.x - b.x) < 1e-9 && std::fabs(a.y - b.y) < 1e-9 && std::fabs(a.z - b.z) < 1e-9; };
    if (!bl.empty() && nb.from < bl.back().to) {
        if (nb.to <= bl.back().to) return;  // (a frame repeating what an earlier one said)
        nb.from = bl.back().to;
    }
    // (staying put: the block before ends where it is and was there all along, or jumped there)
    if (!bl.empty() && !nb.jump && nb.from == bl.back().to && same(nb.at, bl.back().at) &&
        (bl.size() < 2 || bl.back().jump || same(bl[bl.size() - 2].at, bl.back().at))) {
        bl.back().to = nb.to;
        return;
    }
    bl.push_back(nb);
}

SurroundPan admPanPosition(const AdmPoint& at) {
    SurroundPan p;
    p.width = 0;
    p.z = std::clamp(at.z, 0.0, 1.0);
    const double d = std::min(1.0, std::max(std::fabs(at.x), std::fabs(at.y)));
    if (d < 1e-9) {
        p.x = 0, p.y = 0;
        return p;
    }
    const double u = at.x / d, v = at.y / d;  // on the square's edge
    // The sector it is on (polarToCartesian's corners), how far along, and the angle that puts it there.
    struct Corner {
        double az, x, y;
    };
    static const Corner corners[5] = {{0, 0, 1}, {-30, 1, 1}, {-110, 1, -1}, {110, -1, -1}, {30, -1, 1}};
    double az = 0;
    for (int i = 0; i < 5; ++i) {
        const Corner& left = corners[i];
        const Corner& right = corners[(i + 1) % 5];
        const double dx = right.x - left.x, dy = right.y - left.y;
        const double t = std::fabs(dx) > std::fabs(dy) ? (u - left.x) / dx : (v - left.y) / dy;
        if (t < -1e-9 || t > 1 + 1e-9 || std::fabs(left.x + dx * t - u) > 1e-6 || std::fabs(left.y + dy * t - v) > 1e-6) continue;
        double rightAz = right.az, leftAz = left.az;
        while (leftAz - 360 >= rightAz) leftAz -= 360;
        while (leftAz < rightAz) leftAz += 360;
        const double mid = (leftAz + rightAz) / 2, half = rightAz - mid;
        const double w = std::tan(std::clamp(t, 0.0, 1.0) * M_PI / 2);
        const double gainR = std::isinf(w) || w > 1e12 ? 1.0 : w / (1 + w);
        az = mid + std::atan((2 * gainR - 1) * std::tan(half * M_PI / 180)) * 180 / M_PI;
        break;
    }
    p.x = -std::sin(az * M_PI / 180) * d;
    p.y = std::cos(az * M_PI / 180) * d;
    return p;
}

struct ImmersiveMix::Impl {
    const Project* p = nullptr;
    std::string layout = "stereo";
    int bedN = 2;
    bool overhead = false;
    std::vector<int> objects;
    Sequence mixSeq;
    AudioMixer bed;
    std::vector<Sequence> objectSeqs;
    std::vector<std::unique_ptr<AudioMixer>> objectMixers;
    std::vector<float> stereo;
};

ImmersiveMix::ImmersiveMix() : d_(std::make_unique<Impl>()) {}
ImmersiveMix::~ImmersiveMix() = default;
const std::string& ImmersiveMix::layout() const { return d_->layout; }
int ImmersiveMix::bedChannels() const { return d_->bedN; }
bool ImmersiveMix::overhead() const { return d_->overhead; }
const std::vector<int>& ImmersiveMix::objectTracks() const { return d_->objects; }

bool ImmersiveMix::open(const Project& p, const Sequence& s, int rate, std::string* error) {
    if (ambisonicLayout(s.audioLayout)) {
        if (error) *error = "An immersive master is a bed of speakers and objects: an ambisonic mix is delivered as a file of its field (File > Export)";
        return false;
    }
    d_ = std::make_unique<Impl>();
    Impl& d = *d_;
    d.p = &p;
    if (std::find(audioLayouts().begin(), audioLayouts().end(), s.audioLayout) != audioLayouts().end()) d.layout = s.audioLayout;
    d.bedN = layoutChannels(d.layout);
    d.overhead = std::any_of(layoutSpeakers(d.layout).begin(), layoutSpeakers(d.layout).end(), [](const Speaker& k) { return k.elevation > 0; });
    d.objects = admObjectTracks(s);
    // The bed: every track but the objects, in the layout, with the objects' LFE sends and aux sends (a reverb return is
    // part of the bed). Each object: only its track, after its fader, in stereo (centred, straight out, dry, no master
    // inserts), as one channel.
    d.mixSeq = s;
    d.mixSeq.sampleRate = rate;
    d.mixSeq.audioLayout = d.layout;
    std::vector<bool> lfeOnly(s.audioTracks.size(), false);
    for (int t : d.objects) lfeOnly[size_t(t)] = true;
    d.bed.setLfeOnly(lfeOnly);
    for (int t : d.objects) {
        Sequence o = d.mixSeq;
        o.audioLayout = "stereo";
        o.masterEffects.clear();
        Track& tr = o.audioTracks[size_t(t)];
        tr.pan = 0;
        tr.panAuto = Param{};
        tr.output = 0;
        tr.sends.clear();  // heard in the bed's returns
        tr.muted = false;
        tr.solo = false;
        for (Track& other : o.audioTracks) other.solo = false;
        d.objectSeqs.push_back(std::move(o));
        auto m = std::make_unique<AudioMixer>();
        std::vector<bool> only(s.audioTracks.size(), false);
        only[size_t(t)] = true;
        m->setTrackMask(only);
        d.objectMixers.push_back(std::move(m));
    }
    return true;
}

void ImmersiveMix::mix(int64_t start, int n, float* bed, float* const* objects) {
    Impl& d = *d_;
    if (d.bedN > 2) d.bed.mixLayout(*d.p, d.mixSeq, start, n, bed);
    else d.bed.mix(*d.p, d.mixSeq, start, n, bed);
    d.stereo.resize(size_t(n) * 2);
    for (size_t k = 0; k < d.objects.size(); ++k) {
        d.objectMixers[k]->mix(*d.p, d.objectSeqs[k], start, n, d.stereo.data());
        for (int i = 0; i < n; ++i) objects[k][i] = 0.70710678f * (d.stereo[size_t(i) * 2] + d.stereo[size_t(i) * 2 + 1]);
    }
}

AdmPoint ImmersiveMix::objectAt(size_t k, double frame) const {
    const Track& tr = d_->mixSeq.audioTracks[size_t(d_->objects[k])];
    const bool moving = automationReads(trackAutomation(tr)) && surroundAnimated(tr);
    const Position at = admPosition(moving ? trackSurroundAt(tr, frame) : tr.surround, d_->overhead);
    return {at.x, at.y, at.z};
}

bool exportAdmBwf(const Project& p, const Sequence& s, const AdmSettings& settings, const std::string& path, AdmResult* result,
                  const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint >= s.inPoint) first = s.inPoint, end = s.outPoint + 1;
    AdmScene scene;
    std::string why;
    if (!admSceneOf(p, s, first, end, settings.title, scene, &why)) return fail(why);
    ImmersiveMix mix;
    if (!mix.open(p, s, kRate, &why)) return fail(why);
    const int bedN = mix.bedChannels();
    const std::vector<int>& objects = mix.objectTracks();
    const int channels = scene.channels;
    const int64_t firstSample = int64_t(std::llround(double(first) * kRate / s.fpsValue()));
    const int64_t total = scene.samples;
    // Channel by channel: the bed's speakers, then one channel for each object.
    const std::vector<AdmTrack> chans = admTracks(scene);

    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail("Cannot write " + path);
    bool finished = false;
    struct Cleanup {
        QFile& f;
        bool& finished;
        ~Cleanup() {
            if (!finished) {
                f.close();
                f.remove();
            }
        }
    } cleanup{f, finished};

    // RIFF WAVE with room for a ds64 (BS.2088), the format, the channel assignment, then the samples; the ADM after.
    QByteArray head;
    head.append("RIFF");
    put32(head, 0);
    head.append("WAVE");
    head.append("JUNK");
    put32(head, 28);
    head.append(QByteArray(28, '\0'));
    head.append("fmt ");
    put32(head, 16);
    put16(head, 1);  // PCM
    put16(head, uint16_t(channels));
    put32(head, kRate);
    put32(head, uint32_t(kRate * 3 * channels));
    put16(head, uint16_t(3 * channels));
    put16(head, 24);
    head.append("chna");
    put32(head, uint32_t(4 + 40 * chans.size()));
    put16(head, uint16_t(chans.size()));
    put16(head, uint16_t(chans.size()));
    for (size_t i = 0; i < chans.size(); ++i) {
        put16(head, uint16_t(i + 1));
        putText(head, chans[i].uid, 12);
        putText(head, chans[i].trackFormat, 14);
        putText(head, chans[i].pack, 11);
        head.append('\0');
    }
    head.append("data");
    const qint64 dataSizeAt = head.size();
    put32(head, 0);
    if (f.write(head) != head.size()) return fail("Cannot write " + path);
    const qint64 dataStart = f.pos();

    const int chunk = 4800;
    std::vector<float> bedBuf(size_t(chunk) * size_t(bedN)), objBuf(size_t(chunk) * std::max<size_t>(1, objects.size()));
    std::vector<float*> objPtrs;
    for (size_t k = 0; k < objects.size(); ++k) objPtrs.push_back(objBuf.data() + k * size_t(chunk));
    std::vector<float> frame(size_t(chunk) * size_t(channels));
    QByteArray pcm;
    for (int64_t done = 0; done < total; done += chunk) {
        const int n = int(std::min<int64_t>(chunk, total - done));
        mix.mix(firstSample + done, n, bedBuf.data(), objPtrs.data());
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < bedN; ++c) frame[size_t(i) * size_t(channels) + size_t(c)] = bedBuf[size_t(i) * size_t(bedN) + size_t(c)];
            for (size_t k = 0; k < objects.size(); ++k) frame[size_t(i) * size_t(channels) + size_t(bedN) + k] = objPtrs[k][i];
        }
        pcm.resize(n * channels * 3);
        char* d = pcm.data();
        for (int i = 0; i < n * channels; ++i) {
            const int32_t v = int32_t(std::lround(std::clamp(double(frame[size_t(i)]), -1.0, 8388607.0 / 8388608.0) * 8388608.0));
            *d++ = char(v & 0xff);
            *d++ = char((v >> 8) & 0xff);
            *d++ = char((v >> 16) & 0xff);
        }
        if (f.write(pcm) != pcm.size()) return fail("Cannot write " + path + " (is the disk full?)");
        if (progress && (done / chunk) % 10 == 0 && !progress(double(done) / double(total))) return fail("Stopped");
    }
    const uint64_t dataBytes = uint64_t(f.pos() - dataStart);
    if (dataBytes % 2) f.write(QByteArray(1, '\0'));  // chunks are word-aligned

    // The ADM: one programme of the bed (and the objects, if any).
    const QByteArray xml = QByteArray::fromStdString(admDocument(scene));
    QByteArray axml("axml");
    put32(axml, uint32_t(xml.size()));
    axml.append(xml);
    if (xml.size() % 2) axml.append('\0');
    if (f.write(axml) != axml.size()) return fail("Cannot write " + path);

    // The sizes: in the RIFF header while they fit, else in a ds64 chunk (BW64).
    const uint64_t fileBytes = uint64_t(f.pos());
    if (fileBytes - 8 <= 0xFFFFFFFFull) {
        QByteArray v;
        put32(v, uint32_t(fileBytes - 8));
        f.seek(4);
        f.write(v);
        v.clear();
        put32(v, uint32_t(dataBytes));
        f.seek(dataSizeAt);
        f.write(v);
    } else {
        f.seek(0);
        f.write("BW64");
        QByteArray v;
        put32(v, 0xFFFFFFFFu);
        f.write(v);
        f.seek(12);
        QByteArray ds;
        ds.append("ds64");
        put32(ds, 28);
        put64(ds, fileBytes - 8);
        put64(ds, dataBytes);
        put64(ds, uint64_t(total));
        put32(ds, 0);
        f.write(ds);
        f.seek(dataSizeAt);
        f.write(v);
    }
    if (!f.flush()) return fail("Cannot write " + path);
    f.close();
    finished = true;
    if (result) {
        result->path = path;
        result->bedChannels = bedN;
        result->objects = int(objects.size());
        result->samples = total;
        result->bedPack = scene.bedPack;
    }
    if (progress) progress(1.0);
    return true;
}

bool readBwfInfo(const std::string& path, BwfInfo& info, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + path);
    char head[12];
    if (f.read(head, 12) != 12 || std::memcmp(head + 8, "WAVE", 4) != 0) return fail("Not a WAVE file");
    const bool big = std::memcmp(head, "BW64", 4) == 0 || std::memcmp(head, "RF64", 4) == 0;
    if (!big && std::memcmp(head, "RIFF", 4) != 0) return fail("Not a RIFF, RF64 or BW64 file");
    info = BwfInfo{};
    uint64_t dataSize64 = 0, dataBytes = 0;
    for (;;) {
        char ch[8];
        if (f.read(ch, 8) != 8) break;
        uint64_t size = get32(reinterpret_cast<const uint8_t*>(ch + 4));
        const std::string id(ch, 4);
        if (id == "data" && big && size == 0xFFFFFFFFu) size = dataSize64;
        if (id == "data") {
            dataBytes = size;
            f.seek(f.pos() + qint64(size + (size % 2)));
            continue;
        }
        const QByteArray body = f.read(qint64(size));
        if (uint64_t(body.size()) != size) return fail("A chunk runs past the end of the file");
        if (size % 2) f.read(1);
        const auto* b = reinterpret_cast<const uint8_t*>(body.constData());
        if (id == "ds64" && size >= 24) {
            dataSize64 = get64(b + 8);
        } else if (id == "fmt " && size >= 16) {
            info.channels = get16(b + 2);
            info.sampleRate = int(get32(b + 4));
            info.bits = get16(b + 14);
        } else if (id == "chna" && size >= 4) {
            const int n = get16(b + 2);
            for (int i = 0; i < n && 4 + size_t(i + 1) * 40 <= size; ++i) {
                const uint8_t* e = b + 4 + size_t(i) * 40;
                auto text = [](const uint8_t* q, int len) {
                    std::string s(reinterpret_cast<const char*>(q), size_t(len));
                    s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
                    return s;
                };
                info.chna.push_back({get16(e), text(e + 2, 12), text(e + 14, 14), text(e + 28, 11)});
            }
        } else if (id == "axml") {
            info.axml = body.toStdString();
        }
    }
    if (info.channels > 0 && info.bits > 0) info.frames = int64_t(dataBytes / (uint64_t(info.channels) * uint64_t(info.bits / 8)));
    if (info.channels <= 0) return fail("No format chunk");
    return true;
}

}  // namespace montage
