#include "GpuHardware.h"

#include <QtGlobal>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <type_traits>

#include "media/HwAccel.h"
#include "media/HwVulkan.h"

#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) && defined(MONTAGE_HW_VULKAN)
#define MONTAGE_GPU_HW_VULKAN 1
#endif
#if defined(Q_OS_LINUX) && QT_CONFIG(opengl) && __has_include(<EGL/egl.h>) && __has_include(<libavutil/hwcontext_drm.h>)
#define MONTAGE_GPU_HW_EGL 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <dlfcn.h>
extern "C" {
#include <libavutil/hwcontext_drm.h>
}
#endif
#if defined(Q_OS_MACOS) && QT_CONFIG(metal)
#define MONTAGE_GPU_HW_METAL 1
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <CoreVideo/CVPixelBufferIOSurface.h>
// CoreVideo's Metal texture cache, declared here with opaque pointers in place of its Objective-C types (the same
// functions, by their symbols), so this file stays C++.
extern "C" {
CVReturn montageMetalCacheCreate(CFAllocatorRef, CFDictionaryRef, void* device, CFDictionaryRef, void** cache) __asm__("_CVMetalTextureCacheCreate");
CVReturn montageMetalCacheTexture(CFAllocatorRef, void* cache, CVImageBufferRef, CFDictionaryRef, unsigned long pixelFormat, size_t width,
                                  size_t height, size_t plane, void** texture) __asm__("_CVMetalTextureCacheCreateTextureFromImage");
void* montageMetalTexture(void* texture) __asm__("_CVMetalTextureGetTexture");
void montageMetalCacheFlush(void* cache, CVOptionFlags) __asm__("_CVMetalTextureCacheFlush");
}
#endif
#if defined(Q_OS_WIN) && __has_include(<libavutil/hwcontext_d3d11va.h>)
#define MONTAGE_GPU_HW_D3D11 1
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

