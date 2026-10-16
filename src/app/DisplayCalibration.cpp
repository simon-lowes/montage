#include "DisplayCalibration.h"

#include <QScreen>
#include <QTimer>
#include <QUrl>

#include "Settings.h"
#include "render/MonitorCalibration.h"
#include "render/Processing.h"

namespace montage {

namespace {

QString settingsKey(const QString& target) {
    // (screen names hold slashes and backslashes, which settings keys would take as groups)
    return QStringLiteral("calibration/") + QString::fromLatin1(QUrl::toPercentEncoding(target));
}

}  // namespace

DisplayCalibration& DisplayCalibration::instance() {
    static DisplayCalibration* d = new DisplayCalibration;  // (kept to the end: widgets ask while closing)
    return *d;
}

DisplayCalibration::DisplayCalibration() {
    bypassed_ = appSettings().value(QStringLiteral("calibration/bypass"), false).toBool();
    // Calibration software rewrites its LUT where it was: looked at again every second (a file's time and size).
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &DisplayCalibration::recheck);
    timer->start();
}

QString DisplayCalibration::screenTarget(const QScreen* screen, bool hdr) {
    return (hdr ? QStringLiteral("screen-hdr:") : QStringLiteral("screen:")) + (screen ? screen->name() : QString());
}

QString DisplayCalibration::outputTarget(const QString& backend, const QString& device) {
    return QStringLiteral("output:") + backend + QLatin1Char('/') + device;
}

DisplayCalibration::Entry& DisplayCalibration::entry(const QString& target) const {
    auto it = entries_.find(target);
    if (it == entries_.end()) {
        it = entries_.insert(target, Entry{appSettings().value(settingsKey(target)).toString(), nullptr, QString(), QString()});
        reload(*it);
    }
    return *it;
}

bool DisplayCalibration::reload(Entry& e) const {
    std::shared_ptr<const Lut3D> lut;
    std::string why;
    if (!e.path.isEmpty()) lut = loadCalibrationLut(e.path.toStdString(), &why);  // (the file's time and size checked)
    const QString error = lut || e.path.isEmpty() ? QString() : QString::fromStdString(why);
    // A file that cannot be read now (being written again) keeps the LUT last read from it, the error said.
    if (!lut && !e.path.isEmpty() && e.lut && e.lutPath == e.path) lut = e.lut;
    const bool changed = lut != e.lut || error != e.error;
    e.lut = std::move(lut);
    e.lutPath = e.path;
    e.error = error;
    return changed;
}

void DisplayCalibration::recheck() {
    bool changed = false;
    for (Entry& e : entries_)
        if (!e.path.isEmpty()) changed = reload(e) || changed;
    if (changed) emit this->changed();
}

QString DisplayCalibration::lutPath(const QString& target) const { return entry(target).path; }

bool DisplayCalibration::setLutPath(const QString& target, const QString& path, QString* error) {
    if (!path.isEmpty()) {
        std::string why;
        if (!loadCalibrationLut(path.toStdString(), &why)) {
            if (error) *error = QString::fromStdString(why);
            return false;
        }
    }
    QSettings s = appSettings();
    if (path.isEmpty()) s.remove(settingsKey(target));
    else s.setValue(settingsKey(target), path);
    Entry& e = entry(target);
    e.path = path;
    reload(e);
    emit changed();
    return true;
}

std::shared_ptr<const Lut3D> DisplayCalibration::lut(const QString& target) const { return bypassed_ ? nullptr : entry(target).lut; }

QString DisplayCalibration::error(const QString& target) const { return entry(target).error; }

std::shared_ptr<const Lut3D> DisplayCalibration::screenLut(const QScreen* screen, bool hdr) const {
    return screen ? lut(screenTarget(screen, hdr)) : nullptr;
}

bool DisplayCalibration::bypassed() const { return bypassed_; }

void DisplayCalibration::setBypassed(bool on) {
    if (on == bypassed_) return;
    bypassed_ = on;
    appSettings().setValue(QStringLiteral("calibration/bypass"), on);
    emit changed();
}

const QImage& CalibratedPicture::get(const QImage& src, const std::shared_ptr<const Lut3D>& lut) {
    if (!lut || src.isNull()) {
        out_ = QImage();
        lut_.reset();
        key_ = -1;
        return src;
    }
    if (src.cacheKey() != key_ || lut != lut_) {
        out_ = calibratedImage(src, *lut);
        key_ = src.cacheKey();
        lut_ = lut;
    }
    return out_;
}

}  // namespace montage
