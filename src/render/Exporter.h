// Montage — rendering a sequence to a file with FFmpeg encoders.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Exr.h"
#include "render/LightLevel.h"

namespace montage {

struct RenderOptions;

// Review-copy overlays burned into the picture (after captions), as an
// editor sends out for notes: timecode, the clip's name, any text, a logo.
struct BurnIn {
    bool timecode = false;          // the sequence's timecode
    bool clipName = false;          // the name of the top clip on screen
    bool footage = false;           // feet and frames from the sequence's start, in its film gauge (core/FilmCount.h)
    bool keyNumber = false;         // the key number of the top clip's frame (its media's "key_number")
    bool inkNumber = false;         // and its ink number
    std::string text;               // any text ("" = none)
    int corner = 0;                 // 0 top left, 1 top centre, 2 top right, 3 bottom left, 4 bottom centre, 5 bottom right
    double size = 0.035;            // text height, fraction of the frame height
    std::string watermark;          // image file ("" = none), e.g. a logo with transparency
    int watermarkCorner = 5;
    double watermarkWidth = 0.15;   // fraction of the frame width
    double watermarkOpacity = 0.6;
    bool any() const { return timecode || clipName || footage || keyNumber || inkNumber || !text.empty() || !watermark.empty(); }
};

struct ExportSettings {
    std::string path;
    // libx264, libx265, hw_h264, hw_hevc, prores_ks, dnxhd, cfhd, ffv1, v210, mpeg2video (XDCAM HD422), libvpx-vp9,
    // libsvtav1, liboapv (APV), mjpeg, none; also
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
    // A stereoscopic sequence's eyes as delivered (render/Stereo.h names): "sbs" side by side at full size (the
    // default), "sbs_half", "tb", "tb_half", or one picture ("left", "right", "anaglyph"). Packed exports carry stereo
    // metadata (MP4 st3d, Matroska StereoMode; FFmpeg writes none into MOV). The width and height asked for are each eye's.
    // "mv": spatial video, the eyes as MV-HEVC's two views in MOV or MP4 (H.265, 8-bit SDR; render/SpatialVideo.h),
    // stating the cameras' baseline and horizontal field of view.
    std::string stereo;
    double spatialBaseline = 64;      // mm
    double spatialFieldOfView = 60;   // degrees
    // Captions from the sequence's visible caption track (or captionTrack):
    bool burnInCaptions = false;         // drawn into the picture
    bool embedCaptions = false;          // as a subtitle stream (MP4/MOV mov_text, MKV SubRip, WebM WebVTT; MPEG-TS DVB pictures)
    // Embedded as pictures (render/ImageSubtitles.h) drawn in the track's style: "pgs" (Blu-ray) or "dvd" in Matroska,
    // "dvb" in Matroska or MPEG-TS. "" = as text where the file type takes text.
    std::string captionImages;
    Id captionTrack = 0;                 // 0 = the first visible caption track
    // The caption track as CEA-608 line-21 captions inside the video (A/53, as US broadcast and streaming platforms
    // take them): H.264 and HEVC only.
    bool cea608 = false;
    // With cea608, the same captions as CTA-708 service 1 too (core/Cea708.h: the digital captions US broadcast
    // requires, placed and coloured as the track shows them), beside the 608 bytes.
    bool cea708 = true;
    // OpenEXR frames written with the OpenEXR library (media/Exr.h) in this compression: none, rle, zips, zip, piz,
    // pxr24, b44, b44a, dwaa, dwab, zstd, htj2k256 or htj2k32 (High-Throughput JPEG 2000, lossless); "" = FFmpeg's
    // writer, ZIP in blocks of 16 lines.
    std::string exrCompression;
    // More layers in each OpenEXR frame (a VFX pull's source layers and mattes, render/VfxPull.h), written with the
    // OpenEXR library: called with the frame's sequence time and the picture's size, adding to `layers` (the picture is
    // the first, "") and header `attributes`. False stops the export with `error`.
    std::function<bool(FrameTime f, int width, int height, std::vector<ExrOutLayer>& layers, std::map<std::string, std::string>& attributes,
                       std::string& error)>
        exrLayers;
    // Delivery colour space (ColorSpace.h display space); "" = the sequence's.
    // An HDR sequence delivered in SDR is tone mapped. HDR output is 10-bit,
    // tagged, and PQ carries HDR10 mastering and light-level metadata.
    std::string colorSpace;
    // An HDR sequence delivered in SDR: how it is tone mapped (render/ColorSpace.h ToneMapping id), "" = Montage's
    // roll-off, "bt2446a" or "bt2446c". (A sequence graded scene-referred goes through its output transform instead.)
    std::string toneMapping;
    // A trim pass (render/TrimPasses.h): the id of one of the sequence's targets, delivered in its space and peak (in
    // place of colorSpace), each shot trimmed for it; "" = the master. Not for GIF or OpenEXR.
    std::string trimTarget;
    // HDR10+ dynamic metadata (PQ exports, render/Hdr10Plus.h): each scene measured (with the sequence's analysis
    // while it still matches the cut, else before the render) and carried in HEVC and AV1 video on every frame; the
    // same goes beside the file as name.hdr10plus.json, for other codecs and for encoders and checks that read it.
    bool hdr10Plus = false;
    // Adaptive HDR metadata (PQ and HLG exports, render/AdaptiveHdr.h): each scene's reference white, headroom and the
    // gain curve to its SDR rendition (its SDR trim, else Montage's own mapping), carried in HEVC and AV1 video on every
    // frame so displays of any headroom tone map it as the grade did.
    bool adaptiveHdr = false;
    // Loudness normalisation: the mix is measured first (ITU-R BS.1770) and
    // brought to this integrated loudness, with a limiter holding peaks under
    // the ceiling. 0 = off; e.g. -14 LUFS for streaming, -23 for EBU R128.
    double loudnessTarget = 0;
    double peakCeiling = -1;  // dBTP, when normalising
    // The target is the dialogue's loudness (render/DialogueLoudness.h), as Netflix's -27 LKFS is: the mix is measured
    // over its speech (over everything when speech is under 15 % of it).
    bool dialogueGated = false;
    // A loudness report of the sound as delivered (render/LoudnessReport.h), beside the file as name.loudness.pdf and
    // name.loudness.csv: integrated and dialogue loudness (the dialogue found from Dialogue clips or transcripts),
    // ranges, true peak, Leq(M), the readings every 100 ms and every stretch over the target (when normalising) or the
    // ceiling. leqmLimit adds a cinema Leq(M) limit to it (85 trailers, 82 advertising; 0 none).
    bool loudnessReport = false;
    double leqmLimit = 0;
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
    // tracks (empty = all) and, when given, only clips of its role ("Dialogue", "Music", "Effects", "No Role", or several
    // joined by "+", as "Music+Effects", unless a role has that very name): an M&E, a dialogue stem, a dubbed language. They are not loudness-normalised.
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
    // The FFmpeg muxer by name ("mxf_opatom": one picture track in Avid's OP-Atom, render/AvidMedia.h); "" = by the
    // path's extension. And container metadata for it (MXF: material_package_name, reel_name).
    std::string format;
    std::map<std::string, std::string> formatMetadata;
    // Delivered at another frame rate (render/RateConversion.h): "" at the sequence's own; "pulldown" (a 23.976 or 24
    // sequence with 2:3 or 2:2:2:4 pulldown: into fields at 29.97 or 30, coded interlaced, or repeated frames at 59.94
    // or 60); "speed" (each frame shown once at `rate`, the programme faster or slower, as PAL's 25 from 24, its sound
    // with its pitch kept or varispeed, its captions and markers with it); "convert" (a standards conversion at `rate`,
    // each frame made from the two nearest by optical flow, or RIFE where installed, never across a cut).
    std::string rateChange;
    Rational rate{0, 1};          // the rate delivered (pulldown: 0 = 29.97 or 30 interlaced)
    std::string cadence = "2:3";  // pulldown: "2:3" or "2:2:2:4"
    bool rateKeepPitch = true;    // a speed change's sound keeps its pitch
    // Coded as interlaced fields, the top first (each picture's two halves: PsF), as 1080i deliveries ask: MPEG-2,
    // H.264 (AVC-Intra), ProRes and DNxHD.
    bool interlaced = false;
    // Broadcast MXF by the book (render/BroadcastMxf.h), for an .mxf in XDCAM HD422 (MPEG-2) or AVC-Intra, rewrapped
    // after the render with the EBU's bmx: "" = FFmpeg's OP1a as it writes it; "rdd9" = RDD 9 (XDCAM HD422);
    // AS-11 "as11-dpp" (UK DPP HD: AVC-Intra 100, 1080i25, coded interlaced whatever `interlaced` says), "as11-x2"
    // (AVC-Intra HD), "as11-x8" (XDCAM HD422 in RDD 9, the NABA's).
    std::string mxfDelivery;
    // AS-11 descriptive metadata, "Name=value" a line, by the names AS-11 gives them (as11CoreProperties and
    // ukDppProperties). What is not given comes from the sequence: the programme title its name, the audio and caption
    // languages, captions present, EBU R 128 loudness when normalising to -23, audio description, open captions; the
    // DPP's line-up and ident clock from markers named "Line-up" (or "Bars") and "Clock" (or "Ident"), and the parts
    // from markers with a duration named "Part ...", in order (none: the programme is one part).
    std::vector<std::string> as11Metadata;
    // In such an MXF, the caption track as ancillary data in an ST 436 track: CEA-708 caption packets (with the 608
    // bytes, line 9), and OP-47 teletext subtitles on teletextPage (line 12, and 575 for the second field).
    bool ancCaptions = false;
    bool ancTeletext = false;
    int teletextPage = 888;
    // With embedCaptions, these caption tracks too, each as its own subtitle stream (every language at once).
    std::vector<Id> extraCaptions;
    // Content Credentials (media/ContentCredentials.h): the file signed with this certificate chain and key (PEM),
    // saying Montage made it, how AI was used in the range exported, and carrying its sources' own credentials. Empty =
    // not signed. An export that cannot be signed (a format that cannot hold them, a refused certificate) fails, leaving
    // no file.
    std::string credentialCertificate, credentialKey;
    bool credentialAllowAiTraining = false;
};

// Whether an export with these settings can be signed with Content Credentials: a single file of a kind that holds them.
bool exportCarriesCredentials(const ExportSettings& s);

struct ExportPreset {
    std::string name;
    std::string extension;  // without dot
    std::string description;
    ExportSettings settings;
};
const std::vector<ExportPreset>& exportPresets();
// Every preset any build offers, those needing an encoder this one lacks included (for listing them to translators).
std::vector<ExportPreset> exportPresetsOfEveryBuild();
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
enum StemGroups : int { StemsByTrack = 1, StemsByBus = 2, StemsByRole = 3, StemsDialogueAndME = 4 };
struct StemFile {
    std::string name;
    std::string path;
};
bool exportStems(const Project& p, const Sequence& seq, const ExportSettings& s, int groups, std::vector<StemFile>* written,
                 const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// The streams a master can carry after its mix, split as stems are: one per audio track with clips, one per role in
// use (StemsByTrack or StemsByRole), or the dialogue and the M&E (StemsDialogueAndME: the Dialogue role, then Music and
// Effects together), each named for it and in the language of the sequence's first caption track. A role muted or with
// no clip heard gets no stream (nor a part of the M&E).
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

// Where an export's HDR10+ metadata goes: "video" (HEVC and AV1, with the JSON beside it), "json" (other codecs: the
// JSON beside the file only), or "" when it has none (not PQ, EXR or GIF, no video).
std::string hdr10PlusCarriage(const Sequence& seq, const ExportSettings& s);
// Whether an export with these settings carries adaptive HDR metadata (PQ or HLG out, HEVC or AV1 video).
bool adaptiveHdrCarried(const Sequence& seq, const ExportSettings& s);

// An export's name for the files beside it, without the extension (an image sequence's frame number left out).
std::string exportSidecarStem(const std::string& path);
// Where an export's loudness report goes, without the extension: the sidecar stem + ".loudness".
std::string loudnessReportBase(const std::string& path);

// HDR exports measure their light levels as they render (`light`, when given); PQ files to MP4 and MOV carry the
// measured MaxCLL and MaxFALL. Encoders that state them before the first frame (x265) use the sequence's analysed
// levels, or the mastering peak when it has none.
// An export rendered whole but not signed with its Content Credentials fails and is kept, unsigned (`keptUnsigned` set):
// callers that clean up after a failure leave that file be.
bool exportSequence(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                    const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed = nullptr,
                    int* smartRendered = nullptr,  // frames copied by smart rendering
                    LightLevels* light = nullptr, bool* keptUnsigned = nullptr);

// Renders an audio clip's sound with its effects (not its volume, pan or
// fades) to a 24-bit WAV, from its first frame to its last.
// Render and Replace for video: the clip alone through its effects (and speed), at its media's size, to ProRes 4444
// (or, with `apv`, APV 4:4:4:4 12-bit) with its transparency; its transform is not baked in.
bool renderClipVideo(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error = nullptr,
                     const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, bool apv = false);
// Interchange formats (EDL, OpenTimelineIO, FCP XML, AAF) get a compositing graph's clip as its rendered picture: each
// clip of sequence `seq` in `p` with a graph (core/CompositeGraph.h) rendered, as the timeline shows its layer, to ProRes
// 4444 with its transparency (the frame's size, its place in it baked in) in a folder beside `output` ("name
// Composites"), and replaced in `p` by that file, its blend mode kept. `p` is the export's own copy. `baked` gets how
// many; false with `error` when one cannot be rendered.
bool bakeCompositeClips(Project& p, Id seq, const std::string& output, std::string* error = nullptr, int* baked = nullptr,
                        const std::atomic<bool>* cancel = nullptr);
// Whether FFmpeg here has APV's encoder (OpenAPV's liboapv, FFmpeg 8 and later) and decoder.
bool apvEncoderAvailable();
bool apvDecoderAvailable();
bool renderClipAudio(const Project& p, const Sequence& seq, Id clip, const std::string& path, std::string* error,
                     const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Export Frame's choices. `rendition`: "" the frame as SDR screens show it (an HDR sequence through its first SDR trim
// target, else mapped to Rec.709 as the viewer maps it; other sequences as they are), "master" the sequence's own
// space, or a trim target's id. An HDR picture is written as a 16-bit PNG with cICP, mDCV and cLLI, a 10-bit AVIF or HEIF, or a
// JPEG or (`gainMap`) AVIF or HEIF whose base is the SDR rendition with a gain map to the HDR one (TIFF cannot carry it);
// `quality` (0-100) is JPEG's, AVIF's and HEIF's.
struct StillOptions {
    std::string rendition;
    int quality = 90;
    bool gainMap = false;  // an HDR AVIF or HEIF written as an SDR base with a gain map (a JPEG always is)
};
// What was written: its space (ColorSpace.h id), bits per channel and, for HDR, the light levels it states (the
// sequence's measured ones for its master, else the frame's own) in nits.
struct StillResult {
    std::string colorSpace;
    int bits = 8;
    bool gainMap = false;  // an SDR base with a gain map (JPEG, AVIF or HEIF)
    double maxCll = 0, maxFall = 0;
};
// How Export Frame renders a rendition (StillOptions::rendition): `ro` set for it, the space and peak its pictures are
// in. False with `error` for an unknown trim target.
bool stillRendition(const Sequence& seq, const std::string& rendition, RenderOptions& ro, const ColorSpace*& space, double& peak,
                    std::string* error = nullptr);
// Writes a single frame as PNG, JPEG, TIFF or AVIF (by extension).
bool exportStill(const Project& p, const Sequence& seq, FrameTime t, const std::string& path, std::string* error,
                 const StillOptions& options = {}, StillResult* result = nullptr);

}  // namespace montage
