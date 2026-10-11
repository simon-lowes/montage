// Montage — marker lists for review and hand-offs: the sequence's markers as a CSV (Premiere's marker export
// columns), as Avid Media Composer locators, or as a marker EDL for DaVinci Resolve; and markers read back from a
// CSV or Avid locator file (the usual way notes come back from review tools).
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

// "Marker Name,Description,In,Out,Duration,Marker Type,Color" with SMPTE timecodes; chapter markers are "Chapter".
std::string markersToCsv(const Sequence& s);
// One locator a line: user, timecode, track (V1), colour, comment.
std::string markersToAvidLocators(const Sequence& s, const std::string& user = "Montage");
// A CMX 3600 EDL with one event per marker and Resolve's |C:colour |M:name |D:duration notes.
std::string markersToResolveEdl(const Sequence& s);

// Markers from a CSV with a header row (Name / Marker Name, Description / Comment / Notes, In / Start / Timecode,
// Out, Duration, Marker Type, Color) or from Avid locator lines. When every time is an hour or more in and the
// sequence is shorter than that, the list is taken to start at 01:00:00:00 (as Avid and Resolve timelines do). A notes
// file saved from a review page (core/ReviewPage.h) gives a marker a note, coloured by reviewer.
// False (with `error`) if nothing could be read.
bool parseMarkerList(const std::string& text, const Sequence& s, std::vector<Marker>& out, std::string* error = nullptr);

}  // namespace montage
