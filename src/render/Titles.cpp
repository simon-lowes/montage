#include "Titles.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTransform>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <set>
#include <tuple>

#include "Processing.h"

namespace montage {

namespace {

// How far an animation has got (0 hidden, 1 settled), eased out.
double easeOut(double p) { return 1 - std::pow(1 - std::clamp(p, 0.0, 1.0), 3); }
double smoothstep(double v) { return v * v * (3 - 2 * v); }

// ---------------------------------------------------------------------------
// Styles

// A character's style once the title's settings and its runs are taken into account, in output pixels.
struct CharStyle {
    QString family;
    int pixelSize = 1;
    bool bold = true, italic = false;
    double tracking = 0, baseline = 0, outline = 0;
    std::map<std::string, double> axes;
    std::array<double, 3> fill{1, 1, 1}, outlineColor{0, 0, 0};
    bool operator==(const CharStyle&) const = default;
    // What changes the layout (fill and outline do not).
    bool sameShape(const CharStyle& o) const {
        return family == o.family && pixelSize == o.pixelSize && bold == o.bold && italic == o.italic && tracking == o.tracking &&
               axes == o.axes;
    }
};

std::array<double, 3> colourParam(const Effect& g, const std::string& base, FrameTime t, std::array<double, 3> def = {0, 0, 0}) {
    return {std::clamp(g.p(base + ".r", t, def[0]), 0.0, 1.0), std::clamp(g.p(base + ".g", t, def[1]), 0.0, 1.0),
            std::clamp(g.p(base + ".b", t, def[2]), 0.0, 1.0)};
}

// The title's own style for line `line` (lines after the first in theirs with "Style Lines After the First"): px
// output pixels per sequence pixel.
CharStyle baseStyle(const Effect& g, FrameTime t, double px, double defaultSize, int line) {
    CharStyle s;
    s.family = QString::fromStdString(g.s("font", "Sans Serif"));
    const bool sub = line > 0 && g.p("sub_style", t) > 0.5;
    const double size = g.p("size", t, defaultSize) * px;
    s.pixelSize = std::max(1, int(std::lround(sub ? size * g.p("sub_scale", t, 60) / 100.0 : size)));
    s.bold = g.p("bold", t, 1) > 0.5;
    s.italic = g.p("italic", t) > 0.5;
    s.tracking = g.p("tracking", t) * px;
    s.outline = g.p("outline", t) * px;
    s.fill = sub ? colourParam(g, "sub_color", t) : colourParam(g, "color", t);
    s.outlineColor = colourParam(g, "outline_color", t);
    return s;
}

void applyRun(CharStyle& s, const TextRun& r, double px) {
    if (!r.font.empty()) s.family = QString::fromStdString(r.font);
    if (r.size > 0) s.pixelSize = std::max(1, int(std::lround(r.size * px)));
    if (r.bold >= 0) s.bold = r.bold > 0;
    if (r.italic >= 0) s.italic = r.italic > 0;
    if (r.color) s.fill = *r.color;
    if (r.outline >= 0) s.outline = r.outline * px;
    if (r.outlineColor) s.outlineColor = *r.outlineColor;
    if (r.tracking) s.tracking = *r.tracking * px;
    if (r.baseline != 0) s.baseline = r.baseline * px;
    for (const auto& [tag, v] : r.axes) s.axes[tag] = v;
}

bool setsNothing(const TextRun& r) {
    return r.font.empty() && r.size <= 0 && r.bold < 0 && r.italic < 0 && !r.color && r.outline < 0 && !r.outlineColor && !r.tracking &&
           r.baseline == 0 && r.axes.empty();
}

QFont fontOf(const CharStyle& s) {
    QFont f(s.family);
    f.setPixelSize(s.pixelSize);
    f.setBold(s.bold);
    f.setItalic(s.italic);
    f.setLetterSpacing(QFont::AbsoluteSpacing, s.tracking);
    f.setHintingPreference(QFont::PreferNoHinting);
    for (const auto& [tag, v] : s.axes) {
        // The weight axis also picks the weight, so a family of static weights takes its nearest (and builds without
        // variable axes draw it as well as they can).
        if (tag == "wght") f.setWeight(QFont::Weight(std::clamp(int(std::lround(v)), 1, 1000)));
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        if (const auto tg = QFont::Tag::fromString(QString::fromStdString(tag))) f.setVariableAxis(*tg, float(v));
#endif
    }
    return f;
}

// The style of every character of `text` (lines split at '\n'), runs laid over the title's.
std::vector<CharStyle> charStyles(const Effect& g, FrameTime t, double px, double defaultSize, const QString& text,
                                  const std::vector<TextRun>& runs) {
    std::vector<CharStyle> out(size_t(text.size()));
    std::vector<CharStyle> bases;
    int line = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (size_t(line) >= bases.size()) bases.push_back(baseStyle(g, t, px, defaultSize, line));
        out[size_t(i)] = bases[size_t(line)];
        if (text[i] == '\n') ++line;
    }
    for (const TextRun& r : runs)
        for (int i = std::max(0, r.from); i < std::min<int>(int(text.size()), r.from + r.len); ++i) applyRun(out[size_t(i)], r, px);
    return out;
}

// ---------------------------------------------------------------------------
// Layout

struct Cluster {
    int from = 0, len = 0;  // in its line
    double x0 = 0, x1 = 0;  // left and right edges, line pixels
    bool space = false;
    QPainterPath path;      // pen at the line's start, baseline at 0
    std::vector<std::pair<quint32, QPointF>> glyphs;
    CharStyle style;
};

struct LineLayout {
    QString text;
    int start = 0;  // its first character in the title's text
    double width = 0, height = 0, ascent = 0;
    std::vector<Cluster> clusters;
};

LineLayout layoutLine(const QString& text, int start, const std::vector<CharStyle>& styles, const CharStyle& lineBase) {
    LineLayout out;
    out.text = text;
    out.start = start;
    QTextLayout layout(text, fontOf(text.isEmpty() ? lineBase : styles[size_t(start)]));
    QList<QTextLayout::FormatRange> formats;
    for (int i = 0; i < text.size();) {
        int e = i + 1;
        while (e < text.size() && styles[size_t(start + e)].sameShape(styles[size_t(start + i)])) ++e;
        QTextLayout::FormatRange r;
        r.start = i;
        r.length = e - i;
        r.format.setFont(fontOf(styles[size_t(start + i)]), QTextCharFormat::FontPropertiesAll);
        formats << r;
        i = e;
    }
    layout.setFormats(formats);
    QTextOption opt;
    opt.setWrapMode(QTextOption::NoWrap);
    opt.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
    layout.setTextOption(opt);
    layout.setCacheEnabled(true);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLeadingIncluded(false);
        line.setLineWidth(1e7);
    }
    layout.endLayout();
    if (!line.isValid()) return out;
    out.width = line.naturalTextWidth();
    out.ascent = line.ascent();
    out.height = line.ascent() + line.descent();
    const double baseline = line.y() + line.ascent();
    // A glyph shared by several clusters (a ligature, a conjunct) goes with the first of them only.
    std::set<std::tuple<quint32, double, double>> taken;
    QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, text);
    for (int a = 0; a < text.size();) {
        bf.setPosition(a);
        int b = bf.toNextBoundary();
        if (b <= a) b = a + 1;
        Cluster c;
        c.from = a;
        c.len = b - a;
        const double xa = line.cursorToX(a), xb = line.cursorToX(b);
        c.x0 = std::min(xa, xb);
        c.x1 = std::max(xa, xb);
        c.space = text.mid(a, b - a).trimmed().isEmpty();
        c.style = styles[size_t(start + a)];
        c.path.setFillRule(Qt::WindingFill);
        for (const QGlyphRun& run : layout.glyphRuns(a, b - a)) {
            const QRawFont rf = run.rawFont();
            const QList<quint32> ids = run.glyphIndexes();
            const QList<QPointF> pos = run.positions();
            for (int k = 0; k < ids.size() && k < pos.size(); ++k) {
                if (!taken.emplace(ids[k], pos[k].x(), pos[k].y()).second) continue;
                c.glyphs.emplace_back(ids[k], pos[k]);
                QPainterPath gp = rf.pathForGlyph(ids[k]);
                gp.translate(pos[k].x(), pos[k].y() - baseline);
                c.path.addPath(gp);
            }
        }
        if (c.style.baseline != 0) c.path.translate(0, -c.style.baseline);
        out.clusters.push_back(std::move(c));
        a = b;
    }
    return out;
}

