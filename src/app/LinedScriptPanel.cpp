#include "LinedScriptPanel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <cmath>
#include <set>

#include "EditorState.h"
#include "core/ScriptCut.h"

namespace montage {

// The pages, one under another, as wide as the panel.
class LinedScriptPages : public QWidget {
public:
    explicit LinedScriptPages(LinedScriptPanel* panel) : QWidget(panel), panel_(panel) {
        setObjectName(QStringLiteral("linedScriptPages"));
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    double scale() const { return std::max(0.2, (width() - 2.0 * kGap) / LinedScriptLayout::kPageWidth); }
    double pageTop(int page) const { return kGap + page * (LinedScriptLayout::kPageHeight * scale() + kGap); }
    // The page a point is on, and where on it (page points); -1 for none.
    int pageAt(QPointF at, QPointF& onPage) const {
        const double s = scale();
        for (int pg = 0; pg < panel_->layout_.pages; ++pg) {
            const QRectF r(kGap, pageTop(pg), LinedScriptLayout::kPageWidth * s, LinedScriptLayout::kPageHeight * s);
            if (r.contains(at)) {
                onPage = (at - r.topLeft()) / s;
                return pg;
            }
        }
        return -1;
    }
    void fit() {
        const int pages = std::max(1, panel_->layout_.pages);
        setFixedHeight(int(std::ceil(pageTop(pages))));
        update();
    }
    // A point on the page of a script line's first row (widget coordinates), to scroll to it.
    QPointF pointOf(int line) const {
        for (const auto& r : panel_->layout_.rows)
            if (r.line == line) return {kGap + r.x * scale(), pageTop(r.page) + r.y * scale()};
        return {};
    }

protected:
    void resizeEvent(QResizeEvent*) override { fit(); }
    void paintEvent(QPaintEvent* e) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Mid));
        const LinedScriptLayout& l = panel_->layout_;
        if (l.pages == 0) {
            painter.setPen(palette().color(QPalette::Text));
            painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                             tr("No script. Open a screenplay (Fountain, Final Draft or text) to see which takes read each line."));
            return;
        }
        const double s = scale();
        for (int pg = 0; pg < l.pages; ++pg) {
            const QRectF r(kGap, pageTop(pg), LinedScriptLayout::kPageWidth * s, LinedScriptLayout::kPageHeight * s);
            if (!r.intersects(e->rect())) continue;
            painter.save();
            painter.translate(r.topLeft());
            painter.scale(s, s);
            paintLinedScriptPage(painter, l, pg);
            // The line picked, lit.
            for (const auto& row : l.rows)
                if (row.page == pg && row.line >= 0 && row.line == panel_->selected_)
                    painter.fillRect(QRectF(row.x - 2, row.y, row.text.size() * 7.2 + 4, LinedScriptLayout::kRow), QColor(255, 220, 0, 70));
            painter.restore();
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        QPointF at;
        const int pg = pageAt(e->position(), at);
        if (pg < 0 || e->button() != Qt::LeftButton) return;
        if (const auto* seg = panel_->layout_.segmentAt(pg, at)) panel_->cueLine(seg->line, seg->reading);
        else if (const int line = panel_->layout_.lineAt(pg, at); line >= 0) panel_->cueLine(line);
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* h = static_cast<QHelpEvent*>(e);
            QPointF at;
            const int pg = pageAt(h->pos(), at);
            const auto* seg = pg >= 0 ? panel_->layout_.segmentAt(pg, at) : nullptr;
            const Project& p = panel_->state_->project();
            if (seg && size_t(seg->line) < p.script.readings.size() && size_t(seg->reading) < p.script.readings[size_t(seg->line)].size()) {
                const ScriptReading& r = p.script.readings[size_t(seg->line)][size_t(seg->reading)];
                const MediaItem* m = p.findMedia(r.media);
                QString tip = tr("%1\n%2 % of the line said, %3 extra word(s)")
                                  .arg(m ? QString::fromStdString(m->name) : QString())
                                  .arg(std::lround(r.coverage * 100))
                                  .arg(r.extraWords);
                if (m && isCircledTake(*m)) tip += '\n' + tr("Circled");
                if (seg->inCut) tip += '\n' + tr("In the cut");
                QToolTip::showText(h->globalPos(), tip, this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    static constexpr double kGap = 10;
    LinedScriptPanel* panel_;
};

LinedScriptPanel::LinedScriptPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    auto button = [&](const QString& text, const QString& name, const QString& tip) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(name);
        b->setToolTip(tip);
        top->addWidget(b);
        return b;
    };
    QPushButton* open = button(tr("Open Script…"), QStringLiteral("openScript"), tr("Read a screenplay (Fountain, Final Draft .fdx or plain text) and find each line in the transcribed takes"));
    QPushButton* edit = button(tr("Edit…"), QStringLiteral("editScript"), tr("Change the script's text (its lines are found again)"));
    QPushButton* find = button(tr("Find Takes"), QStringLiteral("findScriptTakes"), tr("Look for each line again (after transcribing more takes)"));
    QPushButton* pdf = button(tr("Export PDF…"), QStringLiteral("exportLinedScript"), tr("Write the lined script as a PDF"));
    top->addStretch(1);
    v->addLayout(top);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("linedScriptSummary"));
    v->addWidget(summary_);
    pages_ = new LinedScriptPages(this);
    scroll_ = new QScrollArea(this);
    scroll_->setWidget(pages_);
    scroll_->setWidgetResizable(true);
    v->addWidget(scroll_, 1);

    connect(open, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Script"), QString(),
                                                          tr("Screenplays (*.fountain *.spmd *.fdx *.txt);;All files (*)"));
        if (!path.isEmpty()) openFile(path);
    });
    connect(edit, &QPushButton::clicked, this, [this] { editText(); });
    connect(find, &QPushButton::clicked, this, [this] { findTakes(); });
    connect(pdf, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Lined Script"), tr("Lined Script.pdf"), tr("PDF (*.pdf)"));
        if (!path.isEmpty()) exportPdf(path);
    });
    connect(state_, &EditorState::projectChanged, this, &LinedScriptPanel::scheduleRefresh);
    refresh();
}

