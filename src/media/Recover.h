// Montage — recovering interrupted recordings (as untrunc, Grau Video Repair and Descript's Fix file issue do; no
// NLE offers it). A camera or recorder that loses power, or whose card is pulled, leaves its last file without the
// index a player needs:
//  - an MP4 or MOV without its moov (the sample tables), rebuilt from a good clip from the same camera and settings:
//    its sample descriptions are kept, the media data is walked sample by sample (H.264 and HEVC by their NAL length
//    prefixes and headers, ProRes by its frame header, PCM by chunk, AAC by trial decoding), and a new moov written;
//  - an MXF without its footer (and with an open, incomplete header): re-indexed from its essence KLVs, with the
//    duration in a closed header and a footer carrying an index table;
//  - a WAV, BWF or RF64 whose RIFF and data sizes were never filled in, set from the file's length (RF64 past 4 GiB),
//    keeping bext and iXML.
// Clip-wrapped MXF (one KLV for the whole clip, as P2's OP-Atom files are) is not re-indexed: only frame-wrapped.
// The damaged file is never changed; the repair is written to a new file. Nothing here comes from untrunc (GPL-2.0
// only).
#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>

namespace montage {

enum class RepairKind { None, Movie, Mxf, Wav };

struct RepairCheck {
    RepairKind kind = RepairKind::None;  // what a repair would do; None when it opens or is not something Montage repairs
    bool opens = false;                  // FFmpeg opens it and finds its streams
    bool needsReference = false;         // a good clip from the same camera is needed (MP4 and MOV)
    QString problem;                     // what is wrong, in a sentence
};
// What is wrong with `path` and whether it can be repaired. Without `probe` FFmpeg is not asked whether it opens (only
// the file's structure is read).
RepairCheck checkRecording(const QString& path, bool probe = true);

struct RepairReport {
    RepairKind kind = RepairKind::None;
    int videoFrames = 0;
    int64_t audioSamples = 0;  // per channel
    double seconds = 0;        // of the repaired file
    int64_t bytesUsed = 0;     // of the damaged file's media data
    int64_t bytesLeft = 0;     // at its end, not whole samples (the recording's last moments)
    double timecode = -1;      // start timecode from the camera's sidecar, in seconds; -1 = none (starts at zero)
    QStringList notes;
};

// Whether two paths name the same file (by case too on Windows and macOS, whose file systems mostly ignore it).
bool sameFile(const QString& a, const QString& b);

// The file a repair of `damaged` is written to: beside it, "<name>_repaired.<ext>", numbered if that exists.
QString repairedPath(const QString& damaged);

// Repairs `damaged` into `output`, which may be neither it nor `reference` (an existing `output` is replaced: callers
// ask first). `reference` is a good clip from the same camera (MP4 and MOV only). `progress` gets 0..1 and returns
// false to cancel.
bool repairRecording(const QString& damaged, const QString& reference, const QString& output, RepairReport* report,
                     QString* error, const std::function<bool(double)>& progress = {});

// The start timecode a camera's sidecar beside `path` gives (Sony's <clip>M01.XML LtcChangeTable, a P2 or XF clip
// XML's StartTimecode), in seconds at `fps`; -1 when there is none.
double sidecarTimecode(const QString& path, double fps);

}  // namespace montage
