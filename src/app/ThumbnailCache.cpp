#include "ThumbnailCache.h"

#include <QMutexLocker>
#include <QTimer>
#include <cmath>

#include "media/MediaPool.h"

namespace montage {

ThumbnailCache& ThumbnailCache::instance() {
    static ThumbnailCache* cache = new ThumbnailCache;  // lives for the whole run
    return *cache;
}

ThumbnailCache::ThumbnailCache() { pool_.setMaxThreadCount(2); }

void ThumbnailCache::forget(const QString& path) {
    const QString prefix = path + '|';
    QMutexLocker lock(&m_);
    for (auto it = cache_.begin(); it != cache_.end();) it = it.key().startsWith(prefix) ? cache_.erase(it) : std::next(it);
    order_.removeIf([&](const QString& k) { return k.startsWith(prefix); });
}

QImage ThumbnailCache::get(const QString& path, double seconds, int width, int height, double quantum) {
    if (path.isEmpty() || width <= 0 || height <= 0) return {};
    if (quantum > 0) seconds = std::floor(seconds / quantum) * quantum;
    QString key = QStringLiteral("%1|%2|%3x%4").arg(path).arg(qint64(std::llround(seconds * 1000))).arg(width).arg(height);
    QMutexLocker lock(&m_);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it.value();
    if (pending_.contains(key)) return {};
    pending_.insert(key);
    pool_.start([this, key, path, seconds, width, height] {
        Frame16Ptr f = MediaPool::instance().videoFrame(path.toStdString(), seconds, width, height);
        QImage img;
        if (f) {
            img = QImage(f->width, f->height, QImage::Format_RGBA8888);
            for (int y = 0; y < f->height; ++y) {
                uchar* d = img.scanLine(y);
                const uint16_t* s = f->px.data() + size_t(y) * size_t(f->width) * 4;
                for (int x = 0; x < f->width * 4; ++x) d[x] = uchar(s[x] >> 8);
            }
        }
        QMutexLocker l(&m_);
        pending_.remove(key);
        cache_.insert(key, img);
        order_.append(key);
        while (order_.size() > 4000) cache_.remove(order_.takeFirst());
        if (!notifyQueued_) {
            notifyQueued_ = true;
            QMetaObject::invokeMethod(this, [this] {
                {
                    QMutexLocker l2(&m_);
                    notifyQueued_ = false;
                }
                emit ready();
            }, Qt::QueuedConnection);
        }
    });
    return {};
}

}  // namespace montage