QWidget* LinedScriptPanel::pagesWidget() const { return pages_; }

void LinedScriptPanel::scheduleRefresh() {
    // (a drag changes the project at every step: the pages follow a moment later, and only while they are seen)
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(150, this, [this] {
        if (pending_ && isVisible()) refresh();
    });
}

void LinedScriptPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (pending_) refresh();
}

void LinedScriptPanel::refresh() {
    pending_ = false;
    const Project& p = state_->project();
    layout_ = layoutLinedScript(p, state_->sequence());
    if (p.script.text != shownText_) selected_ = -1;
    shownText_ = p.script.text;
    int read = 0;
    for (const auto& line : p.script.readings) read += line.empty() ? 0 : 1;
    std::set<int> usedLines;
    for (const auto& m : layout_.marks)
        if (m.state == 1) usedLines.insert(m.line);
    const int used = int(usedLines.size());
    const int lines = int(layout_.screenplay.lines.size());
    summary_->setText(p.script.text.empty()
                          ? QString()
                          : tr("%1: %2 of %3 lines read by a take, %4 in the cut")
                                .arg(QFileInfo(QString::fromStdString(p.script.name)).fileName(), QString::number(read), QString::number(lines),
                                     QString::number(used)));
    pages_->fit();
}

bool LinedScriptPanel::setScript(const QString& name, const QString& text, const QString& format) {
    int found = 0;
    const bool ok = state_->edit(tr("Set Script"), [&](Project& p, Sequence&) {
        found = setProjectScript(p, name.toStdString(), format.toStdString(), text.toStdString());
        return true;
    });
    if (!ok) {
        state_->message(tr("Open a sequence first"), 5000);
        return false;
    }
    refresh();
    state_->message(tr("Script: %1 of %2 lines found in the takes").arg(found).arg(layout_.screenplay.lines.size()), 6000);
    return true;
}

