// Montage — the editor for screen readers: what each of Montage's own widgets is to assistive technology (the timeline
// a table of tracks whose cells are its clips and gaps, the monitors their timecode and marks, meters their levels,
// scopes a summary, the keyframes a list), names for the controls that would otherwise have none, and edits announced
// as they happen. Read through QAccessible: UI Automation on Windows, NSAccessibility on macOS, AT-SPI on Linux.
#pragma once

#include <QAccessible>
#include <QString>
#include <functional>
#include <vector>

#include "core/Model.h"

class QWidget;

namespace montage {

class TimelineWidget;

namespace access {

// Installs the interfaces and the naming of unnamed controls (once; again does nothing).
void install();

// Says `text` to a screen reader, without moving the focus (an announcement, from Qt 6.8; before it an alert from the
// window's status line).
void announce(QWidget* source, const QString& text);
QString lastAnnouncement();  // (testing aid)

// A widget whose accessible text is worked out when asked (the video in a monitor, a meter, a scope): its role, name,
// and its value and description if it has them (without a description, the value is read as one). Gone with the
// widget.
struct LiveText {
    QAccessible::Role role = QAccessible::Graphic;
    std::function<QString()> name, value, description;
};
void describe(QWidget* widget, LiveText text);

// The timeline as a screen reader reads it: a row for each track shown, top to bottom, and in each its clips and the
// gaps between them in time order, as "A001_C003, V1, 00:00:10:00 to 00:00:14:12, Cross Dissolve out".
struct TimelineCell {
    Id clip = 0;  // 0: a gap
    FrameTime start = 0, end = 0;
    QString text;
};
struct TimelineRow {
    TrackRef track;
    QString name;  // "V1", or the track's own name
    std::vector<TimelineCell> cells;
};
std::vector<TimelineRow> timelineRows(const Sequence& s, const std::vector<TrackRef>& shown);
// The timeline's value: where the playhead is, and what is selected (named by `focus`, the clip the keyboard is on).
QString timelineValue(const Sequence& s, const std::vector<Id>& selection, Id focus = 0);
// A track as its row is called: its name, or "V1", "A2".
QString trackName(const Sequence& s, TrackRef track);
// A clip as its cell reads ("" if it is not in the sequence).
QString clipDescription(const Sequence& s, Id clip);

// The child of `widget` that has the focus changed (a key or clip selected): screen readers told, as they are of a
// widget's own focus.
void notifyFocus(QWidget* widget);

// The timeline changed (its clips, or its selection): screen readers told.
void timelineChanged(TimelineWidget* timeline);
void timelineSelectionChanged(TimelineWidget* timeline);

// What an unnamed control would be called from what is next to it: its label (a buddy, a form row's, or one before it
// in its row), its tool tip, placeholder text or object name; "" when there is nothing.
QString inferredName(const QWidget* widget);

}  // namespace access
}  // namespace montage
