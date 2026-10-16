// Montage — which calibration LUT (render/MonitorCalibration.h) each display gets: one per screen for the viewers and
// full-screen output (by the screen's name), one per screen for the HDR viewer (made for PQ), and one per reference
// output device. Kept in the app's settings; applied by whatever draws to that display, last, and never to renders or
// exports. Bypass turns them all off at once (to compare with the display uncorrected).
#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <memory>

class QScreen;

namespace montage {

struct Lut3D;

class DisplayCalibration : public QObject {
    Q_OBJECT
public:
    static DisplayCalibration& instance();

    // Targets: "screen:<name>", "screen-hdr:<name>" and "output:<interface>/<device id>".
    static QString screenTarget(const QScreen* screen, bool hdr = false);
    static QString outputTarget(const QString& backend, const QString& device);

    QString lutPath(const QString& target) const;
    // Sets the target's LUT (an empty path clears it); false and why when it cannot be read.
    bool setLutPath(const QString& target, const QString& path, QString* error = nullptr);
    // The target's LUT (kept, and read again within a second of its file changing, which says changed()); null when it
    // has none, it cannot be read, or bypassed.
    std::shared_ptr<const Lut3D> lut(const QString& target) const;
    // Why the target's LUT cannot be read (empty when it can, or has none).
    QString error(const QString& target) const;
    std::shared_ptr<const Lut3D> screenLut(const QScreen* screen, bool hdr = false) const;

    bool bypassed() const;
    void setBypassed(bool on);

signals:
    void changed();

private:
    DisplayCalibration();
    struct Entry {
        QString path;
        std::shared_ptr<const Lut3D> lut;
        QString lutPath;  // the file it was read from
        QString error;
    };
    Entry& entry(const QString& target) const;
    bool reload(Entry& e) const;  // true when its LUT changed
    void recheck();
    mutable QHash<QString, Entry> entries_;
    bool bypassed_ = false;
};

// A picture through a LUT, made again only when the picture or the LUT changes.
class CalibratedPicture {
public:
    const QImage& get(const QImage& src, const std::shared_ptr<const Lut3D>& lut);

private:
    qint64 key_ = -1;
    std::shared_ptr<const Lut3D> lut_;
    QImage out_;
};

}  // namespace montage