bool LinedScriptPanel::openFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        state_->message(tr("Cannot open %1").arg(path), 6000);
        return false;
    }
    const QByteArray bytes = f.readAll();
    return setScript(path, QString::fromUtf8(bytes));
}

bool LinedScriptPanel::findTakes() {
    if (state_->project().script.text.empty()) {
        state_->message(tr("Open a script first"), 5000);
        return false;
    }
    int found = 0;
    const bool ok = state_->edit(tr("Find Script Takes"), [&](Project& p, Sequence&) {
        const auto before = p.script.readings;
        found = matchProjectScript(p);
        return p.script.readings != before;
    });
    refresh();
    state_->message(tr("Script: %1 of %2 lines found in the takes").arg(found).arg(layout_.screenplay.lines.size()), 6000);
    return ok;
}

bool LinedScriptPanel::removeScript() {
    if (state_->project().script.text.empty()) return false;
    const bool ok = state_->edit(tr("Remove Script"), [](Project& p, Sequence&) {
        p.script = {};
        return true;
    });
    refresh();
    return ok;
}

bool LinedScriptPanel::exportPdf(const QString& path) {
    refresh();
    QString error;
    if (!exportLinedScriptPdf(layout_, path, &error)) {
        state_->message(error, 6000);
        return false;
    }
    state_->message(tr("Wrote %n page(s) to %1", nullptr, layout_.pages).arg(QFileInfo(path).fileName()), 6000);
    return true;
}

bool LinedScriptPanel::cueLine(int line, int reading) {
    const Project& p = state_->project();
    if (line < 0 || size_t(line) >= p.script.readings.size() || p.script.readings[size_t(line)].empty()) {
        if (line >= 0) state_->message(tr("No take reads this line"), 4000);
        return false;
    }
    const auto& readings = p.script.readings[size_t(line)];
    if (reading < 0) {
        reading = 0;
        if (const Sequence* s = state_->sequence())
            if (const auto used = scriptLinesInCut(p, *s); size_t(line) < used.size() && used[size_t(line)] >= 0) reading = used[size_t(line)];
    }
    if (size_t(reading) >= readings.size()) return false;
    const ScriptReading& r = readings[size_t(reading)];
    if (!p.findMedia(r.media)) return false;
    selected_ = line;
    pages_->update();
    scroll_->ensureVisible(int(pages_->pointOf(line).x()), int(pages_->pointOf(line).y()), 0, 60);
    const double fps = state_->sequence() ? state_->sequence()->fpsValue() : 30.0;
    // (Out is the last frame played, as the Transcript panel marks words)
    const FrameTime in = FrameTime(std::floor(r.start * fps)), out = std::max(in + 1, FrameTime(std::ceil(r.end * fps)) - 1);
    emit openRequested(r.media, in, out, in);
    return true;
}

void LinedScriptPanel::editText() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit Script"));
    auto* v = new QVBoxLayout(&dlg);
    auto* text = new QPlainTextEdit(&dlg);
    text->setObjectName(QStringLiteral("scriptText"));
    // (Final Draft's XML is edited as the Fountain it reads as, and kept so)
    const ProjectScript& was = state_->project().script;
    text->setPlainText(QString::fromStdString(was.format == "fdx" ? fdxToFountain(was.text) : was.text));
    QFont mono(QStringLiteral("Courier"));
    mono.setStyleHint(QFont::TypeWriter);
    text->setFont(mono);
    text->setPlaceholderText(tr("Fountain or plain text: a name in capitals above each speech"));
    v->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(640, 720);
    if (dlg.exec() != QDialog::Accepted) return;
    const ProjectScript& s = state_->project().script;
    // (typed text is read as the file was, Final Draft's as Fountain; new text by its look)
    const QString format = s.format == "fdx" ? QStringLiteral("fountain") : QString::fromStdString(s.format);
    const QString name = s.name.empty() ? tr("Script") : QString::fromStdString(s.name);
    if (text->toPlainText().trimmed().isEmpty()) removeScript();
    else setScript(name, text->toPlainText(), format);
}

}  // namespace montage
