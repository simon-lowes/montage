// montage-cli — headless rendering, probing and project assembly.
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QGuiApplication>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/ProjectIO.h"
#include "core/Provenance.h"
#include "core/Transcript.h"
#include "media/SpeechSearch.h"
#include "media/TextReader.h"
#include "media/Analysis.h"
#include "render/AafExport.h"
#include "media/Decoder.h"
#include "media/Loudness.h"
#ifdef MONTAGE_WITH_WHISPER
#include "automation/McpServer.h"
#include "automation/PythonHost.h"
#include "automation/ScriptHost.h"
#include "automation/ScriptPanel.h"
#include "media/Inference.h"
#include "media/Proxy.h"
#ifdef MONTAGE_LIVE_BRIDGE
#include "app/LiveBridge.h"
#endif
#include "media/Diarizer.h"
#include "media/Segmenter.h"
#include "media/SpeechEnhance.h"
#include "media/SuperScale.h"
#include "media/Translator.h"
#include "media/VisualSearch.h"
#include "media/SoundSearch.h"
#include "media/StemSeparation.h"
#include "media/Faces.h"
#include "media/DepthMap.h"
#include "media/Rife.h"
#include "media/Matting.h"
#include "media/TextToSpeech.h"
#include "media/ContentCredentials.h"
#include "media/VoiceClone.h"
#include "media/Inpaint.h"
#include "media/Transcriber.h"
#endif
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/Exporter.h"
#include "render/RenderFarm.h"

using namespace montage;

