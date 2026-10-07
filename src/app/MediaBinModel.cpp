#include "MediaBinModel.h"

#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <set>

#include "EditorState.h"
#include "Theme.h"
#include "ThumbnailCache.h"
#include "core/History.h"
#include "core/MediaLog.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

QString kindLabel(const MediaItem& m) {
    switch (m.kind) {
        case MediaKind::Video: return m.hasAudio ? QObject::tr("Video + Audio") : QObject::tr("Video");
        case MediaKind::Audio: return QObject::tr("Audio");
        case MediaKind::Image: return QObject::tr("Still Image");
        case MediaKind::Sequence: return QObject::tr("Sequence");
    }
    return {};
}

// Placeholder tile for media without a picture (audio, sequences).
QPixmap tile(const QString& text, const QColor& color) {
    QPixmap pm(MediaBinModel::kThumbW, MediaBinModel::kThumbH);
    pm.fill(color.darker(220));
    QPainter p(&pm);
    p.setPen(color.lighter(150));
    QFont f = p.font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.drawText(pm.rect(), Qt::AlignCenter, text);
    return pm;
}

bool numericField(const std::string& key) {
    const MediaField* f = mediaField(key);
    return key == "resolution" || key == "timecode" || key == "audio" || key == "transcript" ||
           (f && (f->type == FieldType::Number || f->type == FieldType::Rating || f->type == FieldType::Label));
}

}  // namespace

MediaBinModel::MediaBinModel(EditorState* state, QObject* parent) : QAbstractTableModel(parent), state_(state) {}

const std::vector<std::string>& MediaBinModel::columnKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const MediaField& f : mediaFields())
            if (f.column) k.push_back(f.key);
        return k;
    }();
    return keys;
}

int MediaBinModel::columnOf(const std::string& key) {
    const auto& keys = columnKeys();
    const auto it = std::find(keys.begin(), keys.end(), key);
    return it == keys.end() ? -1 : int(it - keys.begin());
}

void MediaBinModel::setMedia(const std::vector<Id>& ids) {
    usage_ = mediaUsage(state_->project());
    // Forget pictures of removed media (a re-imported file gets a new id).
    for (auto it = thumbs_.begin(); it != thumbs_.end();)
        it = state_->project().findMedia(it->first) ? std::next(it) : thumbs_.erase(it);
    decorated_.clear();
    if (ids == ids_) {
        if (!ids_.empty()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        return;
    }
    beginResetModel();
    ids_ = ids;
    endResetModel();
}

int MediaBinModel::rowOf(Id id) const {
    const auto it = std::find(ids_.begin(), ids_.end(), id);
    return it == ids_.end() ? -1 : int(it - ids_.begin());
}

bool MediaBinModel::setField(const std::vector<Id>& ids, const std::string& key, const QString& value) {
    const MediaField* f = mediaField(key);
    if (!f || !f->editable || ids.empty()) return false;
    const std::string v = value.toStdString();
    return state_->edit(tr("Set %1").arg(tr(f->label)), [ids, key, v](Project& p, Sequence&) {
        bool any = false;
        for (Id id : ids) {
            MediaItem* m = p.findMedia(id);
            if (!m) continue;
            MediaItem before = *m;
            if (!setMediaField(*m, key, v)) {
                *m = before;
                continue;
            }
            if (key == "name" && m->kind == MediaKind::Sequence)
                if (Sequence* s = p.findSequence(m->sequenceId)) s->name = m->name;
            any |= !(*m == before);
        }
        return any;
    });
}

void MediaBinModel::refreshThumbnails() {
    bool any = false;
    for (Id id : ids_) {
        if (thumbs_.count(id)) continue;
        const MediaItem* m = state_->project().findMedia(id);
        if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) continue;
        thumbnail(*m);
        any |= thumbs_.count(id) > 0;
    }
    if (any && !ids_.empty()) emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DecorationRole});
}

