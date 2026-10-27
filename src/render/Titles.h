// Montage — titles: the "title" generator drawn, and its text styled and animated letter by letter (After Effects'
// text animators and text on a path, Resolve's character styling in Text+ and its Follower, Motion's text behaviours).
//
// A title's text can carry styled runs (its string "runs", JSON): a stretch of characters in its own font, size, weight,
// italics, fill, outline, tracking, baseline shift and variable font axes. Text can be set on a path (params path_*: a
// circle, a wave or SVG path data). And it can hold any number of text animations (params "ta<k>.*", k from 1): a range
// selector picking letters, words or lines (start, end and offset as percentages of the text, a shape and an ease) and
// what it changes in them: position, scale, rotation, opacity, blur, tracking, fill and outline colour, with a seeded
// random spread. A title without any of these is drawn as it always was, line by line through QPainterPath::addText.
// With them, each line is laid out by QTextLayout and every glyph cluster is a unit that can move.
#pragma once

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Effects.h"
#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// A run of a title's text in a style of its own; what it leaves unset is the title's (or, on lines after the first
// with "Style Lines After the First", theirs).
struct TextRun {
    int from = 0, len = 0;  // UTF-16 positions in the title's text, newlines counted
    std::string font;       // "" the title's
    double size = 0;        // sequence pixels, 0 the title's
    int bold = -1, italic = -1;
    std::optional<std::array<double, 3>> color, outlineColor;
    double outline = -1;  // sequence pixels, -1 the title's
    std::optional<double> tracking;  // sequence pixels
    double baseline = 0;             // sequence pixels, up
    std::map<std::string, double> axes;  // variable font axes by tag ("wght", "wdth", "slnt"...)
    bool operator==(const TextRun&) const = default;
};
// The runs in a title's "runs" string (a JSON array of {from, len, font, size, bold, italic, color: [r, g, b],
// outline, outline_color, tracking, baseline, axes: {tag: value}}); malformed entries are skipped.
std::vector<TextRun> parseTextRuns(const std::string& json);
std::string textRunsJson(const std::vector<TextRun>& runs);
// `runs` with `add` laid over them (later runs win where they overlap), runs that are empty or set nothing dropped.
std::vector<TextRun> addTextRun(std::vector<TextRun> runs, const TextRun& add);
// `runs` kept on the same characters when the text changes from `before` to `after` (typed into, cut from): runs
// grow with text typed inside them and shrink, or go, with text cut from them.
std::vector<TextRun> shiftTextRuns(std::vector<TextRun> runs, const QString& before, const QString& after);

// A title's text in stretches of one style each, the style given in full (font, size in sequence pixels, bold,
// italic, fill, outline width and colour, tracking, baseline and axes; from and len unset), for interchange.
struct TitleStyleSpan {
    QString text;
    TextRun style;
};
std::vector<TitleStyleSpan> titleStyleSpans(const Effect& g, FrameTime t = 0);
// The other way: a title's text and style from such stretches (a stretch without a style takes the title's). The
// style most of the characters have becomes the title's own and the others its runs.
void applyTitleStyleSpans(Effect& g, const std::vector<std::pair<QString, std::optional<TextRun>>>& spans);

// The text animations a title has, and their params: textAnimators and textAnimatorParams in core/Effects.h.
// How much text animation k selects unit i of n at frame t (clip frames; fps for its sweep), from -1 to 1 with its
// Amount: its range's shape over the units, eased, spread by its random amount.
double textSelection(const Effect& g, int k, FrameTime t, double fps, FrameTime duration, int i, int n);

// Whether the title needs the styled path: it has runs that change something, a path, or text animations.
bool titleIsStyled(const Effect& g, FrameTime t);

// One glyph cluster of a styled title as drawn at frame t.
struct TitleGlyph {
    int index = 0, length = 0;  // its characters in the title's text (newlines counted)
    int line = 0;
    QPainterPath path;          // in output pixels, placed and animated
    QPointF baselineMid;        // the middle of its baseline in output pixels, placed (before animation)
    std::vector<std::pair<quint32, QPointF>> glyphs;  // glyph ids and their pen positions in the line's layout
    QColor fill, outlineColor;
    double outline = 0, opacity = 1, blur = 0;
};
// The glyphs a styled title draws at frame t on a w x h frame (empty for a plain one).
std::vector<TitleGlyph> titleGlyphs(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps);

// The "title" generator at frame t of a clip `duration` frames long.
Image renderTitle(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps);

// A styled title's text as one outline, laid out with its runs (no path or animation), lines `lineHeight` apart, for
// the 3D titles: px output pixels per sequence pixel. Empty when it has no runs.
QPainterPath styledTextOutline(const Effect& g, FrameTime t, double px, double defaultSize, double lineSpacing);

}  // namespace montage
