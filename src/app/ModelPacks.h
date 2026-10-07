// Montage — getting a downloadable model (media/ModelFiles.h) onto the
// machine: asked for once, downloaded with progress, each file checked
// against its SHA-256 before it is used.
#pragma once

#include <QObject>
#include <QString>
#include <cstdint>

#include "media/ModelFiles.h"

class QWidget;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace montage {

// Downloads every file of a model pack that is missing, one after another.
class ModelPackDownload : public QObject {
    Q_OBJECT
public:
    explicit ModelPackDownload(const ModelPack& pack, QObject* parent = nullptr);
    ~ModelPackDownload() override;
    void start();
    void abort();

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString& error);

private:
    void next();
    void fail(const QString& error);

    ModelPack pack_;
    QNetworkAccessManager* net_;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    size_t index_ = 0;
    qint64 done_ = 0;  // bytes of the files already finished
    bool aborted_ = false;
};

// True when `pack` is ready, after asking (with `why`, a sentence on what it
// is for) and downloading it if needed. Blocks in a local event loop.
bool ensureModelPack(QWidget* parent, const ModelPack& pack, const QString& title, const QString& why);
// The object model, after checking that this build can run it.
bool ensureObjectModel(QWidget* parent);

}  // namespace montage
