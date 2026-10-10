#include "Adm.h"

#include <QFile>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

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

struct Channel {
    std::string uid, trackFormat, pack;
};

// Where an object is, in ADM's polar coordinates, as the panner places it: its angle round the room (ADM counts
// azimuth positive to the left), up to the overhead speakers' elevation (`top`) as far as it is raised, and its
// distance.
struct Polar {
    double az = 0, el = 0, dist = 1;
};
Polar admPolar(const SurroundPan& sp, double top) {
    Polar p;
    p.az = -std::atan2(sp.x, sp.y) * 180 / M_PI;
    p.dist = std::min(1.0, std::hypot(sp.x, sp.y));
    p.el = std::clamp(sp.z, 0.0, 1.0) * top;
    return p;
}

// One audioBlockFormat: from `from` to `to` (samples into the master), moving to `at` over its length (or jumping
// there at once).
struct Block {
    int64_t from = 0, to = 0;
    Polar at;
    bool jump = false;
};

// An object's blocks. Still: one. Moving (its position lanes play): a millisecond at where it starts, then the
// position ten times a second, each block gliding to the next point; points on a straight line (within half a degree
// and 0.005 of distance) share a block, and a step across the back (azimuth ±180°) jumps instead of sweeping round
// the front. Boundaries fall on whole samples that hh:mm:ss.fffff writes exactly (every 12 at 48 kHz).
std::vector<Block> objectBlocks(const Track& tr, double top, FrameTime first, FrameTime end, double fps, int64_t total) {
    const bool reads = trackAutomation(tr) == AutomationMode::Read || trackAutomation(tr) == AutomationMode::Latch ||
                       trackAutomation(tr) == AutomationMode::Touch;
    if (!reads || !surroundAnimated(tr)) return {{0, total, admPolar(tr.surround, top), false}};
    auto sampleAt = [&](FrameTime f) {
        const int64_t v = int64_t(std::llround(double(f - first) * kRate / fps));
        return std::min(total, v / 12 * 12);
    };
    const FrameTime step = std::max<FrameTime>(1, FrameTime(std::llround(fps / 10)));
    std::vector<std::pair<int64_t, Polar>> points;
    for (FrameTime f = first; f < end; f += step) points.push_back({sampleAt(f), admPolar(trackSurroundAt(tr, double(f)), top)});
    points.push_back({total, admPolar(trackSurroundAt(tr, double(end)), top)});
    std::vector<Block> out{{0, std::min<int64_t>(total, kRate / 1000), points[0].second, false}};
    auto onLine = [](const Polar& a, const Polar& b, double u, const Polar& m) {
        return std::fabs(a.az + (b.az - a.az) * u - m.az) < 0.5 && std::fabs(a.el + (b.el - a.el) * u - m.el) < 0.5 &&
               std::fabs(a.dist + (b.dist - a.dist) * u - m.dist) < 0.005;
    };
    size_t i = 1;
    while (i < points.size()) {
        const int64_t from = out.back().to;
        if (points[i].first <= from) {  // a point the lead-in already covers
            ++i;
            continue;
        }
        const Polar start = out.back().at;
        if (std::fabs(points[i].second.az - start.az) > 180) {
            out.push_back({from, points[i].first, points[i].second, true});
            ++i;
            continue;
        }
        // As far along as the points between stay on the straight line (at most 200 points to a block).
        size_t j = i;
        while (j + 1 < points.size() && j - i < 200 && std::fabs(points[j + 1].second.az - points[j].second.az) <= 180) {
            const size_t k = j + 1;
            bool straight = true;
            for (size_t m = i; m <= j && straight; ++m) {
                const double u = double(points[m].first - from) / double(points[k].first - from);
                straight = onLine(start, points[k].second, u, points[m].second);
            }
            if (!straight) break;
            j = k;
        }
        out.push_back({from, points[j].first, points[j].second, false});
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

bool exportAdmBwf(const Project& p, const Sequence& s, const AdmSettings& settings, const std::string& path, AdmResult* result,
                  const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string layout = std::find(audioLayouts().begin(), audioLayouts().end(), s.audioLayout) != audioLayouts().end()
                                   ? s.audioLayout
                                   : std::string("stereo");
    const int bedN = layoutChannels(layout);
    const std::vector<std::string> bedChannels = admBedChannels(layout);
    if (std::any_of(bedChannels.begin(), bedChannels.end(), [](const std::string& c) { return c.empty(); }))
        return fail("The sequence's speakers have no ADM names");
    const std::vector<int> objects = admObjectTracks(s);
    const int channels = bedN + int(objects.size());
    if (channels > 128) return fail("An ADM master here holds up to 128 channels: the bed and at most " + std::to_string(128 - bedN) + " objects");
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint >= s.inPoint) first = s.inPoint, end = s.outPoint + 1;
    if (end <= first) return fail("The sequence is empty");
    const double fps = s.fpsValue();
    const int64_t firstSample = int64_t(std::llround(double(first) * kRate / fps));
    const int64_t total = int64_t(std::llround(double(end - first) * kRate / fps));

    // Channel by channel: the bed's speakers, then one channel for each object.
    std::vector<Channel> chans;
    const std::string bedPack = admBedPack(layout);
    for (int i = 0; i < bedN; ++i) chans.push_back({"", "AT_" + bedChannels[size_t(i)].substr(3) + "_01", bedPack});  // BS.2094's
    for (size_t k = 0; k < objects.size(); ++k) {
        const QString hex = QString::asprintf("%04X", unsigned(0x1001 + k));
        chans.push_back({"", ("AT_0003" + hex + "_01").toStdString(), ("AP_0003" + hex).toStdString()});
    }
    for (size_t i = 0; i < chans.size(); ++i) chans[i].uid = QString::asprintf("ATU_%08X", unsigned(i + 1)).toStdString();

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

    // The bed: every track but the objects, in the layout. Each object: only its track, after its fader, in stereo
    // (centred, straight out, no master inserts), as one channel.
    Sequence mixSeq = s;
    mixSeq.sampleRate = kRate;
    mixSeq.audioLayout = layout;
    // Objects keep their LFE send in the bed.
    std::vector<bool> lfeOnly(s.audioTracks.size(), false);
    for (int t : objects) lfeOnly[size_t(t)] = true;
    AudioMixer bed;
    bed.setLfeOnly(lfeOnly);
    std::vector<Sequence> objectSeqs;
    std::vector<std::unique_ptr<AudioMixer>> objectMixers;
    for (int t : objects) {
        Sequence o = mixSeq;
        o.audioLayout = "stereo";
        o.masterEffects.clear();
        Track& tr = o.audioTracks[size_t(t)];
        tr.pan = 0;
        tr.panAuto = Param{};
        tr.output = 0;
        tr.muted = false;
        tr.solo = false;
        for (Track& other : o.audioTracks) other.solo = false;
        objectSeqs.push_back(std::move(o));
        auto m = std::make_unique<AudioMixer>();
        std::vector<bool> only(s.audioTracks.size(), false);
        only[size_t(t)] = true;
        m->setTrackMask(only);
        objectMixers.push_back(std::move(m));
    }
    const int chunk = 4800;
    std::vector<float> bedBuf(size_t(chunk) * size_t(bedN)), objBuf(size_t(chunk) * 2);
    std::vector<float> frame(size_t(chunk) * size_t(channels));
    QByteArray pcm;
    for (int64_t done = 0; done < total; done += chunk) {
        const int n = int(std::min<int64_t>(chunk, total - done));
        if (bedN > 2) bed.mixLayout(p, mixSeq, firstSample + done, n, bedBuf.data());
        else bed.mix(p, mixSeq, firstSample + done, n, bedBuf.data());
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < bedN; ++c) frame[size_t(i) * size_t(channels) + size_t(c)] = bedBuf[size_t(i) * size_t(bedN) + size_t(c)];
        for (size_t k = 0; k < objects.size(); ++k) {
            objectMixers[k]->mix(p, objectSeqs[k], firstSample + done, n, objBuf.data());
            for (int i = 0; i < n; ++i)
                frame[size_t(i) * size_t(channels) + size_t(bedN) + k] = 0.70710678f * (objBuf[size_t(i) * 2] + objBuf[size_t(i) * 2 + 1]);
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
    QByteArray xml;
    {
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
        const QString title = QString::fromStdString(settings.title.empty() ? s.name : settings.title);
        const QString duration = admTime(total);
        x.writeStartElement("audioProgramme");
        x.writeAttribute("audioProgrammeID", "APR_1001");
        x.writeAttribute("audioProgrammeName", title);
        x.writeAttribute("start", admTime(0));
        x.writeAttribute("end", duration);
        x.writeTextElement("audioContentIDRef", "ACO_1001");
        if (!objects.empty()) x.writeTextElement("audioContentIDRef", "ACO_1002");
        x.writeEndElement();
        x.writeStartElement("audioContent");
        x.writeAttribute("audioContentID", "ACO_1001");
        x.writeAttribute("audioContentName", "Bed");
        x.writeTextElement("audioObjectIDRef", "AO_1001");
        x.writeEndElement();
        if (!objects.empty()) {
            x.writeStartElement("audioContent");
            x.writeAttribute("audioContentID", "ACO_1002");
            x.writeAttribute("audioContentName", "Objects");
            for (size_t k = 0; k < objects.size(); ++k) x.writeTextElement("audioObjectIDRef", QString::asprintf("AO_%04X", unsigned(0x1002 + k)));
            x.writeEndElement();
        }
        x.writeStartElement("audioObject");
        x.writeAttribute("audioObjectID", "AO_1001");
        x.writeAttribute("audioObjectName", "Bed");
        x.writeAttribute("start", admTime(0));
        x.writeAttribute("duration", duration);
        x.writeTextElement("audioPackFormatIDRef", QString::fromStdString(bedPack));
        for (int i = 0; i < bedN; ++i) x.writeTextElement("audioTrackUIDRef", QString::fromStdString(chans[size_t(i)].uid));
        x.writeEndElement();
        for (size_t k = 0; k < objects.size(); ++k) {
            const Track& tr = s.audioTracks[size_t(objects[k])];
            x.writeStartElement("audioObject");
            x.writeAttribute("audioObjectID", QString::asprintf("AO_%04X", unsigned(0x1002 + k)));
            x.writeAttribute("audioObjectName", QString::fromStdString(tr.name));
            x.writeAttribute("start", admTime(0));
            x.writeAttribute("duration", duration);
            x.writeTextElement("audioPackFormatIDRef", QString::fromStdString(chans[size_t(bedN) + k].pack));
            x.writeTextElement("audioTrackUIDRef", QString::fromStdString(chans[size_t(bedN) + k].uid));
            x.writeEndElement();
        }
        if (bedPack == kOwnBedPack) {
            x.writeStartElement("audioPackFormat");
            x.writeAttribute("audioPackFormatID", kOwnBedPack);
            x.writeAttribute("audioPackFormatName", "7.1.2");
            x.writeAttribute("typeLabel", "0001");
            x.writeAttribute("typeDefinition", "DirectSpeakers");
            for (const std::string& c : bedChannels) x.writeTextElement("audioChannelFormatIDRef", QString::fromStdString(c));
            x.writeEndElement();
        }
        for (size_t k = 0; k < objects.size(); ++k) {
            const Track& tr = s.audioTracks[size_t(objects[k])];
            const QString hex = QString::asprintf("%04X", unsigned(0x1001 + k)), name = QString::fromStdString(tr.name);
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
            double top = 0;
            for (const Speaker& k : layoutSpeakers(layout)) top = std::max(top, k.elevation);
            const std::vector<Block> blocks = objectBlocks(tr, top, first, end, fps, total);
            for (size_t b = 0; b < blocks.size(); ++b) {
                const Block& bl = blocks[b];
                x.writeStartElement("audioBlockFormat");
                x.writeAttribute("audioBlockFormatID", "AB_0003" + hex + QString::asprintf("_%08X", unsigned(b + 1)));
                x.writeAttribute("rtime", admTime(bl.from));
                x.writeAttribute("duration", blocks.size() == 1 ? duration : admTime(bl.to - bl.from));
                if (bl.jump) {
                    x.writeStartElement("jumpPosition");
                    x.writeAttribute("interpolationLength", "0");
                    x.writeCharacters("1");
                    x.writeEndElement();
                }
                for (auto [coord, v] : {std::pair<const char*, double>{"azimuth", bl.at.az}, {"elevation", bl.at.el}, {"distance", bl.at.dist}}) {
                    x.writeStartElement("position");
                    x.writeAttribute("coordinate", coord);
                    x.writeCharacters(number(std::abs(v) < 5e-7 ? 0.0 : v));
                    x.writeEndElement();
                }
                x.writeEndElement();  // audioBlockFormat
            }
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
        for (const Channel& c : chans) {
            x.writeStartElement("audioTrackUID");
            x.writeAttribute("UID", QString::fromStdString(c.uid));
            x.writeAttribute("sampleRate", QString::number(kRate));
            x.writeAttribute("bitDepth", "24");
            x.writeTextElement("audioTrackFormatIDRef", QString::fromStdString(c.trackFormat));
            x.writeTextElement("audioPackFormatIDRef", QString::fromStdString(c.pack));
            x.writeEndElement();
        }
        x.writeEndElement();  // audioFormatExtended
        x.writeEndElement();  // format
        x.writeEndElement();  // coreMetadata
        x.writeEndElement();  // ebuCoreMain
        x.writeEndDocument();
    }
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
        result->bedPack = bedPack;
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
