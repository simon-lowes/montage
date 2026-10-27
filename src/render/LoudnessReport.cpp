#include "LoudnessReport.h"

#include <QFile>
#include <QFont>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/History.h"

namespace montage {

namespace {

std::string fmt(const char* f, double a, double b) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// Timeline seconds as the sequence's timecode.
std::string timecodeOf(const Sequence& s, double seconds) {
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    return formatTimecode(FrameTime(std::floor(std::max(0.0, seconds) * fps + 1e-6)), s.fps);
}

// The figures, one line each: "Integrated loudness: -23.1 LUFS".
std::vector<std::pair<std::string, std::string>> figures(const DialogueLoudness& d, const LoudnessReportSpec& spec) {
    std::vector<std::pair<std::string, std::string>> f;
    char buf[160];
    auto add = [&](const char* name, const char* format, double v) {
        std::snprintf(buf, sizeof buf, format, v);
        f.push_back({name, buf});
    };
    if (spec.target != 0) {
        std::string t = spec.targetName.empty() ? std::string() : spec.targetName + ": ";
        std::snprintf(buf, sizeof buf, "%.1f %s ±%g LU", spec.target, spec.dialogueGated ? "LKFS dialogue" : "LUFS", spec.tolerance);
        t += buf;
        if (spec.peakCeiling != 0) {
            std::snprintf(buf, sizeof buf, ", %.1f dBTP", spec.peakCeiling);
            t += buf;
        }
        f.push_back({"Target", t});
    } else if (spec.peakCeiling != 0) {
        add("Ceiling", "%.1f dBTP", spec.peakCeiling);
    }
    if (spec.leqmLimit > 0) add("Leq(M) limit", "%.0f", spec.leqmLimit);
    if (d.valid) {
        add("Integrated loudness", "%.1f LUFS", d.programme);
        if (d.dialogue > -70) add("Dialogue loudness", "%.1f LKFS", d.dialogue);
        add("Share of dialogue", "%.0f %%", d.speechShare * 100);
        add("Loudness range", "%.1f LU", d.range);
        if (d.dialogueRange > 0) add("Dialogue loudness range", "%.1f LU", d.dialogueRange);
        add("True peak", "%.1f dBTP", d.truePeak);
    } else {
        f.push_back({"Integrated loudness", "too quiet to measure"});
    }
    if (d.leqm > 0) add("Leq(M)", "%.1f", d.leqm);
    f.push_back({"Result", loudnessPasses(d, spec) ? "passes" : "does not pass"});
    return f;
}

}  // namespace

std::vector<LoudnessOver> loudnessOvers(const DialogueLoudness& d, const LoudnessReportSpec& spec) {
    std::vector<LoudnessOver> out;
    auto scan = [&](bool truePeak, auto over, auto describe) {
        std::vector<LoudnessOver> found;
        double worst = 0;
        bool worstSet = false;
        for (size_t i = 0; i < d.log.size(); ++i) {
            const LoudnessLogPoint& p = d.log[i];
            double v = 0;
            if (!over(p, v)) continue;
            const double a = i > 0 ? d.log[i - 1].t : std::max(0.0, p.t - 0.1);
            if (!found.empty() && a - found.back().to <= 1.0 + 1e-9) {
                found.back().to = p.t;
                worst = std::max(worst, v);
            } else {
                if (!found.empty()) found.back().what = describe(worst);
                found.push_back({a, p.t, {}, truePeak});
                worst = v;
            }
            worstSet = true;
        }
        if (worstSet && !found.empty()) found.back().what = describe(worst);
        // (each stretch says its worst; the earlier ones were described as they closed)
        out.insert(out.end(), found.begin(), found.end());
    };
    if (spec.peakCeiling != 0)
        scan(true, [&](const LoudnessLogPoint& p, double& v) { v = p.truePeak; return p.truePeak > spec.peakCeiling + 0.05; },
             [&](double v) { return fmt("True peak %.1f dBTP over %.1f", v, spec.peakCeiling); });
    if (spec.target != 0) {
        const double limit = spec.target + spec.tolerance;
        // (not over the first 3 s, before the short-term window is full)
        const double full = d.log.empty() ? 0 : d.log.front().t + 2.85;
        scan(false, [&](const LoudnessLogPoint& p, double& v) {
                 v = p.shortTerm;
                 return p.t >= full && (!spec.dialogueGated || p.speech) && p.shortTerm > limit + 1e-9;
             },
             [&](double v) { return fmt(spec.dialogueGated ? "Dialogue short-term %.1f LKFS over %.1f" : "Short-term %.1f LUFS over %.1f", v, limit); });
    }
    std::sort(out.begin(), out.end(), [](const LoudnessOver& a, const LoudnessOver& b) { return a.from < b.from; });
    return out;
}

bool loudnessPasses(const DialogueLoudness& d, const LoudnessReportSpec& spec) {
    if (spec.target != 0 && (!d.valid || std::fabs(d.value() - spec.target) > spec.tolerance + 1e-9)) return false;
    if (spec.peakCeiling != 0 && d.valid && d.truePeak > spec.peakCeiling + 0.05) return false;
    if (spec.leqmLimit > 0 && d.leqm > spec.leqmLimit + 0.05) return false;
    return true;
}

bool writeLoudnessCsv(const std::string& path, const DialogueLoudness& d, const LoudnessReportSpec& spec, const Sequence& s,
                      std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    QTextStream out(&f);
    out << "# Loudness report: " << QString::fromStdString(spec.title.empty() ? s.name : spec.title) << "\n";
    for (const auto& [name, value] : figures(d, spec)) out << "# " << QString::fromStdString(name) << ": " << QString::fromStdString(value) << "\n";
    for (const LoudnessOver& o : loudnessOvers(d, spec))
        out << "# Over: " << QString::fromStdString(timecodeOf(s, o.from)) << " to " << QString::fromStdString(timecodeOf(s, o.to)) << ", "
            << QString::fromStdString(o.what) << "\n";
    out << "seconds,timecode,momentary_lufs,short_term_lufs,true_peak_dbtp,dialogue\n";
    for (const LoudnessLogPoint& p : d.log)
        out << QString::number(p.t, 'f', 1) << ',' << QString::fromStdString(timecodeOf(s, p.t)) << ',' << QString::number(std::max(-70.0f, p.momentary), 'f', 1)
            << ',' << QString::number(std::max(-70.0f, p.shortTerm), 'f', 1) << ',' << QString::number(p.truePeak, 'f', 1) << ',' << (p.speech ? 1 : 0) << '\n';
    out.flush();
    if (f.error() != QFileDevice::NoError) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

bool writeLoudnessPdf(const std::string& path, const DialogueLoudness& d, const LoudnessReportSpec& spec, const Sequence& s,
                      std::string* error) {
    QPdfWriter writer(QString::fromStdString(path));
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(0, 0, 0, 0)));
    writer.setTitle(QStringLiteral("Loudness report"));
    writer.setCreator(QStringLiteral("Montage"));
    QPainter p;
    if (!p.begin(&writer)) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    // (in points on an A4 page, 595 x 842)
    const int W = 595, H = 842, margin = 48;
    p.setWindow(0, 0, W, H);
    p.setViewport(0, 0, writer.width(), writer.height());
    QFont title(QStringLiteral("Helvetica"));
    title.setPixelSize(18);
    title.setBold(true);
    QFont body(QStringLiteral("Helvetica"));
    body.setPixelSize(10);
    QFont small(QStringLiteral("Helvetica"));
    small.setPixelSize(8);
    int y = margin;
    p.setFont(title);
    p.drawText(margin, y + 18, QStringLiteral("Loudness report: %1").arg(QString::fromStdString(spec.title.empty() ? s.name : spec.title)));
    y += 34;
    p.setFont(body);
    for (const auto& [name, value] : figures(d, spec)) {
        p.drawText(margin, y + 10, QString::fromStdString(name));
        p.drawText(margin + 170, y + 10, QString::fromStdString(value));
        y += 15;
    }
    y += 12;