namespace {

std::atomic<bool> gCancel{false};

int usage() {
    std::fprintf(stderr,
                 "Montage %s — command line\n\n"
                 "Usage:\n"
                 "  montage-cli probe <media>\n"
                 "  montage-cli new -o <project.montage> [--size WxH] [--fps N[/D]] [--color-space ID]\n"
                 "                     [--hdr-peak NITS] <media>...\n"
                 "  montage-cli info <project.montage>\n"
                 "  montage-cli render <project.montage> -o <output> [--preset NAME] [--in TC] [--out TC]\n"
                 "                     [--width W] [--height H] [--crf N] [--vcodec C] [--acodec C] [--proxies]\n"
                 "                     [--loudness LUFS [--ceiling dBTP]]  (e.g. --loudness -14: normalise the mix)\n"
                 "                     [--burn-captions [--caption-animation none|word|highlight|pop|one-word]]\n"
                 "                     [--embed-captions] [--color-space ID]\n"
                 "                     [--downmix-stereo] [--stems tracks|buses|roles]  (surround fold-down; WAV stems beside it)\n"
                 "                     [--burn-timecode] [--burn-clip-name] [--burn-text TEXT] [--burn-corner 0-5]\n"
                 "                     [--watermark IMAGE [--watermark-corner 0-5] [--watermark-opacity 0..1]]\n"
                 "                     (corners: 0 top left, 1 top centre, 2 top right, 3-5 bottom)\n"
                 "  montage-cli farm <folder> submit <project.montage> -o <output> [--name NAME] [render's options]\n"
                 "  montage-cli farm <folder> jobs|nodes [--json]\n"
                 "  montage-cli farm <folder> cancel|retry|remove <job>\n"
                 "                     (a render farm: a folder every computer reaches; jobs wait there for render nodes)\n"
                 "  montage-cli node <folder> [--name NAME] [--map FROM=TO]... [--once] [--poll SECONDS]\n"
                 "                     (a render node: renders the farm's waiting jobs one at a time, FROM paths read as TO;\n"
                 "                      Ctrl+C gives its job back for another node)\n"
                 "  montage-cli frame <project.montage> --at TC -o <image.png>\n"
                 "  montage-cli presets\n"
                 "  montage-cli colorspaces\n"
                 "  montage-cli devices                                (where AI models can run: CPU, GPU, NPU)\n"
                 "  montage-cli scenes <video> [--sensitivity 0..1]\n"
                 "  montage-cli proxy <video> -o <proxy.mp4|.mov> [--format h264|prores_proxy|dnxhr_lb|hevc10]\n"
                 "                    [--width 960|half|quarter] [--sound]   (an editing proxy; ProRes and HEVC keep HDR)\n"
                 "  montage-cli upscale <video|image> -o <output> [--factor 2|3|4] [--strength 0..1]  (Super Scale)\n"
                 "  montage-cli loudness <media>\n"
                 "  montage-cli edl <project.montage> [-o out.edl]\n"
                 "  montage-cli otio <project.montage> [-o out.otio]\n"
                 "  montage-cli aaf <project.montage> -o <out.aaf> [--no-picture]   (Pro Tools, Fairlight, Media Composer: audio as\n"
                 "      WAVs in \"<out> Media\", video tracks linked to the original files)\n"
                 "  montage-cli xml <project.montage> [-o out.xml]       (Final Cut Pro 7 XML)\n"
                 "  montage-cli fcpxml <project.montage> [-o out.fcpxml]\n"
                 "  montage-cli import <timeline.xml|.fcpxml|.otio|.edl> -o <project.montage> [--fps N]\n"
                 "  montage-cli bench <project.montage> [--scale 0.5] [--frames 120]\n"
                 "  montage-cli transcribe <media> [--model base.en|PATH] [--language auto|en|...] [--translate] [--speakers [N]]\n"
                 "                     [--srt out.srt] [--vtt out.vtt] [--json out.json] [--txt out.txt]\n"
                 "  montage-cli models\n"
                 "  montage-cli shots <project.montage> \"a red car at night\" [--max N]   (find shots by description)\n"
                 "  montage-cli sounds <project.montage> \"applause\" [--max N]   (find sounds by description)\n"
                 "  montage-cli people <project.montage> [--person NAME|ID]   (who is in the footage, and where)\n"
                 "  montage-cli stems <file> [--stems 2|4|5] [--from S] [--to S] [-o folder]   (vocals, drums, bass... as WAVs)\n"
                 "  montage-cli speak \"text\" -o voice.wav [--voice af_heart] [--speed 1]   (a voiceover from text; --phonemes prints them)\n"
                 "  montage-cli clone-speech \"text\" --voice sample.wav -o out.wav --consent [--language en] [--from S] [--to S]\n"
                 "              [--speed 1] [--exaggeration 0.5]   (in the voice of the sample, with its owner's consent; 23 languages)\n"
                 "  montage-cli credentials <file> [--json]   (its Content Credentials: who signed it, intact or not, AI use)\n"
                 "  montage-cli sign <file> --cert chain.pem --key key.pem [--project <project.montage>] [--allow-ai-training]\n"
                 "              (Content Credentials: made in Montage, with the project's AI use and its sources' credentials)\n"
                 "  montage-cli convert-voice <speech> --voice sample.wav -o out.wav --consent [--from S] [--to S]\n"
                 "              [--voice-from S] [--voice-to S]   (the speech said again in the sample's voice, as long as it was)\n"
                 "  montage-cli script [--project <project.montage>] [--press <button> [--set id=value]...] <file.lua|.py> [arg...]\n"
                 "                                       (a Lua or Python script driving Montage's tools; montage.add_title{text =\n"
                 "                                        \"Hi\"} (Python: montage.add_title(text=\"Hi\")) calls montage_add_title.\n"
                 "                                        Python is the computer's ($MONTAGE_PYTHON, else python3; 3.8 or later).\n"
                 "                                        A script with a panel prints it, or with\n"
                 "                                        --press runs as if that button were pressed, --set filling its controls)\n"
                 "  montage-cli mcp [--live]              (Model Context Protocol server on stdio, for AI agents;\n"
                 "                                        --live: on the project open in Montage, with Tools > Agent Link on)\n"
                 "  montage-cli captions <project.montage> [-o out.srt|.vtt|.scc|.ttml|.stl|.ass] [--transcribe MODEL]\n"
                 "                     [--generate] [--import file.srt|.vtt|.scc|.ttml|.stl|.ass] [--save]\n"
                 "  montage-cli translate <subtitles.srt|.vtt> --to LANG [--from LANG] [-o out.srt|out.vtt]\n"
                 "                     (on this computer, with Opus-MT; LANG is de, fr, es, ja... see `models`)\n",
                 MONTAGE_VERSION);
    return 2;
}

bool parseFps(const std::string& s, Rational& out) {
    auto slash = s.find('/');
    try {
        if (slash == std::string::npos) {
            double v = std::stod(s);
            if (std::fabs(v - 29.97) < 0.01) out = {30000, 1001};
            else if (std::fabs(v - 23.976) < 0.01) out = {24000, 1001};
            else if (std::fabs(v - 59.94) < 0.01) out = {60000, 1001};
            else out = {int(std::lround(v)), 1};
        } else {
            out = {std::stoi(s.substr(0, slash)), std::stoi(s.substr(slash + 1))};
        }
    } catch (...) {
        return false;
    }
    return out.valid();
}

std::string kindName(MediaKind k) {
    switch (k) {
        case MediaKind::Video: return "video";
        case MediaKind::Audio: return "audio";
        case MediaKind::Image: return "image";
        case MediaKind::Sequence: return "sequence";
    }
    return "?";
}

int cmdProbe(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    MediaItem m;
    std::string err;
    if (!probeMedia(args[0], m, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("name:      %s\nkind:      %s\nduration:  %.3f s\n", m.name.c_str(), kindName(m.kind).c_str(), m.duration);
    if (m.hasVideo)
        std::printf("video:     %s %dx%d @ %.3f fps\n", m.videoCodec.c_str(), m.width, m.height, m.fps.toDouble());
    if (m.hasVideo) std::printf("colour:    %s\n", mediaColorSpace(m).label.c_str());
    if (m.hasAudio) std::printf("audio:     %s %d Hz, %d ch\n", m.audioCodec.c_str(), m.sampleRate, m.channels);
    return 0;
}

int cmdNew(const std::vector<std::string>& args) {
    std::string outPath;
    int w = 1920, h = 1080;
    Rational fps{30, 1};
    bool fpsGiven = false, sizeGiven = false;
    std::string colorSpace;
    double hdrPeak = 1000;
    std::vector<std::string> files;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-o" && i + 1 < args.size()) outPath = args[++i];
        else if (a == "--size" && i + 1 < args.size()) {
            if (std::sscanf(args[++i].c_str(), "%dx%d", &w, &h) != 2) return usage();
            sizeGiven = true;
        } else if (a == "--fps" && i + 1 < args.size()) {
            if (!parseFps(args[++i], fps)) return usage();
            fpsGiven = true;
        } else if (a == "--color-space" && i + 1 < args.size()) {
            colorSpace = args[++i];
            const ColorSpace* c = findColorSpace(colorSpace);
            if (!c || c->sceneReferred) {
                std::fprintf(stderr, "error: \"%s\" is not a sequence colour space (see `montage-cli colorspaces`)\n",
                             colorSpace.c_str());
                return 1;
            }
        } else if (a == "--hdr-peak" && i + 1 < args.size()) {
            hdrPeak = std::clamp(std::atof(args[++i].c_str()), 100.0, 10000.0);
        } else files.push_back(a);
    }
    if (outPath.empty()) return usage();
    Project p = makeDefaultProject();
    Sequence* s = p.active();
    std::vector<Id> ids;
    for (const auto& f : files) {
        MediaItem m;
        m.id = p.newId();
        std::string err;
        std::string abs = std::filesystem::absolute(f).string();
        if (!probeMedia(abs, m, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        // The first video defines the sequence settings unless given explicitly.
        if (ids.empty() && m.hasVideo && m.kind == MediaKind::Video) {
            if (!sizeGiven && m.width > 0) {
                w = m.width;
                h = m.height;
            }
            if (!fpsGiven && m.fps.valid()) fps = m.fps;
        }
        p.media.push_back(m);
        ids.push_back(m.id);
    }
    s = p.active();
    s->width = w;
    s->height = h;
    s->fps = fps;
    if (!colorSpace.empty()) s->colorSpace = colorSpace;
    s->hdrPeakNits = hdrPeak;
    FrameTime at = 0;
    for (Id id : ids) {
        auto r = edit::placeMedia(p, *s, id, at, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
        if (!r.ok) {
            std::fprintf(stderr, "warning: %s\n", r.error.c_str());
            continue;
        }
        at = s->duration();
    }
    std::string err;
    if (!saveProject(p, std::filesystem::absolute(outPath).string(), &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s (%zu media, %lld frames)\n", outPath.c_str(), ids.size(), (long long)s->duration());
    return 0;
}

bool load(const std::string& path, Project& p) {
    std::string err;
    if (!loadProject(path, p, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    checkStereoMedia(p);
    return true;
}

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::printf("Wrote %s\n", path.c_str());
    return true;
}

int cmdInfo(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    for (const auto& s : p.sequences) {
        std::printf("Sequence \"%s\" %dx%d @ %.3f fps, %d Hz, duration %s%s\n", s.name.c_str(), s.width, s.height,
                    s.fpsValue(), s.sampleRate, formatTimecode(s.duration(), s.fps).c_str(),
                    s.id == p.activeSequence ? " (active)" : "");
        if (s.multicam) std::printf("  multicam: %zu angles, %zu audio sources\n", s.videoTracks.size(), s.audioTracks.size());
        std::printf("  colour: %s%s\n", sequenceColorSpace(s).label.c_str(),
                    sequenceColorSpace(s).hdr() ? (", " + std::to_string(int(s.hdrPeakNits)) + " nits peak").c_str() : "");
        for (auto r : allTracks(s)) {
            const Track* t = trackAt(s, r);
            std::printf("  %-4s %zu clips, %zu transitions\n", t->name.c_str(), t->clips.size(), t->transitions.size());
            for (const auto& c : t->clips)
                std::printf("       [%s - %s] %s%s\n", formatTimecode(c.start, s.fps).c_str(),
                            formatTimecode(c.end(), s.fps).c_str(), c.name.c_str(), c.effects.empty() ? "" : " (fx)");
        }
    }
    return 0;
}

// `render`'s arguments (the project, then its options) made into the project to render, its settings and its stems;
// 0, or the exit code to end with. (`farm submit` takes the same.)
int prepareRender(const std::vector<std::string>& args, Project& p, ExportSettings& st, int& stems) {
    if (args.empty()) return usage();
    std::string projectPath = args[0], outPath, presetName = "H.264 - High Quality", inTc, outTc;
    st = ExportSettings();
    int width = 0, height = 0, crf = -1;
    std::string vcodec, acodec;
    bool proxies = false;
    double loudness = 0, ceiling = -1;
    int captionAnimation = -1;  // -1: as the project has it
    bool downmix = false;
    stems = 0;  // 1 per track, 2 per bus, 3 per role
    BurnIn burnIns;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "-o") outPath = next();
        else if (a == "--preset") presetName = next();
        else if (a == "--in") inTc = next();
        else if (a == "--out") outTc = next();
        else if (a == "--width") width = std::atoi(next().c_str());
        else if (a == "--height") height = std::atoi(next().c_str());
        else if (a == "--crf") crf = std::atoi(next().c_str());
        else if (a == "--vcodec") vcodec = next();
        else if (a == "--acodec") acodec = next();
        else if (a == "--proxies") proxies = true;
        else if (a == "--burn-captions") st.burnInCaptions = true;
        else if (a == "--caption-animation") {
            static const char* const names[] = {"none", "word", "highlight", "pop", "one-word"};
            const std::string v = next();
            captionAnimation = -1;
            for (int k = 0; k < 5; ++k)
                if (v == names[k]) captionAnimation = k;
            if (captionAnimation < 0) return usage();
        }
        else if (a == "--embed-captions") st.embedCaptions = true;
        else if (a == "--burn-timecode") burnIns.timecode = true;
        else if (a == "--burn-clip-name") burnIns.clipName = true;
        else if (a == "--burn-text") burnIns.text = next();
        else if (a == "--burn-corner") burnIns.corner = std::clamp(std::atoi(next().c_str()), 0, 5);
        else if (a == "--watermark") burnIns.watermark = next();
        else if (a == "--watermark-corner") burnIns.watermarkCorner = std::clamp(std::atoi(next().c_str()), 0, 5);
        else if (a == "--watermark-opacity") burnIns.watermarkOpacity = std::clamp(std::atof(next().c_str()), 0.0, 1.0);
        else if (a == "--color-space") st.colorSpace = next();
        else if (a == "--loudness") loudness = std::atof(next().c_str());
        else if (a == "--ceiling") ceiling = std::atof(next().c_str());
        else if (a == "--downmix-stereo") downmix = true;
        else if (a == "--stems") {
            const std::string v = next();
            stems = v == "tracks" ? 1 : v == "buses" ? 2 : v == "roles" ? 3 : -1;
            if (stems < 0) return usage();
        }
        else return usage();
    }
    if (outPath.empty() || loudness > 0) return usage();
    const ExportPreset* pr = findExportPreset(presetName);
    if (!pr) {
        std::fprintf(stderr, "error: unknown preset \"%s\" (see `montage-cli presets`)\n", presetName.c_str());
        return 1;
    }
    const bool burn = st.burnInCaptions, embed = st.embedCaptions;
    const std::string colorSpace = st.colorSpace;
    if (!colorSpace.empty() && (!findColorSpace(colorSpace) || findColorSpace(colorSpace)->sceneReferred)) {
        std::fprintf(stderr, "error: \"%s\" is not a delivery colour space (see `montage-cli colorspaces`)\n", colorSpace.c_str());
        return 1;
    }
    st = pr->settings;
    st.burnInCaptions = burn;
    st.embedCaptions = embed;
    st.colorSpace = colorSpace;
    st.burnIn = burnIns;
    if (!load(projectPath, p)) return 1;
    if (captionAnimation >= 0)
        for (CaptionTrack& t : p.active()->captionTracks) t.style.animation = captionAnimation;
    const Sequence* s = p.active();
    st.path = outPath;
    if (width > 0) st.width = width;
    if (height > 0) st.height = height;
    if (width > 0 && height <= 0) st.height = int(std::lround(double(width) * s->height / s->width));
    if (crf >= 0) st.crf = crf;
    if (!vcodec.empty()) st.videoCodec = vcodec;
    if (!acodec.empty()) st.audioCodec = acodec;
    st.useProxies = proxies;
    st.loudnessTarget = loudness;
    st.peakCeiling = ceiling;
    st.downmixStereo = downmix;
    if (!inTc.empty() && !parseTimecode(inTc, s->fps, st.in)) return usage();
    if (!outTc.empty() && !parseTimecode(outTc, s->fps, st.out)) return usage();
    if (st.in < 0 && s->inPoint >= 0) st.in = s->inPoint;
    if (st.out < 0 && s->outPoint >= 0) st.out = s->outPoint + 1;  // marks are inclusive
    return 0;
}

int cmdRender(const std::vector<std::string>& args) {
    Project p;
    ExportSettings st;
    int stems = 0;
    if (const int code = prepareRender(args, p, st, stems)) return code;
    const Sequence* s = p.active();
    const std::string outPath = st.path;
    std::signal(SIGINT, [](int) { gCancel = true; });
    std::string err;
    bool ok = exportSequence(
        p, *s, st,
        [](double f, FrameTime) {
            std::fprintf(stderr, "\rRendering... %5.1f%%", f * 100.0);
            std::fflush(stderr);
        },
        &gCancel, &err);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    if (stems > 0) {
        std::vector<StemFile> files;
        if (!exportStems(
                p, *s, st, stems, &files,
                [](double f, FrameTime) {
                    std::fprintf(stderr, "\rStems... %5.1f%%", f * 100.0);
                    std::fflush(stderr);
                },
                &gCancel, &err)) {
            std::fprintf(stderr, "\nerror: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "\n");
        for (const StemFile& f : files) std::printf("Wrote %s\n", f.path.c_str());
    }
    return 0;
}

// The render farm (render/RenderFarm.h): jobs sent, listed, cancelled, retried and removed.
int cmdFarm(const std::vector<std::string>& args) {
    if (args.size() < 2) return usage();
    const std::string farm = args[0], what = args[1];
    std::string err;
    if (what == "submit") {
        // `render`'s arguments, plus --name.
        std::vector<std::string> rest(args.begin() + 2, args.end());
        std::string name;
        for (size_t i = 0; i + 1 < rest.size(); ++i)
            if (rest[i] == "--name") {
                name = rest[i + 1];
                rest.erase(rest.begin() + long(i), rest.begin() + long(i) + 2);
                break;
            }
        Project p;
        ExportSettings st;
        int stems = 0;
        if (const int code = prepareRender(rest, p, st, stems)) return code;
        // (Where the nodes write, as this computer names it; each node maps it to its own.)
        st.path = QFileInfo(QString::fromStdString(st.path)).absoluteFilePath().toStdString();
        if (name.empty()) name = p.active()->name + " - " + QFileInfo(QString::fromStdString(st.path)).fileName().toStdString();
        std::string id;
        if (!submitFarmJob(farm, name, p, p.active()->id, st, stems, &id, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("%s\n", id.c_str());
        return 0;
    }
    if (what == "jobs" || what == "nodes") {
        if (!openFarm(farm, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        const bool json = args.size() > 2 && args[2] == "--json";
        if (args.size() > 2 && !json) return usage();
        QJsonArray list;
        if (what == "jobs") {
            for (const FarmJob& j : farmJobs(farm)) {
                if (json) {
                    list.append(QJsonObject{{"id", QString::fromStdString(j.id)}, {"name", QString::fromStdString(j.name)},
                                            {"state", farmStateName(j.state)}, {"progress", j.progress},
                                            {"node", QString::fromStdString(j.node)}, {"output", QString::fromStdString(j.output)},
                                            {"error", QString::fromStdString(j.error)}, {"submittedBy", QString::fromStdString(j.submittedBy)},
                                            {"submitted", QString::fromStdString(j.submitted)}, {"started", QString::fromStdString(j.started)},
                                            {"finished", QString::fromStdString(j.finished)}, {"cancelRequested", j.cancelRequested}});
                    continue;
                }
                std::string state = farmStateName(j.state);
                if (j.state == FarmState::Rendering) state += " " + std::to_string(int(j.progress * 100)) + "%" + (j.cancelRequested ? " (cancelling)" : "");
                std::printf("%s  %-16s  %-10s  %s%s%s\n", j.id.c_str(), state.c_str(), j.node.c_str(), j.name.c_str(),
                            j.error.empty() || j.state == FarmState::Cancelled ? "" : "  ", j.state == FarmState::Failed ? j.error.c_str() : "");
            }
        } else {
            for (const FarmNodeInfo& n : farmNodes(farm)) {
                if (json) {
                    list.append(QJsonObject{{"name", QString::fromStdString(n.name)}, {"host", QString::fromStdString(n.host)},
                                            {"state", QString::fromStdString(n.state)}, {"job", QString::fromStdString(n.job)},
                                            {"updated", QString::fromStdString(n.updated)}});
                    continue;
                }
                std::printf("%-20s  %-10s  %-24s  %s\n", n.name.c_str(), n.state.c_str(), n.job.c_str(), n.updated.c_str());
            }
        }
        if (json) std::printf("%s", QJsonDocument(list).toJson(QJsonDocument::Indented).constData());
        return 0;
    }
    if (what == "cancel" || what == "retry" || what == "remove") {
        if (args.size() != 3) return usage();
        const bool ok = what == "cancel" ? cancelFarmJob(farm, args[2], &err) : what == "retry" ? retryFarmJob(farm, args[2], &err)
                                                                                                : removeFarmJob(farm, args[2], &err);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        return 0;
    }
    return usage();
}

// A render node on the farm: its waiting jobs rendered one at a time until Ctrl+C (or, --once, until none waits).
int cmdNode(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    FarmNodeOptions o;
    o.farm = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--name") o.name = next();
        else if (a == "--once") o.once = true;
        else if (a == "--poll") o.pollMs = std::max(100, int(std::atof(next().c_str()) * 1000));
        else if (a == "--map") {
            FarmPathMap m;
            if (!parseFarmPathMap(next(), m)) {
                std::fprintf(stderr, "error: --map takes from=to\n");
                return 2;
            }
            o.maps.push_back(m);
        } else return usage();
    }
    // Ctrl+C: the job rendering is given back for another node, and the node ends.
    std::signal(SIGINT, [](int) { gCancel = true; });
    o.stopped = [] { return gCancel.load(); };
    o.log = [](const std::string& line) {
        std::printf("%s  %s\n", QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")).toUtf8().constData(), line.c_str());
        std::fflush(stdout);
    };
    std::string err;
    int rendered = 0;
    if (!runFarmNode(o, &rendered, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

int cmdFrame(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string tc = "0", outPath;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--at" && i + 1 < args.size()) tc = args[++i];
        else if (args[i] == "-o" && i + 1 < args.size()) outPath = args[++i];
        else return usage();
    }
    if (outPath.empty()) return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    FrameTime t = 0;
    if (!parseTimecode(tc, p.active()->fps, t)) return usage();
    std::string err;
    if (!exportStill(p, *p.active(), t, outPath, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", outPath.c_str());
    return 0;
}

int cmdScenes(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    double sensitivity = 0.5;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--sensitivity" && i + 1 < args.size()) sensitivity = std::atof(args[++i].c_str());
    MediaItem m;
    std::string err;
    if (!probeMedia(args[0], m, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    Rational fps = m.fps.valid() ? m.fps : Rational{30, 1};
    auto cuts = detectSceneCuts(args[0], sensitivity, {}, &gCancel, &err);
    if (!err.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (double t : cuts) std::printf("%s  %.3f s\n", formatTimecode(FrameTime(std::llround(t * fps.toDouble())), fps).c_str(), t);
    std::fprintf(stderr, "%zu scene cut(s)\n", cuts.size());
    return 0;
}

int cmdProxy(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out;
    ProxyOptions o;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--width" && i + 1 < args.size()) {
            const std::string w = args[++i];
            o.maxWidth = w == "half" ? 0 : w == "quarter" ? -1 : std::max(16, std::atoi(w.c_str()));
        } else if (args[i] == "--format" && i + 1 < args.size()) {
            if (!parseProxyFormat(args[++i], o.format)) {
                std::fprintf(stderr, "error: --format is h264, prores_proxy, dnxhr_lb or hevc10\n");
                return 2;
            }
        } else if (args[i] == "--sound") o.sound = true;
        else return usage();
    }
    if (out.empty()) return usage();
    std::string err;
    bool ok = createProxy(args[0], out, o, [](double f) {
        std::fprintf(stderr, "\rProxy... %5.1f%%", f * 100.0);
        std::fflush(stderr);
    }, &gCancel, &err);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", out.c_str());
    return 0;
}

int cmdLoudness(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string err;
    auto buf = decodeAudio(args[0], 48000, &err);
    if (!buf) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    LoudnessResult r = measureLoudness(*buf);
    if (!r.valid) std::printf("Integrated: silent (below -70 LUFS)\n");
    else std::printf("Integrated: %.1f LUFS\n", r.integrated);
    std::printf("Peak:       %.1f dBFS\n", r.truePeakDb);
    return 0;
}

// montage-cli import <timeline.xml|.fcpxml|.otio|.edl> -o project.montage [--fps N]
int cmdImport(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string in = args[0], out;
    Rational fps{30, 1};
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--fps" && i + 1 < args.size()) {
            if (!parseFps(args[++i], fps)) return usage();
        } else return usage();
    }
    if (out.empty()) return usage();
    if (std::filesystem::is_directory(in)) in += "/Info.fcpxml";  // an .fcpxmld bundle
    std::ifstream f(in, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "error: cannot read %s\n", in.c_str());
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Project p;
    p.name = std::filesystem::path(in).stem().string();
    const MediaProber prober = [](const std::string& file, MediaItem& m) { return probeMedia(file, m, nullptr); };
    const std::string ext = std::filesystem::path(in).extension().string();
    ImportResult r = ext == ".edl"                         ? importEdl(p, text, fps, prober, std::filesystem::path(in).parent_path().string())
                     : ext == ".xml" || ext == ".fcpxml" ? importXmlTimeline(p, text, prober)
                                                         : importOtio(p, text, prober);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    for (const auto& o : r.offline) std::fprintf(stderr, "offline: %s\n", o.c_str());
    for (const auto& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (!saveProject(p, out)) {
        std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("Imported %d clips into %s\n", r.clips, out.c_str());
    return 0;
}

int cmdInterchange(const std::vector<std::string>& args, const std::string& format) {
    if (args.empty()) return usage();
    std::string out;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
    Project p;
    if (!load(args[0], p)) return 1;
    const Sequence& s = *p.active();
    const std::string text = format == "otio"     ? exportOtio(p, s)
                             : format == "xml"    ? exportFcp7Xml(p, s)
                             : format == "fcpxml" ? exportFcpXml(p, s)
                                                  : exportEdl(p, s);
    if (out.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    return writeFile(out, text) ? 0 : 1;
}

int cmdAaf(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out;
    AafExportOptions options;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--no-picture") options.picture = false;
        else return usage();
    }
    if (out.empty()) return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    std::signal(SIGINT, [](int) { gCancel = true; });
    AafExportResult r;
    std::string err;
    const bool ok = exportAaf(
        p, *p.active(), out, &r,
        [](double f, FrameTime) {
            std::fprintf(stderr, "\rAAF... %5.1f%%", f * 100.0);
            std::fflush(stderr);
        },
        &gCancel, &err, options);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const std::string& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    std::printf("Wrote %s: %d audio tracks, %d clips, %d crossfades, %zu media files; %d video tracks, %d clips, %d dissolves\n",
                out.c_str(), r.audioTracks, r.clips, r.transitions, r.mediaFiles.size(), r.videoTracks, r.videoClips, r.videoTransitions);
    return 0;
}

int cmdBench(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    double scale = 0.5;
    int frames = 120;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--scale" && i + 1 < args.size()) scale = std::atof(args[++i].c_str());
        else if (args[i] == "--frames" && i + 1 < args.size()) frames = std::atoi(args[++i].c_str());
    }
    Project p;
    if (!load(args[0], p)) return 1;
    const Sequence& s = *p.active();
    RenderOptions o;
    o.scale = scale;
    frames = int(std::min<FrameTime>(frames, std::max<FrameTime>(1, s.duration())));
    renderProgramFrame(p, s, 0, o);  // warm up decoders
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) renderProgramFrame(p, s, f, o);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d frames at %dx%d in %.2f s: %.1f fps (%.1f ms/frame), sequence rate %.2f fps\n", frames,
                int(s.width * scale), int(s.height * scale), sec, frames / sec, sec * 1000 / frames, s.fpsValue());
    return 0;
}

#ifdef MONTAGE_WITH_WHISPER
int cmdModels() {
    std::printf("Speech models (folder: %s)\n", whisperModelsDirectory().c_str());
    for (const auto& m : whisperModels())
        std::printf("  %-22s %6.0f MB  %-10s %s\n", m.name.c_str(), double(m.bytes) / 1e6,
                    whisperModelPath(m.name).empty() ? "" : "downloaded", m.label.c_str());
    std::printf("\nDownload a model into the folder from %s\n", whisperModelUrl("<name>").c_str());
    std::printf("\nObject model (EdgeTAM, for object masks; folder: %s)\n", objectModel().directory().c_str());
    if (!segmenterAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.0f MB  %-10s ONNX Runtime %s\n", objectModel().id.c_str(), double(objectModel().bytes()) / 1e6,
                    objectModel().installed() ? "downloaded" : "", segmenterRuntimeVersion().c_str());
    std::printf("\nSound search model (CLAP, for What's Heard; folder: %s)\n", soundModel().directory().c_str());
    if (!soundSearchAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.0f MB  %s\n", soundModel().id.c_str(), double(soundModel().bytes()) / 1e6, soundModel().installed() ? "downloaded" : "");
    std::printf("\nVisual search model (CLIP ViT-B/32, for Find Shots; folder: %s)\n", visualModel().directory().c_str());
    std::printf("  %-22s %6.0f MB  %s\n", visualModel().id.c_str(), double(visualModel().bytes()) / 1e6,
                visualModel().installed() ? "downloaded" : "");
    std::printf("\nSpeaker model (pyannote segmentation + CAM++, for speaker labels; folder: %s)\n", speakerModel().directory().c_str());
    if (!diarizerAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.0f MB  %s\n", speakerModel().id.c_str(), double(speakerModel().bytes()) / 1e6,
                    speakerModel().installed() ? "downloaded" : "");
    std::printf("\nTranslation models (Opus-MT, CC-BY-4.0; one per direction, through English otherwise):\n");
    if (!translatorAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        for (const auto& l : translationLanguages()) {
            if (l.code == "en") continue;
            for (const auto& [a, b] : {std::pair{l.code, std::string("en")}, std::pair{std::string("en"), l.code}})
                if (const ModelPack* m = translationModel(a, b))
                    std::printf("  %-22s %6.0f MB  %s\n", m->id.c_str(), double(m->bytes()) / 1e6, m->installed() ? "downloaded" : "");
        }
    std::printf("\nSpeech enhancement model (DeepFilterNet3, for Enhance Speech; folder: %s)\n", speechModel().directory().c_str());
    if (!speechEnhancerAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.0f MB  %s\n", speechModel().id.c_str(), double(speechModel().bytes()) / 1e6,
                    speechModel().installed() ? "downloaded" : "");
    std::printf("\nSuper Scale model (Real-ESRGAN general x4v3; folder: %s)\n", upscaleModel().directory().c_str());
    if (!upscalerAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", upscaleModel().id.c_str(), double(upscaleModel().bytes()) / 1e6,
                    upscaleModel().installed() ? "downloaded" : "");
    std::printf("\nFace models (YuNet and SFace, for Find People; folder: %s)\n", faceModel().directory().c_str());
    if (!faceSearchAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", faceModel().id.c_str(), double(faceModel().bytes()) / 1e6,
                    faceModel().installed() ? "downloaded" : "");
    std::printf("\nDepth model (Depth Anything V2 Small, for depth effects; folder: %s)\n", depthModel().directory().c_str());
    if (!depthAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", depthModel().id.c_str(), double(depthModel().bytes()) / 1e6,
                    depthModel().installed() ? "downloaded" : "");
    std::printf("\nStem separation models (Spleeter, the authors' release, for Separate Stems; folder: %s)\n",
                std::filesystem::path(stemModel(4).directory()).parent_path().string().c_str());
    if (!stemSeparationAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        for (const int n : {2, 4, 5})
            std::printf("  %-22s %6.1f MB  %s\n", stemModel(n).id.c_str(), double(stemModel(n).bytes()) / 1e6,
                        stemModel(n).installed() ? "downloaded" : "");
    std::printf("\nRIFE 4.26 (the authors' release, for AI slow-motion frames; folder: %s)\n", rifeModel().directory().c_str());
    if (!rifeAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", rifeModel().id.c_str(), double(rifeModel().bytes()) / 1e6,
                    rifeModel().installed() ? "downloaded" : "");
    std::printf("\nPeople model (MODNet, for Remove Background and People masks; folder: %s)\n", mattingModel().directory().c_str());
    if (!mattingAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", mattingModel().id.c_str(), double(mattingModel().bytes()) / 1e6,
                    mattingModel().installed() ? "downloaded" : "");
    std::printf("\nObject removal model (LaMa, for Object Removal; folder: %s)\n", inpaintModel().directory().c_str());
    if (!inpaintAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", inpaintModel().id.c_str(), double(inpaintModel().bytes()) / 1e6,
                    inpaintModel().installed() ? "downloaded" : "");
    std::printf("\nSpeech model (Kokoro-82M with misaki's dictionaries, for voiceovers from text; folder: %s)\n", ttsModel().directory().c_str());
    if (!ttsAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", ttsModel().id.c_str(), double(ttsModel().bytes()) / 1e6, ttsModel().installed() ? "downloaded" : "");
    std::printf("\nVoice cloning model (Chatterbox Multilingual, for speech in a cloned voice; folder: %s)\n", voiceCloneModel().directory().c_str());
    if (!voiceCloneAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.0f MB  %s\n", voiceCloneModel().id.c_str(), double(voiceCloneModel().bytes()) / 1e6,
                    voiceCloneModel().installed() ? "downloaded" : "");
    std::printf("\nSpeech search model (multi-qa-MiniLM, for finding what is said by meaning; folder: %s)\n", sentenceModel().directory().c_str());
    if (!speechSearchAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", sentenceModel().id.c_str(), double(sentenceModel().bytes()) / 1e6,
                    sentenceModel().installed() ? "downloaded" : "");
    std::printf("\nText reading model (PP-OCR, for burned-in subtitles and slates; folder: %s)\n", ocrModel().directory().c_str());
    if (!ocrAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.1f MB  %s\n", ocrModel().id.c_str(), double(ocrModel().bytes()) / 1e6, ocrModel().installed() ? "downloaded" : "");
    return 0;
}

int cmdUpscale(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out;
    int factor = 2;
    double strength = 1;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--factor" && i + 1 < args.size()) factor = std::atoi(args[++i].c_str());
        else if (args[i] == "--strength" && i + 1 < args.size()) strength = std::atof(args[++i].c_str());
        else return usage();
    }
    if (out.empty() || factor < 2 || factor > 4) return usage();
    std::signal(SIGINT, [](int) { gCancel = true; });
    std::string err;
    const bool ok = createSuperScaled(
        args[0], out, factor, std::clamp(strength, 0.0, 1.0),
        [](double f) {
            std::fprintf(stderr, "\rSuper Scale... %5.1f%%", f * 100.0);
            std::fflush(stderr);
        },
        &gCancel, &err);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s\n", out.c_str());
    return 0;
}

int cmdTranscribe(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    TranscribeOptions opts;
    opts.model = "base.en";
    std::string srt, vtt, json, txt;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--model") opts.model = next();
        else if (a == "--language") opts.language = next();
        else if (a == "--translate") opts.translate = true;
        else if (a == "--threads") opts.threads = std::atoi(next().c_str());
        else if (a == "--speakers") {
            // Optional count: --speakers 2; plain --speakers finds out.
            opts.speakers = true;
            if (i + 1 < args.size() && !args[i + 1].empty() && std::isdigit(static_cast<unsigned char>(args[i + 1][0])))
                opts.speakerCount = std::atoi(next().c_str());
        }
        else if (a == "--srt") srt = next();
        else if (a == "--vtt") vtt = next();
        else if (a == "--json") json = next();
        else if (a == "--txt") txt = next();
        else return usage();
    }
    if (whisperModelPath(opts.model).empty()) {
        std::fprintf(stderr, "error: speech model \"%s\" is not downloaded.\nDownload %s into %s\n", opts.model.c_str(),
                     whisperModelUrl(opts.model).c_str(), whisperModelsDirectory().c_str());
        return 1;
    }
    std::signal(SIGINT, [](int) { gCancel = true; });
    Transcript t;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = transcribeMedia(args[0], opts, t, [](double f) {
        std::fprintf(stderr, "\rTranscribing... %5.1f%%", f * 100.0);
        std::fflush(stderr);
    }, &gCancel, &err);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "%zu words, language %s, %.1f s\n", t.wordCount(), t.language.c_str(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    bool wrote = false, failed = false;
    auto out = [&](const std::string& path, const char* format) {
        if (path.empty()) return;
        wrote = true;
        failed |= !writeFile(path, transcriptAs(t, format));
    };
    out(srt, "srt");
    out(vtt, "vtt");
    out(json, "json");
    out(txt, "txt");
    if (!wrote) std::printf("%s\n", t.text().c_str());
    return failed ? 1 : 0;
}
#endif

int cmdCaptions(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out, import, model;
    bool generate = false, save = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--transcribe" && i + 1 < args.size()) model = args[++i], generate = true;
        else if (args[i] == "--import" && i + 1 < args.size()) import = args[++i];
        else if (args[i] == "--generate") generate = true;
        else if (args[i] == "--save") save = true;
        else return usage();
    }
    Project p;
    if (!load(args[0], p)) return 1;
    Sequence& s = *p.active();
    auto addTrack = [&](std::vector<Caption> caps, const std::string& name) {
        CaptionTrack t;
        t.id = p.newId();
        t.name = name;
        t.captions = std::move(caps);
        s.captionTracks.insert(s.captionTracks.begin(), std::move(t));
    };
    if (!model.empty()) {
#ifdef MONTAGE_WITH_WHISPER
        // Transcribe the media the sequence uses that has sound and no transcript yet.
        std::signal(SIGINT, [](int) { gCancel = true; });
        TranscribeOptions opts;
        opts.model = model;
        for (auto& m : p.media) {
            bool used = false;
            for (const auto* list : {&s.audioTracks, &s.videoTracks})
                for (const auto& tr : *list)
                    for (const auto& c : tr.clips) used |= c.mediaId == m.id;
            if (!used || !m.hasAudio || m.path.empty() || m.transcript) continue;
            auto t = std::make_shared<Transcript>();
            std::string err;
            std::fprintf(stderr, "Transcribing %s...\n", m.name.c_str());
            if (!transcribeMedia(m.path, opts, *t, {}, &gCancel, &err)) {
                std::fprintf(stderr, "error: %s: %s\n", m.name.c_str(), err.c_str());
                return 1;
            }
            m.transcript = t;
        }
#else
        std::fprintf(stderr, "error: this build has no speech recognition\n");
        return 1;
#endif
    }
    if (generate) {
        auto caps = captionsFromTranscripts(p, s);
        if (caps.empty()) {
            std::fprintf(stderr, "error: no transcribed speech in the sequence (transcribe the media in the app first)\n");
            return 1;
        }
        std::fprintf(stderr, "%zu captions from transcripts\n", caps.size());
        addTrack(std::move(caps), "Subtitles");
    }
    if (!import.empty()) {
        FILE* f = std::fopen(import.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "error: cannot read %s\n", import.c_str());
            return 1;
        }
        std::string text;
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
        std::vector<Caption> caps;
        std::string err;
        if (!parseSubtitles(text, s.fps, caps, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        addTrack(std::move(caps), std::filesystem::path(import).stem().string());
    }
    const CaptionTrack* t = captionTrackFor(s);
    if (!t && !s.captionTracks.empty()) t = &s.captionTracks.front();
    if (!t) {
        std::fprintf(stderr, "error: the sequence has no captions\n");
        return 1;
    }
    if (save) {
        std::string err;
        if (!saveProject(p, std::filesystem::absolute(args[0]).string(), &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("Saved %s\n", args[0].c_str());
    }
    const std::string ext = std::filesystem::path(out).extension().string();
    const std::string text = exportCaptions(*t, s, captionFormatKnown(ext) ? ext : ".srt");
    if (out.empty()) {
        if (!save) std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    return writeFile(out, text) ? 0 : 1;
}

// Translates a subtitle file, keeping its timings.
int cmdTranslate(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string from = "en", to, out;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--to" && i + 1 < args.size()) to = args[++i];
        else if (args[i] == "--from" && i + 1 < args.size()) from = args[++i];
        else if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else return usage();
    }
    if (to.empty()) return usage();
    const auto route = translationRoute(from, to);
    if (route.empty()) {
        std::fprintf(stderr, "error: no translation from %s to %s (see `montage-cli models`)\n", from.c_str(), to.c_str());
        return 1;
    }
    for (const ModelPack* pack : route)
        if (!pack->installed()) {
            std::fprintf(stderr, "error: the %s is not downloaded (%.0f MB, into %s); translate once in the app to fetch it\n",
                         pack->title.c_str(), double(pack->bytes()) / 1e6, pack->directory().c_str());
            return 1;
        }
    std::string text;
    {
        FILE* f = std::fopen(args[0].c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "error: cannot read %s\n", args[0].c_str());
            return 1;
        }
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    const Rational ms{1000, 1};  // millisecond "frames" keep the times exact
    CaptionTrack t;
    std::string err;
    if (!parseSubtitles(text, ms, t.captions, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::signal(SIGINT, [](int) { gCancel = true; });
    std::vector<std::string> translated;
    if (!translateTexts(captionTexts(t), from, to, translated, {}, &gCancel, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const CaptionTrack done = translatedTrack(t, translated, 1, to, translationLanguageName(to));
    const bool vtt = std::filesystem::path(out.empty() ? args[0] : out).extension() == ".vtt";
    const std::string result = vtt ? captionsToVtt(done.captions, ms) : captionsToSrt(done.captions, ms);
    if (out.empty()) {
        std::fwrite(result.data(), 1, result.size(), stdout);
        return 0;
    }
    return writeFile(out, result) ? 0 : 1;
}

// Finds shots by description, indexing the project's videos first (and saving the index).
int cmdShots(const std::vector<std::string>& args) {
    if (args.size() < 2) return usage();
    size_t max = 10;
    for (size_t i = 2; i < args.size(); ++i)
        if (args[i] == "--max" && i + 1 < args.size()) max = size_t(std::max(1, std::atoi(args[++i].c_str())));
        else return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    std::string err;
    bool changed = false;
    for (MediaItem& m : p.media) {
        if (m.kind != MediaKind::Video || !m.hasVideo || m.path.empty() || (m.visual && !m.visual->samples.empty())) continue;
        VisualIndex v;
        const bool ok = indexVideo(
            m.path, m.duration, v, 0,
            [&](double f) {
                std::fprintf(stderr, "\rIndexing %s... %5.1f%%", m.name.c_str(), f * 100);
                std::fflush(stderr);
            },
            nullptr, &err);
        std::fprintf(stderr, "\n");
        if (!ok) {
            std::fprintf(stderr, "error: %s: %s\n", m.name.c_str(), err.c_str());
            return 1;
        }
        m.visual = std::make_shared<const VisualIndex>(std::move(v));
        changed = true;
    }
    if (changed && !saveProject(p, args[0], &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    auto clip = ClipModel::load(&err);
    const std::vector<float> q = clip ? clip->text(args[1], &err) : std::vector<float>{};
    if (q.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const ShotMatch& h : findShots(p, q, max))
        if (const MediaItem* m = p.findMedia(h.media))
            std::printf("%.3f  %-30s %8.2f - %8.2f s\n", double(h.score), m->name.c_str(), h.start, h.end);
    return 0;
}

// Finds sounds by description, indexing the project's recordings first (and saving the index).
int cmdSounds(const std::vector<std::string>& args) {
    if (args.size() < 2) return usage();
    size_t max = 10;
    for (size_t i = 2; i < args.size(); ++i)
        if (args[i] == "--max" && i + 1 < args.size()) max = size_t(std::max(1, std::atoi(args[++i].c_str())));
        else return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    std::string err;
    bool changed = false;
    for (MediaItem& m : p.media) {
        if (!m.hasAudio || m.path.empty() || m.subclipOf || (m.sounds && !m.sounds->samples.empty())) continue;
        VisualIndex v;
        const bool ok = indexSound(
            m.path, m.duration, v, 0,
            [&](double f) {
                std::fprintf(stderr, "\rListening to %s... %5.1f%%", m.name.c_str(), f * 100);
                std::fflush(stderr);
            },
            nullptr, &err);
        std::fprintf(stderr, "\n");
        if (!ok) {  // (offline or unreadable: the rest are still searched)
            std::fprintf(stderr, "warning: %s: %s\n", m.name.c_str(), err.c_str());
            continue;
        }
        m.sounds = std::make_shared<const VisualIndex>(std::move(v));
        changed = true;
    }
    if (changed && !saveProject(p, args[0], &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    auto model = SoundModel::load(&err);
    const std::vector<float> q = model ? model->text(args[1], &err) : std::vector<float>{};
    if (q.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const ShotMatch& h : findSounds(p, q, max))
        if (const MediaItem* m = p.findMedia(h.media))
            std::printf("%.3f  %-30s %8.2f - %8.2f s\n", double(h.score), m->name.c_str(), h.start, h.end);
    return 0;
}

// Separates a file's sound into stems, one WAV each beside it (or in -o folder).
int cmdStems(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    int stems = 4;
    double from = 0, to = 0;
    std::string folder;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--stems" && i + 1 < args.size()) stems = std::atoi(args[++i].c_str());
        else if (args[i] == "--from" && i + 1 < args.size()) from = std::atof(args[++i].c_str());
        else if (args[i] == "--to" && i + 1 < args.size()) to = std::atof(args[++i].c_str());
        else if (args[i] == "-o" && i + 1 < args.size()) folder = args[++i];
        else return usage();
    if (stemNames(stems).empty()) {
        std::fprintf(stderr, "error: --stems is 2, 4 or 5\n");
        return 1;
    }
    if (!stemModel(stems).installed()) {
        std::fprintf(stderr, "error: the %s is not downloaded (scripts/fetch-models.sh, or Separate Stems in the app)\n",
                     stemModel(stems).title.c_str());
        return 1;
    }
    const std::filesystem::path in(args[0]);
    const std::filesystem::path dir = folder.empty() ? in.parent_path() : std::filesystem::path(folder);
    std::error_code ec;
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    MediaItem m;
    double end = to;
    if (end <= 0 && probeMedia(args[0], m)) end = m.duration;
    std::vector<std::string> outs;
    for (const std::string& name : stemNames(stems))
        outs.push_back((dir / (in.stem().string() + " " + stemTitle(name) + ".wav")).string());
    std::string err;
    const bool ok = separateStemsToFiles(
        args[0], from, end, {}, stems, outs,
        [](double f) {
            std::fprintf(stderr, "\rSeparating... %5.1f%%", f * 100);
            std::fflush(stderr);
        },
        nullptr, &err);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const std::string& o : outs) std::printf("%s\n", o.c_str());
    return 0;
}

// Lists the people in the footage, finding faces in media not looked through yet (and saving them).
int cmdPeople(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string who;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--person" && i + 1 < args.size()) who = args[++i];
        else return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    if (!faceSearchAvailable()) {
        std::fprintf(stderr, "error: this build has no ONNX Runtime\n");
        return 1;
    }
    std::string err;
    bool changed = false;
    for (MediaItem& m : p.media) {
        if ((m.kind != MediaKind::Video && m.kind != MediaKind::Image) || !m.hasVideo || m.path.empty() || m.subclipOf || m.faces) continue;
        FaceIndex f;
        const bool ok = indexFaces(
            m.path, m.kind == MediaKind::Image ? 0.0 : m.duration, f, 0, 8, 32,
            [&](double x) {
                std::fprintf(stderr, "\rLooking for faces in %s... %5.1f%%", m.name.c_str(), x * 100);
                std::fflush(stderr);
            },
            nullptr, &err);
        std::fprintf(stderr, "\n");
        if (!ok) {
            std::fprintf(stderr, "error: %s: %s\n", m.name.c_str(), err.c_str());
            return 1;
        }
        m.faces = std::make_shared<const FaceIndex>(std::move(f));
        changed = true;
    }
    if (changed) {
        groupPeople(p);
        if (!saveProject(p, args[0], &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
    }
    const auto people = peopleIn(p);
    if (who.empty()) {
        for (const PersonSummary& s : people) std::printf("%4d  %-30s %d clip(s)\n", s.id, s.name.c_str(), s.media);
        if (people.empty()) std::printf("No faces found\n");
        return 0;
    }
    for (const PersonSummary& s : people) {
        if (std::to_string(s.id) != who && QString::fromStdString(s.name).compare(QString::fromStdString(who), Qt::CaseInsensitive) != 0) continue;
        for (const PersonMoment& pm : findPerson(p, s.id))
            if (const MediaItem* m = p.findMedia(pm.media)) {
                if (m->kind == MediaKind::Image) std::printf("%-30s still\n", m->name.c_str());
                else std::printf("%-30s %8.2f - %8.2f s\n", m->name.c_str(), pm.start, pm.end);
            }
        return 0;
    }
    std::fprintf(stderr, "error: no person \"%s\"\n", who.c_str());
    return 1;
}

// Speaks text into a WAV file (Kokoro), or prints its phonemes.
int cmdSpeak(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out, voice = "af_heart";
    double speed = 1;
    bool phonemesOnly = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--voice" && i + 1 < args.size()) voice = args[++i];
        else if (args[i] == "--speed" && i + 1 < args.size()) speed = std::atof(args[++i].c_str());
        else if (args[i] == "--phonemes") phonemesOnly = true;
        else return usage();
    }
    std::string err;
    if (phonemesOnly) {
        const TtsVoice* v = findTtsVoice(voice);
        std::string ps;
        if (!textToPhonemes(args[0], v && v->british, ps, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("%s\n", ps.c_str());
        return 0;
    }
    if (out.empty()) return usage();
    std::vector<float> samples;
    if (!synthesizeSpeech(args[0], voice, speed, samples, &err) || !writeSpeechWav(out, samples, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s (%.1f s)\n", out.c_str(), double(samples.size()) / kTtsSampleRate);
    return 0;
}

// Speaks text into a WAV file in a voice cloned from a recording (Chatterbox Multilingual), with its owner's consent.
int cmdCloneSpeech(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out, sample;
    double from = 0, to = 30, speed = 1;
    CloneOptions o;
    bool consent = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--voice" && i + 1 < args.size()) sample = args[++i];
        else if (args[i] == "--language" && i + 1 < args.size()) o.language = args[++i];
        else if (args[i] == "--from" && i + 1 < args.size()) from = std::atof(args[++i].c_str());
        else if (args[i] == "--to" && i + 1 < args.size()) to = std::atof(args[++i].c_str());
        else if (args[i] == "--speed" && i + 1 < args.size()) speed = std::atof(args[++i].c_str());
        else if (args[i] == "--exaggeration" && i + 1 < args.size()) o.exaggeration = std::clamp(std::atof(args[++i].c_str()), 0.25, 2.0);
        else if (args[i] == "--consent") consent = true;
        else return usage();
    }
    if (out.empty() || sample.empty()) return usage();
    if (!consent) {
        std::fprintf(stderr, "error: cloning a voice needs its owner's consent: pass --consent to confirm you have it\n");
        return 1;
    }
    const CloneLanguage* lang = findCloneLanguage(o.language);
    if (!lang) {
        std::fprintf(stderr, "error: voice cloning does not speak \"%s\"; it speaks:", o.language.c_str());
        for (const CloneLanguage& l : voiceCloneLanguages()) std::fprintf(stderr, " %s", l.code);
        std::fprintf(stderr, "\n");
        return 1;
    }
    o.language = lang->code;
    std::vector<float> voice, samples;
    std::string err;
    if (!voiceSampleFrom(sample, from, to, voice, &err) ||
        !cloneSpeechToFit(args[0], voice, kCloneSampleRate, o, speed, 0, false, samples, &err) ||
        !writeClonedSpeechWav(out, samples,
                              "Generated speech (" + std::string(lang->name) + ") in a voice cloned from " +
                                  std::filesystem::path(sample).filename().string() +
                                  " with Chatterbox Multilingual in Montage; the speaker's consent was confirmed.",
                              &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s (%.1f s)\n", out.c_str(), double(samples.size()) / kCloneSampleRate);
    return 0;
}

// A file's Content Credentials.
int cmdCredentials(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    CredentialSummary c;
    std::string err;
    if (!readContentCredentials(args[0], c, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (json) {
        std::printf("%s\n", c.json.c_str());
        return 0;
    }
    if (!c.present) {
        std::printf("No Content Credentials\n");
        return 0;
    }
    std::printf("%s\n", credentialsLabel(c).c_str());
    for (const std::string& a : c.actions) std::printf("  %s\n", a.c_str());
    for (const std::string& p : c.problems) std::printf("  problem: %s\n", p.c_str());
    return c.state == "Invalid" ? 2 : 0;
}

// Signs a file with Content Credentials, with a project's AI use and its sources' credentials.
int cmdSign(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    CredentialSigner signer;
    CredentialOptions o;
    std::string project;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--cert" && i + 1 < args.size()) signer.certificatePath = args[++i];
        else if (args[i] == "--key" && i + 1 < args.size()) signer.keyPath = args[++i];
        else if (args[i] == "--algorithm" && i + 1 < args.size()) signer.algorithm = args[++i];
        else if (args[i] == "--project" && i + 1 < args.size()) project = args[++i];
        else if (args[i] == "--allow-ai-training") o.allowAiTraining = true;
        else return usage();
    }
    if (signer.certificatePath.empty() || signer.keyPath.empty()) return usage();
    Provenance made;
    if (!project.empty()) {
        Project p;
        if (!load(project, p)) return 1;
        if (const Sequence* s = p.active()) made = sequenceProvenance(p, *s);
        o.sources = made.sources;
    }
    std::string err;
    if (!signContentCredentials(args[0], made, signer, o, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Signed %s (%zu AI uses noted)\n", args[0].c_str(), made.ai.size());
    return 0;
}

// Says speech again in a voice cloned from a recording (Chatterbox's decoder), its words and timing kept, with the
// voice's owner's consent.
int cmdConvertVoice(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string out, sample;
    double from = 0, to = 0, voiceFrom = 0, voiceTo = 30;
    bool consent = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--voice" && i + 1 < args.size()) sample = args[++i];
        else if (args[i] == "--from" && i + 1 < args.size()) from = std::atof(args[++i].c_str());
        else if (args[i] == "--to" && i + 1 < args.size()) to = std::atof(args[++i].c_str());
        else if (args[i] == "--voice-from" && i + 1 < args.size()) voiceFrom = std::atof(args[++i].c_str());
        else if (args[i] == "--voice-to" && i + 1 < args.size()) voiceTo = std::atof(args[++i].c_str());
        else if (args[i] == "--consent") consent = true;
        else return usage();
    }
    if (out.empty() || sample.empty()) return usage();
    if (!consent) {
        std::fprintf(stderr, "error: converting speech into someone's voice needs their consent: pass --consent to confirm you have it\n");
        return 1;
    }
    std::vector<float> speech, voice, samples;
    std::string err;
    if (!voiceSoundFrom(args[0], from, to, speech, &err) || !voiceSampleFrom(sample, voiceFrom, voiceTo, voice, &err) ||
        !convertVoice(speech, kCloneSampleRate, voice, kCloneSampleRate, samples, &err) ||
        !writeClonedSpeechWav(out, samples,
                              "Speech from " + std::filesystem::path(args[0]).filename().string() + " converted to a voice cloned from " +
                                  std::filesystem::path(sample).filename().string() +
                                  " with Chatterbox in Montage; the speaker's consent was confirmed.",
                              &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Wrote %s (%.2f s)\n", out.c_str(), double(samples.size()) / kCloneSampleRate);
    return 0;
}

// A Lua or Python (.py) script on Montage's tools, on the project named by --project (the tools that take a project get it when the
// script names none); print() goes to stdout, the error to stderr.
int cmdScript(const std::vector<std::string>& args) {
    // Options before the script; everything after it is the script's ("-p 3" there is an argument).
    std::string file, project;
    QString press;
    std::vector<std::pair<QString, QString>> sets;
    QStringList rest;
    for (size_t i = 0; i < args.size(); ++i) {
        if (!file.empty()) rest << QString::fromStdString(args[i]);
        else if ((args[i] == "--project" || args[i] == "-p") && i + 1 < args.size()) project = args[++i];
        else if (args[i] == "--press" && i + 1 < args.size()) press = QString::fromStdString(args[++i]);
        else if (args[i] == "--set" && i + 1 < args.size()) {
            const QString kv = QString::fromStdString(args[++i]);
            const qsizetype eq = kv.indexOf(QLatin1Char('='));
            if (eq <= 0) {
                std::fprintf(stderr, "error: --set takes id=value\n");
                return 2;
            }
            sets.push_back({kv.left(eq), kv.mid(eq + 1)});
        } else if (args[i] == "--" && i + 1 < args.size()) file = args[++i];
        else file = args[i];
    }
    if (file.empty()) return usage();
    if (press.isEmpty() && !sets.empty()) {
        std::fprintf(stderr, "error: --set fills a panel for --press\n");
        return 2;
    }
    if (!scriptAvailable(QString::fromStdString(file))) {
        std::fprintf(stderr, isPythonScript(QString::fromStdString(file))
                                 ? "error: no Python 3.8 or later was found (set MONTAGE_PYTHON to one)\n"
                                 : "error: this build of Montage has no Lua\n");
        return 1;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", file.c_str());
        return 1;
    }
    const std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    McpServer server;
    ScriptOptions o = toolScriptOptions(server, project.empty() ? QString() : QFileInfo(QString::fromStdString(project)).absoluteFilePath());
    o.args = rest;
    o.file = QFileInfo(QString::fromStdString(file)).absoluteFilePath();
    o.print = [](const QString& line) {
        std::printf("%s\n", line.toUtf8().constData());
        std::fflush(stdout);
    };
    // Ctrl+C stops the script at its next step; a second one (a tool that takes long, a read) ends at once.
    std::signal(SIGINT, [](int) {
        if (gCancel) std::_Exit(130);
        gCancel = true;
    });
    o.stopped = [] { return gCancel.load(); };
    // A panel: printed (and the script ends there), or answered as if --press's button were pressed.
    std::optional<PanelSpec> panel;
    bool pressed = false;
    if (!press.isEmpty()) {
        // (A script that turns out to have no panel must not have changed anything first.)
        auto call = o.call;
        o.call = [call, &pressed](const QString& name, const QJsonObject& args, QString* error) -> QJsonObject {
            if (!pressed) {
                *error = QStringLiteral("with --press, a script calls montage.panel before any tool");
                return {};
            }
            return call(name, args, error);
        };
    }
    o.panel = [&](const QJsonObject& description, QString* error) -> std::optional<QJsonObject> {
        std::optional<PanelSpec> spec = parsePanel(description, error);
        if (!spec) return std::nullopt;
        if (press.isEmpty()) {
            std::printf("%s", QJsonDocument(describePanel(*spec)).toJson(QJsonDocument::Indented).constData());
            std::fflush(stdout);
            return std::nullopt;
        }
        const PanelControl* button = spec->find(press);
        if (!button || button->type != QLatin1String("button")) {
            *error = QStringLiteral("the panel has no button \"%1\"").arg(press);
            return std::nullopt;
        }
        for (const auto& [id, value] : sets)
            if (!setPanelValue(*spec, id, value, error)) return std::nullopt;
        QJsonObject values = panelValues(*spec);
        values["pressed"] = press;
        panel = spec;
        pressed = true;
        return values;
    };
    o.setPanel = [&](const QJsonObject& values, QString* error) {
        if (!panel) {
            *error = QStringLiteral("call montage.panel first");
            return false;
        }
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!setPanelValue(*panel, it.key(), it.value(), error)) return false;
            const QJsonValue shown = panelValues(*panel).value(it.key());
            std::printf("panel: %s = %s\n", it.key().toUtf8().constData(),
                        shown.isString() ? shown.toString().toUtf8().constData()
                                         : QJsonDocument(QJsonArray{shown}).toJson(QJsonDocument::Compact).mid(1).chopped(1).constData());
        }
        std::fflush(stdout);
        return true;
    };
    QString err;
    if (!runScript(QString::fromUtf8(code.data(), qsizetype(code.size())), QString::fromStdString(std::filesystem::path(file).filename().string()), o, &err)) {
        std::fprintf(stderr, "error: %s\n", err.toUtf8().constData());
        return gCancel ? 130 : 1;
    }
    if (!press.isEmpty() && !pressed) {
        std::fprintf(stderr, "error: --press: the script has no panel\n");
        return 1;
    }
    return 0;
}

int cmdPresets() {
    for (const auto& p : exportPresets())
        std::printf("%-28s .%-5s %s\n", p.name.c_str(), p.extension.c_str(), p.description.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);  // fonts for title rendering
    initInference();  // (the provider libraries and Tools › AI Models Run On, as the app has them)
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    std::string cmd = args[0];
    args.erase(args.begin());
    if (cmd == "probe") return cmdProbe(args);
    if (cmd == "new") return cmdNew(args);
    if (cmd == "info") return cmdInfo(args);
    if (cmd == "render") return cmdRender(args);
    if (cmd == "farm") return cmdFarm(args);
    if (cmd == "node") return cmdNode(args);
    if (cmd == "frame") return cmdFrame(args);
    if (cmd == "presets") return cmdPresets();
    if (cmd == "devices") {  // where the AI models can run (media/Inference.h)
        for (const InferenceDevice& d : inferenceDevices())
            std::printf("%-4s %s%s\n", d.kind.c_str(), d.provider.c_str(), d.vendor.empty() ? "" : (" (" + d.vendor + ")").c_str());
        std::printf("Models run: %s%s\n", inferenceModeName(inferenceMode()),
                    inferenceModeOverridden() ? " (from MONTAGE_INFERENCE)" : " (Tools > AI Models Run On; MONTAGE_INFERENCE=auto|efficient|cpu overrides it)");
        if (const std::string e = lastInferenceLibraryError(); !e.empty()) std::printf("A provider library was not taken: %s\n", e.c_str());
        std::string why;
        const int before = inferenceFallbacks();
        const bool ok = inferenceCheck(&why);
        if (!ok) std::printf("A test model: %s\n", why.c_str());
        else if (inferenceFallbacks() > before) std::printf("A test model: ran on the processor (the device would not take it: %s)\n", lastInferenceFallback().c_str());
        else std::printf("A test model: ran\n");
        return ok ? 0 : 1;
    }
    if (cmd == "colorspaces") {
        for (const auto& c : colorSpaces())
            std::printf("%-20s %s%s\n", c.id.c_str(), c.label.c_str(), c.sceneReferred ? "  (media only)" : "");
        return 0;
    }
    if (cmd == "scenes") return cmdScenes(args);
    if (cmd == "proxy") return cmdProxy(args);
    if (cmd == "loudness") return cmdLoudness(args);
    if (cmd == "upscale") return cmdUpscale(args);
    if (cmd == "bench") return cmdBench(args);
    if (cmd == "captions") return cmdCaptions(args);
    if (cmd == "translate") return cmdTranslate(args);
    if (cmd == "import") return cmdImport(args);
    if (cmd == "edl" || cmd == "otio" || cmd == "xml" || cmd == "fcpxml") return cmdInterchange(args, cmd);
    if (cmd == "aaf") return cmdAaf(args);
#ifdef MONTAGE_WITH_WHISPER
    if (cmd == "transcribe") return cmdTranscribe(args);
    if (cmd == "models") return cmdModels();
#else
    if (cmd == "transcribe" || cmd == "models") {
        std::fprintf(stderr, "error: this build has no speech recognition (MONTAGE_WITH_WHISPER=OFF)\n");
        return 1;
    }
#endif
    if (cmd == "shots") return cmdShots(args);
    if (cmd == "sounds") return cmdSounds(args);
    if (cmd == "stems") return cmdStems(args);
    if (cmd == "people") return cmdPeople(args);
    if (cmd == "speak") return cmdSpeak(args);
    if (cmd == "clone-speech") return cmdCloneSpeech(args);
    if (cmd == "convert-voice") return cmdConvertVoice(args);
    if (cmd == "credentials") return cmdCredentials(args);
    if (cmd == "sign") return cmdSign(args);
    if (cmd == "script") return cmdScript(args);
    if (cmd == "mcp") {
#ifdef MONTAGE_LIVE_BRIDGE
        // --live: the tools work on the project open in the running app (Tools > Agent Link), relayed to it.
        if (std::find(args.begin(), args.end(), "--live") != args.end()) return runLiveBridge(std::cin, std::cout);
#endif
        // A Model Context Protocol server on stdin/stdout: stdout carries only protocol messages.
        McpServer server;
        return server.run(std::cin, std::cout);
    }
    if (cmd == "--version" || cmd == "version") {
        std::printf("Montage %s\n", MONTAGE_VERSION);
        return 0;
    }
    return usage();
}
