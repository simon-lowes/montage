// Montage — the lined script: the project's screenplay (core/ScriptCut.h readScreenplay) set out in pages as a
// script supervisor lines it, with a vertical line for each take down the script lines it reads, the circled takes
// ringed, a tick beside each line the cut uses and a red bar beside each line no take reads. The Script panel shows
// these pages and the same pages are written as a PDF.
//
// Pages are US Letter landscape in points (792 by 612): the script in screenplay layout (12 pt Courier, ten
// characters an inch, six lines an inch) on the left, the takes' columns on the right.
#pragma once

#include <QColor>
#include <QPointF>
#include <QString>
#include <vector>

#include "core/ScriptCut.h"

class QPainter;

namespace montage {

struct LinedScriptLayout {
    static constexpr double kPageWidth = 792, kPageHeight = 612;
    static constexpr double kRow = 12;  // a text row's height
    QString title;
    int pages = 0;
    Screenplay screenplay;
    // A row of text where it sits on its page (top left), and the script line it shows (-1 for none).
    struct Row {
        int page = 0;
        double x = 0, y = 0;
        QString text;
        ScriptBlock::Kind kind = ScriptBlock::Kind::Action;
        int line = -1;
    };
    // A take's column on a page.
    struct Column {
        int page = 0;
        double x = 0;
        Id media = 0;
        QString label;
        bool circled = false;
        QColor color;
    };
    // A take reading a line: its column's stretch beside the line's rows. `reading` indexes
    // Project::script.readings[line].
    struct Segment {
        int page = 0;
        int column = 0;  // into columns
        double y0 = 0, y1 = 0;
        int line = 0, reading = 0;
        bool inCut = false;
    };
    // Beside each script line: 1 used in the cut, 0 read by a take but not used, -1 read by none.
    struct Mark {
        int page = 0;
        double y0 = 0, y1 = 0;
        int line = 0;
        int state = 0;
    };
    std::vector<Row> rows;
    std::vector<Column> columns;
    std::vector<Segment> segments;
    std::vector<Mark> marks;

    int lineAt(int page, QPointF at) const;               // the script line under a point of a page (-1)
    const Segment* segmentAt(int page, QPointF at) const;  // a take's stretch under it (null)
};

// Lays the project's script out (empty when it has none); `cut` (may be null) gives the ticks.
LinedScriptLayout layoutLinedScript(const Project& p, const Sequence* cut);
// Draws a page in page points (the painter's coordinates are the page's).
void paintLinedScriptPage(QPainter& painter, const LinedScriptLayout& l, int page);
// Writes every page to a PDF.
bool exportLinedScriptPdf(const LinedScriptLayout& l, const QString& path, QString* error = nullptr);

}  // namespace montage
