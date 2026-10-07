// Montage — asynchronous thumbnail generation for the media bin and timeline.
#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>

namespace montage {

class ThumbnailCache : public QObject {
    Q_OBJECT
public:
    static ThumbnailCache& instance();

    // Returns the thumbnail if cached, otherwise a null image and schedules
    // decoding; ready() fires when new thumbnails arrive. `seconds` is
    // quantised to `quantum` to keep the cache small.
    QImage get(const QString& path, double seconds, int width, int height, double quantum = 0.0);

signals:
    void ready();

private:
    ThumbnailCache();
    QMutex m_;
    QHash<QString, QImage> cache_;
    QSet<QString> pending_;
    QList<QString> order_;
    QThreadPool pool_;
    bool notifyQueued_ = false;
};

}  // namespace montage
