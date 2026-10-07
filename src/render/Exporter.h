// Montage — rendering a sequence to a file with FFmpeg encoders.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct ExportSettings {
    std::string path;
    std::string videoCodec = "libx264";  // libx264, libx265, hw_h264, hw_hevc, prores_ks, dnxhd, libvpx-vp9, libsvtav1, mjpeg, none
    std::string audioCodec = "aac";      // aac, pcm_s16le, pcm_s24le, libopus, none
    int width = 0;                       // 0 = sequence size
    int height = 0;
    int crf = 18;
    int gop = 0;                         // keyframe interval in frames; 0 = 2 seconds
    int64_t videoBitrate = 0;            // bits/s; 0 = constant quality (crf)
    std::string preset = "medium";       // encoder speed preset
    std::string profile;                 // e.g. ProRes "hq" / "4444", DNxHR "dnxhr_hq"
    std::string pixFmt;                  // empty = sensible default for the codec
    int audioBitrate = 320000;
    int sampleRate = 0;                  // 0 = sequence rate
    FrameTime in = -1;                   // range; -1 = start / end of sequence
    FrameTime out = -1;
    bool useProxies = false;
    bool alpha = false;                  // keep transparency (ProRes 4444 etc.)
    // Captions from the sequence's visible caption track (or captionTrack):
    bool burnInCaptions = false;         // drawn into the picture
    bool embedCaptions = false;          // as a subtitle stream (MP4/MOV mov_text, MKV SubRip, WebM WebVTT)
    Id captionTrack = 0;                 // 0 = the first visible caption track
};

struct ExportPreset {
    std::string name;
    std::string extension;  // without dot
    std::string description;
    ExportSettings settings;
};
const std::vector<ExportPreset>& exportPresets();
const ExportPreset* findExportPreset(const std::string& name);

using ExportProgress = std::function<void(double fraction, FrameTime frame)>;

// videoCodec "hw_h264" / "hw_hevc" picks this machine's hardware encoder
// (VideoToolbox, NVENC, Quick Sync, AMF, Media Foundation) and falls back to
// x264 / x265; `encoderUsed` receives the encoder that ran.
bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed = nullptr);

// Renders an audio clip's sound with its effects (not its volume, pan or
// fades) to a 24-bit WAV, from its first frame to its last.
bool renderClipAudio(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Writes a single frame as PNG/JPEG/TIFF (by extension).
bool exportStill(const Project& p, const Sequence& seq, FrameTime t, const std::string& path, std::string* error);

}  // namespace montage
