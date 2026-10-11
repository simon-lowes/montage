// Montage — the storyboard bin (Premiere's Freeform view, Avid's Frame view): a bin's media arranged freely on a
// board (MediaItem::board, in thumbnail cells), read in rows, top to bottom and left to right, and assembled into a
// sequence in that order.
#pragma once

#include <map>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

using BoardSpot = MediaItem::BoardSpot;

// `media` in reading order on the board: rows top to bottom (an item whose top is within half a cell of a row's first
// joins that row), left to right in each; media not on the board follow, in the order given.
std::vector<Id> storyboardOrder(const Project& p, const std::vector<Id>& media);

// Where each of `media` shows on the board: its own spot, or, for media not yet placed, the next free cells in rows
// `columns` wide below everything placed, in the order given.
std::map<Id, BoardSpot> boardLayout(const Project& p, const std::vector<Id>& media, int columns);

// Puts `media` on the board in a grid `columns` wide, in the order given (Arrange in Grid).
void arrangeStoryboard(Project& p, const std::vector<Id>& media, int columns);

// Places `media` one after another in `s` from `at` (a subclip as its range, a still for the default still length),
// pictures on `video` and sound on `audio`; `insert` pushes what follows along, else they overwrite. The result's
// created clips, in order.
edit::Result assembleStoryboard(Project& p, Sequence& s, const std::vector<Id>& media, FrameTime at, TrackRef video,
                                TrackRef audio, bool insert);

}  // namespace montage
