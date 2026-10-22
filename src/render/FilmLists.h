// Montage — film lists, for a cut that goes back to the negative (Media Composer's FilmScribe, Lightworks' cut lists):
// the lists a negative cutter and an optical house work from, counted in feet and frames of the sequence's gauge
// (core/FilmCount.h) and naming the negative by the key numbers (and the workprint by the ink numbers) its media
// carries.
//
//  - The cut list: each shot in record order, its footage in and out, its roll and its key numbers in and out (the
//    first and last frame of negative it plays).
//  - The optical list: what the lab makes as an optical rather than a cut: dissolves, fades and wipes (with the
//    negative each side needs, handles included), speed changes, reverse motion and speed ramps, repositions
//    (a clip's motion moved, scaled, rotated, cropped or flipped), superimpositions and titles.
//  - The dupe list: negative a cut uses twice (a duplicate negative is struck for the second use).
//  - The pull list: the shots by roll, in the order they sit on it, for pulling the negative.
//  - With an earlier cut, the change list: Compare with Sequence's changes (core/Reconform.h) in feet and frames.
//
// As text, as CSV (one table, a List column naming each row's list) or as a PDF.
#pragma once

#include <QString>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/FilmCount.h"
#include "core/Model.h"
#include "core/Reconform.h"

namespace montage {

struct FilmListOptions {
    std::string gauge;    // "": the sequence's (35 mm 4-perf where it has none)
    int videoTrack = -1;  // the picture: -1 every video track (higher ones superimposed), else that track alone
};

struct FilmEvent {
    int number = 0;                   // from 1, in record order
    int track = 0;                    // V1 = 0
    FrameTime recIn = 0, recOut = 0;  // sequence frames, [in, out)
    std::string name;                 // the clip's name
    std::string roll;                 // the key number's prefix, else the ink number's, else its tape (camera roll) or media
    Id mediaId = 0;                   // 0: a generator (a title, a matte)
    int64_t srcIn = 0, srcOut = 0;    // the media's frames it plays, [in, out) (whichever way it plays)
    int64_t negIn = 0, negOut = 0;    // with the handles its transitions take
    // Key numbers of the first and last frame it plays, and their ink numbers, in its media's gauge
    // (core/FilmCount.h mediaFilmGauge); "" where the media has none.
    std::string keyIn, keyOut, inkIn, inkOut;
    int64_t rollPosition = 0;  // where its first frame is along the roll, in perforations (for the pull list)
};

struct FilmOptical {
    std::string kind;                 // "Dissolve", "Dip", "Wipe", "Fade in", "Fade out", "Speed", "Reverse",
                                      // "Speed ramp", "Reposition", "Superimposition", "Title"
    int event = 0;                    // the event it is on (a dissolve's incoming shot, 0 for a fade out's none)
    int outgoing = 0;                 // a dissolve's or wipe's outgoing event (0: none)
    FrameTime recIn = 0, recOut = 0;  // where it plays, [in, out)
    std::string detail;               // "48 frames", "50%", "scale 120%, position 40, -12"; and where the media
                                      // runs out before the transition does ("3 frames short")
    // The negative it takes: the event's (a dissolve's incoming side), and a dissolve's outgoing side.
    std::string keyIn, keyOut, outKeyIn, outKeyOut;
};

struct FilmDupe {
    int first = 0, second = 0;  // the events using the same negative (second: the one that needs the dupe)
    std::string roll;
    int64_t srcIn = 0, srcOut = 0;  // the media frames used twice, [in, out)
    std::string keyIn, keyOut;
    int64_t frames() const { return srcOut - srcIn; }
};

struct FilmLists {
    std::string sequence;
    FilmGauge gauge;  // the sequence's: its footage (each media's key numbers in its own)
    Rational fps{24, 1};
    FrameTime length = 0;
    std::vector<FilmEvent> events;
    std::vector<FilmOptical> opticals;
    std::vector<FilmDupe> dupes;
    // The pull list: each roll with its events in the order they sit on it.
    std::vector<std::pair<std::string, std::vector<int>>> pulls;
};

FilmLists filmLists(const Project& p, const Sequence& s, const FilmListOptions& options = {});

// `changes` (optional): the change list from an earlier cut, in feet and frames of the lists' gauge.
std::string filmListsText(const FilmLists& l, const CutChanges* changes = nullptr);
std::string filmListsCsv(const FilmLists& l, const CutChanges* changes = nullptr);
bool writeFilmListsPdf(const FilmLists& l, const CutChanges* changes, const QString& path, QString* error = nullptr);
// As the file's extension says: .txt, .csv or .pdf.
bool writeFilmLists(const FilmLists& l, const CutChanges* changes, const QString& path, QString* error = nullptr);

}  // namespace montage