namespace montage::gpu::hw {

namespace {

// The texture formats the compositor uploads a plane in (GpuCompositor's planes path), and the plane's size.
QRhiTexture::Format planeFormat(const YcbcrPlanes& yc, int i) {
    using T = QRhiTexture;
    if (i > 0 && yc.interleaved) return yc.wide ? T::RG16 : T::RG8;
    return yc.wide ? T::R16 : T::R8;
}

QSize planeSize(const YcbcrPlanes& yc, int i, int w, int h) {
    return i ? QSize((w + (1 << yc.log2ChromaW) - 1) >> yc.log2ChromaW, (h + (1 << yc.log2ChromaH) - 1) >> yc.log2ChromaH) : QSize(w, h);
}

bool shareWanted() { return qgetenv("MONTAGE_GPU_SHARE").toLower() != "off"; }

#ifdef MONTAGE_GPU_HW_VULKAN
bool hasExtension(const AVVulkanDeviceContext* vk, const char* name) {
    for (int i = 0; i < vk->nb_enabled_dev_extensions; ++i)
        if (!std::strcmp(vk->enabled_dev_extensions[i], name)) return true;
    return false;
}
#endif

}  // namespace

AVBufferRef* sharedVulkanDevice(void** instance, QRhiNativeHandles* handles) {
#ifdef MONTAGE_GPU_HW_VULKAN
    if (!shareWanted()) return nullptr;
    AVBufferRef* dev = hwDevice(AV_HWDEVICE_TYPE_VULKAN);
    if (!dev) return nullptr;
    const auto* vk = static_cast<const AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
    uint32_t family = 0;
    int count = 0;
    if (!vulkanQueueFamily(vk, VK_QUEUE_GRAPHICS_BIT, &family, &count)) return nullptr;
    auto* h = static_cast<QRhiVulkanNativeHandles*>(handles);
    h->physDev = vk->phys_dev;
    h->dev = vk->act_dev;
    h->gfxQueueFamilyIdx = family;
    h->gfxQueueIdx = 0;
    *instance = vk->inst;
    return av_buffer_ref(dev);
#else
    (void)instance, (void)handles;
    return nullptr;
#endif
}


AVBufferRef* sharedD3D11Device(QRhiNativeHandles* handles) {
#ifdef MONTAGE_GPU_HW_D3D11
    if (!shareWanted()) return nullptr;
    AVBufferRef* dev = hwDevice(AV_HWDEVICE_TYPE_D3D11VA);
    if (!dev) return nullptr;
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
    // (FFmpeg's decoders and the compositor take turns at the device's context: one lock for both, media/HwAccel's.)
    if (!d3d->device || !d3d->device_context) return nullptr;
    auto* h = static_cast<QRhiD3D11NativeHandles*>(handles);
    h->dev = d3d->device;
    h->context = d3d->device_context;
    h->featureLevel = int(d3d->device->GetFeatureLevel());
    return av_buffer_ref(dev);
#else
    (void)handles;
    return nullptr;
#endif
}

void lockSharedD3D11(bool lock) {
#ifdef MONTAGE_GPU_HW_D3D11
    if (lock) d3d11DeviceMutex().lock();
    else d3d11DeviceMutex().unlock();
#else
    (void)lock;
#endif
}

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Vulkan: on FFmpeg's own device.

#ifdef MONTAGE_GPU_HW_VULKAN
class VulkanMapper : public Mapper {
public:
    VulkanMapper(QRhi* rhi, AVBufferRef* device) : rhi_(rhi), device_(av_buffer_ref(device)) {
        vk_ = static_cast<AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device_->data)->hwctx);
        auto gipa = vk_->get_proc_addr;
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(vk_->inst, "vkGetDeviceProcAddr"));
        if (!gdpa) return;
        auto load = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gdpa(vk_->act_dev, name)); };
        load(createCommandPool_, "vkCreateCommandPool");
        load(destroyCommandPool_, "vkDestroyCommandPool");
        load(resetCommandPool_, "vkResetCommandPool");
        load(allocateCommandBuffers_, "vkAllocateCommandBuffers");
        load(beginCommandBuffer_, "vkBeginCommandBuffer");
        load(endCommandBuffer_, "vkEndCommandBuffer");
        load(cmdPipelineBarrier_, "vkCmdPipelineBarrier");
        load(cmdCopyImage_, "vkCmdCopyImage");
        load(queueSubmit_, "vkQueueSubmit");
        load(getDeviceQueue_, "vkGetDeviceQueue");
        load(createFence_, "vkCreateFence");
        load(destroyFence_, "vkDestroyFence");
        load(waitForFences_, "vkWaitForFences");
        load(resetFences_, "vkResetFences");
        int count = 0;
        if (!createCommandPool_ || !destroyCommandPool_ || !resetCommandPool_ || !allocateCommandBuffers_ ||
            !beginCommandBuffer_ || !endCommandBuffer_ || !cmdPipelineBarrier_ || !cmdCopyImage_ || !queueSubmit_ || !getDeviceQueue_ ||
            !createFence_ || !destroyFence_ || !waitForFences_ || !resetFences_ || !vulkanQueueFamily(vk_, VK_QUEUE_GRAPHICS_BIT, &family_, &count))
            return;
        getDeviceQueue_(vk_->act_dev, family_, 0, &queue_);
        VkCommandPoolCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = family_;
        VkFenceCreateInfo fi = {};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (createCommandPool_(vk_->act_dev, &pi, vk_->alloc, &pool_) != VK_SUCCESS || createFence_(vk_->act_dev, &fi, vk_->alloc, &fence_) != VK_SUCCESS)
            return;
        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        ok_ = allocateCommandBuffers_(vk_->act_dev, &ai, &cmd_) == VK_SUCCESS;
        // (VA-API's frames come over through DMA-BUF, which FFmpeg imports where the device has both extensions.)
        dmabuf_ = hasExtension(vk_, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) && hasExtension(vk_, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    }
    ~VulkanMapper() override {
        endFrame();
        for (auto& [key, d] : derived_) av_buffer_unref(&d.source), av_buffer_unref(&d.vulkan);
        if (fence_) destroyFence_(vk_->act_dev, fence_, vk_->alloc);
        if (pool_) destroyCommandPool_(vk_->act_dev, pool_, vk_->alloc);
        av_buffer_unref(&device_);
    }
    bool ok() const { return ok_; }

    std::vector<AVHWDeviceType> types() const override {
        if (dmabuf_) return {AV_HWDEVICE_TYPE_VULKAN, AV_HWDEVICE_TYPE_VAAPI};
        return {AV_HWDEVICE_TYPE_VULKAN};
    }

    bool map(const YcbcrPlanes& yc, QRhiCommandBuffer*, Planes& out, std::string* why) override {
        std::shared_ptr<const AVFrame> frame = yc.hw;
        if (frame->format == AV_PIX_FMT_VAAPI) {
            // VA-API's surface onto this device, through DMA-BUF (FFmpeg's import).
            AVFrame* mapped = dmabuf_ ? mapOnto(frame.get()) : nullptr;
            if (!mapped) {
                *why = dmabuf_ ? "its VA-API surface could not be imported into Vulkan" : "this Vulkan device does not import DMA-BUF";
                return false;
            }
            frame = std::shared_ptr<const AVFrame>(mapped, [](const AVFrame* f) {
                AVFrame* g = const_cast<AVFrame*>(f);
                av_frame_free(&g);
            });
        }
        if (frame->format != AV_PIX_FMT_VULKAN) {
            *why = std::string("frames of ") + yc.hwType + " are not mapped into Vulkan";
            return false;
        }
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (fc->device_ctx != reinterpret_cast<AVHWDeviceContext*>(device_->data)) {
            *why = "a Vulkan frame of another device";
            return false;
        }
        auto* vkfc = static_cast<AVVulkanFramesContext*>(fc->hwctx);
        auto* vkf = reinterpret_cast<AVVkFrame*>(frame->data[0]);
        const int n = yc.interleaved ? 2 : 3;
        int images = 0;
        while (images < AV_NUM_DATA_POINTERS && vkf->img[images]) ++images;
        if (images == n) return wrap(yc, frame, fc, vkfc, vkf, out, why);
        if (images == 1) return copy(yc, frame, fc, vkfc, vkf, out, why);
        *why = "a Vulkan frame of " + std::to_string(images) + " images for " + std::to_string(n) + " planes";
        return false;
    }

    void endFrame() override {
        // The frame's images left as the compositor's last pass left them: FFmpeg's record of each says so.
        for (Wrapped& w : wrapped_) {
            auto* fc = reinterpret_cast<AVHWFramesContext*>(w.frame->hw_frames_ctx->data);
            auto* vkfc = static_cast<AVVulkanFramesContext*>(fc->hwctx);
            auto* vkf = reinterpret_cast<AVVkFrame*>(w.frame->data[0]);
            const QRhiTexture::NativeTexture native = w.tex->nativeTexture();
            vkfc->lock_frame(fc, vkf);
            vkf->layout[w.image] = VkImageLayout(native.layout);
            vkf->access[w.image] = VkAccessFlagBits(VK_ACCESS_SHADER_READ_BIT);
            vkfc->unlock_frame(fc, vkf);
            delete w.tex;
        }
        wrapped_.clear();
        for (QRhiTexture* t : copies_) delete t;
        copies_.clear();
        // (VA-API's frames let go once their decoder has, and none of its frames mapped here is left: the derived
        // frames hold the only other reference.)
        for (auto it = derived_.begin(); it != derived_.end();)
            if (av_buffer_get_ref_count(it->second.source) <= (it->second.vulkan ? 2 : 1) &&
                (!it->second.vulkan || av_buffer_get_ref_count(it->second.vulkan) == 1)) {
                av_buffer_unref(&it->second.source), av_buffer_unref(&it->second.vulkan);
                it = derived_.erase(it);
            } else {
                ++it;
            }
    }

