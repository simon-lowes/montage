#include "media/ImageSequence.h"
#include "media/Psd.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>
#include <cstdio>
#include <set>

namespace montage {

namespace {

constexpr char kMark = '\x1e';  // never in a file name

// The file name split round its last number: prefix, digits, extension (with its dot).
bool splitNumbered(const QString& name, QString& prefix, QString& digits, QString& ext) {
    static const QRegularExpression re(QStringLiteral("^(.*?)(\\d+)(\\.[A-Za-z0-9]+)$"));
    const auto m = re.match(name);
    if (!m.hasMatch()) return false;
    prefix = m.captured(1);
    digits = m.captured(2);
    ext = m.captured(3);
    return true;
}

}  // namespace

bool isImageSequencePath(const std::string& path) { return path.find(kMark) != std::string::npos; }

bool parseImageSequencePath(const std::string& path, ImageSequence& out) {
    const size_t mark = path.find(kMark);
    if (mark == std::string::npos) return false;
    int num = 0, den = 0, first = 0;
    if (std::sscanf(path.c_str() + mark + 1, "%d,%d:%d", &num, &den, &first) != 3 || num <= 0 || den <= 0) return false;
    out.pattern = path.substr(0, mark);
    out.fps = Rational{num, den};
    out.first = first;
    out.last = first;
    const size_t dash = path.find('-', mark);
    if (dash != std::string::npos) out.last = std::max(first, std::atoi(path.c_str() + dash + 1));
    return true;
}

std::string imageSequencePath(const ImageSequence& s) {
    return s.pattern + kMark + std::to_string(s.fps.num) + ',' + std::to_string(s.fps.den) + ':' + std::to_string(s.first) + '-' +
           std::to_string(s.last);
}

std::string imageSequenceFrame(const ImageSequence& s, int n) {
    char buf[4096];
    std::snprintf(buf, sizeof buf, s.pattern.c_str(), n);
    return buf;
}

std::string imageSequenceName(const ImageSequence& s) {
    const QString file = QFileInfo(QString::fromStdString(s.pattern)).fileName();
    static const QRegularExpression spec(QStringLiteral("%0?\\d*d"));
    const auto m = spec.match(file);
    if (!m.hasMatch()) return file.toStdString();
    const std::string f = m.captured(0).toStdString();
    char a[64], b[64];
    std::snprintf(a, sizeof a, f.c_str(), s.first);
    std::snprintf(b, sizeof b, f.c_str(), s.last);
    return (file.left(m.capturedStart()) + QStringLiteral("[%1-%2]").arg(a, b) + file.mid(m.capturedEnd())).toStdString();
}

std::string mediaFileOnDisk(const std::string& path) {
    ImageSequence s;
    if (parseImageSequencePath(path, s)) return imageSequenceFrame(s, s.first);
    std::string file;
    int layer = 0;
    return parsePsdLayerPath(path, file, layer) ? file : path;
}

bool detectImageSequence(const std::string& file, ImageSequence& out) {
    const QFileInfo fi(QString::fromStdString(file));
    QString prefix, digits, ext;
    if (!fi.exists() || !splitNumbered(fi.fileName(), prefix, digits, ext)) return false;
    const bool padded = digits.size() > 1 && digits.startsWith(QLatin1Char('0'));
    const int width = int(digits.size());
    // The other files written the same way.
    std::set<int> numbers;
    for (const QString& other : QDir(fi.absolutePath()).entryList(QDir::Files)) {
        QString p, d, e;
        if (!splitNumbered(other, p, d, e) || p != prefix || e.compare(ext, Qt::CaseInsensitive) != 0) continue;
        if (padded ? d.size() < width || (d.size() > width && d.startsWith(QLatin1Char('0'))) : (d.size() > 1 && d.startsWith(QLatin1Char('0'))))
            continue;
        numbers.insert(d.toInt());
    }
    const int n = digits.toInt();
    int first = n, last = n;
    while (numbers.count(first - 1)) --first;
    while (numbers.count(last + 1)) ++last;
    if (last == first) return false;
    out.pattern = QDir(fi.absolutePath()).filePath(prefix + (padded ? QStringLiteral("%0") + QString::number(width) + QLatin1Char('d') : QStringLiteral("%d")) + ext).toStdString();
    out.first = first;
    out.last = last;
    return true;
}

namespace edit {

Result setImageSequenceRate(Project& p, Id media, Rational fps) {
    MediaItem* m = p.findMedia(media);
    ImageSequence seq;
    if (!m || !parseImageSequencePath(m->path, seq)) return Result::fail("Only image sequences take a frame rate");
    if (!fps.valid() || fps.toDouble() <= 0 || fps.toDouble() > 1000) return Result::fail("Not a frame rate");
    if (seq.fps == fps) return Result::fail("");
    const double ratio = seq.fps.toDouble() / fps.toDouble();  // media seconds now per media second before
    seq.fps = fps;
    m->path = imageSequencePath(seq);
    m->fps = fps;
    m->duration = seq.frames() / fps.toDouble();
    for (Sequence& s : p.sequences)
        for (TrackRef r : allTracks(s))
            for (Clip& c : trackAt(s, r)->clips) {
                if (c.mediaId != media) continue;
                c.sourceIn *= ratio;  // the same frame
                const double available = m->duration * s.fpsValue() - c.sourceIn;
                if (c.sourceExtent() > available) c.duration = std::max<FrameTime>(1, FrameTime(std::floor(available / c.speed)));
            }
    for (MediaItem& sub : p.media)
        if (sub.subclipOf == media) sub.subclipIn *= ratio, sub.subclipOut *= ratio;
    return {};
}

}  // namespace edit

Rational rateFor(double fps) {
    if (std::fabs(fps - std::round(fps)) < 1e-6) return Rational{int(std::lround(fps)), 1};
    if (const double ntsc = fps * 1001 / 1000; std::fabs(ntsc - std::round(ntsc)) < 0.002) return Rational{int(std::lround(ntsc)) * 1000, 1001};
    return Rational{int(std::lround(fps * 1000)), 1000};
}

bool isFrameFormat(const std::string& file) {
    static const QStringList exts = {"exr", "dpx", "png", "tif", "tiff", "tga", "bmp", "cin", "sgi"};
    return exts.contains(QFileInfo(QString::fromStdString(file)).suffix().toLower());
}

}  // namespace montage
