#include "BroadcastMxf.h"

#include <QDate>
#include <QTemporaryFile>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>

#include "../core/Cea708.h"
#include "../core/History.h"
#include "../core/Teletext.h"

#ifdef MONTAGE_WITH_BMX
#include <bmx/BMXException.h>
#include <bmx/Logging.h>
#include <bmx/MXFUtils.h>
#include <bmx/apps/AS11Helper.h>
#include <bmx/as11/AS11CoreFramework.h>
#include <bmx/as11/AS11DMS.h>
#include <bmx/as11/UKDPPDMS.h>
#include <bmx/as11/AS11Info.h>
#include <bmx/as11/AS11Labels.h>
#include <bmx/as11/AS11SegmentationFramework.h>
#include <bmx/as11/UKDPPFramework.h>
#include <bmx/clip_writer/ClipWriter.h>
#include <bmx/mxf_helper/MXFFileFactory.h>
#include <bmx/mxf_op1a/OP1AFile.h>
#include <bmx/mxf_reader/MXFFileReader.h>
#include <bmx/rdd9_mxf/RDD9File.h>
#include <bmx/st436/ST436Element.h>
#endif

namespace montage {

const std::vector<std::string>& as11CoreProperties() {
    static const std::vector<std::string> v = {"SeriesTitle",       "ProgrammeTitle",       "EpisodeTitleNumber",    "ShimName",
                                               "ShimVersion",       "AudioTrackLayout",     "PrimaryAudioLanguage",  "ClosedCaptionsPresent",
                                               "ClosedCaptionsType", "ClosedCaptionsLanguage"};
    return v;
}

const std::vector<std::string>& ukDppProperties() {
    static const std::vector<std::string> v = {
        "ProductionNumber", "Synopsis", "Originator", "CopyrightYear", "OtherIdentifier", "OtherIdentifierType", "Genre", "Distributor",
        "PictureRatio", "ThreeD", "ThreeDType", "ProductPlacement", "PSEPass", "PSEManufacturer", "PSEVersion", "VideoComments",
        "SecondaryAudioLanguage", "TertiaryAudioLanguage", "AudioLoudnessStandard", "AudioComments", "LineUpStart", "IdentClockStart",
        "TotalNumberOfParts", "TotalProgrammeDuration", "AudioDescriptionPresent", "AudioDescriptionType", "OpenCaptionsPresent",
        "OpenCaptionsType", "OpenCaptionsLanguage", "SigningPresent", "SignLanguage", "CompletionDate", "TextlessElementsExist",
        "ProgrammeHasText", "ProgrammeTextLanguage", "ContactEmail", "ContactTelephoneNumber"};
    return v;
}

std::vector<std::string> missingAs11(const BroadcastMxfOptions& o) {
    std::vector<std::string> missing;
    if (o.shim.rfind("as11", 0) != 0 || (o.shim != "as11-dpp" && o.dpp.empty())) return missing;
    for (const char* k : {"ProductionNumber", "Synopsis", "Originator", "ContactEmail", "ContactTelephoneNumber"}) {
        const auto it = o.dpp.find(k);
        if (it == o.dpp.end() || it->second.empty()) missing.push_back(k);
    }
    return missing;
}

void ancCaptions(const BroadcastMxfInfo& info, int page, std::vector<Caption>& cea, std::vector<Caption>& teletext) {
    cea.clear();
    teletext.clear();
    const Rational rate = info.editRate.valid() ? info.editRate : Rational{25, 1};
    std::vector<std::pair<double, CcTriplet>> triplets;
    std::vector<std::vector<TeletextPacket>> fields;
    size_t perFrame = 1;
    for (const std::vector<AncPacket>& frame : info.anc) {
        size_t sdps = 0;
        for (const AncPacket& p : frame)
            if (p.did == 0x43 && p.sdid == 0x02) ++sdps;
        perFrame = std::max(perFrame, sdps);
    }
    for (size_t f = 0; f < info.anc.size(); ++f) {
        size_t sdps = 0;
        for (const AncPacket& p : info.anc[f]) {
            if (p.did == 0x61 && p.sdid == 0x01) {
                std::vector<CcTriplet> cc;
                if (parseCdp(p.data.data(), p.data.size(), cc))
                    for (const CcTriplet& c : cc) triplets.push_back({double(f) / rate.toDouble(), c});
            } else if (p.did == 0x43 && p.sdid == 0x02 && sdps < perFrame) {
                std::vector<TeletextPacket> packets;
                parseOp47Sdp(p.data.data(), p.data.size(), packets);
                fields.push_back(std::move(packets));
                ++sdps;
            }
        }
        if (!fields.empty())
            for (; sdps < perFrame; ++sdps) fields.emplace_back();  // (a field without its packet)
    }
    if (!triplets.empty()) captionsFromCc(triplets, rate, cea);
    if (!fields.empty()) teletextSubtitles(fields, rate, page, teletext, perFrame == 2);
}

#ifndef MONTAGE_WITH_BMX

bool broadcastMxfAvailable() { return false; }

bool rewrapBroadcastMxf(const std::string&, const std::string&, const BroadcastMxfOptions&, std::string* error) {
    if (error) *error = "This build of Montage has no broadcast MXF writer (bmx)";
    return false;
}

bool readBroadcastMxf(const std::string&, BroadcastMxfInfo&, std::string* error) {
    if (error) *error = "This build of Montage has no broadcast MXF reader (bmx)";
    return false;
}

#else

namespace {

// bmx keeps its logging and some state in globals: one file at a time, its warnings and errors kept for the message.
std::mutex& bmxMutex() {
    static std::mutex m;
    return m;
}

std::string& bmxLog() {
    static std::string s;
    return s;
}

void captureVlog2(bmx::LogLevel level, const char* source, const char* format, va_list args) {
    if (level < bmx::WARN_LOG) return;
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, format, args);
    std::string line = buf;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return;
    (void)source;
    std::string& log = bmxLog();
    if (log.size() < 4000) log += (log.empty() ? "" : "; ") + line;
}

void captureVlog(bmx::LogLevel level, const char* format, va_list args) { captureVlog2(level, nullptr, format, args); }

void captureLog(bmx::LogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    captureVlog2(level, nullptr, format, args);
    va_end(args);
}

struct BmxSession {
    std::lock_guard<std::mutex> lock{bmxMutex()};
    BmxSession() {
        static bool connected = false;
        if (!connected) {
            bmx::log = captureLog;
            bmx::vlog = captureVlog;
            bmx::vlog2 = captureVlog2;
            bmx::LOG_LEVEL = bmx::WARN_LOG;
            bmx::connect_libmxf_logging();
            connected = true;
        }
        bmxLog().clear();
    }
};

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

std::string withLog(const std::string& why) {
    const std::string& log = bmxLog();
    return log.empty() || why.find(log) != std::string::npos ? why : why + " (" + log + ")";
}

bool isMpeg2Hd(bmx::EssenceType t) {
    return t == bmx::MPEG2LG_422P_HL_1080I || t == bmx::MPEG2LG_422P_HL_1080P || t == bmx::MPEG2LG_422P_HL_720P ||
           t == bmx::MPEG2LG_MP_HL_1920_1080I || t == bmx::MPEG2LG_MP_HL_1920_1080P || t == bmx::MPEG2LG_MP_HL_720P;
}

bool isAvcIntra(bmx::EssenceType t) {
    switch (t) {
    case bmx::AVCI200_1080I: case bmx::AVCI200_1080P: case bmx::AVCI200_720P:
    case bmx::AVCI100_1080I: case bmx::AVCI100_1080P: case bmx::AVCI100_720P:
    case bmx::AVCI50_1080I: case bmx::AVCI50_1080P: case bmx::AVCI50_720P:
        return true;
    default:
        return false;
    }
}

bmx::Timecode bmxTimecode(Rational rate, bool drop, int64_t frames) {
    return bmx::Timecode(bmx::Rational{rate.num, rate.den}, drop, frames);
}

std::string tcText(const bmx::Timecode& tc) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d%c%02d", tc.GetHour(), tc.GetMin(), tc.GetSec(), tc.IsDropFrame() ? ';' : ':', tc.GetFrame());
    return buf;
}

