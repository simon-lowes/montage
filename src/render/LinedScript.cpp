#include "LinedScript.h"

#include <QFileInfo>
#include <QFont>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace montage {

namespace {

using K = ScriptBlock::Kind;
using L = LinedScriptLayout;

constexpr double kChar = 7.2;  // 12 pt Courier: ten characters an inch
constexpr double kLeft = 54, kTop = 72, kBottom = L::kPageHeight - 36;
constexpr double kTextRight = kLeft + 60 * kChar;  // action's 60 characters
constexpr double kStripLeft = kTextRight + 30, kStripRight = L::kPageWidth - 30;
constexpr int kRowsPerPage = int((kBottom - kTop) / L::kRow);

// Words wrapped to `width` characters; a word longer than that is cut. Line breaks in the text are kept.
QStringList wrap(const QString& text, int width) {
    QStringList out;
    for (const QString& para : text.split('\n')) {
        QString cur;
        for (QString w : para.split(' ', Qt::SkipEmptyParts)) {
            while (w.size() > width) {
                if (!cur.isEmpty()) out << cur, cur.clear();
                out << w.left(width);
                w = w.mid(width);
            }
            if (cur.isEmpty()) cur = w;
            else if (cur.size() + 1 + w.size() <= width) cur += ' ' + w;
            else out << cur, cur = w;
        }
        if (!cur.isEmpty() || out.isEmpty()) out << cur;
    }
    return out;
}

// Where a kind of block starts and how wide it is, in characters.
void placeOf(K kind, double& x, int& width) {
    switch (kind) {
        case K::Character: x = kLeft + 22 * kChar, width = 38; break;
        case K::Parenthetical: x = kLeft + 15 * kChar, width = 25; break;
        case K::Dialogue: x = kLeft + 10 * kChar, width = 35; break;
        case K::Lyric: x = kLeft + 10 * kChar, width = 35; break;
        default: x = kLeft, width = 60; break;
    }
}

QString takeLabel(const MediaItem& m) {
    auto field = [&](const char* key) {
        const auto it = m.metadata.find(key);
        return it == m.metadata.end() ? QString() : QString::fromStdString(it->second).trimmed();
    };
    const QString scene = field("scene"), take = field("take");
    QString label;
    if (!scene.isEmpty() && !take.isEmpty()) label = scene + '-' + take;
    else if (!take.isEmpty()) label = QStringLiteral("T") + take;
    else label = QString::fromStdString(m.name);
    if (label.size() > 16) label = label.left(15) + QChar(0x2026);
    return label;
}

QColor cameraColor(const MediaItem& m) {
    static const QColor palette[] = {QColor(30, 90, 200), QColor(200, 60, 40), QColor(20, 140, 70), QColor(150, 60, 170),
                                     QColor(200, 130, 0), QColor(0, 140, 150)};
    const auto it = m.metadata.find("camera");
    if (it == m.metadata.end() || it->second.empty()) return palette[0];
    return palette[std::hash<std::string>{}(it->second) % (sizeof(palette) / sizeof(palette[0]))];
}

}  // namespace

int LinedScriptLayout::lineAt(int page, QPointF at) const {
    for (const Row& r : rows)
        if (r.page == page && r.line >= 0 && at.y() >= r.y && at.y() < r.y + kRow && at.x() >= kLeft - 20 && at.x() <= kTextRight)
            return r.line;
    return -1;
}

const LinedScriptLayout::Segment* LinedScriptLayout::segmentAt(int page, QPointF at) const {
    const Segment* best = nullptr;
    double bestDx = 6;
    for (const Segment& s : segments) {
        if (s.page != page || at.y() < s.y0 || at.y() > s.y1) continue;
        const double dx = std::fabs(at.x() - columns[size_t(s.column)].x);
        if (dx < bestDx) bestDx = dx, best = &s;
    }
    return best;
}

