// Montage — application entry point.
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>

#include "EditorState.h"
#include "MainWindow.h"
#include "Theme.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Montage");
    QApplication::setOrganizationName("Montage");
    QApplication::setApplicationVersion(MONTAGE_VERSION);
    montage::theme::apply(app);

    QCommandLineParser parser;
    parser.setApplicationDescription("Montage — professional non-linear video editor");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("project", "Project (.montage) or media files to open");
    QCommandLineOption screenshot("screenshot", "Save a screenshot of the window to <png> and quit (testing aid).", "png");
    QCommandLineOption frame("frame", "Move the playhead to <frame> after opening.", "frame");
    QCommandLineOption select("select-at", "Select the clip under the playhead on track V<n>.", "n");
    QCommandLineOption tool("source", "Open media item <index> (1-based) in the source monitor.", "index");
    QCommandLineOption panel("panel", "Bring the named panel(s) to the front (comma separated).", "names");
    parser.addOptions({screenshot, frame, select, tool, panel});
    parser.process(app);

    montage::MainWindow w;
    w.show();
    QStringList media;
    for (const QString& arg : parser.positionalArguments()) {
        if (arg.endsWith(".montage")) w.openProject(QFileInfo(arg).absoluteFilePath());
        else media << QFileInfo(arg).absoluteFilePath();
    }
    if (!media.isEmpty()) w.state()->importFiles(media);
    if (parser.isSet(frame)) w.state()->setPlayhead(parser.value(frame).toLongLong());
    if (parser.isSet(select)) {
        const montage::Sequence* s = w.state()->sequence();
        int track = parser.value(select).toInt() - 1;
        if (s) {
            if (const montage::Clip* c = montage::edit::clipAt(*s, {montage::TrackKind::Video, track}, s->playhead))
                w.state()->setSelection({c->id});
        }
    }
    if (parser.isSet(tool)) {
        int idx = parser.value(tool).toInt() - 1;
        const auto& m = w.state()->project().media;
        if (idx >= 0 && idx < int(m.size())) w.state()->setSourceMedia(m[size_t(idx)].id);
    }
    if (parser.isSet(panel))
        for (const QString& n : parser.value(panel).split(',')) w.raisePanel(n.trimmed());
    if (parser.isSet(screenshot)) w.scheduleScreenshot(parser.value(screenshot), 3500);
    return app.exec();
}
