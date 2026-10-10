// Montage — rendering a sequence to a file with FFmpeg encoders.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/LightLevel.h"

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
    // libx264, libx265, hw_h264, hw_hevc, prores_ks, dnxhd, cfhd, ffv1, v210, mpeg2video (XDCAM HD422), libvpx-vp9,
    // libsvtav1, mjpeg, none; also
    // gif (an animated GIF: its own palette, dithered) and png or tiff (an image sequence: the
    // path gets a frame number, name_000000.png, unless it already has a printf pattern).
    std::string videoCodec = "libx264";  // also exr (half float, scene-linear) and dpx (10-bit) image sequences
    int startNumber = 0;                 // an image sequence's first frame number
    std::string audioCodec = "aac";      // aac, pcm_s16le, pcm_s24le, libopus, flac, none
    int width = 0;                       // 0 = sequence size (a GIF: at most 480 wide)
    int height = 0;                      // 0 = in proportion to the width
    double fps = 0;                      // GIF frame rate; 0 = 15 (or the sequence's, if lower)
    int crf = 18;
    int gop = 0;                         // keyframe interval in frames; 0 = 2 seconds
    int64_t videoBitrate = 0;            // bits/s; 0 = constant quality (crf)
    std::string preset = "medium";       // encoder speed preset
    std::string profile;                 // e.g. ProRes "hq" / "4444", DNxHR "dnxhr_hq", x264 "avci100" (AVC-Intra 100)
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
    // The caption track as CEA-608 line-21 captions inside the video (A/53, as US broadcast and streaming platforms
    // take them): H.264 and HEVC only.
    bool cea608 = false;
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
    // The sequence's chapter markers become chapters in MP4, MOV and MKV files (players and YouTube read them).
    bool chapters = true;
    // Smart rendering: with ProRes or DNxHR, frames that are one untouched clip of footage already in the same
    // flavour, size and rate are copied from the source, not re-encoded.
    bool smartRender = true;
    // Masters with several audio streams (Resolve 21.1's separate output tracks, broadcast and streaming deliverables):
    // the mix first (named and tagged with a language), then one stream for each of these, each hearing only its
    // tracks (empty = all) and, when given, only clips of its role ("Dialogue", "Music", "Effects", "No Role"): an
    // M&E, a dialogue stem, a dubbed language. They are not loudness-normalised.
    struct AudioStream {
        std::string name;
        std::string language;  // ISO 639-1
        std::vector<bool> tracks;
        std::string role;
        std::vector<std::string> unmute;  // roles heard in this stream even where the sequence mutes them
        bool normalise = false;           // brought to the loudness target and limited like the mix (stems are not)
        bool descriptions = false;        // marked as audio description for the visually impaired
    };
    std::vector<AudioStream> extraAudio;
    // Audio description (core/AudioDescription.h): the mix without the descriptions (clips of the role "Description")
    // and, after it, a stream of the programme with them, named describedName in the mix's language.
    // Only where the container carries several audio streams (containerCarriesStreams); with stems, never.
    bool describedStream = false;
    std::string describedName = "Audio Description";
    std::string audioName, audioLanguage;  // the mix's title and language
    // Broadcast MXF (XDCAM HD422, AVC-Intra, DNxHR OP1a) carries each channel as its own mono track: the mix's channels,
    // then each extra stream's, then silence up to this many (0 = the mix as one stream, as everywhere else).
    int monoAudioTracks = 0;
    // The file's starting timecode ("10:00:00:00"; "" = none), where the container keeps one (MXF, MOV).
    std::string startTimecode;
    // With embedCaptions, these caption tracks too, each as its own subtitle stream (every language at once).
    std::vector<Id> extraCaptions;
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
// Stems for delivery (split-track masters): one 24-bit WAV per audio track,
// per bus (the tracks routed to it, and "Main" for those going straight to
// the master) or per audio role (Final Cut's role stems: Dialogue, Music,
// Effects and the editor's own, "No Role" for clips without one), each
// through its effects, fader and pan exactly as in the full mix, in the
// sequence's channel layout (or stereo with downmixStereo). Written beside
// `s.path` as "<name> - <stem>.wav"; silent and muted groups are skipped.
enum StemGroups : int { StemsByTrack = 1, StemsByBus = 2, StemsByRole = 3 };
struct StemFile {
    std::string name;
    std::string path;
};
bool exportStems(const Project& p, const Sequence& seq, const ExportSettings& s, int groups, std::vector<StemFile>* written,
                 const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// The streams a master can carry after its mix, split as stems are: one per audio track with clips, or one per role
// in use (StemsByTrack or StemsByRole), each named for it and in the language of the sequence's first caption track.
std::vector<ExportSettings::AudioStream> stemStreams(const Sequence& seq, int groups);

// Whether audio encoder `codec` opens with a sequence layout's channels (FFmpeg's layout for it).
bool exportCodecCarries(const std::string& codec, const std::string& layout);
// The layout an export in `codec` carries for a sequence mixed in `layout`: the layout when the codec takes it, else
// its ear-level fold (an immersive mix's heights folded into 5.1 or 7.1), else 5.1, else stereo (the mix's stereo
// fold-down, as ExportSettings::downmixStereo gives). exportSequence mixes in that layout unless the export has mono
// tracks; the export dialog's summary says when channels fold.
std::string exportAudioLayout(const std::string& layout, const std::string& codec);
// Whether the file `path` names (by its extension) can hold several audio streams: MP4 (M4A), MOV, MKV (MKA), MXF,
// WebM, TS.
bool containerCarriesStreams(const std::string& path);

// HDR exports measure their light levels as they render (`light`, when given); PQ files to MP4 and MOV carry the
// measured MaxCLL and MaxFALL. Encoders that state them before the first frame (x265) use the sequence's analysed
// levels, or the mastering peak when it has none.
bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed = nullptr,
                    int* smartRendered = nullptr,  // frames copied by smart rendering
                    LightLevels* light = nullptr);

// Renders an audio clip's sound with its effects (not its volume, pan or
// fades) to a 24-bit WAV, from its first frame to its last.
// Render and Replace for video: the clip alone through its effects (and speed), at its media's size, to ProRes 4444
// with its transparency; its transform is not baked in.
bool renderClipVideo(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error = nullptr,
                     const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);
bool renderClipAudio(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Writes a single frame as PNG/JPEG/TIFF (by extension).
bool exportStill(const Project& p, const Sequence& seq, FrameTime t, const std::string& path, std::string* error);

}  // namespace montage