private:
    struct Wrapped {
        std::shared_ptr<const AVFrame> frame;
        int image;
        QRhiTexture* tex;
    };

    // An image a plane: each sampled where it is, once the frame's semaphores say decoding (or uploading) is done.
    bool wrap(const YcbcrPlanes& yc, const std::shared_ptr<const AVFrame>& frame, AVHWFramesContext* fc, AVVulkanFramesContext* vkfc,
              AVVkFrame* vkf, Planes& out, std::string* why) {
        const int n = yc.interleaved ? 2 : 3;
        // (A frame drawn twice in this frame, as two layers: its images wrapped once, so their layouts stay known.)
        int found = 0;
        for (const Wrapped& w : wrapped_)
            if (w.frame->data[0] == frame->data[0] && w.image < n) out.tex[w.image] = w.tex, ++found;
        if (found == n) {
            out.how = std::string("mapped from ") + yc.hwType + "'s Vulkan images, no copy";
            return true;
        }
        std::fill(out.tex, out.tex + 3, nullptr);
        VkImageLayout layouts[3] = {};
        vkfc->lock_frame(fc, vkf);
        for (int i = 0; i < n; ++i) layouts[i] = vkf->layout[i];
        vkfc->unlock_frame(fc, vkf);
        if (!vulkanFrameWait(frame.get(), uint64_t(5) * 1000000000)) {
            *why = "the decoder did not finish the frame";
            return false;
        }
        QRhiTexture* made[3] = {};
        for (int i = 0; i < n; ++i) {
            made[i] = rhi_->newTexture(planeFormat(yc, i), planeSize(yc, i, fc->width, fc->height));
            if (!made[i]->createFrom({quint64(reinterpret_cast<quintptr>(vkf->img[i])), int(layouts[i])})) {
                for (QRhiTexture* t : made) delete t;
                *why = "the RHI could not take its Vulkan images";
                return false;
            }
        }
        for (int i = 0; i < n; ++i) {
            wrapped_.push_back({frame, i, made[i]});
            out.tex[i] = made[i];
        }
        out.how = std::string("mapped from ") + yc.hwType + "'s Vulkan images, no copy";
        return true;
    }

    // One image of all the planes: each plane copied on the GPU into a texture of its own, in a submission of its own
    // that waits for the frame's semaphore and signals it again when done (as FFmpeg's own work on a frame does).
    bool copy(const YcbcrPlanes& yc, const std::shared_ptr<const AVFrame>& frame, AVHWFramesContext* fc, AVVulkanFramesContext* vkfc,
              AVVkFrame* vkf, Planes& out, std::string* why) {
        const int n = yc.interleaved ? 2 : 3;
        QRhiTexture* made[3] = {};
        VkImage dst[3] = {};
        for (int i = 0; i < n; ++i) {
            made[i] = rhi_->newTexture(planeFormat(yc, i), planeSize(yc, i, fc->width, fc->height));
            if (!made[i]->create()) {
                for (QRhiTexture* t : made) delete t;
                *why = "no texture for its planes";
                return false;
            }
            dst[i] = reinterpret_cast<VkImage>(quintptr(made[i]->nativeTexture().object));
        }
        constexpr VkImageAspectFlagBits kPlanes[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_ASPECT_PLANE_2_BIT};
        vkfc->lock_frame(fc, vkf);
        resetCommandPool_(vk_->act_dev, pool_, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        beginCommandBuffer_(cmd_, &bi);
        auto barrier = [](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dstAccess) {
            VkImageMemoryBarrier b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = src, b.dstAccessMask = dstAccess;
            b.oldLayout = from, b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return b;
        };
        VkImageMemoryBarrier before[4];
        before[0] = barrier(vkf->img[0], vkf->layout[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (int i = 0; i < n; ++i)
            before[1 + i] = barrier(dst[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        cmdPipelineBarrier_(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(n + 1), before);
        for (int i = 0; i < n; ++i) {
            const QSize size = planeSize(yc, i, fc->width, fc->height);
            VkImageCopy region = {};
            region.srcSubresource = {VkImageAspectFlags(kPlanes[i]), 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {uint32_t(size.width()), uint32_t(size.height()), 1};
            cmdCopyImage_(cmd_, vkf->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        VkImageMemoryBarrier after[3];
        for (int i = 0; i < n; ++i)
            after[i] = barrier(dst[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT);
        cmdPipelineBarrier_(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(n), after);
        endCommandBuffer_(cmd_);
        const uint64_t wait = vkf->sem_value[0], signal = wait + 1;
        VkTimelineSemaphoreSubmitInfo ti = {};
        ti.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        ti.waitSemaphoreValueCount = 1, ti.pWaitSemaphoreValues = &wait;
        ti.signalSemaphoreValueCount = 1, ti.pSignalSemaphoreValues = &signal;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext = &ti;
        si.waitSemaphoreCount = 1, si.pWaitSemaphores = &vkf->sem[0], si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1, si.pCommandBuffers = &cmd_;
        si.signalSemaphoreCount = 1, si.pSignalSemaphores = &vkf->sem[0];
        VkResult rc;
        {
            std::lock_guard queue(vulkanQueueMutex());
            rc = queueSubmit_(queue_, 1, &si, fence_);
        }
        if (rc == VK_SUCCESS) {
            vkf->sem_value[0] = signal;
            vkf->layout[0] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkf->access[0] = VkAccessFlagBits(VK_ACCESS_TRANSFER_READ_BIT);
        }
        vkfc->unlock_frame(fc, vkf);
        if (rc != VK_SUCCESS || waitForFences_(vk_->act_dev, 1, &fence_, VK_TRUE, uint64_t(5) * 1000000000) != VK_SUCCESS) {
            if (rc == VK_SUCCESS) waitForFences_(vk_->act_dev, 1, &fence_, VK_TRUE, UINT64_MAX);  // (before the pool is reset)
            resetFences_(vk_->act_dev, 1, &fence_);
            for (QRhiTexture* t : made) delete t;
            *why = "its planes could not be copied on the GPU";
            return false;
        }
        resetFences_(vk_->act_dev, 1, &fence_);
        for (int i = 0; i < n; ++i) {
            made[i]->setNativeLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            copies_.push_back(made[i]);
            out.tex[i] = made[i];
        }
        (void)frame;
        out.copied = true;
        out.how = std::string("copied plane by plane on the GPU from ") + yc.hwType + "'s Vulkan image";
        return true;
    }

    // A VA-API frame as a Vulkan frame on this device (null if FFmpeg cannot map it).
    AVFrame* mapOnto(const AVFrame* src) {
        Derived& d = derived_[src->hw_frames_ctx->data];
        if (!d.source) d.source = av_buffer_ref(src->hw_frames_ctx);
        if (!d.vulkan) {
            if (d.failed) return nullptr;  // (tried once for these frames)
            if (av_hwframe_ctx_create_derived(&d.vulkan, AV_PIX_FMT_VULKAN, device_, src->hw_frames_ctx, 0) < 0) {
                d.vulkan = nullptr;
                d.failed = true;
                return nullptr;
            }
        }
        AVFrame* dst = av_frame_alloc();
        if (!dst) return nullptr;
        dst->format = AV_PIX_FMT_VULKAN;
        dst->hw_frames_ctx = av_buffer_ref(d.vulkan);
        if (!dst->hw_frames_ctx || av_hwframe_map(dst, src, AV_HWFRAME_MAP_READ) < 0) {
            av_frame_free(&dst);
            return nullptr;
        }
        return dst;
    }

    QRhi* rhi_;
    AVBufferRef* device_;
    AVVulkanDeviceContext* vk_ = nullptr;
    bool ok_ = false, dmabuf_ = false;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    PFN_vkCreateCommandPool createCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool_ = nullptr;
    PFN_vkResetCommandPool resetCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers_ = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer_ = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer_ = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier_ = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage_ = nullptr;
    PFN_vkQueueSubmit queueSubmit_ = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue_ = nullptr;
    PFN_vkCreateFence createFence_ = nullptr;
    PFN_vkDestroyFence destroyFence_ = nullptr;
    PFN_vkWaitForFences waitForFences_ = nullptr;
    PFN_vkResetFences resetFences_ = nullptr;
    std::vector<Wrapped> wrapped_;
    std::vector<QRhiTexture*> copies_;
    struct Derived {
        AVBufferRef* source = nullptr;  // (VA-API's frames, kept while their key is)
        AVBufferRef* vulkan = nullptr;
        bool failed = false;
    };
    std::map<const void*, Derived> derived_;
};
#endif

// ---------------------------------------------------------------------------------------------------------------------
// OpenGL through EGL: DMA-BUF planes, an EGL image each.

#ifdef MONTAGE_GPU_HW_EGL
class EglMapper : public Mapper {
public:
    // (Only where the compositor's context is EGL's, the EGL library loaded already by Qt's platform.)
    static std::unique_ptr<EglMapper> make(QRhi* rhi) {
        void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (!lib) return nullptr;
        auto m = std::unique_ptr<EglMapper>(new EglMapper(rhi, lib));
        return m->ok_ ? std::move(m) : nullptr;
    }
    ~EglMapper() override {
        endFrame();
        dlclose(lib_);
    }

    std::vector<AVHWDeviceType> types() const override {
        std::vector<AVHWDeviceType> t = {AV_HWDEVICE_TYPE_VAAPI};
#ifdef MONTAGE_HW_VULKAN
        // (Vulkan's frames leave as DMA-BUF only from a device with DRM format modifiers.)
        if (AVBufferRef* dev = hwDevice(AV_HWDEVICE_TYPE_VULKAN)) {
            const auto* vk = static_cast<const AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
            for (int i = 0; i < vk->nb_enabled_dev_extensions; ++i)
                if (!std::strcmp(vk->enabled_dev_extensions[i], VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) t.push_back(AV_HWDEVICE_TYPE_VULKAN);
        }
#endif
        return t;
    }

    bool map(const YcbcrPlanes& yc, QRhiCommandBuffer*, Planes& out, std::string* why) override {
        AVFrame* drm = av_frame_alloc();
        if (!drm) return false;
        drm->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(drm, yc.hw.get(), AV_HWFRAME_MAP_READ) < 0) {
            av_frame_free(&drm);
            *why = std::string("its ") + yc.hwType + " frame could not be exported as DMA-BUF";
            return false;
        }
        const auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(drm->data[0]);
        struct Plane {
            int object;
            ptrdiff_t offset, pitch;
        };
        std::vector<Plane> planes;
        for (int l = 0; l < desc->nb_layers; ++l)
            for (int p = 0; p < desc->layers[l].nb_planes; ++p)
                planes.push_back({desc->layers[l].planes[p].object_index, desc->layers[l].planes[p].offset, desc->layers[l].planes[p].pitch});
        const int n = yc.interleaved ? 2 : 3;
        if (int(planes.size()) != n) {
            av_frame_free(&drm);
            *why = "its DMA-BUF has " + std::to_string(planes.size()) + " planes for " + std::to_string(n);
            return false;
        }
        auto fourcc = [](char a, char b, char c, char d) { return EGLint(uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24); };
        QOpenGLFunctions* gl = QOpenGLContext::currentContext() ? QOpenGLContext::currentContext()->functions() : nullptr;
        if (!gl) {
            av_frame_free(&drm);
            *why = "no OpenGL context";
            return false;
        }
        Frame f;
        f.drm = drm;
        bool ok = true;
        for (int i = 0; i < n && ok; ++i) {
            const QRhiTexture::Format format = planeFormat(yc, i);
            const QSize size = planeSize(yc, i, yc.width, yc.height);
            const EGLint code = format == QRhiTexture::R8    ? fourcc('R', '8', ' ', ' ')
                                : format == QRhiTexture::RG8 ? fourcc('G', 'R', '8', '8')
                                : format == QRhiTexture::R16 ? fourcc('R', '1', '6', ' ')
                                                             : fourcc('G', 'R', '3', '2');
            const AVDRMObjectDescriptor& obj = desc->objects[planes[size_t(i)].object];
            std::vector<EGLint> attribs = {EGL_WIDTH, size.width(), EGL_HEIGHT, size.height(), EGL_LINUX_DRM_FOURCC_EXT, code,
                                           EGL_DMA_BUF_PLANE0_FD_EXT, obj.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(planes[size_t(i)].offset),
                                           EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(planes[size_t(i)].pitch)};
            constexpr uint64_t kInvalidModifier = 0x00ffffffffffffffULL;
            if (obj.format_modifier != kInvalidModifier && modifiers_) {
                attribs.insert(attribs.end(), {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(obj.format_modifier & 0xffffffff),
                                               EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(obj.format_modifier >> 32)});
            }
            attribs.push_back(EGL_NONE);
            EGLImageKHR image = createImage_(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
            if (image == EGL_NO_IMAGE_KHR) {
                ok = false;
                break;
            }
            f.images.push_back(image);
            GLuint tex = 0;
            gl->glGenTextures(1, &tex);
            f.gl.push_back(tex);
            gl->glBindTexture(GL_TEXTURE_2D, tex);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            target_(GL_TEXTURE_2D, image);
            gl->glBindTexture(GL_TEXTURE_2D, 0);
            QRhiTexture* t = rhi_->newTexture(format, size);
            f.tex.push_back(t);
            if (!t->createFrom({quint64(tex), 0})) ok = false;
            out.tex[i] = t;
        }
        frames_.push_back(std::move(f));
        if (!ok) {
            *why = "its DMA-BUF planes could not be imported into OpenGL";
            for (QRhiTexture*& t : out.tex) t = nullptr;
            return false;
        }
        out.how = std::string("mapped from ") + yc.hwType + " through DMA-BUF into OpenGL, no copy";
        return true;
    }

    void endFrame() override {
        if (frames_.empty()) return;
        rhi_->makeThreadLocalNativeContextCurrent();
        QOpenGLFunctions* gl = QOpenGLContext::currentContext() ? QOpenGLContext::currentContext()->functions() : nullptr;
        for (Frame& f : frames_) {
            for (QRhiTexture* t : f.tex) delete t;
            if (gl && !f.gl.empty()) gl->glDeleteTextures(GLsizei(f.gl.size()), f.gl.data());
            for (EGLImageKHR image : f.images) destroyImage_(display_, image);
            av_frame_free(&f.drm);
        }
        frames_.clear();
    }

private:
    EglMapper(QRhi* rhi, void* lib) : rhi_(rhi), lib_(lib) {
        auto getProc = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(lib, "eglGetProcAddress"));
        auto currentDisplay = reinterpret_cast<EGLDisplay (*)()>(dlsym(lib, "eglGetCurrentDisplay"));
        auto currentContext = reinterpret_cast<EGLContext (*)()>(dlsym(lib, "eglGetCurrentContext"));
        auto queryString = reinterpret_cast<const char* (*)(EGLDisplay, EGLint)>(dlsym(lib, "eglQueryString"));
        if (!getProc || !currentDisplay || !currentContext || !queryString || !rhi->makeThreadLocalNativeContextCurrent()) return;
        if (currentContext() == EGL_NO_CONTEXT || (display_ = currentDisplay()) == EGL_NO_DISPLAY) return;  // (GLX, say)
        const char* ext = queryString(display_, EGL_EXTENSIONS);
        if (!ext || !std::strstr(ext, "EGL_EXT_image_dma_buf_import")) return;
        modifiers_ = std::strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        createImage_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(getProc("eglCreateImageKHR"));
        destroyImage_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(getProc("eglDestroyImageKHR"));
        target_ = reinterpret_cast<void (*)(GLenum, void*)>(getProc("glEGLImageTargetTexture2DOES"));
        ok_ = createImage_ && destroyImage_ && target_;
    }

    struct Frame {
        AVFrame* drm = nullptr;
        std::vector<EGLImageKHR> images;
        std::vector<GLuint> gl;
        std::vector<QRhiTexture*> tex;
    };
    QRhi* rhi_;
    void* lib_;
    bool ok_ = false, modifiers_ = false;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC createImage_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage_ = nullptr;
    void (*target_)(GLenum, void*) = nullptr;
    std::vector<Frame> frames_;
};
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Metal: VideoToolbox's pixel buffers, a texture a plane of their IOSurface.

#ifdef MONTAGE_GPU_HW_METAL
class MetalMapper : public Mapper {
public:
    explicit MetalMapper(QRhi* rhi) : rhi_(rhi) {
        const auto* h = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
        if (!h || !h->dev || montageMetalCacheCreate(kCFAllocatorDefault, nullptr, h->dev, nullptr, &cache_) != kCVReturnSuccess) cache_ = nullptr;
    }
    ~MetalMapper() override {
        endFrame();
        if (cache_) CFRelease(cache_);
    }
    bool ok() const { return cache_ != nullptr; }
    std::vector<AVHWDeviceType> types() const override { return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX}; }

    bool map(const YcbcrPlanes& yc, QRhiCommandBuffer*, Planes& out, std::string* why) override {
        if (yc.hw->format != AV_PIX_FMT_VIDEOTOOLBOX || !yc.hw->data[3]) {
            *why = std::string("frames of ") + yc.hwType + " are not mapped into Metal";
            return false;
        }
        auto pixels = reinterpret_cast<CVPixelBufferRef>(yc.hw->data[3]);
        const int n = yc.interleaved ? 2 : 3;
        if (int(CVPixelBufferGetPlaneCount(pixels)) != n || !CVPixelBufferGetIOSurface(pixels)) {
            *why = "its pixel buffer is not planes on an IOSurface";
            return false;
        }
        // (MTLPixelFormatR8Unorm, RG8Unorm, R16Unorm and RG16Unorm)
        auto metalFormat = [](QRhiTexture::Format f) {
            return f == QRhiTexture::R8 ? 10ul : f == QRhiTexture::RG8 ? 30ul : f == QRhiTexture::R16 ? 20ul : 60ul;
        };
        for (int i = 0; i < n; ++i) {
            const QRhiTexture::Format format = planeFormat(yc, i);
            const size_t w = CVPixelBufferGetWidthOfPlane(pixels, size_t(i)), h = CVPixelBufferGetHeightOfPlane(pixels, size_t(i));
            void* texture = nullptr;
            if (montageMetalCacheTexture(kCFAllocatorDefault, cache_, pixels, nullptr, metalFormat(format), w, h, size_t(i), &texture) != kCVReturnSuccess ||
                !texture) {
                *why = "its IOSurface could not be taken into Metal";
                return false;
            }
            textures_.push_back(texture);
            QRhiTexture* t = rhi_->newTexture(format, QSize(int(w), int(h)));
            tex_.push_back(t);
            if (!t->createFrom({quint64(reinterpret_cast<quintptr>(montageMetalTexture(texture))), 0})) {
                *why = "the RHI could not take its Metal textures";
                return false;
            }
            out.tex[i] = t;
        }
        frames_.push_back(yc.hw);
        out.how = "mapped from videotoolbox's IOSurface into Metal, no copy";
        return true;
    }

    void endFrame() override {
        for (QRhiTexture* t : tex_) delete t;
        tex_.clear();
        for (void* t : textures_) CFRelease(t);
        textures_.clear();
        frames_.clear();
        if (cache_) montageMetalCacheFlush(cache_, 0);
    }

private:
    QRhi* rhi_;
    void* cache_ = nullptr;
    std::vector<void*> textures_;  // (CVMetalTextureRef)
    std::vector<QRhiTexture*> tex_;
    std::vector<std::shared_ptr<const AVFrame>> frames_;
};
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Direct3D 11: on FFmpeg's own device, each frame's slice copied on the GPU into a texture shaders can read.

#ifdef MONTAGE_GPU_HW_D3D11
class D3D11Mapper : public Mapper {
public:
    D3D11Mapper(QRhi* rhi, AVBufferRef* device) : rhi_(rhi), device_(av_buffer_ref(device)) {
        d3d_ = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device_->data)->hwctx);
    }
    ~D3D11Mapper() override {
        endFrame();
        av_buffer_unref(&device_);
    }
    std::vector<AVHWDeviceType> types() const override { return {AV_HWDEVICE_TYPE_D3D11VA}; }

    bool map(const YcbcrPlanes& yc, QRhiCommandBuffer*, Planes& out, std::string* why) override {
        if (yc.hw->format != AV_PIX_FMT_D3D11 || !yc.interleaved) {
            *why = std::string("frames of ") + yc.hwType + " are not mapped into Direct3D 11";
            return false;
        }
        const auto* fc = reinterpret_cast<const AVHWFramesContext*>(yc.hw->hw_frames_ctx->data);
        if (fc->device_ctx != reinterpret_cast<AVHWDeviceContext*>(device_->data)) {
            *why = "a Direct3D 11 frame of another device";
            return false;
        }
        auto* src = reinterpret_cast<ID3D11Texture2D*>(yc.hw->data[0]);
        const UINT index = UINT(reinterpret_cast<intptr_t>(yc.hw->data[1]));
        D3D11_TEXTURE2D_DESC desc;
        src->GetDesc(&desc);
        const UINT mips = desc.MipLevels;
        desc.ArraySize = 1, desc.MipLevels = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0, desc.CPUAccessFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        ID3D11Texture2D* own = nullptr;
        if (FAILED(d3d_->device->CreateTexture2D(&desc, nullptr, &own)) || !own) {
            *why = "this GPU's shaders cannot read its texture's format";
            return false;
        }
        owned_.push_back(own);
        d3d_->device_context->CopySubresourceRegion(own, 0, 0, 0, 0, src, index * mips, nullptr);  // (mip 0 of slice `index`)
        for (int i = 0; i < 2; ++i) {
            QRhiTexture* t = rhi_->newTexture(planeFormat(yc, i), planeSize(yc, i, int(desc.Width), int(desc.Height)));
            tex_.push_back(t);
            if (!t->createFrom({quint64(reinterpret_cast<quintptr>(own)), 0})) {
                *why = "the RHI could not take its texture's planes";
                return false;
            }
            out.tex[i] = t;
        }
        out.copied = true;
        out.how = "copied on the GPU from d3d11va's texture array, a view a plane";
        return true;
    }

    void endFrame() override {
        for (QRhiTexture* t : tex_) delete t;
        tex_.clear();
        for (ID3D11Texture2D* t : owned_) t->Release();
        owned_.clear();
    }

private:
    QRhi* rhi_;
    AVBufferRef* device_;
    AVD3D11VADeviceContext* d3d_ = nullptr;
    std::vector<QRhiTexture*> tex_;
    std::vector<ID3D11Texture2D*> owned_;
};
#endif

}  // namespace

std::unique_ptr<Mapper> Mapper::make(QRhi* rhi, AVBufferRef* device) {
    switch (rhi->backend()) {
#ifdef MONTAGE_GPU_HW_VULKAN
        case QRhi::Vulkan:
            if (device) {
                auto m = std::make_unique<VulkanMapper>(rhi, device);
                if (m->ok()) return m;
            }
            return nullptr;
#endif
#ifdef MONTAGE_GPU_HW_EGL
        case QRhi::OpenGLES2: return EglMapper::make(rhi);
#endif
#ifdef MONTAGE_GPU_HW_METAL
        case QRhi::Metal: {
            auto m = std::make_unique<MetalMapper>(rhi);
            if (m->ok()) return m;
            return nullptr;
        }
#endif
#ifdef MONTAGE_GPU_HW_D3D11
        case QRhi::D3D11:
            if (device) return std::make_unique<D3D11Mapper>(rhi, device);
            return nullptr;
#endif
        default: (void)device; return nullptr;
    }
}

}  // namespace montage::gpu::hw
