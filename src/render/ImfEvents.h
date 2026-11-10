// Montage — a sequence's markers in an IMF package as event-based, text-based metadata (SMPTE ST 2067-206:2026).
//
// Each marker within the master becomes an event of a Simple Scheme Timed Events Resource (ST 2067-206 Annex A, as
// XML): its frame and the frame it ends on as startCount and endCount at the composition's edit rate (a marker without
// a duration an instant: both the same), and its name, comment, colour and kind (a chapter or an ad break) in
// elements of Montage's own namespace, which the Simple Scheme leaves room for. The document's Event Scheme is
// Montage's markers scheme, a child of the Simple Scheme (its URI first in the track file's EventSchemeURIs, the
// Simple Scheme's after it), so other readers of the Simple Scheme take the events' times and ids.
//
// Montage's checker holds a document to Annex A: the document element in the Simple Scheme's namespace with its id
// (a URI), editRate ("n d", both positive), objects and events in that order; every object and event an id without a
// comma, unique in its list; counts that are whole numbers (as strings); no event ending before it starts.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"

namespace montage {

constexpr const char* kImfSimpleScheme = "http://www.smpte-ra.org/ns/2067-206/2023/simple-scheme";
constexpr const char* kImfSimpleSchemeResource = "http://www.smpte-ra.org/ns/2067-206/2023/simple-scheme/timed-events-resource";
constexpr const char* kImfMarkerScheme = "urn:montage:imf:markers";  // Montage's child of the Simple Scheme, and its elements' namespace

// An event: [start, end] in edit units of its document's timeline (end == start: an instant), and what a marker has.
struct ImfEvent {
    std::string id;  // as read (written: M1, M2, ... in order)
    int64_t start = 0, end = 0;
    std::string name, comment;
    int colour = 0;  // a label (core/MediaLog.h labelName), 0 none
    bool chapter = false, adBreak = false;
    // The same event (its id aside).
    bool same(const ImfEvent& o) const {
        return start == o.start && end == o.end && name == o.name && comment == o.comment && colour == o.colour && chapter == o.chapter &&
               adBreak == o.adBreak;
    }
};

// The markers of a sequence a master of [first, first + programme) carries, in its edit units (a composition of
// `frames`, the programme and any padding after it): those that start in it (not shot changes found by scene
// detection), ending no later than the composition. In order of their frames.
std::vector<ImfEvent> imfEventsOfMarkers(const std::vector<Marker>& markers, FrameTime first, int64_t programme, int64_t frames);
// A marker for an event at composition frame `at` (its start), lasting to its end.
Marker markerOfImfEvent(const ImfEvent& e, int64_t at, int64_t length);

// The Simple Scheme XML document of `events` at the edit rate num/den; its id a name-based UUID of what it holds (so
// the same events make the same document).
std::string imfEventsDocument(const std::vector<ImfEvent>& events, uint32_t num, uint32_t den);
// Reads a Simple Scheme document (XML): its events (counts as written; an event without startCount starts at
// INT64_MIN, one without endCount ends at INT64_MAX) and its edit rate.
bool parseImfEventsDocument(const std::string& xml, std::vector<ImfEvent>& events, uint32_t& num, uint32_t& den, std::string* error = nullptr);
// What Annex A's schema and A.5 ask of a Simple Scheme XML document that it lacks (empty: none).
std::vector<std::string> checkImfEventsDocument(const std::string& xml);

// The stretches [from, to) of a composition of `frames` edit units where `now`'s events differ from `was`'s (at the
// same positions): each changed event's (an instant's frame), widened until no event of either crosses an end of a
// stretch, those that touch joined.
std::vector<std::pair<int64_t, int64_t>> imfChangedEventStretches(const std::vector<ImfEvent>& was, const std::vector<ImfEvent>& now, int64_t frames);

}  // namespace montage
