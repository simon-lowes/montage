#pragma once

// Hardware-decoded frames taken into the GPU compositor's API as they are, without a copy through memory (Phase 14):
// internal to render/GpuCompositor, built with it where Qt's RHI is.
//
// - Vulkan (Linux): the compositor runs on FFmpeg's own Vulkan device (sharedVulkanDevice), so Vulkan Video's frames
//   are on it already, and VA-API's are mapped onto it by FFmpeg through DMA-BUF. An image a plane is sampled where it
//   is; one image of all the planes (as Vulkan Video decodes into, which the RHI cannot sample a plane of) is copied
//   plane by plane on the GPU. Each waits for the frame's timeline semaphore first, and leaves the frame's layout and
//   semaphore as FFmpeg expects them.
// - OpenGL through EGL (Linux): VA-API's and Vulkan's frames as DMA-BUF planes, each an EGL image bound to a texture.
// - Metal (macOS): VideoToolbox's pixel buffers' IOSurfaces, a Metal texture a plane.
// - Direct3D 11 (Windows): the compositor runs on FFmpeg's D3D11VA device (sharedD3D11Device); each frame's slice of
//   the decoder's texture array is copied on the GPU into a texture the shaders can read, a view a plane.
// A frame that cannot be mapped is copied back by the caller (YcbcrPlanes' planesInMemory), as before.
//
// What takes work to make of a frame is made once and kept with the frame while the frame cache holds it
// (YcbcrPlanes::gpuKept), so a frame drawn again is not imported or copied again: VA-API's frame as a Vulkan frame or as
// EGL images, a CUDA frame copied by FFmpeg into a Vulkan frame (through memory Vulkan exports to CUDA), the planes of a
// one-image Vulkan frame copied on the GPU, a D3D11VA frame's slice copied on the GPU. Not done, as Qt's RHI gives no
// way to: a view of one plane of a multiplane image or of one slice of the decoder's texture array (those are still
// copied on the GPU, once a cached frame), and the decoder's semaphore waited for in the compositor's own submission
// (it is waited for before the frame is drawn, or in the copy's submission). D3D12VA's frames are not taken: FFmpeg
// does not make them shareable with another device.

#include <memory>
#include <string>
#include <vector>

#include <rhi/qrhi.h>

#include "media/Image.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace montage::gpu::hw {

// FFmpeg's Vulkan device for the compositor to run on, when there is one with a graphics queue (null otherwise, or
// with MONTAGE_GPU_SHARE=off): its instance (void*: a VkInstance) and the handles QRhi imports it by (a
// QRhiVulkanNativeHandles, filled in).
AVBufferRef* sharedVulkanDevice(void** instance, QRhiNativeHandles* handles);
// The same for Direct3D 11 (a QRhiD3D11NativeHandles).
AVBufferRef* sharedD3D11Device(QRhiNativeHandles* handles);
// (While the compositor draws on a shared D3D11 device, FFmpeg's decoders wait: held around each frame.)
void lockSharedD3D11(bool lock);

struct Planes {
    QRhiTexture* tex[3] = {};  // Y, then CbCr (or Cb and Cr): the formats the compositor uploads planes in
    bool copied = false;       // copied on the GPU for this draw (a device copy, not a read-back)
    bool imported = false;     // made for this draw (an import or a copy), not kept from an earlier draw of the frame
    std::string how;           // e.g. "mapped from vulkan, no copy"
};

class Mapper {
public:
    // What `rhi` can map (null for nothing), running on `device` (FFmpeg's, when it imported it).
    static std::unique_ptr<Mapper> make(QRhi* rhi, AVBufferRef* device);
    virtual ~Mapper() = default;
    // The device types whose frames it maps (decoders keep those in the device's memory: media/HwAccel).
    virtual std::vector<AVHWDeviceType> types() const = 0;
    // `yc`'s planes (a hardware frame's) as textures for this frame, kept until endFrame; false, with why, when it
    // cannot (on the compositor's thread, between passes of the frame `cb` records).
    virtual bool map(const YcbcrPlanes& yc, QRhiCommandBuffer* cb, Planes& out, std::string* why) = 0;
    // The frame finished on the GPU: what was mapped for it let go (and given back to FFmpeg as it expects).
    virtual void endFrame() = 0;
};

}  // namespace montage::gpu::hw
