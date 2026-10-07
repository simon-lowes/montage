#include "ObjectModel.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProgressDialog>
#include <QTimer>

#include "media/Segmenter.h"

namespace montage {

namespace {
QString megabytes(int64_t bytes) { return QLocale().toString(double(bytes) / 1e6, 'f', 0) + QStringLiteral(" MB"); }
QString fileIn(const std::string& name) {
    return QString::fromStdString(segmenterModelDirectory()) + "/" + QString::fromStdString(name);
}
}  // namespace

ObjectModelDownload::ObjectModelDownload(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {}

ObjectModelDownload::~ObjectModelDownload() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    if (file_ && file_->isOpen()) {
        file_->close();
        file_->remove();
    }
}

void ObjectModelDownload::start() {
    index_ = 0;
    done_ = 0;
    aborted_ = false;
    QMetaObject::invokeMethod(this, [this] { next(); }, Qt::QueuedConnection);
}

void ObjectModelDownload::abort() {
    aborted_ = true;
    if (reply_) reply_->abort();
}

void ObjectModelDownload::fail(const QString& error) { emit finished(false, error); }

void ObjectModelDownload::next() {
    const auto& files = segmenterFiles();
    // Files already in place (from an earlier, interrupted download) are kept.
    while (index_ < files.size() && QFileInfo(fileIn(files[index_].name)).size() == files[index_].bytes) {
        done_ += files[index_].bytes;
        ++index_;
    }
    if (index_ >= files.size()) {
        emit finished(true, {});
        return;
    }
    const SegmenterFile& f = files[index_];
    const QString dir = QString::fromStdString(segmenterModelDirectory());
    delete file_;
    file_ = new QFile(fileIn(f.name) + ".part", this);
    if (!QDir().mkpath(dir) || !file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write to %1").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    QNetworkRequest req(QUrl(QString::fromStdString(segmenterFileUrl(f.name))));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Montage/" MONTAGE_VERSION));
    reply_ = net_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (reply_ && file_) file_->write(reply_->readAll());
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64) { emit progress(done_ + got, segmenterDownloadBytes()); });
    connect(reply_, &QNetworkReply::finished, this, [this] {
        QNetworkReply* r = reply_;
        reply_ = nullptr;
        r->deleteLater();
        file_->write(r->readAll());
        file_->close();
        const SegmenterFile& f = segmenterFiles()[index_];
        const QString part = file_->fileName(), target = fileIn(f.name);
        const QVariant status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        QString err;
        if (aborted_ || r->error() == QNetworkReply::OperationCanceledError) {
            QFile::remove(part);
            emit finished(false, {});
            return;
        }
        if (r->error() != QNetworkReply::NoError) err = tr("The download failed: %1").arg(r->errorString());
        else if (status.isValid() && status.toInt() >= 400) err = tr("The download failed (HTTP %1)").arg(status.toInt());
        else if (!segmenterFileVerified(part.toStdString(), f)) err = tr("The downloaded %1 is damaged (its checksum does not match)").arg(QString::fromStdString(f.name));
        else {
            QFile::remove(target);
            if (!QFile::rename(part, target)) err = tr("Cannot save %1").arg(QDir::toNativeSeparators(target));
        }
        if (!err.isEmpty()) {
            QFile::remove(part);
            fail(err);
            return;
        }
        done_ += f.bytes;
        ++index_;
        next();
    });
}

bool ensureObjectModel(QWidget* parent) {
    if (!segmenterAvailable()) {
        QMessageBox::information(parent, QObject::tr("Object Mask"),
                                 QObject::tr("This build of Montage cannot pick objects: it was built without ONNX Runtime."));
        return false;
    }
    if (segmenterModelInstalled()) return true;
    const auto answer = QMessageBox::question(
        parent, QObject::tr("Object Mask"),
        QObject::tr("Picking objects uses EdgeTAM, a segmentation model from Meta (Apache-2.0) that runs on this computer. "
                    "It is a one-time %1 download.\n\nDownload it now?")
            .arg(megabytes(segmenterDownloadBytes())),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) return false;
    QProgressDialog dlg(QObject::tr("Downloading the object model…"), QObject::tr("Cancel"), 0, 1000, parent);
    dlg.setWindowTitle(QObject::tr("Object Mask"));
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    ObjectModelDownload download;
    QEventLoop loop;
    bool ok = false;
    QString error;
    QObject::connect(&download, &ObjectModelDownload::progress, &dlg, [&dlg](qint64 got, qint64 total) {
        if (total > 0) dlg.setValue(int(1000 * got / total));
        dlg.setLabelText(QObject::tr("Downloading the object model: %1 of %2").arg(megabytes(got), megabytes(total)));
    });
    QObject::connect(&dlg, &QProgressDialog::canceled, &download, &ObjectModelDownload::abort);
    QObject::connect(&download, &ObjectModelDownload::finished, &loop, [&](bool success, const QString& err) {
        ok = success;
        error = err;
        loop.quit();
    });
    download.start();
    loop.exec();
    dlg.disconnect();  // closing a progress dialog emits canceled()
    for (QTimer* t : dlg.findChildren<QTimer*>()) t->stop();
    dlg.close();
    if (!ok && !error.isEmpty()) QMessageBox::warning(parent, QObject::tr("Object Mask"), error);
    return ok && segmenterModelInstalled();
}

}  // namespace montage