    // The graph: short-term (solid) and momentary (light) loudness, the target band and the ceiling.
    const QRectF g(margin + 30, y, W - 2 * margin - 30, 220);
    const double top = 0, bottom = -60;
    auto yOf = [&](double lufs) { return g.top() + (top - std::clamp(lufs, bottom, top)) / (top - bottom) * g.height(); };
    const double t0 = d.log.empty() ? 0 : std::max(0.0, d.log.front().t - 0.1), t1 = d.log.empty() ? 1 : std::max(t0 + 0.1, d.log.back().t);
    auto xOf = [&](double t) { return g.left() + (t - t0) / (t1 - t0) * g.width(); };
    p.setPen(QPen(QColor(200, 200, 200), 0.5));
    p.setFont(small);
    for (int l = 0; l >= -60; l -= 10) {
        p.drawLine(QPointF(g.left(), yOf(l)), QPointF(g.right(), yOf(l)));
        p.drawText(QRectF(margin, yOf(l) - 5, 26, 10), Qt::AlignRight | Qt::AlignVCenter, QString::number(l));
    }
    if (spec.target != 0) {
        p.fillRect(QRectF(g.left(), yOf(spec.target + spec.tolerance), g.width(), yOf(spec.target - spec.tolerance) - yOf(spec.target + spec.tolerance)),
                   QColor(63, 185, 80, 50));
    }
    if (spec.peakCeiling != 0) {
        p.setPen(QPen(QColor(220, 60, 50), 0.8, Qt::DashLine));
        p.drawLine(QPointF(g.left(), yOf(spec.peakCeiling)), QPointF(g.right(), yOf(spec.peakCeiling)));
    }
    auto curve = [&](auto value, const QColor& c, double width) {
        QPainterPath path;
        bool started = false;
        for (const LoudnessLogPoint& pt : d.log) {
            const QPointF at(xOf(pt.t), yOf(value(pt)));
            if (!started) path.moveTo(at), started = true;
            else path.lineTo(at);
        }
        p.setPen(QPen(c, width));
        p.drawPath(path);
    };
    curve([](const LoudnessLogPoint& pt) { return double(pt.momentary); }, QColor(150, 180, 230), 0.4);
    curve([](const LoudnessLogPoint& pt) { return double(pt.shortTerm); }, QColor(30, 80, 170), 1.0);
    curve([](const LoudnessLogPoint& pt) { return double(pt.truePeak); }, QColor(220, 60, 50), 0.4);
    p.setPen(QPen(Qt::black, 0.6));
    p.setBrush(Qt::NoBrush);
    p.drawRect(g);
    p.drawText(QRectF(g.left(), g.bottom() + 2, 120, 10), Qt::AlignLeft, QString::fromStdString(timecodeOf(s, t0)));
    p.drawText(QRectF(g.right() - 120, g.bottom() + 2, 120, 10), Qt::AlignRight, QString::fromStdString(timecodeOf(s, t1)));
    p.drawText(QRectF(g.left(), g.bottom() + 14, g.width(), 10), Qt::AlignLeft,
               QStringLiteral("Short-term (dark blue) and momentary (light blue) loudness, LUFS; true peak (red), dBTP"));
    y = int(g.bottom()) + 40;

    // The overs, as many pages as they need.
    p.setFont(body);
    const std::vector<LoudnessOver> overs = loudnessOvers(d, spec);
    p.drawText(margin, y + 10, overs.empty() ? QStringLiteral("Nothing over the limits.") : QStringLiteral("Over the limits:"));
    y += 18;
    for (const LoudnessOver& o : overs) {
        if (y > H - margin) {
            writer.newPage();
            y = margin;
        }
        p.drawText(margin, y + 10, QStringLiteral("%1 to %2").arg(QString::fromStdString(timecodeOf(s, o.from)), QString::fromStdString(timecodeOf(s, o.to))));
        p.drawText(margin + 170, y + 10, QString::fromStdString(o.what));
        y += 14;
    }
    p.end();
    return true;
}

}  // namespace montage
