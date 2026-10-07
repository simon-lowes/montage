// Montage — building an imported timeline: a new sequence, its tracks and
// clips, and media items for the files it references (internal to the
// interchange readers).
#pragma once

#include <map>
#include <string>

#include "Interchange.h"

namespace montage {

class TimelineBuilder {
public:
    TimelineBuilder(Project& p, const std::string& name, Rational fps, const MediaProber& probe);

    Sequence& sequence() { return seq_; }
    // Track `index` of a kind, adding tracks up to it.
    Track& track(TrackKind kind, int index);
    // The media item for a file: the project's own if it already has the path,
    // else probed (or added offline with the details given). `seconds` is
    // the media length the timeline implies.
    Id media(const std::string& path, const std::string& name, bool video, bool audio, double seconds);
    // A clip at sequence frames [start, start + duration) showing the media
    // from `sourceIn` (sequence frames). The pointer is valid until the next add.
    Clip* addClip(TrackKind kind, int track, Id media, FrameTime start, FrameTime duration, double sourceIn,
                  const std::string& name = {});
    Clip* addGenerator(int track, const std::string& type, FrameTime start, FrameTime duration, const std::string& name = {});
    // A transition centred on the edit between two adjacent clips.
    void addTransition(TrackKind kind, int track, Id clipA, Id clipB, FrameTime duration, const std::string& type);
    void warn(const std::string& w) { res_.warnings.push_back(w); }
    // Sorts tracks, links picture and sound of the same source, and adds the
    // sequence to the project (active).
    ImportResult finish();

private:
    void linkMatching();

    Project& p_;
    MediaProber probe_;
    Sequence seq_;
    ImportResult res_;
    std::map<std::string, Id> byPath_;
};

}  // namespace montage