std::string boolText(bool b) { return b ? "true" : "false"; }

}  // namespace

bool broadcastMxfAvailable() { return true; }

bool rewrapBroadcastMxf(const std::string& in, const std::string& out, const BroadcastMxfOptions& o, std::string* error) {
    using namespace bmx;
    const bool as11 = o.shim.rfind("as11", 0) == 0;
    const bool rdd9 = o.shim == "rdd9" || o.shim == "as11-x8";
    if (!rdd9 && o.shim != "as11-dpp" && o.shim != "as11-x2") return fail(error, "Unknown broadcast MXF delivery \"" + o.shim + "\"");
    BmxSession session;
    std::unique_ptr<ClipWriter> clip;
    bool opened = false;
    auto failed = [&](const std::string& why) {
        clip.reset();
        if (opened) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(out), ec);
        }
        return fail(error, withLog(why));
    };
    try {
        MXFFileReader reader;
        const MXFFileReader::OpenResult r = reader.Open(in);
        if (r != MXFFileReader::MXF_RESULT_SUCCESS) return failed("Could not read the MXF file to rewrap: " + MXFFileReader::ResultToString(r));
        if (!reader.IsComplete()) return failed("The MXF file to rewrap is incomplete");
        const mxfRational editRate = reader.GetEditRate();
        const Rational rate{int(editRate.numerator), int(editRate.denominator)};
        const int64_t duration = reader.GetDuration();
        if (duration <= 0) return failed("The MXF file to rewrap has no frames");

        // The source's tracks: one picture, then PCM, each channel a mono track of its own.
        struct Source {
            size_t reader = 0;
            uint32_t channel = 0, channels = 1, bytesPerSample = 0;
            uint32_t output = 0;
        };
        std::vector<Source> sources;
        const MXFPictureTrackInfo* picture = nullptr;
        size_t pictureReader = 0;
        for (size_t i = 0; i < reader.GetNumTrackReaders(); ++i) {
            MXFTrackReader* tr = reader.GetTrackReader(i);
            const MXFTrackInfo* info = tr->GetTrackInfo();
            if (const auto* pic = dynamic_cast<const MXFPictureTrackInfo*>(info)) {
                if (picture) {
                    tr->SetEnable(false);
                    continue;
                }
                picture = pic;
                pictureReader = i;
            } else if (dynamic_cast<const MXFSoundTrackInfo*>(info) && info->essence_type == WAVE_PCM) {
            } else {
                tr->SetEnable(false);  // (ancillary and other data: replaced, not copied)
            }
        }
        if (!picture) return failed("The MXF file to rewrap has no picture");
        const EssenceType pictureType = picture->essence_type;
        if (rdd9 && !isMpeg2Hd(pictureType))
            return failed(std::string(o.shim == "rdd9" ? "RDD 9" : "AS-11 X8") + " carries MPEG-2 HD (XDCAM HD422), not " +
                          essence_type_to_string(pictureType));
        if (!rdd9 && !isAvcIntra(pictureType)) return failed("AS-11 " + std::string(o.shim == "as11-dpp" ? "UK DPP HD" : "X2") +
                                                             " carries AVC-Intra, not " + essence_type_to_string(pictureType));
        if (o.shim == "as11-dpp" && (pictureType != AVCI100_1080I || !(rate == Rational{25, 1})))
            return failed("UK DPP HD is AVC-Intra 100 at 1080i25 (interlaced, 25 frames a second)");

        DefaultMXFFileFactory files;
        int flavour;
        if (rdd9) {
            flavour = as11 ? RDD9_AS11_FLAVOUR : 0;
            clip.reset(ClipWriter::OpenNewRDD9Clip(flavour, files.OpenNew(out), editRate));
        } else {
            flavour = OP1A_DEFAULT_FLAVOUR | OP1A_AS11_FLAVOUR | OP1A_MP_TRACK_NUMBER_FLAVOUR;
            clip.reset(ClipWriter::OpenNewOP1AClip(flavour, files.OpenNew(out), editRate));
        }
        opened = true;

        // The start timecode asked for, else the source's.
        Timecode start = bmxTimecode(rate, isDropFrameRate(rate), 0);
        if (!o.startTimecode.empty()) {
            FrameTime f = 0;
            if (!parseTimecode(o.startTimecode, rate, f)) return failed("Not a timecode: " + o.startTimecode);
            start = bmxTimecode(rate, isDropFrameRate(rate) && o.startTimecode.find(';') != std::string::npos, f);
        } else if (reader.HaveMaterialTimecode()) {
            start = reader.GetMaterialTimecode(0);
        } else if (reader.HaveFileSourceTimecode()) {
            start = reader.GetFileSourceTimecode(0);
        }
        clip->SetStartTimecode(start);
        if (!o.clipName.empty()) clip->SetClipName(o.clipName);
        mxfProductVersion version = {0, 0, 0, 0, 0};
        std::sscanf(MONTAGE_VERSION, "%hu.%hu.%hu", &version.major, &version.minor, &version.patch);
        static const mxfUUID productUid = {0x4d, 0x6f, 0x6e, 0x74, 0x61, 0x67, 0x45, 0x8a, 0x9c, 0x31, 0x52, 0x0e, 0x7b, 0x2d, 0x61, 0x10};
        clip->SetProductInfo("Montage", "Montage", version, MONTAGE_VERSION, productUid);

        // Output tracks: the picture, the sound (a mono track a channel), then the ancillary data.
        ClipWriterTrack* pic = clip->CreateTrack(pictureType);
        pic->SetAspectRatio(picture->aspect_ratio);
        if (picture->afd) pic->SetAFD(picture->afd);
        if (isAvcIntra(pictureType)) {
            MXFTrackReader* tr = reader.GetTrackReader(pictureReader);
            pic->SetAVCIMode(AVCI_ALL_FRAME_HEADER_MODE);
            if (tr->HaveAVCIHeader()) pic->SetAVCIHeader(tr->GetAVCIHeader(), AVCI_HEADER_SIZE);
            else return failed("The AVC-Intra picture has no header to carry");
        }
        sources.push_back({pictureReader, 0, 1, 0, 0});
        uint32_t outputs = 1;
        for (size_t i = 0; i < reader.GetNumTrackReaders(); ++i) {
            MXFTrackReader* tr = reader.GetTrackReader(i);
            if (!tr->IsEnabled()) continue;
            const auto* sound = dynamic_cast<const MXFSoundTrackInfo*>(tr->GetTrackInfo());
            if (!sound) continue;
            const uint32_t channels = std::max<uint32_t>(sound->channel_count, 1);
            for (uint32_t c = 0; c < channels; ++c) {
                ClipWriterTrack* t = clip->CreateTrack(WAVE_PCM);
                t->SetSamplingRate(sound->sampling_rate);
                t->SetQuantizationBits(sound->bits_per_sample);
                t->SetChannelCount(1);
                t->SetLocked(true);
                sources.push_back({i, c, channels, (sound->bits_per_sample + 7) / 8, outputs++});
            }
        }
        if (rdd9 && sources.size() - 1 != 2 && sources.size() - 1 != 4 && sources.size() - 1 != 8)
            return failed("RDD 9 carries 2, 4 or 8 mono sound tracks, not " + std::to_string(sources.size() - 1));
        const bool withAnc = !o.anc.empty();
        uint32_t ancOutput = 0;
        if (withAnc) {
            clip->CreateTrack(ANC_DATA);
            ancOutput = outputs++;
        }

        AS11Helper helper;
        QTemporaryFile segments;
        if (as11) {
            if (o.shim != "as11-dpp" && !helper.ParseSpecificationId(o.shim)) return failed("Unknown AS-11 shim " + o.shim);
            std::map<std::string, std::string> core = o.core;
            auto def = [](std::map<std::string, std::string>& m, const char* k, const std::string& v) {
                if (!m.count(k)) m[k] = v;
            };
            def(core, "ProgrammeTitle", o.clipName);
            // A one-off's series and episode are its programme title, as the DPP's guidance has it.
            def(core, "SeriesTitle", core["ProgrammeTitle"]);
            def(core, "EpisodeTitleNumber", core["ProgrammeTitle"]);
            def(core, "ShimName", o.shim == "as11-dpp" ? "UK DPP HD" : o.shim == "as11-x2" ? "AS-11 X2" : "AS-11 X8");
            def(core, "ShimVersion", o.shim == "as11-dpp" ? "1.1" : "1.0");
            def(core, "AudioTrackLayout", "3");  // EBU R 48: 2a, the programme's stereo on tracks 1 and 2
            def(core, "PrimaryAudioLanguage", "und");
            def(core, "ClosedCaptionsPresent", "false");
            for (const auto& [k, v] : core)
                if (!helper.SetFrameworkProperty("as11", k.c_str(), v.c_str())) return failed("Not an AS-11 core property: " + k);
            if (o.shim == "as11-dpp" || !o.dpp.empty()) {
                std::map<std::string, std::string> dpp = o.dpp;
                def(dpp, "CopyrightYear", std::to_string(QDate::currentDate().year()));
                def(dpp, "ThreeD", "false");
                def(dpp, "PSEPass", "2");  // not tested
                def(dpp, "SecondaryAudioLanguage", "zxx");
                def(dpp, "TertiaryAudioLanguage", "zxx");
                def(dpp, "AudioLoudnessStandard", "0");
                def(dpp, "LineUpStart", "0");
                def(dpp, "IdentClockStart", "0");
                def(dpp, "AudioDescriptionPresent", "false");
                def(dpp, "OpenCaptionsPresent", "false");
                def(dpp, "SigningPresent", "1");  // no
                def(dpp, "CompletionDate", QDate::currentDate().toString(Qt::ISODate).toStdString());
                for (const auto& [k, v] : dpp)
                    if (!helper.SetFrameworkProperty("dpp", k.c_str(), v.c_str())) return failed("Not a UK DPP property: " + k);
            }
            // The parts, as bmx reads them: part/total, the start and end timecodes.
            std::vector<MxfPart> parts = o.parts;
            if (parts.empty()) parts.push_back({0, duration, 1, 1});
            std::string text;
            int64_t last = -1;
            for (const MxfPart& p : parts) {
                if (p.start < 0 || p.duration <= 0 || p.start + p.duration > duration || p.start < last)
                    return failed("AS-11 parts run in order inside the programme, each at least a frame long");
                last = p.start + p.duration;
                const Timecode s = bmxTimecode(rate, start.IsDropFrame(), start.GetOffset() + p.start);
                const Timecode e = bmxTimecode(rate, start.IsDropFrame(), start.GetOffset() + p.start + p.duration);
                text += std::to_string(p.number) + "/" + std::to_string(p.total) + " " + tcText(s) + " E" + tcText(e) + "\n";
            }
            if (!segments.open()) return failed("Could not write the AS-11 parts");
            segments.write(text.data(), qint64(text.size()));
            segments.close();
            if (!helper.ParseSegmentationFile(segments.fileName().toLocal8Bit().constData(), editRate)) return failed("AS-11 parts refused");
        }

        clip->PrepareHeaderMetadata();
        if (as11) helper.AddMetadata(clip.get());
        clip->PrepareWrite();

        ByteArray buffer, ancBuffer;
        int64_t frame = 0;
        while (frame < duration) {
            const uint32_t n = reader.Read(1);
            if (n != 1) break;
            // Each source track's frame (sound split into its channels), in output order.
            std::vector<std::unique_ptr<Frame>> frames(reader.GetNumTrackReaders());
            for (size_t i = 0; i < reader.GetNumTrackReaders(); ++i) {
                MXFTrackReader* tr = reader.GetTrackReader(i);
                if (tr->IsEnabled()) frames[i].reset(tr->GetFrameBuffer()->GetLastFrame(true));
            }
            for (const Source& s : sources) {
                Frame* f = frames[s.reader].get();
                if (!f || f->IsEmpty()) return failed("The MXF file to rewrap ends early");
                if (s.output == 0 || s.channels == 1) {
                    const uint32_t samples = s.output == 0 ? f->num_samples : f->GetSize() / std::max<uint32_t>(s.bytesPerSample, 1);
                    clip->WriteSamples(s.output, f->GetBytes(), f->GetSize(), samples);
                    continue;
                }
                const uint32_t frameBytes = s.bytesPerSample * s.channels;
                const uint32_t samples = f->GetSize() / frameBytes;
                buffer.Allocate(samples * s.bytesPerSample);
                buffer.SetSize(samples * s.bytesPerSample);
                for (uint32_t k = 0; k < samples; ++k)
                    std::memcpy(buffer.GetBytes() + k * s.bytesPerSample, f->GetBytes() + k * frameBytes + s.channel * s.bytesPerSample,
                                s.bytesPerSample);
                clip->WriteSamples(s.output, buffer.GetBytes(), buffer.GetSize(), samples);
            }
            if (withAnc) {
                ST436Element element(false);
                std::vector<std::vector<uint8_t>> payloads;
                if (size_t(frame) < o.anc.size()) {
                    for (const AncPacket& p : o.anc[size_t(frame)]) {
                        std::vector<uint8_t> bytes{p.did, p.sdid, uint8_t(std::min<size_t>(p.data.size(), 255))};
                        bytes.insert(bytes.end(), p.data.begin(), p.data.begin() + std::min<size_t>(p.data.size(), 255));
                        payloads.push_back(std::move(bytes));
                    }
                }
                size_t k = 0;
                for (const AncPacket& p : size_t(frame) < o.anc.size() ? o.anc[size_t(frame)] : std::vector<AncPacket>{}) {
                    ST436Line line(false);
                    line.line_number = p.line;
                    line.wrapping_type = VANC_FRAME;
                    line.payload_sample_coding = ANC_8_BIT_COMP_LUMA;
                    line.payload_sample_count = uint16_t(payloads[k].size());
                    line.payload_data = payloads[k].data();
                    line.payload_size = uint32_t(payloads[k].size());
                    element.lines.push_back(line);
                    ++k;
                }
                ancBuffer.SetSize(0);
                element.Construct(&ancBuffer);
                clip->WriteSamples(ancOutput, ancBuffer.GetBytes(), ancBuffer.GetSize(), 1);
            }
            ++frame;
        }
        if (frame < duration) return failed("The MXF file to rewrap could not be read to its end");
        if (as11) helper.Complete();
        clip->CompleteWrite();
        clip.reset();
        return true;
    } catch (const BMXException& e) {
        return failed(std::string("Broadcast MXF: ") + e.what());
    } catch (const std::exception& e) {
        return failed(std::string("Broadcast MXF: ") + e.what());
    } catch (...) {
        return failed("Broadcast MXF: the file could not be written");
    }
}

