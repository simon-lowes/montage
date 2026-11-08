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
#include <functional>
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

// Whether the title needs the styled path: it has runs that change something, a path, text animations or font axes
// of its own.
bool titleIsStyled(const Effect& g, FrameTime t);

// A path the title does not hold itself, to set its text on (path_shape 4, a mask of an effect on its clip, or 5, a
// path of a shape layer under it; render/Compositor finds them): in output pixels, as lines, cubic Béziers and
// elliptic arcs, in the order the text runs along them.
struct TitlePathCurve {
    struct Segment {
        int kind = 0;    // 0 a line p[0] to p[1], 1 a cubic p[0] p[1] p[2] p[3], 2 an elliptic arc
        QPointF p[4];    // (an arc: its centre in p[0], its radii in p[1])
        double rotation = 0, from = 0, to = 0;  // an arc's: its axes turned (radians), and its angles
    };
    std::vector<Segment> segments;
    bool closed = false;
};
// The path at a time (clip frames, fractional within a frame for motion blur); nothing when there is none.
using TitlePathFn = std::function<std::optional<TitlePathCurve>(double t)>;
// The path a title clip `c` in `seq` sets its text on when it is not its own (render/Compositor.cpp): one of its
// effects' masks (path_shape 4, the Mask Number-th ellipse, rectangle or Bézier mask) or a shape layer's path under it
// (5), in pixels of the title's w x h picture, at a clip time. Empty for a title on any other path. It refers to `p`,
// `seq` and `c`, which must outlive it.
TitlePathFn titlePathSource(const Project& p, const Sequence& seq, const Clip& c, int w, int h);
// The title's font axes of its own (params "axis.<tag>", for every character; runs' axes override them).
std::vector<std::string> titleAxes(const Effect& g);

// One glyph cluster of a styled title as drawn at frame t.
struct TitleGlyph {
    int index = 0, length = 0;  // its characters in the title's text (newlines counted)
    int line = 0;
    QPainterPath path;          // in output pixels, placed and animated
    QPointF baselineMid;        // the middle of its baseline in output pixels, placed (before animation)
    double angle = 0;           // how far its baseline is turned (degrees, clockwise on screen) where it is placed
    std::vector<std::pair<quint32, QPointF>> glyphs;  // glyph ids and their pen positions in the line's layout
    QColor fill, outlineColor;
    double outline = 0, opacity = 1, blur = 0;
};
// The glyphs a styled title draws at frame t on a w x h frame (empty for a plain one).
std::vector<TitleGlyph> titleGlyphs(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps,
                                    const TitlePathFn& path = {});

// The "title" generator at frame t of a clip `duration` frames long. With Motion Blur (letter_blur), the average of
// letter_samples renders spread evenly over the shutter (letter_shutter degrees of a frame) centred on the frame.
Image renderTitle(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps, const TitlePathFn& path = {});
// One render at a time between frames, without motion blur.
Image renderTitleAt(const Effect& g, double t, int w, int h, double scale, FrameTime duration, double fps, const TitlePathFn& path = {});

// A styled title's text as one outline, laid out with its runs (no path or animation), lines `lineHeight` apart, for
// the 3D titles: px output pixels per sequence pixel. Empty when it has no runs.
QPainterPath styledTextOutline(const Effect& g, FrameTime t, double px, double defaultSize, double lineSpacing);
// The same text a glyph cluster at a time, for depths set letter by letter: each cluster's outline and its place
// among the letters, letters not spaces, words and lines (-1 for a space), in reading order. `defaultText` is the
// title's text when it has none set.
struct TitleCharOutline {
    QPainterPath path;
    int letter = 0, solid = -1, word = -1, line = 0;
};
std::vector<TitleCharOutline> titleCharOutlines(const Effect& g, FrameTime t, double px, double defaultSize, double lineSpacing,
                                                const QString& defaultText, int& letters, int& solids, int& words, int& lines);

}  // namespace montage