LinedScriptLayout layoutLinedScript(const Project& p, const Sequence* cut) {
    LinedScriptLayout l;
    if (p.script.text.empty()) return l;
    l.screenplay = readScreenplay(p.script);
    const Screenplay& sp = l.screenplay;
    l.title = !sp.title.empty() ? QString::fromStdString(sp.title) : QFileInfo(QString::fromStdString(p.script.name)).completeBaseName();
    const std::vector<int> inCut = cut ? scriptLinesInCut(p, *cut) : std::vector<int>();
    auto readingsOf = [&](int line) -> const std::vector<ScriptReading>* {
        return line >= 0 && size_t(line) < p.script.readings.size() ? &p.script.readings[size_t(line)] : nullptr;
    };

    // Rows, page by page.
    int page = 0, row = 0;
    bool any = false;
    auto newPage = [&] {
        ++page;
        row = 0;
    };
    auto put = [&](K kind, const QString& text, int line, double x) {
        l.rows.push_back({page, x, kTop + row * L::kRow, text, kind, line});
        ++row;
        any = true;
    };
    bool speech = false;  // inside a character's speech (no blank rows between its parts)
    size_t unit = 0;      // the first row of the heading or cue the block under way belongs with
    K last = K::PageBreak;
    for (size_t b = 0; b < sp.blocks.size(); ++b) {
        const ScriptBlock& block = sp.blocks[b];
        if (block.kind == K::PageBreak) {
            if (row > 0) newPage();
            speech = false;
            last = K::PageBreak;
            continue;
        }
        const bool joins = speech && (block.kind == K::Dialogue || block.kind == K::Parenthetical);
        speech = block.kind == K::Character || joins;
        if (!joins && !(block.kind == K::Character && last == K::Heading)) unit = l.rows.size();
        last = block.kind;
        double x = kLeft;
        int width = 60;
        placeOf(block.kind, x, width);
        // The block's rows, each with the script line it shows.
        std::vector<std::pair<QString, int>> rows;
        if (block.kind == K::Dialogue && block.lineCount > 0) {
            for (int line = block.firstLine; line < block.firstLine + block.lineCount && size_t(line) < sp.lines.size(); ++line)
                for (const QString& r : wrap(QString::fromStdString(sp.lines[size_t(line)].text), width)) rows.push_back({r, line});
        } else {
            QString text = QString::fromStdString(block.text);
            if (block.kind == K::Character || block.kind == K::Heading || block.kind == K::Transition) text = text.toUpper();
            for (const QString& r : wrap(text, width)) rows.push_back({r, -1});
        }
        // A heading or a cue keeps two rows of what follows with it.
        const bool keepWithNext = block.kind == K::Heading || block.kind == K::Character;
        const int need = int(rows.size()) + (row > 0 && !joins ? 1 : 0) + (keepWithNext ? 2 : 0);
        if (row > 0 && row + need > kRowsPerPage) {
            // A speech's cue (and a heading over it) goes over with it, rather than stay alone at the bottom.
            std::vector<L::Row> moved;
            if (joins && unit < l.rows.size() && l.rows[unit].page == page && l.rows[unit].y > kTop)
                moved.assign(l.rows.begin() + std::ptrdiff_t(unit), l.rows.end());
            if (!moved.empty() && int(moved.size()) * 2 < kRowsPerPage) {
                l.rows.resize(unit);
                const double base = moved.front().y;
                newPage();
                for (L::Row& r : moved) {
                    r.page = page;
                    r.y = kTop + (r.y - base);
                    row = int(std::lround((r.y - kTop) / L::kRow)) + 1;
                    l.rows.push_back(r);
                }
                unit = l.rows.size() - moved.size();
            } else {
                newPage();
            }
        }
        if (row > 0 && !joins) ++row;  // a blank row between blocks
        for (const auto& [text, line] : rows) {
            if (row >= kRowsPerPage) newPage();
            double rx = x;
            if (block.kind == K::Transition) rx = kTextRight - text.size() * kChar;
            else if (block.kind == K::Centered) rx = (kLeft + kTextRight - text.size() * kChar) / 2;
            put(block.kind, text, line, rx);
        }
    }
    l.pages = any ? page + 1 : 0;

    // Each script line's extent on each page it reaches.
    struct Extent {
        int page;
        double y0, y1;
    };
    std::map<int, std::vector<Extent>> extents;
    for (const L::Row& r : l.rows) {
        if (r.line < 0) continue;
        auto& ex = extents[r.line];
        if (!ex.empty() && ex.back().page == r.page) ex.back().y1 = r.y + L::kRow;
        else ex.push_back({r.page, r.y, r.y + L::kRow});
    }
    // The takes' columns, page by page, in the project's order of media.
    std::map<Id, size_t> order;
    for (size_t i = 0; i < p.media.size(); ++i) order[p.media[i].id] = i;
    for (int pg = 0; pg < l.pages; ++pg) {
        std::set<std::pair<size_t, Id>> takes;
        for (const auto& [line, ex] : extents)
            if (std::any_of(ex.begin(), ex.end(), [&](const Extent& e) { return e.page == pg; }))
                if (const auto* rs = readingsOf(line))
                    for (const ScriptReading& r : *rs)
                        if (order.count(r.media)) takes.insert({order[r.media], r.media});
        if (takes.empty()) continue;
        const double step = std::min(20.0, (kStripRight - kStripLeft) / std::max<size_t>(1, takes.size()));
        double x = kStripLeft + step / 2;
        for (const auto& [index, media] : takes) {
            const MediaItem& m = p.media[index];
            l.columns.push_back({pg, x, media, takeLabel(m), isCircledTake(m), cameraColor(m)});
            x += step;
        }
    }
    auto columnOf = [&](int pg, Id media) {
        for (size_t c = 0; c < l.columns.size(); ++c)
            if (l.columns[c].page == pg && l.columns[c].media == media) return int(c);
        return -1;
    };
    for (const auto& [line, ex] : extents) {
        const auto* rs = readingsOf(line);
        const int used = size_t(line) < inCut.size() ? inCut[size_t(line)] : -1;
        for (const Extent& e : ex) {
            int state = rs && !rs->empty() ? 0 : -1;
            if (used >= 0) state = 1;
            l.marks.push_back({e.page, e.y0, e.y1, line, state});
            if (!rs) continue;
            // One stretch a take: its reading the cut uses, else its best.
            std::set<Id> drawn;
            if (used >= 0 && size_t(used) < rs->size())
                if (const int c = columnOf(e.page, (*rs)[size_t(used)].media); c >= 0) {
                    l.segments.push_back({e.page, c, e.y0, e.y1, line, used, true});
                    drawn.insert((*rs)[size_t(used)].media);
                }
            for (size_t k = 0; k < rs->size(); ++k)
                if (const int c = columnOf(e.page, (*rs)[k].media); c >= 0 && drawn.insert((*rs)[k].media).second)
                    l.segments.push_back({e.page, c, e.y0, e.y1, line, int(k), false});
        }
    }
    return l;
}