std::vector<LineLayout> layoutText(const QStringList& lines, const std::vector<CharStyle>& styles, const Effect& g, FrameTime t,
                                   double px, double defaultSize) {
    std::vector<LineLayout> out;
    int start = 0;
    for (int i = 0; i < lines.size(); ++i) {
        out.push_back(layoutLine(lines[i], start, styles, baseStyle(g, t, px, defaultSize, i)));
        start += int(lines[i].size()) + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text animations

struct Animator {
    int k = 0, by = 0, shape = 0, sweep = 0;
    double start = 0, end = 100, offset = 0, ease = 0, amount = 100, sweepDur = 1, random = 0;
    int seed = 0;
    bool shuffle = false;
    mutable std::vector<int> order;  // the shuffled positions, for `orderOf` units (each frame's own copy)
    mutable int orderOf = -1;
    double x = 0, y = 0, scale = 100, rotation = 0, opacity = 100, blur = 0, tracking = 0, fillMix = 0, outlineMix = 0;
    std::array<double, 3> fill{1, 1, 1}, outlineColor{0, 0, 0};
};

Animator readAnimator(const Effect& g, int k, FrameTime t) {
    Animator a;
    a.k = k;
    const std::string pre = "ta" + std::to_string(k) + ".";
    auto p = [&](const char* n, double def) { return g.p(pre + n, t, def); };
    a.by = int(std::lround(p("by", 0)));
    a.shape = int(std::lround(p("shape", 0)));
    a.start = p("start", 0);
    a.end = p("end", 100);
    a.offset = p("offset", 0);
    a.ease = std::clamp(p("ease", 0) / 100.0, 0.0, 1.0);
    a.amount = p("amount", 100);
    a.sweep = int(std::lround(p("sweep", 0)));
    a.sweepDur = std::max(0.01, p("sweep_dur", 1.0));
    a.random = std::clamp(p("random", 0) / 100.0, 0.0, 1.0);
    a.seed = int(std::lround(p("seed", 0)));
    a.shuffle = p("shuffle", 0) > 0.5;
    a.x = p("x", 0);
    a.y = p("y", 0);
    a.scale = p("scale", 100);
    a.rotation = p("rotation", 0);
    a.opacity = p("opacity", 100);
    a.blur = std::max(0.0, p("blur", 0));
    a.tracking = p("tracking", 0);
    a.fillMix = std::clamp(p("fill_mix", 0) / 100.0, 0.0, 1.0);
    a.outlineMix = std::clamp(p("outline_mix", 0) / 100.0, 0.0, 1.0);
    a.fill = colourParam(g, pre + "fill", t, {1, 1, 1});
    a.outlineColor = colourParam(g, pre + "outline_color", t);
    return a;
}

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double random01(int seed, int k, int i) {
    uint64_t s = uint64_t(uint32_t(seed)) * 0x100000001B3ull ^ uint64_t(uint32_t(k)) << 40 ^ uint64_t(uint32_t(i)) * 0x9E3779B1ull;
    splitmix(s);
    return double(splitmix(s) >> 11) * (1.0 / 9007199254740992.0);
}

// Where unit i of n sits in the selector's order (a seeded shuffle with Random Order).
int unitPosition(const Animator& a, int i, int n) {
    if (!a.shuffle || n < 2) return i;
    if (a.orderOf != n) {
        a.order.assign(static_cast<size_t>(n), 0);
        std::iota(a.order.begin(), a.order.end(), 0);
        uint64_t s = uint64_t(uint32_t(a.seed)) * 0x2545F4914F6CDD1Dull + uint64_t(a.k);
        for (int j = n - 1; j > 0; --j) std::swap(a.order[size_t(j)], a.order[size_t(splitmix(s) % uint64_t(j + 1))]);
        a.orderOf = n;
    }
    return a.order[size_t(i)];
}

double selection(const Animator& a, double sec, double remaining, int i, int n) {
    if (n <= 0 || i < 0) return 0;
    const int pos = unitPosition(a, i, n);
    double s = std::min(a.start, a.end) / 100.0, e = std::max(a.start, a.end) / 100.0;
    double off = a.offset / 100.0, gain = 1;
    // Sweeps: across the text from wholly before it to wholly after it (In; Out is In played backwards at the end), or
    // the whole selection let go of (In) or taken up (Out) together.
    if (a.sweep == 1 || a.sweep == 2) {
        const double p = std::clamp((a.sweep == 1 ? sec : remaining) / a.sweepDur, 0.0, 1.0);
        off += -e + (1 - s + e) * p;
    } else if (a.sweep == 3 || a.sweep == 4) {
        gain = 1 - smoothstep(std::clamp((a.sweep == 3 ? sec : remaining) / a.sweepDur, 0.0, 1.0));
    }
    s += off, e += off;
    double v = 0;
    if (a.shape == 0) {
        const double lo = double(pos) / n, hi = double(pos + 1) / n;
        v = std::clamp((std::min(hi, e) - std::max(lo, s)) * n, 0.0, 1.0);
    } else {
        const double x = (pos + 0.5) / n;
        const double u = e > s ? (x - s) / (e - s) : (x < s ? -1.0 : 2.0);
        const bool in = u >= 0 && u <= 1;
        switch (a.shape) {
            case 1: v = std::clamp(u, 0.0, 1.0); break;      // ramp up
            case 2: v = 1 - std::clamp(u, 0.0, 1.0); break;  // ramp down
            case 3: v = in ? 1 - std::abs(2 * u - 1) : 0; break;  // triangle
            case 4: v = in ? std::sqrt(std::max(0.0, 1 - (2 * u - 1) * (2 * u - 1))) : 0; break;  // round
            case 5: v = in ? 0.5 - 0.5 * std::cos(2 * M_PI * u) : 0; break;  // smooth
            default: break;
        }
    }
    v += a.ease * (smoothstep(v) - v);
    if (a.random > 0) v *= 1 - a.random + a.random * random01(a.seed, a.k, i);
    return v * gain * a.amount / 100.0;
}

// ---------------------------------------------------------------------------
// Paths

// SVG path data (M L H V C S Q T Z, absolute and relative; arcs as straight lines) as a QPainterPath.
QPainterPath parseSvgPath(const std::string& d) {
    QPainterPath path;
    size_t i = 0;
    auto skip = [&] {
        while (i < d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) ++i;
    };
    auto number = [&](double& v) {
        skip();
        if (i >= d.size()) return false;
        const char* begin = d.c_str() + i;
        char* end = nullptr;
        v = std::strtod(begin, &end);
        if (end == begin) return false;
        i += size_t(end - begin);
        return true;
    };
    char cmd = 0;
    QPointF cur, start, ctrl;
    char lastCmd = 0;
    while (true) {
        skip();
        if (i >= d.size()) break;
        if (std::isalpha(static_cast<unsigned char>(d[i]))) cmd = d[i++];
        else if (!cmd) break;
        const bool rel = std::islower(static_cast<unsigned char>(cmd));
        const QPointF o = rel ? cur : QPointF();
        double a[7];
        auto need = [&](int n) {
            for (int k = 0; k < n; ++k)
                if (!number(a[k])) return false;
            return true;
        };
        switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M':
                if (!need(2)) return path;
                cur = start = o + QPointF(a[0], a[1]);
                path.moveTo(cur);
                cmd = rel ? 'l' : 'L';  // further pairs are lines
                break;
            case 'L':
                if (!need(2)) return path;
                cur = o + QPointF(a[0], a[1]);
                path.lineTo(cur);
                break;
            case 'H':
                if (!need(1)) return path;
                cur.setX((rel ? cur.x() : 0) + a[0]);
                path.lineTo(cur);
                break;
            case 'V':
                if (!need(1)) return path;
                cur.setY((rel ? cur.y() : 0) + a[0]);
                path.lineTo(cur);
                break;
            case 'C':
                if (!need(6)) return path;
                ctrl = o + QPointF(a[2], a[3]);
                path.cubicTo(o + QPointF(a[0], a[1]), ctrl, cur = o + QPointF(a[4], a[5]));
                break;
            case 'S': {
                if (!need(4)) return path;
                const QPointF c1 = (lastCmd == 'C' || lastCmd == 'S') ? 2 * cur - ctrl : cur;
                ctrl = o + QPointF(a[0], a[1]);
                path.cubicTo(c1, ctrl, cur = o + QPointF(a[2], a[3]));
                break;
            }
            case 'Q':
                if (!need(4)) return path;
                ctrl = o + QPointF(a[0], a[1]);
                path.quadTo(ctrl, cur = o + QPointF(a[2], a[3]));
                break;
            case 'T':
                if (!need(2)) return path;
                ctrl = (lastCmd == 'Q' || lastCmd == 'T') ? 2 * cur - ctrl : cur;
                path.quadTo(ctrl, cur = o + QPointF(a[0], a[1]));
                break;
            case 'A':
                if (!need(7)) return path;
                cur = o + QPointF(a[5], a[6]);
                path.lineTo(cur);
                break;
            case 'Z':
                path.closeSubpath();
                cur = start;
                lastCmd = 'Z';
                cmd = 0;  // a number after Z starts nothing
                continue;
            default: return path;
        }
        lastCmd = char(std::toupper(static_cast<unsigned char>(cmd)));
    }
    return path;
}

// A baseline to set text on, by distance along it: a circle, or a polyline (a wave, or SVG path data flattened).
struct PathSampler {
    bool circle = false, closed = false;
    QPointF centre;
    double radius = 0, dir = 1;
    std::vector<QPointF> pts;
    std::vector<double> cum;
    double length() const { return circle ? 2 * M_PI * radius : (cum.empty() ? 0 : cum.back()); }
    // The point at distance s and the direction there (radians, y down).
    void at(double s, QPointF& p, double& angle) const {
        if (circle) {
            const double th = -M_PI / 2 + dir * s / radius;
            p = centre + radius * QPointF(std::cos(th), std::sin(th));
            angle = th + dir * M_PI / 2;
            return;
        }
        if (pts.size() < 2) {
            p = pts.empty() ? QPointF() : pts[0], angle = 0;
            return;
        }
        const double L = length();
        if (closed && L > 0) s = std::fmod(std::fmod(s, L) + L, L);
        size_t j;
        if (s <= 0) j = 1;
        else if (s >= L) j = pts.size() - 1;
        else j = size_t(std::upper_bound(cum.begin(), cum.end(), s) - cum.begin());
        j = std::clamp<size_t>(j, 1, pts.size() - 1);
        const QPointF a = pts[j - 1], b = pts[j];
        const double seg = std::max(1e-12, cum[j] - cum[j - 1]);
        const double u = (s - cum[j - 1]) / seg;  // past the ends: carried on along the end's direction
        p = a + (b - a) * u;
        angle = std::atan2(b.y() - a.y(), b.x() - a.x());
    }
    void fromPolygon(std::vector<QPointF> poly, bool isClosed) {
        closed = isClosed;
        pts.clear();
        for (const QPointF& q : poly)
            if (pts.empty() || QLineF(pts.back(), q).length() > 1e-9) pts.push_back(q);
        cum.assign(pts.size(), 0);
        for (size_t j = 1; j < pts.size(); ++j) cum[j] = cum[j - 1] + QLineF(pts[j - 1], pts[j]).length();
    }
};

// The title's path at frame t around (cx, cy) in output pixels; false for none.
bool titlePath(const Effect& g, FrameTime t, double scale, double cx, double cy, int w, int h, PathSampler& out) {
    const int shape = int(std::lround(g.p("path_shape", t)));
    const bool reverse = g.p("path_reverse", t) > 0.5;
    if (shape == 1) {
        out.circle = true;
        out.closed = true;
        out.centre = QPointF(cx, cy);
        out.radius = std::max(1.0, g.p("path_radius", t, 300) * scale);
        out.dir = reverse ? -1 : 1;
        return true;
    }
    if (shape == 2) {
        const double amp = g.p("path_radius", t, 300) * scale * 0.25, lambda = std::max(4.0, g.p("path_wavelength", t, 600) * scale);
        const double half = 2.0 * std::max(w, h);
        std::vector<QPointF> poly;
        const int n = std::max(64, int(2 * half / 2));
        for (int i = 0; i <= n; ++i) {
            const double x = -half + 2 * half * i / n;
            poly.emplace_back(cx + x, cy + amp * std::sin(2 * M_PI * x / lambda));
        }
        if (reverse) std::reverse(poly.begin(), poly.end());
        out.fromPolygon(std::move(poly), false);
        return true;
    }
    if (shape == 3) {
        QPainterPath p = parseSvgPath(g.s("path_data"));
        if (p.isEmpty()) return false;
        QTransform tr;
        tr.translate(cx, cy);
        tr.scale(scale, scale);
        const QList<QPolygonF> polys = tr.map(p).toSubpathPolygons();
        if (polys.isEmpty() || polys[0].size() < 2) return false;
        std::vector<QPointF> poly(polys[0].begin(), polys[0].end());
        const bool isClosed = QLineF(poly.front(), poly.back()).length() < 1e-6;
        if (reverse) std::reverse(poly.begin(), poly.end());
        out.fromPolygon(std::move(poly), isClosed);
        return out.pts.size() >= 2;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The title

// Legacy text animation's motion of one unit at progress pr (Rise, Fade, Pop, Drop, Wave; Scramble changes letters).
void unitMotion(int textAnim, double pr, double sec, int u, double uh, double& dyU, double& scU, double& opU) {
    const double e = easeOut(pr);
    dyU = 0, scU = 1, opU = 1;
    switch (textAnim) {
        case 1: dyU = (1 - e) * 0.6 * uh, opU = e; break;  // rise into place
        case 2: opU = e; break;                             // fade in
        case 3: {                                           // pop, overshooting a little
            const double c1 = 1.70158, c3 = c1 + 1;
            scU = pr <= 0 ? 0 : 1 + c3 * std::pow(pr - 1, 3) + c1 * std::pow(pr - 1, 2);
            opU = std::min(1.0, 2 * pr);
            break;
        }
        case 4: {  // drop in from above and bounce
            double b = pr;
            const double n1 = 7.5625, d1 = 2.75;
            if (b < 1 / d1) b = n1 * b * b;
            else if (b < 2 / d1) b -= 1.5 / d1, b = n1 * b * b + 0.75;
            else if (b < 2.5 / d1) b -= 2.25 / d1, b = n1 * b * b + 0.9375;
            else b -= 2.625 / d1, b = n1 * b * b + 0.984375;
            dyU = -(1 - b) * 1.2 * uh;
            opU = std::min(1.0, 3 * pr);
            break;
        }
        case 5: dyU = 0.12 * uh * std::sin(2 * M_PI * (sec * 0.8 - u * 0.09)); break;  // a wave running through
        default: break;
    }
}

// What a frame of a title draws.
struct Piece {
    QPainterPath path;
    double opacity = 1;
    bool sub = false;
    // A styled title's: its own colours, outline and blur (output pixels).
    bool styled = false;
    QColor fill, outlineColor;
    double outline = 0, blur = 0;
    static Piece plain(QPainterPath path, double opacity, bool sub) {
        Piece p;
        p.path = std::move(path);
        p.opacity = opacity;
        p.sub = sub;
        return p;
    }
};

struct TitleScene {
    bool hidden = false;
    std::vector<Piece> pieces;
    std::vector<TitleGlyph> glyphs;
    double cx = 0, cy = 0, dx = 0, dy = 0, sc = 1, op = 1, wipe = 1;
    QRectF block;
    double barW = 0, barGap = 0, left = 0, blockW = 0, boxOp = 0;
    int barKind = 0;
    bool anyLines = false;
};

TitleScene buildTitle(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps) {
    TitleScene sc0;
    QFont f(QString::fromStdString(g.s("font", "Sans Serif")));
    f.setPixelSize(std::max(1, int(std::lround(g.p("size", t, 96) * scale))));
    f.setBold(g.p("bold", t, 1) > 0.5);
    f.setItalic(g.p("italic", t) > 0.5);
    f.setLetterSpacing(QFont::AbsoluteSpacing, g.p("tracking", t) * scale);
    f.setHintingPreference(QFont::PreferNoHinting);
    // Lines after the first can have their own size and colour.
    const bool sub = g.p("sub_style", t) > 0.5;
    QFont fs = f;
    if (sub) fs.setPixelSize(std::max(1, int(std::lround(g.p("size", t, 96) * scale * g.p("sub_scale", t, 60) / 100.0))));
    const QString fullText = QString::fromStdString(g.s("text", "Title"));
    const QStringList lines = fullText.split('\n');
    auto fontOfLine = [&](int i) -> const QFont& { return i > 0 && sub ? fs : f; };
    sc0.anyLines = !lines.isEmpty();
    const bool styled = titleIsStyled(g, t);

    // Animation: how far in (from the start) and out (towards the end) at this frame.
    fps = fps > 0 ? fps : 30;
    const double sec = double(t) / fps;
    const int inKind = int(std::lround(g.p("anim_in", t))), outKind = int(std::lround(g.p("anim_out", t)));
    const double inDur = std::max(0.01, g.p("anim_in_dur", t, 0.5)), outDur = std::max(0.01, g.p("anim_out_dur", t, 0.5));
    const double pin = inKind ? std::clamp(sec / inDur, 0.0, 1.0) : 1.0;
    const double pout = outKind && duration > 0 ? std::clamp((double(duration - 1 - t)) / fps / outDur, 0.0, 1.0) : 1.0;
    double dx = 0, dy = 0, sc = 1, op = 1, wipe = 1, typed = 1;
    auto animate = [&](int kind, double p, bool entering) {
        const double e = easeOut(p), sign = entering ? 1 : -1;
        switch (kind) {
            case 1: op *= e; break;                                                // fade
            case 2: dy += sign * (1 - e) * 0.12 * h; op *= std::min(1.0, 2 * p); break;   // slide up
            case 3: dy -= sign * (1 - e) * 0.12 * h; op *= std::min(1.0, 2 * p); break;   // slide down
            case 4: dx += sign * (1 - e) * 0.15 * w; op *= std::min(1.0, 2 * p); break;   // slide left
            case 5: dx -= sign * (1 - e) * 0.15 * w; op *= std::min(1.0, 2 * p); break;   // slide right
            case 6: {                                                                       // pop, overshooting a little
                const double c1 = 1.70158, c3 = c1 + 1, q = std::clamp(p, 0.0, 1.0);
                const double back = 1 + c3 * std::pow(q - 1, 3) + c1 * std::pow(q - 1, 2);
                sc *= entering ? 0.6 + 0.4 * back : 0.6 + 0.4 * q;
                op *= std::min(1.0, 2 * q);
                break;
            }
            case 7: typed = std::min(typed, p); break;  // typewriter
            case 8: wipe = std::min(wipe, e); break;    // wipe from the left
            default: break;
        }
    };
    animate(inKind, pin, true);
    animate(outKind, pout, false);
    if (op <= 0 || wipe <= 0) {
        sc0.hidden = true;
        return sc0;
    }

    // Text animation units (letters, words or lines) and how far through its turn each is, from the text itself.
    const int textAnim = int(std::lround(g.p("text_anim", t)));
    const int by = int(std::lround(g.p("text_anim_by", t)));  // 0 letters, 1 words, 2 lines
    const double across = std::max(0.1, g.p("text_anim_dur", t, 1.0));
    const bool animateOut = g.p("text_anim_out", t) > 0.5;
    struct Unit {
        int line, from, len;
    };
    std::vector<Unit> units;
    if (textAnim != 0)
        for (int i = 0; i < lines.size(); ++i) {
            const QString& ln = lines[i];
            if (by == 2) {
                if (!ln.trimmed().isEmpty()) units.push_back({i, 0, int(ln.size())});
                continue;
            }
            for (int k = 0; k < ln.size();) {
                if (ln[k].isSpace()) {
                    ++k;
                    continue;
                }
                int e = k + 1;
                if (by == 1)
                    while (e < ln.size() && !ln[e].isSpace()) ++e;
                units.push_back({i, k, e - k});
                k = e;
            }
        }
    const int n = int(units.size());
    // Each unit takes a share of the time, starting one after another so the last ends `across` in.
    const double unitDur = std::clamp(across * (n > 1 ? 0.45 : 1.0), 0.08, across);
    const double stagger = n > 1 ? (across - unitDur) / (n - 1) : 0;
    auto unitProgress = [&](int u) {
        double pr = std::clamp((sec - u * stagger) / unitDur, 0.0, 1.0);
        if (animateOut && duration > 0) {
            const double remaining = double(duration - 1 - t) / fps;  // the last unit leaves last
            pr = std::min(pr, std::clamp((remaining - (n - 1 - u) * stagger) / unitDur, 0.0, 1.0));
        }
        return pr;
    };
    static const QString noise = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$%&*@");

    // A styled title: laid out with its runs (letters still shuffling under Scramble shown as such).
    std::vector<LineLayout> rich;
    if (styled) {
        QStringList shownLines = lines;
        if (textAnim == 6)
            for (int u = 0; u < n; ++u)
                if (unitProgress(u) < 1)
                    for (int k = 0; k < units[size_t(u)].len; ++k) {
                        QString& ln = shownLines[units[size_t(u)].line];
                        const int at = units[size_t(u)].from + k;
                        if (ln[at].isSpace() || ln[at].isSurrogate()) continue;
                        const uint32_t hsh = uint32_t(u + 1) * 73856093u ^ uint32_t(k + 1) * 19349663u ^ uint32_t(t / 2 + 1) * 83492791u;
                        ln[at] = noise[int(hsh % uint32_t(noise.size()))];
                    }
        const std::vector<TextRun> runs = parseTextRuns(g.s("runs"));
        rich = layoutText(shownLines, charStyles(g, t, scale, 96, shownLines.join('\n'), runs), g, t, scale, 96);
        // Measured on the real letters, so the block does not move while they shuffle.
        if (shownLines != lines) {
            const std::vector<LineLayout> real = layoutText(lines, charStyles(g, t, scale, 96, fullText, runs), g, t, scale, 96);
            for (size_t i = 0; i < rich.size(); ++i) rich[i].width = real[i].width, rich[i].height = real[i].height, rich[i].ascent = real[i].ascent;
        }
    }

    // Layout on the whole text, so nothing moves while it types on.
    std::vector<double> widths, heights, ascents;
    double blockW = 0, totalH = 0;
    for (int i = 0; i < lines.size(); ++i) {
        if (styled) {
            widths.push_back(rich[size_t(i)].width);
            heights.push_back(rich[size_t(i)].height);
            ascents.push_back(rich[size_t(i)].ascent);
        } else {
            QFontMetricsF fm(fontOfLine(i));
            widths.push_back(fm.horizontalAdvance(lines[i]));
            heights.push_back(fm.height());
            ascents.push_back(fm.ascent());
        }
        blockW = std::max(blockW, widths.back());
    }
    const double spacing = g.p("line_spacing", t, 1.15);
    std::vector<double> tops;
    for (int i = 0; i < lines.size(); ++i) {
        tops.push_back(totalH);
        totalH += i + 1 < lines.size() ? heights[size_t(i)] * spacing : heights[size_t(i)];
    }
    const double boxOp = g.p("box_opacity", t) / 100.0;
    const double pad = boxOp > 0 ? g.p("box_padding", t, 24) * scale : 0.0;
    const int barKind = int(std::lround(g.p("bar", t)));
    const double barW = barKind ? g.p("bar_width", t, 8) * scale : 0.0, barGap = barKind == 1 ? barW * 1.5 : 0.0;
    // Placement: Free is Position from the centre; the anchors sit inside the title-safe area (8 %).
    const int anchor = int(std::lround(g.p("anchor", t)));
    const double mx = 0.08 * w, my = 0.08 * h, px = g.p("pos_x", t) * scale, py = g.p("pos_y", t) * scale;
    const double halfW = blockW / 2 + pad, halfH = totalH / 2 + pad + (barKind == 2 ? barW * 2 : 0);
    double cx = w * 0.5 + px, cy = h * 0.5 + py;
    if (anchor == 1 || anchor == 4) cx = mx + barW + barGap + halfW + px;
    if (anchor == 3 || anchor == 5) cx = w - mx - halfW + px;
    if (anchor == 2) cx = w * 0.5 + px;
    if (anchor >= 1 && anchor <= 3) cy = h - my - halfH + py;
    if (anchor == 4 || anchor == 5) cy = my + halfH + py;
    const int align = int(g.p("align", t, 1));
    double left = cx - blockW / 2, top = cy - totalH / 2;
    // Roll and crawl: from where it starts to where it ends over the clip, at a steady speed between the eases.
    if (const int motion = int(std::lround(g.p("motion", t))); motion > 0 && duration > 1) {
        const double u = std::clamp(double(t) / double(duration - 1), 0.0, 1.0);
        const double a = std::clamp(g.p("motion_ease", t) * fps / double(duration - 1), 0.0, 0.5);
        const double k = a <= 0 ? u
                         : u < a ? u * u / (2 * a * (1 - a))
                         : u > 1 - a ? 1 - (1 - u) * (1 - u) / (2 * a * (1 - a))
                                     : (u - a / 2) / (1 - a);
        const bool startOff = g.p("start_off", t, 1) > 0.5, endOff = g.p("end_off", t, 1) > 0.5;
        if (motion == 1) {
            // Up the frame. Not off screen: where it is laid out if it fits, else its first (last) line inside the safe area.
            const bool fits = 2 * halfH <= h - 2 * my;
            const double from = startOff ? h + halfH : (fits ? cy : my + halfH);
            const double to = endOff ? -halfH : (fits ? cy : h - my - halfH);
            dy += from + (to - from) * k - cy;
        } else {
            const double hw = halfW + (barKind == 1 ? (barW + barGap) / 2 : 0.0);
            const bool fits = 2 * hw <= w - 2 * mx, leftward = motion == 2;
            const double enter = leftward ? w + hw : -hw, exit = leftward ? -hw : w + hw;
            const double from = startOff ? enter : (fits ? cx : (leftward ? mx + hw : w - mx - hw));
            const double to = endOff ? exit : (fits ? cx : (leftward ? w - mx - hw : mx + hw));
            dx += from + (to - from) * k - cx;
        }
    }

    // The text (typed so far), first line and the rest apart; with a text animation (CapCut's and Descript's), each
    // letter, word or line on its own, moved, scaled and faded by how far through its turn it is.
    std::vector<Piece>& pieces = sc0.pieces;
    int totalChars = 0;
    std::vector<int> lineStart;
    for (const QString& l : lines) lineStart.push_back(totalChars), totalChars += int(l.size());
    int budget = int(std::floor(typed * totalChars + 1e-9));
    auto lineX = [&](int i) {
        const double lw = widths[size_t(i)];
        return align == 0 ? left : (align == 2 ? left + blockW - lw : cx - lw / 2);
    };
    auto plain = [&] {
        QPainterPath mainPath, subPath;
        for (int i = 0; i < lines.size(); ++i) {
            const QString shown = lines[i].left(std::max(0, budget - lineStart[size_t(i)]));
            if (shown.isEmpty()) continue;
            const double y = top + tops[size_t(i)] + ascents[size_t(i)];
            (i > 0 && sub ? subPath : mainPath).addText(QPointF(lineX(i), y), fontOfLine(i), shown);
        }
        pieces.clear();
        pieces.push_back(Piece::plain(mainPath, 1, false));
        pieces.push_back(Piece::plain(subPath, 1, true));
    };
    if (styled) {
        // Every glyph cluster a unit: placed on its line (or the path), then moved by the text animations.
        std::vector<Animator> anims;
        for (int k : textAnimators(g)) anims.push_back(readAnimator(g, k, t));
        const double remaining = duration > 0 ? double(duration - 1 - t) / fps : 1e9;
        // Indices for the selectors: letters, letters not spaces, words, lines.
        struct Index {
            int letter = -1, solid = -1, word = -1;
        };
        std::vector<std::vector<Index>> idx(rich.size());
        int nLetters = 0, nSolid = 0, nWords = 0;
        for (size_t i = 0; i < rich.size(); ++i) {
            bool inWord = false;
            for (const Cluster& c : rich[i].clusters) {
                Index x;
                x.letter = nLetters++;
                if (!c.space) {
                    x.solid = nSolid++;
                    if (!inWord) ++nWords;
                    x.word = nWords - 1;
                }
                inWord = !c.space;
                idx[i].push_back(x);
            }
        }
        const int nLines = int(rich.size());
        // Legacy units by their first character.
        auto legacyUnit = [&](int line, int from) {
            for (int u = 0; u < n; ++u)
                if (units[size_t(u)].line == line && from >= units[size_t(u)].from && from < units[size_t(u)].from + units[size_t(u)].len) return u;
            return -1;
        };
        PathSampler path;
        const bool onPath = titlePath(g, t, scale, cx, cy, w, h, path);
        const double pathLen = onPath ? path.length() : 0;
        const bool upright = g.p("path_upright", t) > 0.5;
        QRectF placedBounds;
        for (size_t i = 0; i < rich.size(); ++i) {
            const LineLayout& L = rich[i];
            const int nc = int(L.clusters.size());
            // Each cluster's selection by each animation, and the tracking they add (spreading from the left).
            std::vector<std::vector<double>> sel(size_t(nc), std::vector<double>(anims.size(), 0.0));
            std::vector<double> trackAdd(size_t(nc), 0.0);
            for (int c = 0; c < nc; ++c)
                for (size_t a = 0; a < anims.size(); ++a) {
                    const Animator& an = anims[a];
                    const Index& x = idx[i][size_t(c)];
                    double v = 0;
                    switch (an.by) {
                        case 0: v = selection(an, sec, remaining, x.letter, nLetters); break;
                        case 1: v = selection(an, sec, remaining, x.solid, nSolid); break;
                        case 2: v = selection(an, sec, remaining, x.word, nWords); break;
                        default: v = selection(an, sec, remaining, int(i), nLines); break;
                    }
                    sel[size_t(c)][a] = v;
                    trackAdd[size_t(c)] += an.tracking * scale * v;
                }
            std::vector<int> visual(static_cast<size_t>(nc));
            std::iota(visual.begin(), visual.end(), 0);
            std::stable_sort(visual.begin(), visual.end(), [&](int a, int b) { return L.clusters[size_t(a)].x0 < L.clusters[size_t(b)].x0; });
            std::vector<double> shift(size_t(nc), 0.0);
            double run = 0;
            for (int c : visual) shift[size_t(c)] = run, run += trackAdd[size_t(c)];
            const double lineW = L.width + run;
            const double alignShift = align == 0 ? 0.0 : (align == 2 ? -run : -run / 2);
            const double baseY = top + tops[i] + ascents[i];
            // On a path: where the line's alignment point sits along it, and how far below the first line it is.
            const double alignRef = align == 0 ? 0.0 : (align == 2 ? lineW : lineW / 2);
            const double anchorAt = (path.closed ? 0.0 : pathLen * (align == 0 ? 0.0 : align == 2 ? 1.0 : 0.5)) +
                                    pathLen * g.p("path_offset", t) / 100.0;
            const double below = tops[i] + ascents[i] - (ascents.empty() ? 0 : ascents[0]);
            for (int c = 0; c < nc; ++c) {
                const Cluster& cl = L.clusters[size_t(c)];
                if (lineStart[i] + cl.from >= budget) continue;  // not typed on yet
                const double mid = (cl.x0 + cl.x1) / 2 + shift[size_t(c)];
                QTransform place;
                if (onPath) {
                    QPointF pt;
                    double ang;
                    path.at(anchorAt + mid - alignRef, pt, ang);
                    place.translate(pt.x(), pt.y());
                    if (!upright) place.rotateRadians(ang);
                    place.translate(0, below);
                    place.translate(-mid, 0);
                } else {
                    place.translate(lineX(int(i)) + alignShift, baseY);
                }
                const QPointF baselineMid = place.map(QPointF(mid, 0));
                // The animations, about the cluster's middle.
                double ox = 0, oy = 0, scl = 1, rot = 0, opac = 1, blur = 0;
                std::array<double, 3> fill = cl.style.fill, oc = cl.style.outlineColor;
                for (size_t a = 0; a < anims.size(); ++a) {
                    const Animator& an = anims[a];
                    const double v = sel[size_t(c)][a];
                    if (v == 0) continue;
                    ox += an.x * scale * v;
                    oy += an.y * scale * v;
                    scl *= std::max(0.0, 1 + (an.scale / 100.0 - 1) * v);
                    rot += an.rotation * v;
                    opac *= std::max(0.0, 1 + (an.opacity / 100.0 - 1) * v);
                    blur += an.blur * scale * std::abs(v);
                    const double fm = std::clamp(an.fillMix * v, 0.0, 1.0), om = std::clamp(an.outlineMix * v, 0.0, 1.0);
                    for (int ch = 0; ch < 3; ++ch) {
                        fill[size_t(ch)] += (an.fill[size_t(ch)] - fill[size_t(ch)]) * fm;
                        oc[size_t(ch)] += (an.outlineColor[size_t(ch)] - oc[size_t(ch)]) * om;
                    }
                }
                double dyU = 0, scU = 1, opU = 1;
                if (textAnim != 0 && textAnim != 6)
                    if (const int u = legacyUnit(int(i), cl.from); u >= 0) unitMotion(textAnim, unitProgress(u), sec, u, heights[i], dyU, scU, opU);
                opac = std::min(1.0, opac * opU);
                scl *= scU;
                QRectF placedBox = place.mapRect(cl.path.boundingRect().translated(shift[size_t(c)], 0));
                if (!cl.space) placedBounds = placedBounds.isNull() ? placedBox : placedBounds.united(placedBox);
                const QPointF pivot(mid, -L.ascent * 0.35);
                QTransform anim;
                anim.translate(pivot.x() + ox, pivot.y() + oy + dyU);
                anim.rotate(rot);
                anim.scale(scl, scl);
                anim.translate(-pivot.x(), -pivot.y());
                QTransform shiftTr;
                shiftTr.translate(shift[size_t(c)], 0);
                const QTransform m = shiftTr * anim * place;
                Piece pc;
                if (opac > 0 && scl > 0) pc.path = m.map(cl.path);
                pc.opacity = opac;
                pc.styled = true;
                pc.fill = QColor::fromRgbF(float(fill[0]), float(fill[1]), float(fill[2]));
                pc.outlineColor = QColor::fromRgbF(float(oc[0]), float(oc[1]), float(oc[2]));
                pc.outline = cl.style.outline;
                pc.blur = blur;
                TitleGlyph tg;
                tg.index = L.start + cl.from;
                tg.length = cl.len;
                tg.line = int(i);
                tg.path = pc.path;
                tg.baselineMid = baselineMid;
                tg.glyphs = cl.glyphs;
                tg.fill = pc.fill;
                tg.outlineColor = pc.outlineColor;
                tg.outline = pc.outline;
                tg.opacity = opac;
                tg.blur = blur;
                sc0.glyphs.push_back(std::move(tg));
                if (!pc.path.isEmpty()) pieces.push_back(std::move(pc));
            }
        }
        // Text on a path: its box, bar and wipe go round where the glyphs are.
        if (onPath && !placedBounds.isNull()) {
            left = placedBounds.left();
            top = placedBounds.top();
            blockW = placedBounds.width();
            totalH = placedBounds.height();
        }
    } else if (textAnim == 0) {
        plain();
    } else {
        bool settled = true;  // every unit in place: drawn as the plain title is, kerned across the units
        for (int u = 0; u < n; ++u) {
            const Unit& un = units[size_t(u)];
            const int typedHere = budget - lineStart[size_t(un.line)] - un.from;
            if (typedHere <= 0) continue;  // not typed on yet
            QString shown = lines[un.line].mid(un.from, std::min(un.len, typedHere));
            const QFont& font = fontOfLine(un.line);
            const QFontMetricsF fm(font);
            const double x0 = lineX(un.line) + fm.horizontalAdvance(lines[un.line].left(un.from));
            const double y = top + tops[size_t(un.line)] + ascents[size_t(un.line)];
            const double uw = fm.horizontalAdvance(shown), uh = heights[size_t(un.line)];
            const double pr = unitProgress(u);
            double dyU = 0, scU = 1, opU = 1;
            unitMotion(textAnim, pr, sec, u, uh, dyU, scU, opU);
            if (textAnim == 6 && pr < 1)  // letters shuffling until they settle
                for (int k = 0; k < shown.size(); ++k)
                    if (!shown[k].isSpace()) {
                        const uint32_t hsh = uint32_t(u + 1) * 73856093u ^ uint32_t(k + 1) * 19349663u ^ uint32_t(t / 2 + 1) * 83492791u;
                        shown[k] = noise[int(hsh % uint32_t(noise.size()))];
                    }
            if (dyU != 0 || scU != 1 || opU != 1 || (textAnim == 6 && pr < 1)) settled = false;
            if (opU <= 0 || scU <= 0) continue;
            QPainterPath path;
            path.addText(QPointF(x0, y), font, shown);
            const double pcx = x0 + uw / 2, pcy = y - fm.ascent() * 0.35;
            QTransform tr;
            tr.translate(pcx, pcy + dyU);
            tr.scale(scU, scU);
            tr.translate(-pcx, -pcy);
            pieces.push_back(Piece::plain(tr.map(path), opU, un.line > 0 && sub));
        }
        if (settled) plain();
    }
    sc0.cx = cx, sc0.cy = cy, sc0.dx = dx, sc0.dy = dy, sc0.sc = sc, sc0.op = op, sc0.wipe = wipe;
    sc0.block = QRectF(left - pad, top - pad, blockW + 2 * pad, totalH + 2 * pad);
    sc0.barW = barW, sc0.barGap = barGap, sc0.left = left, sc0.blockW = blockW, sc0.boxOp = boxOp, sc0.barKind = barKind;
    return sc0;
}

// A blurred piece: drawn (shadow, outline, fill) into a layer of its own in device pixels, blurred and laid down.
void drawBlurred(QPainter& pa, const Piece& pc, double sh, double shOp, double ow, int w, int h) {
    const QTransform dev = pa.transform();
    const double scaleNow = std::sqrt(std::abs(dev.determinant()));
    const double radius = pc.blur * std::max(1e-3, scaleNow);
    QRectF r = pc.path.boundingRect().adjusted(-ow, -ow, ow + std::max(0.0, sh), ow + std::max(0.0, sh));
    r = dev.mapRect(r).adjusted(-3 * radius - 2, -3 * radius - 2, 3 * radius + 2, 3 * radius + 2);
    const QRect area = r.toAlignedRect().intersected(QRect(0, 0, w, h));
    if (area.isEmpty()) return;
    QImage layer(area.size(), QImage::Format_RGBA8888_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter lp(&layer);
        lp.setRenderHint(QPainter::Antialiasing);
        lp.translate(-area.topLeft());
        lp.setTransform(dev, true);
        if (sh > 0 && shOp > 0) lp.fillPath(pc.path.translated(sh, sh), QColor::fromRgbF(0, 0, 0, float(shOp * pc.opacity)));
        if (pc.outline > 0 && ow > 0) {
            QColor oc = pc.outlineColor;
            oc.setAlphaF(float(std::clamp(pc.opacity, 0.0, 1.0)));
            lp.strokePath(pc.path, QPen(oc, pc.outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
        QColor fc = pc.fill;
        fc.setAlphaF(float(std::clamp(pc.opacity, 0.0, 1.0)));
        lp.fillPath(pc.path, fc);
    }
    Image img(layer.width(), layer.height());
    const float k = 1.0f / 255.0f;
    for (int y = 0; y < img.height; ++y) {
        const uchar* s = layer.constScanLine(y);
        float* d = img.row(y);
        for (int x = 0; x < img.width * 4; ++x) d[x] = s[x] * k;
    }
    gaussianBlur(img, radius);
    for (int y = 0; y < img.height; ++y) {
        uchar* d = layer.scanLine(y);
        const float* s = img.row(y);
        for (int x = 0; x < img.width * 4; ++x) d[x] = uchar(std::clamp(int(std::lround(s[x] * 255.0f)), 0, 255));
    }
    pa.save();
    pa.resetTransform();
    pa.drawImage(area.topLeft(), layer);
    pa.restore();
}

}  // namespace

// ---------------------------------------------------------------------------
// Runs

std::vector<TextRun> parseTextRuns(const std::string& json) {
    std::vector<TextRun> out;
    if (json.empty()) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isArray()) return out;
    auto rgb = [](const QJsonValue& v) -> std::optional<std::array<double, 3>> {
        const QJsonArray a = v.toArray();
        if (a.size() < 3) return std::nullopt;
        return std::array<double, 3>{std::clamp(a[0].toDouble(), 0.0, 1.0), std::clamp(a[1].toDouble(), 0.0, 1.0),
                                     std::clamp(a[2].toDouble(), 0.0, 1.0)};
    };
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        TextRun r;
        r.from = o.value("from").toInt(-1);
        r.len = o.value("len").toInt(0);
        if (r.from < 0 || r.len <= 0 || r.from > (1 << 24)) continue;
        r.len = std::min(r.len, 1 << 24);
        r.font = o.value("font").toString().toStdString();
        r.size = std::max(0.0, o.value("size").toDouble(0));
        if (o.contains("bold")) r.bold = o.value("bold").toBool() ? 1 : 0;
        if (o.contains("italic")) r.italic = o.value("italic").toBool() ? 1 : 0;
        if (o.contains("color")) r.color = rgb(o.value("color"));
        if (o.contains("outline")) r.outline = std::max(0.0, o.value("outline").toDouble());
        if (o.contains("outline_color")) r.outlineColor = rgb(o.value("outline_color"));
        if (o.contains("tracking")) r.tracking = o.value("tracking").toDouble();
        r.baseline = o.value("baseline").toDouble(0);
        const QJsonObject axes = o.value("axes").toObject();
        for (auto it = axes.begin(); it != axes.end(); ++it)
            if (it.key().size() == 4) r.axes[it.key().toStdString()] = it.value().toDouble();
        out.push_back(std::move(r));
    }
    return out;
}

std::string textRunsJson(const std::vector<TextRun>& runs) {
    QJsonArray arr;
    auto rgb = [](const std::array<double, 3>& c) { return QJsonArray{c[0], c[1], c[2]}; };
    for (const TextRun& r : runs) {
        QJsonObject o;
        o["from"] = r.from;
        o["len"] = r.len;
        if (!r.font.empty()) o["font"] = QString::fromStdString(r.font);
        if (r.size > 0) o["size"] = r.size;
        if (r.bold >= 0) o["bold"] = r.bold > 0;
        if (r.italic >= 0) o["italic"] = r.italic > 0;
        if (r.color) o["color"] = rgb(*r.color);
        if (r.outline >= 0) o["outline"] = r.outline;
        if (r.outlineColor) o["outline_color"] = rgb(*r.outlineColor);
        if (r.tracking) o["tracking"] = *r.tracking;
        if (r.baseline != 0) o["baseline"] = r.baseline;
        if (!r.axes.empty()) {
            QJsonObject axes;
            for (const auto& [tag, v] : r.axes) axes[QString::fromStdString(tag)] = v;
            o["axes"] = axes;
        }
        arr.append(o);
    }
    return arr.isEmpty() ? std::string() : QJsonDocument(arr).toJson(QJsonDocument::Compact).toStdString();
}

std::vector<TextRun> addTextRun(std::vector<TextRun> runs, const TextRun& add) {
    runs.push_back(add);
    std::erase_if(runs, [](const TextRun& r) { return r.len <= 0 || r.from < 0 || setsNothing(r); });
    return runs;
}

std::vector<TextRun> shiftTextRuns(std::vector<TextRun> runs, const QString& before, const QString& after) {
    // The one stretch that changed: what lies before it and after it is the same in both.
    int pre = 0;
    const int most = int(std::min(before.size(), after.size()));
    while (pre < most && before[pre] == after[pre]) ++pre;
    int post = 0;
    while (post < most - pre && before[before.size() - 1 - post] == after[after.size() - 1 - post]) ++post;
    const int cutEnd = int(before.size()) - post, added = int(after.size()) - post - pre;
    auto map = [&](int pos) {
        if (pos < pre) return pos;
        if (pos >= cutEnd && pos > pre) return pos - (cutEnd - pre) + added;  // after the change
        if (pos == pre && cutEnd > pre) return pre;  // a run from where the cut starts takes what replaces it
        return pre + added;  // typed at its start (or cut from its middle): after the new text
    };
    for (TextRun& r : runs) {
        const int a = map(r.from), b = map(r.from + r.len);
        r.from = a, r.len = b - a;
    }
    std::erase_if(runs, [](const TextRun& r) { return r.len <= 0; });
    return runs;
}

namespace {
void overlay(TextRun& s, const TextRun& r) {
    if (!r.font.empty()) s.font = r.font;
    if (r.size > 0) s.size = r.size;
    if (r.bold >= 0) s.bold = r.bold;
    if (r.italic >= 0) s.italic = r.italic;
    if (r.color) s.color = r.color;
    if (r.outline >= 0) s.outline = r.outline;
    if (r.outlineColor) s.outlineColor = r.outlineColor;
    if (r.tracking) s.tracking = r.tracking;
    if (r.baseline != 0) s.baseline = r.baseline;
    for (const auto& [tag, v] : r.axes) s.axes[tag] = v;
}
}  // namespace

std::vector<TitleStyleSpan> titleStyleSpans(const Effect& g, FrameTime t) {
    const QString text = QString::fromStdString(g.s("text", "Title"));
    const std::vector<TextRun> runs = parseTextRuns(g.s("runs"));
    const bool sub = g.p("sub_style", t) > 0.5;
    std::vector<TitleStyleSpan> out;
    int line = 0;
    for (int i = 0; i < text.size(); ++i) {
        TextRun st;
        st.font = g.s("font", "Sans Serif");
        st.size = g.p("size", t, 96) * (line > 0 && sub ? g.p("sub_scale", t, 60) / 100.0 : 1.0);
        st.bold = g.p("bold", t, 1) > 0.5;
        st.italic = g.p("italic", t) > 0.5;
        const std::array<double, 3> fill = colourParam(g, line > 0 && sub ? "sub_color" : "color", t);
        st.color = fill;
        st.outline = g.p("outline", t);
        st.outlineColor = colourParam(g, "outline_color", t);
        st.tracking = g.p("tracking", t);
        for (const TextRun& r : runs)
            if (i >= r.from && i < r.from + r.len) overlay(st, r);
        if (st.outline <= 0) st.outline = 0, st.outlineColor = std::array<double, 3>{0, 0, 0};
        if (!out.empty() && out.back().style == st) out.back().text += text[i];
        else out.push_back({QString(text[i]), st});
        if (text[i] == '\n') ++line;
    }
    return out;
}

void applyTitleStyleSpans(Effect& g, const std::vector<std::pair<QString, std::optional<TextRun>>>& spans) {
    QString text;
    std::vector<std::pair<TextRun, int>> counts;
    for (const auto& [str, st] : spans) {
        text += str;
        if (!st) continue;
        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == *st; });
        if (it == counts.end()) counts.emplace_back(*st, int(str.size()));
        else it->second += int(str.size());
    }
    g.strings["text"] = text.toStdString();
    g.strings.erase("runs");
    if (counts.empty()) return;
    const TextRun base = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    if (!base.font.empty()) g.strings["font"] = base.font;
    if (base.size > 0) g.params["size"] = Param(base.size);
    if (base.bold >= 0) g.params["bold"] = Param(double(base.bold));
    if (base.italic >= 0) g.params["italic"] = Param(double(base.italic));
    const char* ch[3] = {".r", ".g", ".b"};
    for (int k = 0; k < 3; ++k) {
        if (base.color) g.params[std::string("color") + ch[k]] = Param((*base.color)[size_t(k)]);
        if (base.outlineColor) g.params[std::string("outline_color") + ch[k]] = Param((*base.outlineColor)[size_t(k)]);
    }
    if (base.outline >= 0) g.params["outline"] = Param(base.outline);
    if (base.tracking) g.params["tracking"] = Param(*base.tracking);
    std::vector<TextRun> runs;
    int at = 0;
    for (const auto& [str, st] : spans) {
        // (A title has no baseline shift or axes of its own: those stay runs even in its own style.)
        if (st && (!(*st == base) || st->baseline != 0 || !st->axes.empty())) {
            TextRun r;
            r.from = at, r.len = int(str.size());
            if (st->font != base.font && !st->font.empty()) r.font = st->font;
            if (st->size != base.size && st->size > 0) r.size = st->size;
            if (st->bold != base.bold) r.bold = st->bold;
            if (st->italic != base.italic) r.italic = st->italic;
            if (st->color != base.color) r.color = st->color;
            if (st->outline != base.outline) r.outline = std::max(0.0, st->outline);
            if (st->outlineColor != base.outlineColor) r.outlineColor = st->outlineColor;
            if (st->tracking != base.tracking) r.tracking = st->tracking.value_or(0);
            r.baseline = st->baseline;
            r.axes = st->axes;
            if (!setsNothing(r)) runs.push_back(r);
        }
        at += int(str.size());
    }
    if (!runs.empty()) g.strings["runs"] = textRunsJson(runs);
}

// ---------------------------------------------------------------------------
// Text animations

double textSelection(const Effect& g, int k, FrameTime t, double fps, FrameTime duration, int i, int n) {
    fps = fps > 0 ? fps : 30;
    const double remaining = duration > 0 ? double(duration - 1 - t) / fps : 1e9;
    return selection(readAnimator(g, k, t), double(t) / fps, remaining, i, n);
}

bool titleIsStyled(const Effect& g, FrameTime t) {
    if (std::lround(g.p("path_shape", t)) > 0) return true;
    if (!textAnimators(g).empty()) return true;
    const std::vector<TextRun> runs = parseTextRuns(g.s("runs"));
    if (runs.empty()) return false;
    // Runs that change nothing (a style the title already has) leave it plain.
    const QString text = QString::fromStdString(g.s("text", "Title"));
    // (At eight times the size, so a run a fraction of a pixel bigger still counts.)
    return charStyles(g, t, 8, 96, text, runs) != charStyles(g, t, 8, 96, text, {});
}

std::vector<TitleGlyph> titleGlyphs(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps) {
    if (!titleIsStyled(g, t)) return {};
    return buildTitle(g, t, w, h, scale, duration, fps).glyphs;
}

Image renderTitle(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps) {
    QImage qi(w, h, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(Qt::transparent);
    {
        const TitleScene s = buildTitle(g, t, w, h, scale, duration, fps);
        if (s.hidden) return Image(w, h);
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::Antialiasing);
        pa.setRenderHint(QPainter::TextAntialiasing);
        auto col = [&](const char* base, double a) {
            std::string b(base);
            return QColor::fromRgbF(float(std::clamp(g.p(b + ".r", t), 0.0, 1.0)), float(std::clamp(g.p(b + ".g", t), 0.0, 1.0)),
                                    float(std::clamp(g.p(b + ".b", t), 0.0, 1.0)), float(std::clamp(a, 0.0, 1.0)));
        };
        auto withAlpha = [](QColor c, double a) {
            c.setAlphaF(float(std::clamp(a, 0.0, 1.0)));
            return c;
        };
        pa.setOpacity(std::clamp(g.p("opacity", t, 100) / 100.0 * s.op, 0.0, 1.0));
        pa.translate(s.cx + s.dx, s.cy + s.dy);
        pa.scale(s.sc, s.sc);
        pa.translate(-s.cx, -s.cy);
        const QRectF& block = s.block;
        if (s.wipe < 1) {
            const double from = block.left() - s.barW - s.barGap;
            pa.setClipRect(QRectF(from, -h, (block.right() - from) * s.wipe, 3.0 * h));
        }
        if (s.boxOp > 0 && s.anyLines) pa.fillRect(block, col("box_color", s.boxOp));
        if (s.barKind == 1) pa.fillRect(QRectF(block.left() - s.barGap - s.barW, block.top(), s.barW, block.height()), col("bar_color", 1.0));
        if (s.barKind == 2) pa.fillRect(QRectF(s.left, block.bottom() + s.barW, s.blockW, s.barW), col("bar_color", 1.0));
        const double sh = g.p("shadow", t, 4) * scale;
        const double shOp = g.p("shadow_opacity", t, 50) / 100.0;
        const double ow = g.p("outline", t) * scale;
        // A styled title's pieces: blurred ones on their own first; the rest merged where they look alike (so joined
        // scripts draw without seams), then drawn as a plain title's are.
        std::vector<Piece> merged;
        for (const Piece& pc : s.pieces) {
            if (!pc.styled) {
                merged.push_back(pc);
                continue;
            }
            if (pc.blur > 0.05) {
                drawBlurred(pa, pc, sh, shOp, std::max(ow, pc.outline), w, h);
                continue;
            }
            if (!merged.empty() && merged.back().styled && merged.back().fill == pc.fill && merged.back().outline == pc.outline &&
                merged.back().outlineColor == pc.outlineColor && merged.back().opacity == pc.opacity) {
                merged.back().path.addPath(pc.path);
            } else {
                merged.push_back(pc);
                merged.back().path.setFillRule(Qt::WindingFill);
            }
        }
        if (sh > 0 && shOp > 0)
            for (const Piece& pc : merged) pa.fillPath(pc.path.translated(sh, sh), QColor::fromRgbF(0, 0, 0, float(shOp * pc.opacity)));
        for (const Piece& pc : merged) {
            if (pc.styled) {
                if (pc.outline > 0)
                    pa.strokePath(pc.path, QPen(withAlpha(pc.outlineColor, pc.opacity), pc.outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            } else if (ow > 0) {
                pa.strokePath(pc.path, QPen(col("outline_color", pc.opacity), ow * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            }
        }
        for (const Piece& pc : merged)
            pa.fillPath(pc.path, pc.styled ? withAlpha(pc.fill, pc.opacity) : col(pc.sub ? "sub_color" : "color", pc.opacity));
    }
    Image img(w, h);
    const float k = 1.0f / 255.0f;
    for (int y = 0; y < h; ++y) {
        const uchar* s = qi.constScanLine(y);
        float* d = img.row(y);
        for (int x = 0; x < w * 4; ++x) d[x] = s[x] * k;
    }
    return img;
}

QPainterPath styledTextOutline(const Effect& g, FrameTime t, double px, double defaultSize, double lineSpacing) {
    const std::vector<TextRun> runs = parseTextRuns(g.s("runs"));
    if (runs.empty()) return {};
    const QString text = QString::fromStdString(g.s("text", "3D Title"));
    const std::vector<CharStyle> styles = charStyles(g, t, px, defaultSize, text, runs);
    if (styles == charStyles(g, t, px, defaultSize, text, {})) return {};
    const std::vector<LineLayout> lines = layoutText(text.split('\n'), styles, g, t, px, defaultSize);
    double blockW = 0;
    for (const LineLayout& l : lines) blockW = std::max(blockW, l.width);
    const int align = int(std::lround(g.p("align", t, 1)));
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    double y = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const LineLayout& l = lines[i];
        const double x = align == 0 ? 0 : align == 2 ? blockW - l.width : (blockW - l.width) / 2;
        for (const Cluster& c : l.clusters) out.addPath(c.path.translated(x, y + l.ascent));
        y += l.height * lineSpacing;
    }
    return out;
}

}  // namespace montage