QPixmap MediaBinModel::thumbnail(const MediaItem& m) const {
    if (m.kind == MediaKind::Audio) return tile(tr("AUDIO"), theme::kAudioClip);
    if (m.kind == MediaKind::Sequence) return tile(tr("SEQUENCE"), theme::kCompoundClip);
    if (const auto it = thumbs_.find(m.id); it != thumbs_.end()) return it->second;
    const double aspect = m.width > 0 && m.height > 0 ? double(m.width) / m.height : 16.0 / 9;
    int w = kThumbW, h = int(kThumbW / aspect);
    if (h > kThumbH) {
        h = kThumbH;
        w = int(kThumbH * aspect);
    }
    const double t = m.kind == MediaKind::Video ? std::min(1.0, m.duration * 0.1) : 0.0;
    const QImage img = ThumbnailCache::instance().get(QString::fromStdString(m.path), t, std::max(2, w), std::max(2, h));
    if (img.isNull()) return tile(QString(), theme::kVideoClip);
    QPixmap pm(kThumbW, kThumbH);
    pm.fill(Qt::black);
    QPainter pa(&pm);
    pa.drawImage(QPoint((kThumbW - img.width()) / 2, (kThumbH - img.height()) / 2), img);
    pa.end();
    thumbs_[m.id] = pm;
    return pm;
}

// The thumbnail with the label as a bar along the bottom and the rating in the corner.
QPixmap MediaBinModel::decorated(const MediaItem& m) const {
    const qint64 thumb = thumbs_.count(m.id) ? thumbs_.at(m.id).cacheKey() : 0;
    if (const auto it = decorated_.find(m.id);
        it != decorated_.end() && it->second.rating == m.rating && it->second.label == m.label && it->second.thumb == thumb)
        return it->second.pixmap;
    QPixmap pm = thumbnail(m);
    if (m.rating != 0 || m.label > 0) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (m.label > 0) p.fillRect(QRect(0, kThumbH - 5, kThumbW, 5), theme::labelColor(m.label));
        if (m.rating < 0) {
            // Rejected: dimmed, with a red cross.
            p.fillRect(pm.rect(), QColor(0, 0, 0, 140));
            p.setPen(QPen(QColor(0xe0, 0x50, 0x50), 3));
            p.drawLine(QPoint(6, 6), QPoint(18, 18));
            p.drawLine(QPoint(18, 6), QPoint(6, 18));
        } else if (m.rating > 0) {
            QFont f = p.font();
            f.setPixelSize(12);
            p.setFont(f);
            const QString stars(m.rating, QChar(0x2605));
            const QRect r(4, 4, p.fontMetrics().horizontalAdvance(stars) + 6, p.fontMetrics().height() + 2);
            p.fillRect(r, QColor(0, 0, 0, 150));
            p.setPen(QColor(0xf2, 0xc2, 0x3a));
            p.drawText(r, Qt::AlignCenter, stars);
        }
    }
    decorated_[m.id] = {m.rating, m.label, thumb, pm};
    return pm;
}

