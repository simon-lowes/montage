#include "ReviewExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <cmath>

namespace montage {

namespace {

QString reviewBase(const Sequence& s) {
    QString base = QString::fromStdString(s.name.empty() ? std::string("Sequence") : s.name);
    for (QChar& c : base)
        if (QStringLiteral("\\/:*?\"<>|").contains(c)) c = '-';
    return base + QStringLiteral(" - Review");
}

QString versionName(const QString& base, int version) { return version <= 1 ? base : base + QStringLiteral(" v%1").arg(version); }

}  // namespace

std::vector<int> reviewVersions(const Sequence& s, const std::string& folder) {
    const QString base = reviewBase(s);
    std::vector<int> out;
    const QDir dir(QString::fromStdString(folder));
    // (Every page listed and matched by name, case and all: a name's [ ] or * are not wildcards here.)
    for (const QString& f : dir.entryList({QStringLiteral("*.html")}, QDir::Files)) {
        if (!f.startsWith(base, Qt::CaseSensitive)) continue;
        const QString rest = f.mid(base.size(), f.size() - base.size() - 5);
        bool ok = true;
        const int v = rest.isEmpty() ? 1 : rest.startsWith(QStringLiteral(" v")) ? rest.mid(2).toInt(&ok) : 0;
        if (ok && v >= 1) out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    return out;
}

ReviewPackage reviewPackage(const Sequence& s, const std::string& folder, const ReviewExportOptions& o) {
    ReviewPackage p;
    const QString base = reviewBase(s);
    const QDir dir(QString::fromStdString(folder));
    const std::vector<int> before = reviewVersions(s, folder);
    const int version = o.version > 0 ? o.version : before.empty() ? 1 : before.back() + 1;
    const QString name = versionName(base, version);
    p.videoPath = dir.filePath(name + QStringLiteral(".mp4")).toStdString();
    p.pagePath = dir.filePath(name + QStringLiteral(".html")).toStdString();

    ExportSettings& st = p.settings;
    st.path = p.videoPath;
    st.videoCodec = "libx264";
    st.crf = 21;
    st.preset = "medium";
    st.pixFmt = "yuv420p";
    st.audioCodec = "aac";
    st.audioBitrate = 192000;
    st.colorSpace = "rec709";  // HDR cuts are tone mapped: review copies are watched on ordinary screens
    int w = s.width, h = s.height;
    if (o.maxHeight > 0 && h > o.maxHeight) {
        w = std::max(2, int(std::lround(double(s.width) * o.maxHeight / s.height / 2.0)) * 2);
        h = o.maxHeight;
        st.width = w;
        st.height = h;
    }
    st.in = o.in;
    st.out = o.out;
    st.burnInCaptions = std::any_of(s.captionTracks.begin(), s.captionTracks.end(),
                                    [](const CaptionTrack& t) { return t.visible && !t.captions.empty(); });
    st.burnIn.timecode = o.timecode;
    st.burnIn.text = o.watermark;
    st.burnIn.corner = 0;
    st.smartRender = false;

    p.page = reviewPageInfo(s, QFileInfo(QString::fromStdString(p.videoPath)).fileName().toStdString(), o.in, o.out, o.markers);
    p.page.width = w;
    p.page.height = h;
    p.page.note = o.note;
    p.page.version = version;
    // The latest earlier version whose page and copy are both here, to compare with.
    for (auto it = before.rbegin(); o.compare && it != before.rend(); ++it) {
        if (*it >= version) continue;
        QFile f(dir.filePath(versionName(base, *it) + QStringLiteral(".html")));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const auto old = readReviewPage(f.readAll().toStdString());
        if (!old || old->videoFile.find_first_of("/\\") != std::string::npos || !dir.exists(QString::fromStdString(old->videoFile))) continue;
        p.page.previous = ReviewVersion{*it, old->videoFile, old->fps, old->offset, old->frames};
        break;
    }
    return p;
}

bool writeReviewPage(const ReviewPackage& p, std::string* error) {
    QFile f(QString::fromStdString(p.pagePath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Cannot write " + p.pagePath;
        return false;
    }
    const std::string html = reviewPageHtml(p.page);
    if (f.write(html.data(), qint64(html.size())) != qint64(html.size())) {
        if (error) *error = "Cannot write " + p.pagePath;
        return false;
    }
    return true;
}

}  // namespace montage
