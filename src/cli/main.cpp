// montage-cli — headless rendering, probing and project assembly.
#include <QGuiApplication>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
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
#include "core/Transcript.h"
#include "media/Analysis.h"
#include "media/Decoder.h"
#include "media/Loudness.h"
#ifdef MONTAGE_WITH_WHISPER
#include "media/Segmenter.h"
#include "media/Transcriber.h"
#endif
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/Exporter.h"

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
                 "                     [--burn-captions] [--embed-captions] [--color-space ID]\n"
                 "  montage-cli frame <project.montage> --at TC -o <image.png>\n"
                 "  montage-cli presets\n"
                 "  montage-cli colorspaces\n"
                 "  montage-cli scenes <video> [--sensitivity 0..1]\n"
                 "  montage-cli proxy <video> -o <proxy.mp4> [--width 960]\n"
                 "  montage-cli loudness <media>\n"
                 "  montage-cli edl <project.montage> [-o out.edl]\n"
                 "  montage-cli otio <project.montage> [-o out.otio]\n"
                 "  montage-cli xml <project.montage> [-o out.xml]       (Final Cut Pro 7 XML)\n"
                 "  montage-cli fcpxml <project.montage> [-o out.fcpxml]\n"
                 "  montage-cli import <timeline.xml|.fcpxml|.otio|.edl> -o <project.montage> [--fps N]\n"
                 "  montage-cli bench <project.montage> [--scale 0.5] [--frames 120]\n"
                 "  montage-cli transcribe <media> [--model base.en|PATH] [--language auto|en|...] [--translate]\n"
                 "                     [--srt out.srt] [--vtt out.vtt] [--json out.json] [--txt out.txt]\n"
                 "  montage-cli models\n"
                 "  montage-cli captions <project.montage> [-o out.srt|out.vtt|out.scc] [--transcribe MODEL]\n"
                 "                     [--generate] [--import file.srt] [--save]\n",
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

int cmdRender(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    std::string projectPath = args[0], outPath, presetName = "H.264 - High Quality", inTc, outTc;
    ExportSettings st;
    int width = 0, height = 0, crf = -1;
    std::string vcodec, acodec;
    bool proxies = false;
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
        else if (a == "--embed-captions") st.embedCaptions = true;
        else if (a == "--color-space") st.colorSpace = next();
        else return usage();
    }
    if (outPath.empty()) return usage();
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
    Project p;
    if (!load(projectPath, p)) return 1;
    const Sequence* s = p.active();
    st.path = outPath;
    if (width > 0) st.width = width;
    if (height > 0) st.height = height;
    if (width > 0 && height <= 0) st.height = int(std::lround(double(width) * s->height / s->width));
    if (crf >= 0) st.crf = crf;
    if (!vcodec.empty()) st.videoCodec = vcodec;
    if (!acodec.empty()) st.audioCodec = acodec;
    st.useProxies = proxies;
    if (!inTc.empty() && !parseTimecode(inTc, s->fps, st.in)) return usage();
    if (!outTc.empty() && !parseTimecode(outTc, s->fps, st.out)) return usage();
    if (st.in < 0 && s->inPoint >= 0) st.in = s->inPoint;
    if (st.out < 0 && s->outPoint >= 0) st.out = s->outPoint + 1;  // marks are inclusive
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
    int width = 960;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--width" && i + 1 < args.size()) width = std::atoi(args[++i].c_str());
        else return usage();
    }
    if (out.empty()) return usage();
    std::string err;
    bool ok = createProxy(args[0], out, width, [](double f) {
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
    std::printf("\nObject model (EdgeTAM, for object masks; folder: %s)\n", segmenterModelDirectory().c_str());
    if (!segmenterAvailable()) std::printf("  unavailable: this build has no ONNX Runtime\n");
    else
        std::printf("  %-22s %6.0f MB  %-10s ONNX Runtime %s\n", "edgetam-video", double(segmenterDownloadBytes()) / 1e6,
                    segmenterModelInstalled() ? "downloaded" : "", segmenterRuntimeVersion().c_str());
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
    const std::string text = ext == ".vtt" ? captionsToVtt(t->captions, s.fps)
                             : ext == ".scc" ? captionsToScc(t->captions, s.fps)
                                             : captionsToSrt(t->captions, s.fps);
    if (out.empty()) {
        if (!save) std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    return writeFile(out, text) ? 0 : 1;
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
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    std::string cmd = args[0];
    args.erase(args.begin());
    if (cmd == "probe") return cmdProbe(args);
    if (cmd == "new") return cmdNew(args);
    if (cmd == "info") return cmdInfo(args);
    if (cmd == "render") return cmdRender(args);
    if (cmd == "frame") return cmdFrame(args);
    if (cmd == "presets") return cmdPresets();
    if (cmd == "colorspaces") {
        for (const auto& c : colorSpaces())
            std::printf("%-20s %s%s\n", c.id.c_str(), c.label.c_str(), c.sceneReferred ? "  (media only)" : "");
        return 0;
    }
    if (cmd == "scenes") return cmdScenes(args);
    if (cmd == "proxy") return cmdProxy(args);
    if (cmd == "loudness") return cmdLoudness(args);
    if (cmd == "bench") return cmdBench(args);
    if (cmd == "captions") return cmdCaptions(args);
    if (cmd == "import") return cmdImport(args);
    if (cmd == "edl" || cmd == "otio" || cmd == "xml" || cmd == "fcpxml") return cmdInterchange(args, cmd);
#ifdef MONTAGE_WITH_WHISPER
    if (cmd == "transcribe") return cmdTranscribe(args);
    if (cmd == "models") return cmdModels();
#else
    if (cmd == "transcribe" || cmd == "models") {
        std::fprintf(stderr, "error: this build has no speech recognition (MONTAGE_WITH_WHISPER=OFF)\n");
        return 1;
    }
#endif
    if (cmd == "--version" || cmd == "version") {
        std::printf("Montage %s\n", MONTAGE_VERSION);
        return 0;
    }
    return usage();
}
