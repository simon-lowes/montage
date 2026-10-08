// Montage — rendering a sequence to a file with FFmpeg encoders.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

// Review-copy overlays burned into the picture (after captions), as an
// editor sends out for notes: timecode, the clip's name, any text, a logo.
struct BurnIn {
    bool timecode = false;          // the sequence's timecode
    bool clipName = false;          // the name of the top clip on screen
    std::string text;               // any text ("" = none)
    int corner = 0;                 // 0 top left, 1 top centre, 2 top right, 3 bottom left, 4 bottom centre, 5 bottom right
    double size = 0.035;            // text height, fraction of the frame height
    std::string watermark;          // image file ("" = none), e.g. a logo with transparency
    int watermarkCorner = 5;
    double watermarkWidth = 0.15;   // fraction of the frame width
    double watermarkOpacity = 0.6;
    bool any() const { return timecode || clipName || !text.empty() || !watermark.empty(); }
};

struct ExportSettings {
    std::string path;
    // libx264, libx265, hw_h264, hw_hevc, prores_ks, dnxhd, cfhd, ffv1, v210, libvpx-vp9, libsvtav1, mjpeg, none; also
    // gif (an animated GIF: its own palette, dithered) and png or tiff (an image sequence: the
    // path gets a frame number, name_000000.png, unless it already has a printf pattern).
    std::string videoCodec = "libx264";
    std::string audioCodec = "aac";      // aac, pcm_s16le, pcm_s24le, libopus, flac, none
    int width = 0;                       // 0 = sequence size (a GIF: at most 480 wide)
    int height = 0;                      // 0 = in proportion to the width
    double fps = 0;                      // GIF frame rate; 0 = 15 (or the sequence's, if lower)
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
    // Delivery colour space (ColorSpace.h display space); "" = the sequence's.
    // An HDR sequence delivered in SDR is tone mapped. HDR output is 10-bit,
    // tagged, and PQ carries HDR10 mastering and light-level metadata.
    std::string colorSpace;
    // Loudness normalisation: the mix is measured first (ITU-R BS.1770) and
    // brought to this integrated loudness, with a limiter holding peaks under
    // the ceiling. 0 = off; e.g. -14 LUFS for streaming, -23 for EBU R128.
    double loudnessTarget = 0;
    double peakCeiling = -1;  // dBTP, when normalising
    BurnIn burnIn;
    // A 5.1 or 7.1 sequence's audio is written with all its channels, or folded down to stereo.
    bool downmixStereo = false;
    // Only these audio tracks are heard (true = in); empty = all. Used for stems.
    std::vector<bool> audioTracks;
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
// Stems for delivery (split-track masters): one 24-bit WAV per audio track, or
// per bus (the tracks routed to it, and "Main" for those going straight to
// the master), each through its own effects, fader and pan exactly as in the
// full mix, in the sequence's channel layout (or stereo with downmixStereo).
// Written beside `s.path` as "<name> - <stem>.wav"; silent groups are skipped.
struct StemFile {
    std::string name;
    std::string path;
};
bool exportStems(const Project& p, const Sequence& seq, const ExportSettings& s, bool byBus, std::vector<StemFile>* written,
                 const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed = nullptr);

// Renders an audio clip's sound with its effects (not its volume, pan or
// fades) to a 24-bit WAV, from its first frame to its last.
bool renderClipAudio(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Writes a single frame as PNG/JPEG/TIFF (by extension).
bool exportStill(const Project& p, const Sequence& seq, FrameTime t, const std::string& path, std::string* error);

}  // namespace montage
