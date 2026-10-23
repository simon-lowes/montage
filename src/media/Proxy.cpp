#include "Proxy.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>

#include "core/EditOps.h"
#include "core/Interpretation.h"
#include "media/Decoder.h"
#include "media/ImageSequence.h"
#include "render/ColorSpace.h"
#include "render/Exporter.h"

namespace montage {

const char* proxyFormatName(ProxyFormat f) {
    switch (f) {
        case ProxyFormat::H264: return "h264";
        case ProxyFormat::ProResProxy: return "prores_proxy";
        case ProxyFormat::DnxhrLb: return "dnxhr_lb";
        case ProxyFormat::Hevc10: return "hevc10";
        case ProxyFormat::Apv: return "apv";
    }
    return "h264";
}

bool parseProxyFormat(const std::string& name, ProxyFormat& out) {
    for (ProxyFormat f : {ProxyFormat::H264, ProxyFormat::ProResProxy, ProxyFormat::DnxhrLb, ProxyFormat::Hevc10, ProxyFormat::Apv})
        if (name == proxyFormatName(f)) {
            out = f;
            return true;
        }
    return false;
}

std::string proxyExtension(ProxyFormat f) {
    return f == ProxyFormat::ProResProxy || f == ProxyFormat::DnxhrLb || f == ProxyFormat::Apv ? ".mov" : ".mp4";
}

bool proxyKeepsHdr(ProxyFormat f) { return f == ProxyFormat::ProResProxy || f == ProxyFormat::Hevc10 || f == ProxyFormat::Apv; }

bool createProxy(const std::string& source, const std::string& dest, const ProxyOptions& o, const ProxyProgress& progress,
                 const std::atomic<bool>* cancel, std::string* error) {
    // ProRes and DNxHR (and their PCM sound) live in QuickTime or MXF, not MP4.
    const QString ext = QFileInfo(QString::fromStdString(dest)).suffix().toLower();
    if ((o.format == ProxyFormat::ProResProxy || o.format == ProxyFormat::DnxhrLb) && ext != QLatin1String("mov") && ext != QLatin1String("mxf")) {
        if (error) *error = "ProRes and DNxHR proxies are .mov files";
        return false;
    }
    if (o.format == ProxyFormat::Apv) {
        if (ext != QLatin1String("mov")) {
            if (error) *error = "APV proxies are .mov files";
            return false;
        }
        if (!avcodec_find_encoder_by_name("liboapv")) {
            if (error) *error = "APV needs FFmpeg 8 or later with the OpenAPV encoder (liboapv)";
            return false;
        }
    }
    MediaItem m;
    if (!probeMedia(source, m, error)) return false;
    if (!m.hasVideo || m.kind != MediaKind::Video) {
        if (error) *error = "Proxies are only made for video files";
        return false;
    }
    // Rendered through a sequence of the file's own size and rate (and, where the format can carry it, its HDR).
    Project p = makeDefaultProject();
    Sequence& s = *p.active();
    s.width = m.width;
    s.height = m.height;
    s.fps = m.fps.valid() ? m.fps : Rational{30, 1};
    // 360° footage whole, not as a view. (A stereo file's proxy holds its left eye: the right is read from the file.)
    s.spherical = !m.projection.empty();
    s.vr180 = m.projection == "vr180";
    const ColorSpace& space = mediaColorSpace(m);
    if (space.hdr() && proxyKeepsHdr(o.format)) s.colorSpace = space.id;
    m.id = p.newId();
    p.media.push_back(m);
    const bool sound = o.sound && m.hasAudio;
    edit::placeMedia(p, s, m.id, 0, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false);
    ExportSettings st;
    st.path = dest;
    st.gop = 1;  // intra-only: every frame is a seek point
    switch (o.format) {
        case ProxyFormat::H264:
            st.videoCodec = "libx264";
            st.crf = 23;
            st.preset = "veryfast";
            st.audioCodec = "aac";
            st.audioBitrate = 160000;
            break;
        case ProxyFormat::ProResProxy:
            st.videoCodec = "prores_ks";
            st.profile = "proxy";
            st.audioCodec = "pcm_s16le";
            break;
        case ProxyFormat::DnxhrLb:
            st.videoCodec = "dnxhd";
            st.profile = "dnxhr_lb";
            st.audioCodec = "pcm_s16le";
            break;
        case ProxyFormat::Hevc10:
            // x265 where FFmpeg has it, else the computer's hardware HEVC encoder.
            st.videoCodec = avcodec_find_encoder_by_name("libx265") ? "libx265" : "hw_hevc";
            st.crf = 26;
            st.preset = "veryfast";
            st.pixFmt = st.videoCodec == "libx265" ? "yuv420p10le" : "";
            st.audioCodec = "aac";
            st.audioBitrate = 160000;
            break;
        case ProxyFormat::Apv:
            st.videoCodec = "liboapv";
            st.crf = 36;  // its QP: about ProRes Proxy's size
            st.preset = "fastest";
            st.pixFmt = "yuv422p10le";
            st.audioCodec = "pcm_s16le";
            break;
    }
    if (!sound) st.audioCodec = "none";
    double scale = o.maxWidth > 0 ? std::min(1.0, double(o.maxWidth) / std::max(1, m.width)) : o.maxWidth == 0 ? 0.5 : 0.25;
    // (DNxHR wants whole 16-pixel blocks and at least 256 x 120, so a small proxy stays that big; the others even.)
    const bool dnx = o.format == ProxyFormat::DnxhrLb;
    if (dnx) scale = std::max({scale, 256.0 / std::max(1, m.width), 128.0 / std::max(1, m.height)});
    const int align = dnx ? 16 : 2;
    st.width = std::max(dnx ? 256 : align, int(std::lround(m.width * scale)) / align * align);
    st.height = std::max(dnx ? 128 : align, int(std::lround(m.height * scale)) / align * align);
    return exportSequence(p, s, st, [&](double f, FrameTime) { if (progress) progress(f); }, cancel, error);
}

namespace {

struct Seen {
    bool yes = false;
    std::chrono::steady_clock::time_point when;
};
std::mutex gSeenMutex;
std::map<std::string, Seen> gOnline;
struct Probed {
    bool sound = false;
    int width = 0;
    std::string colorSpace;
};
std::map<std::string, Probed> gProbed;

Probed probed(const std::string& path) {
    {
        std::lock_guard lock(gSeenMutex);
        const auto it = gProbed.find(path);
        if (it != gProbed.end()) return it->second;
    }
    MediaItem m;
    Probed out;
    if (probeMedia(path, m)) out = {m.hasAudio, m.hasVideo ? m.width : 0, m.colorSpace};
    std::lock_guard lock(gSeenMutex);
    gProbed[path] = out;
    return out;
}

}  // namespace

bool fileOnline(const std::string& path) {
    if (path.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(gSeenMutex);
        const auto it = gOnline.find(path);
        if (it != gOnline.end() && now - it->second.when < std::chrono::seconds(2)) return it->second.yes;
    }
    const bool yes = QFileInfo::exists(QString::fromStdString(mediaFileOnDisk(path)));
    std::lock_guard lock(gSeenMutex);
    gOnline[path] = {yes, now};
    return yes;
}

bool fileHasSound(const std::string& path) { return probed(path).sound; }

int fileWidth(const std::string& path) { return probed(path).width; }

void forgetFileFacts(const std::string& path) {
    std::lock_guard lock(gSeenMutex);
    gOnline.erase(path);
    gProbed.erase(path);
}

std::string pictureSource(const MediaItem& m, bool useProxies, bool offlineProxies) {
    if (m.proxyPath.empty()) return m.path;
    const bool here = fileOnline(m.proxyPath);
    if (useProxies && here) return m.proxyPath;  // (a proxy that is gone: the file)
    return offlineProxies && here && !fileOnline(m.path) ? m.proxyPath : m.path;
}

std::string soundSource(const MediaItem& m, bool offlineProxies) {
    if (!offlineProxies || m.proxyPath.empty() || fileOnline(m.path) || !fileOnline(m.proxyPath)) return m.path;
    return fileHasSound(m.proxyPath) ? m.proxyPath : m.path;
}

const ColorSpace& pictureColorSpace(const MediaItem& m, const std::string& path) {
    // A gain-map JPEG's SDR picture (read in place of its HDR rendition: render/Compositor.cpp).
    if (m.gainMap && m.colorOverride.empty() && path != m.path && path == gainMapPath(m.path, false)) return rec709Space();
    if (m.proxyPath.empty() || path != m.proxyPath) return mediaColorSpace(m);
    const ColorSpace* c = findColorSpace(probed(path).colorSpace);
    return c ? *c : rec709Space();
}

std::string analysisSource(const MediaItem& m) {
    if (m.proxyPath.empty() || !fileOnline(m.proxyPath) || fileWidth(m.proxyPath) < 640) return m.path;
    return m.proxyPath;
}

std::map<Id, std::string> findCameraProxies(const Project& p, const std::vector<Id>& media, const std::string& folder) {
    // The project's own files are originals, never another's proxy (a multicam shoot's "Scene1_CamB" for "Scene1").
    std::set<QString> originals;
    for (const MediaItem& m : p.media)
        if (!m.path.empty()) originals.insert(QFileInfo(QString::fromStdString(mediaFileOnDisk(m.path))).absoluteFilePath());
    // The folder's video files once, by lower-case name.
    std::vector<std::pair<QString, QString>> files;  // (base name lower-case, path)
    QDirIterator it(QString::fromStdString(folder), QDir::Files, QDirIterator::Subdirectories);
    static const QStringList kVideo = {"mp4", "mov", "mxf", "m4v", "mkv", "avi", "mts", "m2ts"};
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        if (kVideo.contains(fi.suffix().toLower()) && !originals.count(fi.absoluteFilePath()))
            files.push_back({fi.completeBaseName().toLower(), fi.absoluteFilePath()});
    }
    std::map<Id, std::string> found;
    for (Id id : media) {
        const MediaItem* m = p.findMedia(id);
        if (!m || m->kind != MediaKind::Video || m->path.empty() || m->subclipOf) continue;
        const QFileInfo original(QString::fromStdString(mediaFileOnDisk(m->path)));
        const QString name = original.completeBaseName().toLower();
        // The closest name first (exactly the same, then the shortest that begins with it and goes on after a
        // separator: "A001_Proxy" or "A001 S03" for "A001", never "A0010"), its length and shape checked.
        std::vector<std::pair<QString, QString>> candidates;
        for (const auto& f : files)
            if (f.first.startsWith(name) && (f.first.size() == name.size() || !f.first.at(name.size()).isLetterOrNumber()) &&
                f.second != original.absoluteFilePath())
                candidates.push_back(f);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first.size() < b.first.size(); });
        for (const auto& c : candidates) {
            MediaItem probe;
            if (!probeMedia(c.second.toStdString(), probe) || !probe.hasVideo) continue;
            if (m->duration > 0 && std::fabs(probe.duration - m->duration) > 1.0) continue;
            if (m->width > 0 && m->height > 0 && probe.width > 0 && probe.height > 0 &&
                std::fabs(double(probe.width) / probe.height - double(m->width) / m->height) > 0.03 * double(m->width) / m->height)
                continue;  // (another shape: not this shot)
            found[id] = c.second.toStdString();
            break;
        }
    }
    return found;
}

}  // namespace montage
