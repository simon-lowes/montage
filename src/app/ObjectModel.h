// Montage — getting the object model (media/Segmenter.h) onto the machine:
// asked for once, downloaded with progress, each file checked against its
// SHA-256 before it is used.
#pragma once

#include <QObject>
#include <QString>
#include <cstdint>

class QWidget;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace montage {

// Downloads every file of the model that is missing, one after another.
class ObjectModelDownload : public QObject {
    Q_OBJECT
public:
    explicit ObjectModelDownload(QObject* parent = nullptr);
    ~ObjectModelDownload() override;
    void start();
    void abort();

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString& error);

private:
    void next();
    void fail(const QString& error);

    QNetworkAccessManager* net_;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    size_t index_ = 0;
    qint64 done_ = 0;  // bytes of the files already finished
    bool aborted_ = false;
};

// True when the model is ready, after asking to download it (and doing so)
// if needed. Blocks in a local event loop while downloading.
bool ensureObjectModel(QWidget* parent);

}  // namespace montage
