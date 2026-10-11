// Montage — speech-to-text for media in the bin: choose a model and language,
// download the model on first use, transcribe in the background and keep the
// word-timed transcript on the media item.
#pragma once

#include <QDialog>
#include <QObject>

#include "core/Model.h"
#include "media/Transcriber.h"

class QCheckBox;
class QComboBox;
class QFile;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;

namespace montage {

class EditorState;

// Downloads a whisper model into whisperModelsDirectory(), through a ".part"
// file renamed only once the download is complete and looks like a model.
class ModelDownload : public QObject {
    Q_OBJECT
public:
    explicit ModelDownload(QObject* parent = nullptr);
    ~ModelDownload() override;
    void start(const std::string& model);
    void abort();
    bool running() const { return reply_ != nullptr; }

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString& error);

private:
    void done(bool ok, const QString& error);

    QNetworkAccessManager* net_;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    QString target_;
};

class TranscribeDialog : public QDialog {
    Q_OBJECT
public:
    explicit TranscribeDialog(int mediaCount, QWidget* parent = nullptr);
    TranscribeOptions options() const;

private:
    void updateState();

    QComboBox* model_;
    QComboBox* language_;
    QCheckBox* translate_;
    QCheckBox* speakers_;
    QComboBox* speakerCount_;
    QLabel* note_;
};

// Transcribes the media (downloading the model first if needed) behind a
// progress dialog and stores the transcripts as one undoable edit.
void startTranscription(EditorState* state, const std::vector<Id>& media, const TranscribeOptions& options,
                        QWidget* parent);

// Asks for a file and writes the media's transcript as SRT, WebVTT, text or JSON.
void exportTranscript(const MediaItem& media, QWidget* parent);

}  // namespace montage
