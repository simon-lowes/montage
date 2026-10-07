#include "HwAccel.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace montage {

namespace {

std::atomic<int> gMode{-1};  // -1 = not yet read from the environment
std::atomic<int> gActiveHw{0};

}  // namespace

void setHwDecodeMode(HwDecodeMode m) { gMode = int(m); }

HwDecodeMode hwDecodeMode() {
    int m = gMode.load();
    if (m < 0) {
        const char* env = std::getenv("MONTAGE_HWACCEL");
        m = env && (std::string(env) == "off" || std::string(env) == "0") ? int(HwDecodeMode::Off) : int(HwDecodeMode::Auto);
        gMode = m;
    }
    return HwDecodeMode(m);
}

std::vector<std::string> hwDeviceCandidates() {
#if defined(__APPLE__)
    return {"videotoolbox"};
#elif defined(_WIN32)
    return {"d3d11va", "d3d12va", "cuda", "dxva2"};
#else
    return {"vaapi", "cuda"};
#endif
}

AVBufferRef* hwDevice(AVHWDeviceType type) {
    static std::mutex m;
    static std::map<int, AVBufferRef*> devices;  // nullptr = tried and unavailable
    std::lock_guard lock(m);
    auto it = devices.find(int(type));
    if (it != devices.end()) return it->second;
    AVBufferRef* dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) dev = nullptr;
    devices[int(type)] = dev;  // kept for the process lifetime
    return dev;
}

bool acquireHwDecoderSlot() {
    int n = gActiveHw.load();
    while (n < kMaxHwDecoders)
        if (gActiveHw.compare_exchange_weak(n, n + 1)) return true;
    return false;
}

void releaseHwDecoderSlot() { --gActiveHw; }

int activeHwDecoders() { return gActiveHw.load(); }

std::vector<std::string> hwEncoderCandidates(const std::string& family) {
    const std::string f = family == "hevc" ? "hevc" : "h264";
#if defined(__APPLE__)
    return {f + "_videotoolbox"};
#elif defined(_WIN32)
    return {f + "_nvenc", f + "_qsv", f + "_amf", f + "_mf"};
#else
    return {f + "_nvenc", f + "_qsv"};
#endif
}

std::string pickHwEncoder(const std::string& family, int width, int height, int fpsNum, int fpsDen) {
    static std::mutex m;
    static std::map<std::string, std::string> picked;
    std::lock_guard lock(m);
    if (auto it = picked.find(family); it != picked.end() && !it->second.empty()) return it->second;
    for (const std::string& name : hwEncoderCandidates(family)) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) continue;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{fpsDen, fpsNum};
        ctx->framerate = AVRational{fpsNum, fpsDen};
        const bool nv12 = name.find("_qsv") != std::string::npos || name.find("_mf") != std::string::npos;
        ctx->pix_fmt = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        ctx->bit_rate = 8000000;
        AVDictionary* opts = nullptr;
        if (name.find("videotoolbox") != std::string::npos) av_dict_set(&opts, "allow_sw", "1", 0);
        const bool ok = avcodec_open2(ctx, codec, &opts) >= 0;
        av_dict_free(&opts);
        avcodec_free_context(&ctx);
        if (ok) {
            picked[family] = name;
            return name;
        }
    }
    return {};
}

}  // namespace montage
