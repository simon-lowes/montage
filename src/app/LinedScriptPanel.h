// Montage — the Script panel: the project's screenplay as a lined script (render/LinedScript.h), each take a line down
// the script lines it reads, the circled takes ringed, ticks beside what the active sequence uses and red bars beside
// what no take reads. Clicking a line opens its take in the Source monitor at that line (the one the cut uses, else
// the best); clicking a take's line opens that take there. Open Script reads Fountain, Final Draft or plain text and
// finds its lines in the transcribed takes; Find Takes looks again after more are transcribed; Export PDF writes the
// pages.
#pragma once

#include <QWidget>

#include "core/Model.h"
#include "render/LinedScript.h"

class QLabel;
class QScrollArea;

namespace montage {

class EditorState;
class LinedScriptPages;

class LinedScriptPanel : public QWidget {
    Q_OBJECT
public:
    explicit LinedScriptPanel(EditorState* state, QWidget* parent = nullptr);

    // Each one undo step; false (with a message) when nothing changed.
    bool openFile(const QString& path);
    bool setScript(const QString& name, const QString& text, const QString& format = QString());
    bool findTakes();
    bool removeScript();
    bool exportPdf(const QString& path);
    // Opens the take of a script line in the Source monitor at that line: `reading` indexes the line's readings
    // (-1: the one the active sequence uses, else the best). False when the line has none.
    bool cueLine(int line, int reading = -1);

    void refresh();  // lays the pages out again now
    const LinedScriptLayout& layout() const { return layout_; }
    int selectedLine() const { return selected_; }
    QWidget* pagesWidget() const;

signals:
    void openRequested(Id media, FrameTime in, FrameTime out, FrameTime at);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void editText();
    void scheduleRefresh();

    EditorState* state_;
    LinedScriptLayout layout_;
    LinedScriptPages* pages_;
    QScrollArea* scroll_;
    QLabel* summary_;
    int selected_ = -1;
    std::string shownText_;  // the script's text when last laid out
    bool pending_ = false;
    friend class LinedScriptPages;
};

}  // namespace montage
