// Montage — FFmpeg's Vulkan device, as decoding (media/HwAccel) and the GPU compositor (render/GpuHardware) share it.
// Linux only, where the Vulkan headers are found.
#pragma once

#if defined(__linux__) && __has_include(<vulkan/vulkan.h>) && __has_include(<libavutil/hwcontext_vulkan.h>)
#define MONTAGE_HW_VULKAN 1

#include <cstdint>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/version.h>
}

namespace montage {

// The device's queue families that can do `flags` (VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_VIDEO_DECODE_BIT_KHR): the first,
// with how many of its queues FFmpeg uses. False if it has none.
inline bool vulkanQueueFamily(const AVVulkanDeviceContext* vk, VkQueueFlags flags, uint32_t* family, int* count) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 39, 100)
    for (int i = 0; i < vk->nb_qf; ++i)
        if ((vk->qf[i].flags & flags) == flags && vk->qf[i].num > 0) {
            *family = uint32_t(vk->qf[i].idx);
            *count = vk->qf[i].num;
            return true;
        }
    return false;
#else
    int idx = -1, num = 0;
    if (flags & VK_QUEUE_GRAPHICS_BIT) idx = vk->queue_family_index, num = vk->nb_graphics_queues;
    else if (flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) idx = vk->queue_family_decode_index, num = vk->nb_decode_queues;
    if (idx < 0 || num <= 0) return false;
    *family = uint32_t(idx);
    *count = num;
    return true;
#endif
}

// A Vulkan device made by FFmpeg that decodes video (Vulkan Video): it has a video decoding queue.
inline bool vulkanDecodeQueues(AVBufferRef* device) {
    const auto* vk = static_cast<const AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
    uint32_t family = 0;
    int count = 0;
    return vulkanQueueFamily(vk, VK_QUEUE_VIDEO_DECODE_BIT_KHR, &family, &count);
}

// Waits on the host until the GPU's work on a Vulkan frame (`f`, of AV_PIX_FMT_VULKAN) is done: its timeline
// semaphores at their values. False if it is not done in `timeoutNs`.
inline bool vulkanFrameWait(const AVFrame* f, uint64_t timeoutNs) {
    auto* fc = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    auto* vk = static_cast<const AVVulkanDeviceContext*>(fc->device_ctx->hwctx);
    auto* vkfc = static_cast<AVVulkanFramesContext*>(fc->hwctx);
    auto* vkf = reinterpret_cast<AVVkFrame*>(f->data[0]);
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk->get_proc_addr(vk->inst, "vkGetDeviceProcAddr"));
    auto wait = gdpa ? reinterpret_cast<PFN_vkWaitSemaphores>(gdpa(vk->act_dev, "vkWaitSemaphores")) : nullptr;
    if (!wait) return false;
    VkSemaphore sems[AV_NUM_DATA_POINTERS] = {};
    uint64_t values[AV_NUM_DATA_POINTERS] = {};
    uint32_t n = 0;
    vkfc->lock_frame(fc, vkf);
    for (int i = 0; i < AV_NUM_DATA_POINTERS && vkf->img[i]; ++i)
        if (vkf->sem[i]) sems[n] = vkf->sem[i], values[n] = vkf->sem_value[i], ++n;
    vkfc->unlock_frame(fc, vkf);
    if (!n) return true;
    VkSemaphoreWaitInfo wi = {};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = n;
    wi.pSemaphores = sems;
    wi.pValues = values;
    return wait(vk->act_dev, &wi, timeoutNs) == VK_SUCCESS;
}

}  // namespace montage

#endif
