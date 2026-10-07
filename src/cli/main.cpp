// montage-cli — headless rendering, probing and project assembly.
#include <QGuiApplication>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/ProjectIO.h"
#include "media/Analysis.h"
#include "media/Decoder.h"
#include "media/Loudness.h"
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
                 "  montage-cli new -o <project.montage> [--size WxH] [--fps N[/D]] <media>...\n"
                 "  montage-cli info <project.montage>\n"
                 "  montage-cli render <project.montage> -o <output> [--preset NAME] [--in TC] [--out TC]\n"
                 "                     [--width W] [--height H] [--crf N] [--vcodec C] [--acodec C] [--proxies]\n"
                 "  montage-cli frame <project.montage> --at TC -o <image.png>\n"
                 "  montage-cli presets\n"
                 "  montage-cli scenes <video> [--sensitivity 0..1]\n"
                 "  montage-cli proxy <video> -o <proxy.mp4> [--width 960]\n"
                 "  montage-cli loudness <media>\n"
                 "  montage-cli edl <project.montage> [-o out.edl]\n"
                 "  montage-cli otio <project.montage> [-o out.otio]\n"
                 "  montage-cli bench <project.montage> [--scale 0.5] [--frames 120]\n",
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
    if (m.hasAudio) std::printf("audio:     %s %d Hz, %d ch\n", m.audioCodec.c_str(), m.sampleRate, m.channels);
    return 0;
}

int cmdNew(const std::vector<std::string>& args) {
    std::string outPath;
    int w = 1920, h = 1080;
    Rational fps{30, 1};
    bool fpsGiven = false, sizeGiven = false;
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

int cmdInfo(const std::vector<std::string>& args) {
    if (args.empty()) return usage();
    Project p;
    if (!load(args[0], p)) return 1;
    for (const auto& s : p.sequences) {
        std::printf("Sequence \"%s\" %dx%d @ %.3f fps, %d Hz, duration %s%s\n", s.name.c_str(), s.width, s.height,
                    s.fpsValue(), s.sampleRate, formatTimecode(s.duration(), s.fps).c_str(),
                    s.id == p.activeSequence ? " (active)" : "");
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
        else return usage();
    }
    if (outPath.empty()) return usage();
    const ExportPreset* pr = findExportPreset(presetName);
    if (!pr) {
        std::fprintf(stderr, "error: unknown preset \"%s\" (see `montage-cli presets`)\n", presetName.c_str());
        return 1;
    }
    st = pr->settings;
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

int cmdInterchange(const std::vector<std::string>& args, bool otio) {
    if (args.empty()) return usage();
    std::string out;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
    Project p;
    if (!load(args[0], p)) return 1;
    std::string text = otio ? exportOtio(p, *p.active()) : exportEdl(p, *p.active());
    if (out.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "error: cannot write %s\n", out.c_str());
        return 1;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::printf("Wrote %s\n", out.c_str());
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
    if (cmd == "scenes") return cmdScenes(args);
    if (cmd == "proxy") return cmdProxy(args);
    if (cmd == "loudness") return cmdLoudness(args);
    if (cmd == "bench") return cmdBench(args);
    if (cmd == "edl") return cmdInterchange(args, false);
    if (cmd == "otio") return cmdInterchange(args, true);
    if (cmd == "--version" || cmd == "version") {
        std::printf("Montage %s\n", MONTAGE_VERSION);
        return 0;
    }
    return usage();
}
