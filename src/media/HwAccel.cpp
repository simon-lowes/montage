#include "HwAccel.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#include "HwVulkan.h"
#if defined(_WIN32) && __has_include(<libavutil/hwcontext_d3d11va.h>)
#define MONTAGE_HW_D3D11 1
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

namespace montage {

namespace {

std::atomic<int> gMode{-1};  // -1 = not yet read from the environment
std::atomic<int> gActiveHw{0};
std::atomic<uint64_t> gMapped{0};  // (a bit a device type)
std::atomic<long> gCopies{0};
std::mutex gUploadMutex;
std::string gUpload;

#ifdef MONTAGE_HW_D3D11
void lockD3D11(void*) { d3d11DeviceMutex().lock(); }
void unlockD3D11(void*) { d3d11DeviceMutex().unlock(); }
#endif
#ifdef MONTAGE_HW_VULKAN
void lockQueue(AVHWDeviceContext*, uint32_t, uint32_t) { vulkanQueueMutex().lock(); }
void unlockQueue(AVHWDeviceContext*, uint32_t, uint32_t) { vulkanQueueMutex().unlock(); }
#endif

}  // namespace

void setHwFramesMapped(const std::vector<AVHWDeviceType>& types) {
    uint64_t bits = 0;
    for (AVHWDeviceType t : types)
        if (int(t) > 0 && int(t) < 64) bits |= uint64_t(1) << int(t);
    gMapped = bits;
}

bool hwFramesMapped(AVHWDeviceType type) { return int(type) > 0 && int(type) < 64 && (gMapped.load() >> int(type) & 1); }

long hwFrameCopies() { return gCopies.load(); }
void countHwFrameCopy() { ++gCopies; }

std::recursive_mutex& d3d11DeviceMutex() {
    static std::recursive_mutex m;
    return m;
}

std::recursive_mutex& vulkanQueueMutex() {
    static std::recursive_mutex m;
    return m;
}

void setHwUploadForTesting(const std::string& how) {
    std::lock_guard lock(gUploadMutex);
    gUpload = how;
}

std::string hwUploadForTesting() {
    std::lock_guard lock(gUploadMutex);
    return gUpload;
}

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
    // (Vulkan Video ahead of CUDA where the GPU compositor takes Vulkan frames as they are: they need no copy then.)
    if (hwFramesMapped(AV_HWDEVICE_TYPE_VULKAN)) return {"vaapi", "vulkan", "cuda"};
    return {"vaapi", "cuda", "vulkan"};
#endif
}

bool hwDeviceDecodes(AVHWDeviceType type, AVBufferRef* device) {
    if (type != AV_HWDEVICE_TYPE_VULKAN) return device != nullptr;
#ifdef MONTAGE_HW_VULKAN
    return device && vulkanDecodeQueues(device);
#else
    return false;
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
#ifdef MONTAGE_HW_VULKAN
    // (Its queues locked by one mutex, which the GPU compositor takes too when it runs on this device.)
    if (dev && type == AV_HWDEVICE_TYPE_VULKAN) {
        auto* vk = static_cast<AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
        vk->lock_queue = lockQueue;
        vk->unlock_queue = unlockQueue;
    }
#endif
#ifdef MONTAGE_HW_D3D11
    // (Its context locked by one mutex, which the GPU compositor takes too when it runs on this device: set before any
    // decoder has the device.)
    if (dev && type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
        d3d->lock = lockD3D11;
        d3d->unlock = unlockD3D11;
    }
#endif
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
        // Media Foundation would otherwise settle for Microsoft's software encoder.
        if (name.find("_mf") != std::string::npos) av_dict_set(&opts, "hw_encoding", "1", 0);
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
