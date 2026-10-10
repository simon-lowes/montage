// Montage — hardware video decoding and encoding through FFmpeg.
//
// Decoding tries this platform's devices in order (VideoToolbox on macOS;
// D3D11VA, D3D12VA, CUDA, DXVA2 on Windows; VAAPI, CUDA on Linux) and falls
// back to software per stream whenever a device, codec or frame is not
// supported. Hardware frames are copied back to memory for the CPU pipeline;
// zero-copy display comes with the GPU compositor.
#pragma once

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