void paintLinedScriptPage(QPainter& painter, const LinedScriptLayout& l, int page) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(QRectF(0, 0, L::kPageWidth, L::kPageHeight), Qt::white);
    QFont mono;
    mono.setFamilies({QStringLiteral("Courier Prime"), QStringLiteral("Courier New"), QStringLiteral("Courier"),
                      QStringLiteral("Liberation Mono"), QStringLiteral("DejaVu Sans Mono")});
    mono.setStyleHint(QFont::TypeWriter);
    mono.setPixelSize(12);
    QFont small = mono;
    small.setPixelSize(8);
    // The page's number and the script's title.
    painter.setPen(Qt::black);
    painter.setFont(mono);
    painter.drawText(QRectF(kLeft, 30, kTextRight - kLeft, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(page + 1) + '.');
    painter.setFont(small);
    painter.drawText(QRectF(kLeft, 30, kTextRight - kLeft, 14), Qt::AlignLeft | Qt::AlignVCenter, l.title);
    // The script.
    painter.setFont(mono);
    for (const L::Row& r : l.rows)
        if (r.page == page) painter.drawText(QRectF(r.x, r.y, kTextRight + 40 - r.x, L::kRow), Qt::AlignLeft | Qt::AlignVCenter, r.text);
    // Beside each line: a tick where the cut uses it, a red bar where no take reads it.
    for (const L::Mark& m : l.marks) {
        if (m.page != page) continue;
        if (m.state == 1) {
            QPainterPath tick;
            const double y = (m.y0 + std::min(m.y1, m.y0 + L::kRow)) / 2;
            tick.moveTo(kLeft - 22, y);
            tick.lineTo(kLeft - 18, y + 4);
            tick.lineTo(kLeft - 11, y - 5);
            painter.setPen(QPen(QColor(20, 140, 70), 1.6));
            painter.drawPath(tick);
        } else if (m.state < 0) {
            painter.fillRect(QRectF(kLeft - 14, m.y0 + 1, 3, m.y1 - m.y0 - 2), QColor(210, 40, 40));
        }
    }
    // The takes: a label (ringed when circled) over each column, and a line down what it reads.
    painter.setFont(small);
    for (const L::Column& c : l.columns) {
        if (c.page != page) continue;
        painter.save();
        painter.translate(c.x, kTop - 14);
        painter.rotate(-90);
        painter.setPen(c.color);
        painter.drawText(QRectF(0, -5, 56, 10), Qt::AlignLeft | Qt::AlignVCenter, c.label);
        painter.restore();
        if (c.circled) {
            painter.setPen(QPen(c.color, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(c.x, kTop - 7), 4.5, 4.5);
        }
    }
    // A take that reads one line and the next is joined across the rows between them (a cue), dotted.
    std::map<int, const L::Segment*> previous;  // by column
    for (const L::Segment& s : l.segments) {
        if (s.page != page) continue;
        const L::Column& c = l.columns[size_t(s.column)];
        if (const auto it = previous.find(s.column); it != previous.end() && it->second->line + 1 == s.line && s.y0 > it->second->y1) {
            painter.setPen(QPen(c.color, 1.0, Qt::DotLine));
            painter.drawLine(QPointF(c.x, it->second->y1), QPointF(c.x, s.y0));
        }
        previous[s.column] = &s;
        painter.setPen(QPen(c.color, s.inCut ? 3.0 : 1.2, Qt::SolidLine, Qt::FlatCap));
        painter.drawLine(QPointF(c.x, s.y0 + 1), QPointF(c.x, s.y1 - 1));
        // A bar where the reading starts on this page.
        painter.setPen(QPen(c.color, 1.0));
        painter.drawLine(QPointF(c.x - 3, s.y0 + 1), QPointF(c.x + 3, s.y0 + 1));
    }
    painter.restore();
}

bool exportLinedScriptPdf(const LinedScriptLayout& l, const QString& path, QString* error) {
    if (l.pages <= 0) {
        if (error) *error = QStringLiteral("The project has no script");
        return false;
    }
    QPdfWriter writer(path);
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::Letter), QPageLayout::Landscape, QMarginsF(0, 0, 0, 0)));
    writer.setTitle(l.title.isEmpty() ? QStringLiteral("Lined script") : l.title);
    writer.setCreator(QStringLiteral("Montage"));
    QPainter painter;
    if (!painter.begin(&writer)) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    painter.setWindow(0, 0, int(L::kPageWidth), int(L::kPageHeight));
    for (int page = 0; page < l.pages; ++page) {
        if (page > 0) writer.newPage();
        paintLinedScriptPage(painter, l, page);
    }
    painter.end();
    return true;
}

}  // namespace montage
