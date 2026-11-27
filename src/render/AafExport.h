// Montage — AAF export for audio post and conform (Pro Tools, Fairlight, Nuendo, Media
// Composer, Resolve, Premiere): the sequence's audio tracks as an AAF composition linked to
// mono 24-bit WAV files, one per channel of each source, written to a
// "<name> Media" folder beside it. Clip positions and source offsets carry
// over exactly, crossfades become AAF transitions (with their handles),
// fades the clips' fade properties, clip gain and its keyframes "Audio Gain" operations, a mono clip's pan
// and its keyframes "Audio Pan" operations, and each track's volume and pan (its fader and automation lanes) the same
// operations over the track's sequence. Keys are held (AAF's constant interpolation) or linear between points; a
// curve AAF cannot hold (smooth, Bezier, the easings, and gain, which AAF interpolates in amplitude, between keys far
// apart in level) gets points between its keys, close enough that it plays as it did. A speed ramp on a video clip is
// Motion Control with a varying speed ratio. Sound clips that change speed or play backwards, and
// nested sequences, are rendered to their own files first. The video tracks
// come too, linked to the original picture files where they are (a master
// clip and file mob each, with a CDCI descriptor and the file's URL): source
// offsets exact, dissolves as transitions at their edit points (reaching into
// the handles the media has), constant speed (and a conformed frame rate) as
// Motion Control; titles, generated clips and nested sequences leave gaps. Or everything with media Media Composer
// keeps as its own (AafAvidMedia).
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "Exporter.h"
#include "core/Model.h"

namespace montage {

// Media for Media Composer (render/AvidMedia.h): the cut's media written as MXF OP-Atom into a numbered folder of
// "Avid MediaFiles/MXF" and the AAF linked to it, so the sequence opens with its media and nothing to link or import.
// The picture at the sequence's size and rate in a DNxHD or DNxHR flavour, the sound as mono 24-bit PCM (each source
// whole, a file per channel). Left off (the default), each used stretch of a picture source (and `handles` frames
// either side, where the source has them) becomes one file, cut as it is in Montage, its grades and effects left
// behind; baked, each clip is rendered through its effects and transform to a file of its own, with its handles.
// Image sequences and Photoshop layers come too, and, baked, titles, generated clips and nested sequences.
struct AafAvidMedia {
    bool enabled = false;
    std::string drive;  // where "Avid MediaFiles" is ("" = the AAF's folder), or that folder, or its MXF folder
    std::string codec = "dnxhr_sq";
    int handles = 30;  // frames
    bool bake = false;
};

struct AafExportOptions {
    bool picture = true;  // the video tracks too, linked to the original files
    AafAvidMedia avid;
    // The tracks' volume automation (and faders) folded into each clip's gain, for applications that read only clip
    // gain; left off, each track's volume and pan are written over its sequence.
    bool bakeAutomation = false;
};

struct AafExportResult {
    int audioTracks = 0;  // AAF audio tracks (mono: a stereo track is two)
    int clips = 0;        // sound source clips placed
    int transitions = 0;  // crossfades
    int videoTracks = 0;
    int videoClips = 0;
    int videoTransitions = 0;  // dissolves
    int curves = 0;            // lanes with curves AAF cannot hold, written as points along them
    int trackLanes = 0;        // track volume and pan written over tracks' sequences
    std::vector<std::string> mediaFiles;
    std::string mediaFolder;  // with Avid media: the numbered folder it went to
    std::vector<std::string> warnings;
};

bool exportAaf(const Project& p, const Sequence& seq, const std::string& path, AafExportResult* result = nullptr,
               const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr,
               const AafExportOptions& options = {});

}  // namespace montage