bool readBroadcastMxf(const std::string& path, BroadcastMxfInfo& info, std::string* error) {
    using namespace bmx;
    info = {};
    BmxSession session;
    try {
        MXFFileReader reader;
        AS11Info::RegisterExtensions(reader.GetHeaderMetadata());
        const MXFFileReader::OpenResult r = reader.Open(path);
        if (r != MXFFileReader::MXF_RESULT_SUCCESS) return fail(error, withLog("Could not read " + path + ": " + MXFFileReader::ResultToString(r)));
        info.editRate = {int(reader.GetEditRate().numerator), int(reader.GetEditRate().denominator)};
        info.duration = reader.GetDuration();
        if (reader.HaveMaterialTimecode()) info.startTimecode = tcText(reader.GetMaterialTimecode(0));
        size_t ancReader = SIZE_MAX;
        for (size_t i = 0; i < reader.GetNumTrackReaders(); ++i) {
            MXFTrackReader* tr = reader.GetTrackReader(i);
            const MXFTrackInfo* t = tr->GetTrackInfo();
            if (const auto* pic = dynamic_cast<const MXFPictureTrackInfo*>(t)) {
                if (info.pictureEssence.empty()) {
                    info.pictureEssence = essence_type_to_enum_string(t->essence_type);
                    info.separateFields = pic->frame_layout == MXF_SEPARATE_FIELDS;
                }
            } else if (dynamic_cast<const MXFSoundTrackInfo*>(t)) {
                ++info.soundTracks;
            } else if (t->essence_type == ANC_DATA && ancReader == SIZE_MAX) {
                ancReader = i;
                continue;
            }
            tr->SetEnable(false);
        }

        AS11Info as11;
        if (as11.Read(reader.GetHeaderMetadata())) {
            for (const UL& l : as11.spec_identifiers) {
                if (l == AS11_BLOCKS_FF_2) info.as11Spec = "as11-x2";
                else if (l == AS11_BLOCKS_FF_9_WIP) info.as11Spec = "as11-x8";
            }
            if (AS11CoreFramework* c = as11.core) {
                auto& m = info.core;
                m["SeriesTitle"] = c->GetSeriesTitle();
                m["ProgrammeTitle"] = c->GetProgrammeTitle();
                m["EpisodeTitleNumber"] = c->GetEpisodeTitleNumber();
                m["ShimName"] = c->GetShimName();
                if (c->haveItem(&MXF_ITEM_K(AS11CoreFramework, AS11ShimVersion))) {
                    const mxfVersionType v = c->GetShimVersion();
                    m["ShimVersion"] = std::to_string(v >> 8) + "." + std::to_string(v & 0xFF);
                }
                m["AudioTrackLayout"] = std::to_string(c->GetAudioTrackLayout());
                m["PrimaryAudioLanguage"] = c->GetPrimaryAudioLanguage();
                m["ClosedCaptionsPresent"] = boolText(c->GetClosedCaptionsPresent());
                if (c->HaveClosedCaptionsType()) m["ClosedCaptionsType"] = std::to_string(c->GetClosedCaptionsType());
                if (c->HaveClosedCaptionsLanguage()) m["ClosedCaptionsLanguage"] = c->GetClosedCaptionsLanguage();
            }
            if (UKDPPFramework* d = as11.ukdpp) {
                auto& m = info.dpp;
                m["ProductionNumber"] = d->GetProductionNumber();
                m["Synopsis"] = d->GetSynopsis();
                m["Originator"] = d->GetOriginator();
                m["CopyrightYear"] = std::to_string(d->GetCopyrightYear());
                if (d->HaveOtherIdentifier()) m["OtherIdentifier"] = d->GetOtherIdentifier();
                if (d->HaveOtherIdentifierType()) m["OtherIdentifierType"] = d->GetOtherIdentifierType();
                if (d->HaveGenre()) m["Genre"] = d->GetGenre();
                if (d->HaveDistributor()) m["Distributor"] = d->GetDistributor();
                if (d->HavePictureRatio())
                    m["PictureRatio"] = std::to_string(d->GetPictureRatio().numerator) + "/" + std::to_string(d->GetPictureRatio().denominator);
                m["ThreeD"] = boolText(d->Get3D());
                if (d->Have3DType()) m["ThreeDType"] = std::to_string(d->Get3DType());
                if (d->HaveProductPlacement()) m["ProductPlacement"] = boolText(d->GetProductPlacement());
                m["PSEPass"] = std::to_string(d->GetPSEPass());
                if (d->HavePSEManufacturer()) m["PSEManufacturer"] = d->GetPSEManufacturer();
                if (d->HavePSEVersion()) m["PSEVersion"] = d->GetPSEVersion();
                if (d->HaveVideoComments()) m["VideoComments"] = d->GetVideoComments();
                m["SecondaryAudioLanguage"] = d->GetSecondaryAudioLanguage();
                m["TertiaryAudioLanguage"] = d->GetTertiaryAudioLanguage();
                m["AudioLoudnessStandard"] = std::to_string(d->GetAudioLoudnessStandard());
                if (d->HaveAudioComments()) m["AudioComments"] = d->GetAudioComments();
                m["LineUpStart"] = std::to_string(d->GetLineUpStart());
                m["IdentClockStart"] = std::to_string(d->GetIdentClockStart());
                m["TotalNumberOfParts"] = std::to_string(d->GetTotalNumberOfParts());
                m["TotalProgrammeDuration"] = std::to_string(d->GetTotalProgrammeDuration());
                m["AudioDescriptionPresent"] = boolText(d->GetAudioDescriptionPresent());
                if (d->HaveAudioDescriptionType()) m["AudioDescriptionType"] = std::to_string(d->GetAudioDescriptionType());
                m["OpenCaptionsPresent"] = boolText(d->GetOpenCaptionsPresent());
                if (d->HaveOpenCaptionsType()) m["OpenCaptionsType"] = std::to_string(d->GetOpenCaptionsType());
                if (d->HaveOpenCaptionsLanguage()) m["OpenCaptionsLanguage"] = d->GetOpenCaptionsLanguage();
                m["SigningPresent"] = std::to_string(d->GetSigningPresent());
                if (d->HaveSignLanguage()) m["SignLanguage"] = std::to_string(d->GetSignLanguage());
                {
                    const mxfTimestamp t = d->GetCompletionDate();
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", int(t.year), unsigned(t.month), unsigned(t.day));
                    m["CompletionDate"] = buf;
                }
                if (d->HaveTextlessElementsExist()) m["TextlessElementsExist"] = boolText(d->GetTextlessElementsExist());
                if (d->HaveProgrammeHasText()) m["ProgrammeHasText"] = boolText(d->GetProgrammeHasText());
                if (d->HaveProgrammeTextLanguage()) m["ProgrammeTextLanguage"] = d->GetProgrammeTextLanguage();
                m["ContactEmail"] = d->GetContactEmail();
                m["ContactTelephoneNumber"] = d->GetContactTelephoneNumber();
            }
            int64_t at = 0;
            for (mxfpp::StructuralComponent* c : as11.segmentation) {
                const int64_t length = c->getDuration();
                if (auto* seg = dynamic_cast<mxfpp::DMSegment*>(c); seg && seg->haveDMFramework()) {
                    if (auto* fw = dynamic_cast<AS11SegmentationFramework*>(seg->getDMFramework()))
                        info.parts.push_back({at, length, fw->GetPartNumber(), fw->GetPartTotal()});
                }
                at += length;
            }
        }

        if (ancReader != SIZE_MAX) {
            reader.GetTrackReader(ancReader)->SetEnable(true);
            while (reader.Read(1) == 1) {
                std::unique_ptr<Frame> f(reader.GetTrackReader(ancReader)->GetFrameBuffer()->GetLastFrame(true));
                std::vector<AncPacket>& packets = info.anc.emplace_back();
                if (!f || f->IsEmpty()) continue;
                ST436Element element(false);
                element.Parse(f->GetBytes(), f->GetSize());
                for (const ST436Line& line : element.lines) {
                    if (line.payload_size < 3 || line.payload_sample_coding != ANC_8_BIT_COMP_LUMA) continue;
                    AncPacket p;
                    p.line = line.line_number;
                    p.did = line.payload_data[0];
                    p.sdid = line.payload_data[1];
                    const uint32_t count = std::min<uint32_t>(line.payload_data[2], line.payload_size - 3);
                    p.data.assign(line.payload_data + 3, line.payload_data + 3 + count);
                    packets.push_back(std::move(p));
                }
            }
        }
        return true;
    } catch (const BMXException& e) {
        return fail(error, withLog(std::string("Broadcast MXF: ") + e.what()));
    } catch (const std::exception& e) {
        return fail(error, withLog(std::string("Broadcast MXF: ") + e.what()));
    } catch (...) {
        return fail(error, withLog("Broadcast MXF: the file could not be read"));
    }
}

#endif

}  // namespace montage
