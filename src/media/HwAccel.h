// Montage — hardware video decoding and encoding through FFmpeg.
//
// Decoding tries this platform's devices in order (VideoToolbox on macOS;
// D3D11VA, D3D12VA, CUDA, DXVA2 on Windows; VAAPI, CUDA, Vulkan Video on Linux,
// Vulkan ahead of CUDA while the GPU compositor maps Vulkan frames) and falls
// back to software per stream whenever a device, codec or frame is not
// supported. Hardware frames are copied back to memory for the CPU pipeline,
// or, where the GPU compositor maps a device's frames into its own API
// (render/GpuCompositor), left in the device's memory for it.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace montage {

enum class HwDecodeMode { Off, Auto };

// Default Auto; MONTAGE_HWACCEL=off in the environment starts with it Off.
// Changing it affects decoders opened afterwards.
void setHwDecodeMode(HwDecodeMode m);
HwDecodeMode hwDecodeMode();

// Device type names tried for decoding on this platform, in order.
std::vector<std::string> hwDeviceCandidates();

// The shared device context for a type, created on first use (borrowed;
// av_buffer_ref it). nullptr when the device is unavailable (remembered).
AVBufferRef* hwDevice(AVHWDeviceType type);
// Whether that device decodes video (a Vulkan device needs a video decoding queue).
bool hwDeviceDecodes(AVHWDeviceType type, AVBufferRef* device);

// The device types whose frames the GPU compositor takes as they are (set while it runs): decoders of those keep
// their frames in the device's memory (YcbcrPlanes::hw) rather than copying them back.
void setHwFramesMapped(const std::vector<AVHWDeviceType>& types);
bool hwFramesMapped(AVHWDeviceType type);
// Hardware frames copied back to memory since the program started: after decoding, as before, or later, when the CPU
// needed one that was kept for the GPU (planesInMemory), or when the GPU could not map it.
long hwFrameCopies();
void countHwFrameCopy();
// Kept frames a decoder holds at most before it copies back again (fixed pools, VA-API's and D3D11's, have this many
// surfaces more than decoding needs).
constexpr int kMaxKeptHwFrames = 16;
// The Vulkan device's queues, shared by FFmpeg and the GPU compositor when it runs on that device (each submits while
// holding this; FFmpeg through its lock_queue).
std::recursive_mutex& vulkanQueueMutex();
// FFmpeg's D3D11VA device's context, locked by its decoders through this (set when the device is made) and by the
// GPU compositor while it draws on that device.
std::recursive_mutex& d3d11DeviceMutex();
// Tests: frames decoded in software handed on as a hardware decoder would hand them, uploaded by FFmpeg to that
// device's frames ("vulkan": an image a plane; "vulkan-multiplane": one image of all the planes, as Vulkan Video
// decodes into), with "" for none. Decoders opened afterwards.
void setHwUploadForTesting(const std::string& how);
std::string hwUploadForTesting();

// Hardware decoders are a limited resource (NVDEC sessions, VRAM): at most
// kMaxHwDecoders streams decode in hardware, the rest in software.
constexpr int kMaxHwDecoders = 8;
bool acquireHwDecoderSlot();
void releaseHwDecoderSlot();
int activeHwDecoders();

// Hardware encoders tried for a family ("h264" or "hevc"), in order.
std::vector<std::string> hwEncoderCandidates(const std::string& family);
// The first candidate that opens for this size and rate, or "" if none
// (cached per family after the first successful probe).
std::string pickHwEncoder(const std::string& family, int width, int height, int fpsNum, int fpsDen);

}  // namespace montage