QString MediaBinModel::toolTip(const MediaItem& m) const {
    const QString name = QString::fromStdString(m.name);
    QString tip = QString("<b>%1</b><br>%2").arg(name.toHtmlEscaped(), kindLabel(m));
    if (m.hasVideo && m.width > 0)
        tip += QString("<br>%1×%2 @ %3 fps, %4")
                   .arg(m.width)
                   .arg(m.height)
                   .arg(m.fps.toDouble(), 0, 'f', 3)
                   .arg(QString::fromStdString(m.videoCodec));
    if (m.hasVideo && m.kind != MediaKind::Sequence)
        tip += "<br>" + tr("Colour: %1").arg(QString::fromStdString(mediaColorSpace(m).label)) +
               (m.colorOverride.empty() ? QString() : tr(" (interpreted)"));
    if (m.hasAudio && m.sampleRate > 0)
        tip += QString("<br>%1 Hz, %2 ch, %3").arg(m.sampleRate).arg(m.channels).arg(QString::fromStdString(m.audioCodec));
    if (m.duration > 0) {
        const Rational r = m.fps.valid() ? m.fps : Rational{30, 1};
        tip += "<br>" + QString::fromStdString(formatTimecode(FrameTime(m.duration * r.toDouble()), r));
    }
    if (m.rating < 0) tip += "<br>" + tr("Rejected");
    else if (m.rating > 0) tip += "<br>" + QString(m.rating, QChar(0x2605));
    if (!m.keywords.empty()) tip += "<br>" + tr("Keywords: %1").arg(QString::fromStdString(joinKeywords(m.keywords)).toHtmlEscaped());
    if (m.transcript)
        tip += "<br>" + tr("Transcript: %n word(s)", "", int(m.transcript->wordCount())) +
               (m.transcript->language.empty() ? QString() : QStringLiteral(" (%1)").arg(QString::fromStdString(m.transcript->language)));
    if (!m.path.empty()) tip += "<br><i>" + QString::fromStdString(m.path).toHtmlEscaped() + "</i>";
    return tip;
}

int MediaBinModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(ids_.size()); }

int MediaBinModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(columnKeys().size()); }

QVariant MediaBinModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount()) return {};
    const MediaItem* m = state_->project().findMedia(ids_[size_t(index.row())]);
    if (!m) return {};
    const std::string& key = columnKeys()[size_t(index.column())];
    switch (role) {
        case Qt::DisplayRole: {
            const std::string text = mediaFieldText(*m, key, &usage_);
            if (key == "kind") return kindLabel(*m);
            if (key == "rating" && m->rating < 0) return tr("Rejected");
            if (key == "label" && m->label > 0) return tr(labelName(m->label));
            return QString::fromStdString(text);
        }
        case Qt::EditRole:
            if (key == "rating") return QString::number(m->rating);
            return QString::fromStdString(mediaFieldText(*m, key, &usage_));
        case Qt::DecorationRole:
            if (key == "name") return decorated(*m);
            if (key == "label" && m->label > 0) {
                QPixmap sw(10, 10);
                sw.fill(theme::labelColor(m->label));
                return sw;
            }
            return {};
        case Qt::ForegroundRole:
            if (m->rating < 0) return QColor(0x80, 0x80, 0x80);
            if (key == "rating" && m->rating > 0) return QColor(0xf2, 0xc2, 0x3a);
            return {};
        case Qt::ToolTipRole: return toolTip(*m);
        case SortRole:
            if (numericField(key)) return mediaFieldNumber(*m, key, &usage_);
            return QString::fromStdString(mediaFieldText(*m, key, &usage_)).toLower();
        case IdRole: return QVariant::fromValue<qulonglong>(m->id);
        default: return {};
    }
}

QVariant MediaBinModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section < 0 || section >= columnCount()) return {};
    const std::string& key = columnKeys()[size_t(section)];
    if (role == Qt::DisplayRole) return tr(mediaField(key)->label);
    if (role == KeyRole) return QString::fromStdString(key);
    return {};
}

Qt::ItemFlags MediaBinModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    const MediaField* field = mediaField(columnKeys()[size_t(index.column())]);
    if (field && field->editable) f |= Qt::ItemIsEditable;
    return f;
}

bool MediaBinModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || !index.isValid()) return false;
    return setField({mediaAt(index.row())}, columnKeys()[size_t(index.column())], value.toString());
}

QStringList MediaBinModel::mimeTypes() const { return {"application/x-montage-media"}; }

QMimeData* MediaBinModel::mimeData(const QModelIndexList& indexes) const {
    QStringList ids;
    std::set<int> rows;
    for (const QModelIndex& i : indexes)
        if (rows.insert(i.row()).second) ids << QString::number(mediaAt(i.row()));
    auto* m = new QMimeData;
    m->setData("application/x-montage-media", ids.join(',').toUtf8());
    return m;
}

}  // namespace montage
