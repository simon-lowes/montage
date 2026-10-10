#include "ReviewExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <cmath>

namespace montage {

ReviewPackage reviewPackage(const Sequence& s, const std::string& folder, const ReviewExportOptions& o) {
    ReviewPackage p;
    QString base = QString::fromStdString(s.name.empty() ? std::string("Sequence") : s.name);
    for (QChar& c : base)
        if (QStringLiteral("\\/:*?\"<>|").contains(c)) c = '-';
    const QDir dir(QString::fromStdString(folder));
    p.videoPath = dir.filePath(base + QStringLiteral(" - Review.mp4")).toStdString();
    p.pagePath = dir.filePath(base + QStringLiteral(" - Review.html")).toStdString();

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
