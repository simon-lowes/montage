// Montage — timeline interchange with other editors (Resolve, Premiere, Final
// Cut Pro, Avid, Nuke...): CMX 3600 EDL, OpenTimelineIO, Final Cut Pro 7 XML
// (xmeml) and FCPXML, both ways.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Model.h"

namespace montage {

// CMX 3600 EDL of one video track and the audio tracks (cuts and dissolves).
// The sequence as the exports write it: adjustment layers have no
// equivalent in other editors' interchange formats (they would arrive as
// solid mattes over the picture), so they are left out.
Sequence interchangeSequence(const Sequence& s);

std::string exportEdl(const Project& p, const Sequence& s, int videoTrack = 0);

// OpenTimelineIO (.otio, JSON) of the whole sequence: every track, gaps,
// transitions, speed changes, markers, generators.
std::string exportOtio(const Project& p, const Sequence& s);

// ---- Import ------------------------------------------------------------------
// Reads a media file's details (the app passes media::probeMedia); false when
// the file cannot be read, and the media item is then added offline.
using MediaProber = std::function<bool(const std::string& path, MediaItem& out)>;

struct ImportResult {
    bool ok = false;
    std::string error;
    Id sequence = 0;                    // the new sequence (made active)
    int clips = 0;
    std::vector<std::string> offline;   // media that could not be found
    std::vector<std::string> warnings;  // what could not be carried over
};

// Each adds the timeline as a new sequence of `p`, adding media items for
// the files it references (reusing project media with the same path).
ImportResult importOtio(Project& p, const std::string& json, const MediaProber& probe = {});
// `fps` is the EDL's frame rate (EDLs do not say). `mediaDir` is searched
// for clips the EDL names but does not locate.
ImportResult importEdl(Project& p, const std::string& text, Rational fps, const MediaProber& probe = {},
                       const std::string& mediaDir = {});

// Final Cut Pro 7 XML (xmeml 5), which Premiere Pro and Resolve read and write.
std::string exportFcp7Xml(const Project& p, const Sequence& s);
// FCPXML, version 1.15 (the newest) or "1.10": V1 as the
// primary storyline, other tracks as connected clips (each kept to the clip
// it was connected to when read from FCPXML), dissolves, titles, speed
// changes and their frame sampling, markers, caption tracks, audio roles,
// each clip's transform, trim, opacity, blend mode, volume and pan with
// their keyframes, and what a clip read from FCPXML held that Montage does
// not use, written back as it was.
std::string exportFcpXml(const Project& p, const Sequence& s, const std::string& version = "1.15");
// Either XML flavour (told apart by the root element).
ImportResult importXmlTimeline(Project& p, const std::string& xml, const MediaProber& probe = {});

}  // namespace montage
