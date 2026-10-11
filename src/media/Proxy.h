// Montage — proxy media (Premiere's and Resolve's proxy workflows, Final Cut's proxy media, Avid's dual-resolution
// media): lighter copies of camera files to edit with, made here in a choice of formats and sizes, with or without
// sound, HDR kept where the format can carry it; or the camera's own proxies attached by name. A file that is offline
// plays from its proxy (its picture, and its sound when the proxy carries sound), so a cut goes on with the proxies
// alone, as on a laptop away from the camera originals.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

enum class ProxyFormat {
    H264,         // fast-seeking intra-frame H.264 (.mp4)
    ProResProxy,  // ProRes 422 Proxy, 10-bit (.mov)
    DnxhrLb,      // DNxHR LB, 8-bit (.mov)
    Hevc10,       // 10-bit HEVC (.mp4): small, keeps HDR
};
struct ProxyOptions {
    ProxyFormat format = ProxyFormat::H264;
    int maxWidth = 960;  // at most this wide; 0 = half the source's width, -1 = a quarter
    bool sound = false;  // with the file's sound (AAC in .mp4, PCM in .mov)
};
const char* proxyFormatName(ProxyFormat f);  // "h264", "prores_proxy", "dnxhr_lb", "hevc10"
bool parseProxyFormat(const std::string& name, ProxyFormat& out);
std::string proxyExtension(ProxyFormat f);  // ".mp4" or ".mov"
// Whether the format keeps an HDR source's HDR (10-bit and tagged); the rest are tone mapped to Rec.709.
bool proxyKeepsHdr(ProxyFormat f);

using ProxyProgress = std::function<void(double fraction)>;
// Writes a proxy of a video file.
bool createProxy(const std::string& source, const std::string& dest, const ProxyOptions& options, const ProxyProgress& progress = {},
                 const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// The file a media item's picture is read from: its proxy when proxies are on, or when the file itself is offline and
// a proxy is there; else the file.
std::string pictureSource(const MediaItem& m, bool useProxies);
// Its sound: the file, or its proxy when the file is offline and the proxy carries sound.
std::string soundSource(const MediaItem& m);
// The file shot analysis reads (visual search and auto-tags, scene cuts, slates; not faces, whose boxes are in the
// file's pixels): the proxy when one is here at least 640 pixels wide (quicker to read than a camera file, and there
// when the file is offline), else the file.
std::string analysisSource(const MediaItem& m);
// Whether a file is there (looked at once every two seconds at most); whether it has a sound stream, and how wide its
// picture is (each probed once).
bool fileOnline(const std::string& path);
bool fileHasSound(const std::string& path);
int fileWidth(const std::string& path);
void forgetFileFacts(const std::string& path);  // (a file written again: looked at afresh)

// Camera proxies (the low-resolution files cameras record beside the originals, or proxies made elsewhere) for
// `media`, found under `folder`: a video file whose name begins with the media's own name (case aside; "_Proxy",
// "_S03" and the like may follow) and whose length matches within a second. By media id.
std::map<Id, std::string> findCameraProxies(const Project& p, const std::vector<Id>& media, const std::string& folder);

}  // namespace montage
