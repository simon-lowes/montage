#include "ModelPacks.h"

#include <QApplication>
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

#include "media/LensDatabase.h"
#include "media/Segmenter.h"
#include "media/DepthMap.h"
#include "media/VideoDepth.h"
#include "media/Matting.h"
#include "media/Inpaint.h"
#include "media/Faces.h"
#include "media/FaceMesh.h"
#include "media/Rife.h"
#include "media/TextToSpeech.h"
#include "media/VoiceClone.h"
#include "media/LipSync.h"
#include "media/MotionDeblur.h"
#include "media/SuperScale.h"
#include "media/SpeechEnhance.h"
#include "media/BandwidthExtension.h"
#include "media/MediaPool.h"
#include "audio/SpeechCleanup.h"

namespace montage {

namespace {
QString megabytes(int64_t bytes) { return QLocale().toString(double(bytes) / 1e6, 'f', 0) + QStringLiteral(" MB"); }
}  // namespace

ModelPackDownload::ModelPackDownload(const ModelPack& pack, QObject* parent)
    : QObject(parent), pack_(pack), net_(new QNetworkAccessManager(this)) {}

ModelPackDownload::~ModelPackDownload() {
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

void ModelPackDownload::start() {
    index_ = 0;
    done_ = 0;
    aborted_ = false;
    QMetaObject::invokeMethod(this, [this] { next(); }, Qt::QueuedConnection);
}

void ModelPackDownload::abort() {
    aborted_ = true;
    if (reply_) reply_->abort();
}

void ModelPackDownload::fail(const QString& error) { emit finished(false, error); }

void ModelPackDownload::next() {
    const auto& files = pack_.files;
    auto fileIn = [this](const ModelFile& f) { return QString::fromStdString(pack_.path(f)); };
    // Files already in place (from an earlier, interrupted download) are kept.
    while (index_ < files.size() && QFileInfo(fileIn(files[index_])).size() == files[index_].bytes) {
        done_ += files[index_].bytes;
        ++index_;
    }
    if (index_ >= files.size()) {
        emit finished(true, {});
        return;
    }
    const ModelFile& f = files[index_];
    const QString dir = QString::fromStdString(pack_.directory());
    delete file_;
    file_ = new QFile(fileIn(f) + ".part", this);
    if (!QDir().mkpath(dir) || !file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write to %1").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    QNetworkRequest req(QUrl(QString::fromStdString(pack_.url(f))));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Montage/" MONTAGE_VERSION));
    reply_ = net_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (reply_ && file_) file_->write(reply_->readAll());
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64) { emit progress(done_ + got, pack_.bytes()); });
    connect(reply_, &QNetworkReply::finished, this, [this, fileIn] {
        QNetworkReply* r = reply_;
        reply_ = nullptr;
        r->deleteLater();
        file_->write(r->readAll());
        file_->close();
        const ModelFile& f = pack_.files[index_];
        const QString part = file_->fileName(), target = fileIn(f);
        const QVariant status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        QString err;
        if (aborted_ || r->error() == QNetworkReply::OperationCanceledError) {
            QFile::remove(part);
            emit finished(false, {});
            return;
        }
        if (r->error() != QNetworkReply::NoError) err = tr("The download failed: %1").arg(r->errorString());
        else if (status.isValid() && status.toInt() >= 400) err = tr("The download failed (HTTP %1)").arg(status.toInt());
        else if (!modelFileVerified(part.toStdString(), f)) err = tr("The downloaded %1 is damaged (its checksum does not match)").arg(QString::fromStdString(f.name));
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

bool ensureModelPack(QWidget* parent, const ModelPack& pack, const QString& title, const QString& why) {
    if (pack.installed()) return true;
    const auto answer = QMessageBox::question(
        parent, title, QObject::tr("%1 It is a one-time %2 download.\n\nDownload it now?").arg(why, megabytes(pack.bytes())),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) return false;
    const QString name = QString::fromStdString(pack.title);
    QProgressDialog dlg(QObject::tr("Downloading the %1…").arg(name), QObject::tr("Cancel"), 0, 1000, parent);
    dlg.setWindowTitle(title);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    ModelPackDownload download(pack);
    QEventLoop loop;
    bool ok = false;
    QString error;
    QObject::connect(&download, &ModelPackDownload::progress, &dlg, [&dlg, name](qint64 got, qint64 total) {
        if (total > 0) dlg.setValue(int(1000 * got / total));
        dlg.setLabelText(QObject::tr("Downloading the %1: %2 of %3").arg(name, megabytes(got), megabytes(total)));
    });
    QObject::connect(&dlg, &QProgressDialog::canceled, &download, &ModelPackDownload::abort);
    QObject::connect(&download, &ModelPackDownload::finished, &loop, [&](bool success, const QString& err) {
        ok = success;
        error = err;
        loop.quit();
    });
    download.start();
    loop.exec();
    dlg.disconnect();  // closing a progress dialog emits canceled()
    for (QTimer* t : dlg.findChildren<QTimer*>()) t->stop();
    dlg.close();
    if (!ok && !error.isEmpty()) QMessageBox::warning(parent, title, error);
    if (ok && pack.installed()) retryCleanupsMissingModels();  // (sound left as it was for want of it)
    return ok && pack.installed();
}

bool ensureObjectModel(QWidget* parent) {
    if (!segmenterAvailable()) {
        QMessageBox::information(parent, QObject::tr("Object Mask"),
                                 QObject::tr("This build of Montage cannot pick objects: it was built without ONNX Runtime."));
        return false;
    }
    return ensureModelPack(parent, objectModel(), QObject::tr("Object Mask"),
                           QObject::tr("Picking objects uses EdgeTAM, a segmentation model from Meta (Apache-2.0) that runs on this computer."));
}

bool ensureVoiceCloneModel(QWidget* parent) {
    if (!voiceCloneAvailable()) {
        QMessageBox::information(parent, QObject::tr("Cloned Voice"),
                                 QObject::tr("This build of Montage cannot clone voices: it was built without ONNX Runtime."));
        return false;
    }
    return ensureModelPack(parent, voiceCloneModel(), QObject::tr("Cloned Voice"),
                           QObject::tr("Speaking in a cloned voice uses Chatterbox Multilingual (Resemble AI, MIT), which runs on this "
                                       "computer. Only clone a voice with its owner's consent: every file it makes notes that it is "
                                       "generated, and from whose voice."));
}

bool ensureLipSyncModels(QWidget* parent) {
    const QString title = QObject::tr("Lip Sync");
    if (!lipSyncAvailable() || !faceSearchAvailable() || !faceMeshAvailable()) {
        QMessageBox::information(parent, title, QObject::tr("This build of Montage cannot use Lip Sync: it was built without ONNX Runtime."));
        return false;
    }
    if (!ensureModelPack(parent, faceModel(), title,
                         QObject::tr("Lip Sync finds faces with YuNet and SFace (MIT and Apache-2.0), which run on this computer.")))
        return false;
    if (!ensureModelPack(parent, faceMeshModel(), title,
                         QObject::tr("Lip Sync fits a mesh of 478 points to each face with MediaPipe's Face Mesh V2 (Apache-2.0), "
                                     "which runs on this computer.")))
        return false;
    return ensureModelPack(parent, lipSyncModel(), title,
                           QObject::tr("Lip Sync redraws the speaker's mouth to say another track's sound (a dub) with MuseTalk 1.5 "
                                       "(MIT), whisper-tiny (Apache-2.0) and sd-vae-ft-mse (MIT), which run on this computer. "
                                       "Signed exports' Content Credentials name it as AI-generated. It takes about 4 s a frame "
                                       "on four cores, so the monitors show it once the frames are rendered (Sequence › Render In to Out); "
                                       "exports always include it."));
}

bool ensureEffectModel(QWidget* parent, const std::string& type) {
    if (type == "lens_correction") {
        if (lensfunDatabaseFound()) return true;
        const bool ok = ensureModelPack(parent, lensDatabasePack(), QObject::tr("Lens Correction"),
                                        QObject::tr("Lens Correction uses lensfun's database of lens profiles (CC BY-SA 3.0)."));
        reloadLensDatabase();
        // (Lenses the user calibrated serve without it.)
        return (ok || lensDatabase()) && lensDatabase();
    }
    if (type == "super_scale") {
        if (!upscalerAvailable()) {
            QMessageBox::information(parent, QObject::tr("Super Scale"),
                                     QObject::tr("This build of Montage cannot use Super Scale: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, upscaleModel(), QObject::tr("Super Scale"),
                               QObject::tr("Super Scale uses Real-ESRGAN (BSD-3-Clause), an image model that runs on this computer."));
    }
    if (type == "motion_deblur") {
        if (!deblurAvailable()) {
            QMessageBox::information(parent, QObject::tr("Motion Deblur"),
                                     QObject::tr("This build of Montage cannot use Motion Deblur: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, deblurModel(), QObject::tr("Motion Deblur"),
                               QObject::tr("Motion Deblur uses NAFNet (MIT, trained on the GoPro dataset, CC BY 4.0), an image model that "
                                           "runs on this computer. It takes seconds a frame, so the monitors show it once the frames are rendered "
                                           "(Sequence › Render In to Out); exports always include it."));
    }
    if (type == "depth_blur" || type == "depth_fog" || type == "depth_map" || type == "relight" || type == "mask.depth") {
        if (!depthAvailable()) {
            QMessageBox::information(parent, QObject::tr("Depth"),
                                     QObject::tr("This build of Montage cannot see depth: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, depthModel(), QObject::tr("Depth"),
                               QObject::tr("Depth effects use Depth Anything V2 Small (Apache-2.0), a model that works out how near each "
                                           "part of the picture is. It runs on this computer."));
    }
    if (type == "video_depth") {
        if (!videoDepthAvailable()) {
            QMessageBox::information(parent, QObject::tr("Steady Depth"),
                                     QObject::tr("This build of Montage cannot see depth: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, videoDepthModel(), QObject::tr("Steady Depth"),
                               QObject::tr("Steady depth uses Video Depth Anything Small (Apache-2.0), a model that works out how near "
                                           "each part of the picture is from 32 frames at a time, so depth holds still as the camera moves. "
                                           "It runs on this computer; a clip is analysed once and kept."));
    }
    if (type == "object_removal") {
        if (!inpaintAvailable()) {
            QMessageBox::information(parent, QObject::tr("Object Removal"),
                                     QObject::tr("This build of Montage cannot remove objects: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, inpaintModel(), QObject::tr("Object Removal"),
                               QObject::tr("Object Removal fills what you mask with LaMa (Apache-2.0), an inpainting model that runs on "
                                           "this computer."));
    }
    if (type == "face_refine" || type == "blemish_remover" || type == "redact_faces" || type == "focus.face") {
        const QString title = type == "redact_faces" ? QObject::tr("Redact Faces")
                              : type == "focus.face" ? QObject::tr("Nearest Face")
                                                     : QObject::tr("Face Refinement");
        if (!faceSearchAvailable()) {
            QMessageBox::information(parent, title, QObject::tr("This build of Montage cannot find faces: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, faceModel(), title,
                               QObject::tr("Face Refinement, Blemish Remover, Redact Faces and Lens Blur's Nearest Face find faces with "
                                           "YuNet and SFace (MIT and Apache-2.0), which run on this computer. Without them no face "
                                           "is found."));
    }
    if (type == "face_shape" || type == "look_at_lens") {
        const QString title = type == "face_shape" ? QObject::tr("Face Shape") : QObject::tr("Look at Lens");
        if (!faceSearchAvailable() || !faceMeshAvailable()) {
            QMessageBox::information(parent, title, QObject::tr("This build of Montage cannot find faces: it was built without ONNX Runtime."));
            return false;
        }
        if (!ensureModelPack(parent, faceModel(), title,
                             QObject::tr("%1 finds faces with YuNet and SFace (MIT and Apache-2.0), which run on this computer.").arg(title)))
            return false;
        return ensureModelPack(parent, faceMeshModel(), title,
                               QObject::tr("%1 fits a mesh of 478 points (the irises with it) to each face with MediaPipe's "
                                           "Face Mesh V2 (Apache-2.0), which runs on this computer.")
                                   .arg(title));
    }
    if (type == "lip_sync") {
        const QString title = QObject::tr("Lip Sync");
        const auto answer = QMessageBox::question(
            parent, title,
            QObject::tr("Lip Sync redraws the mouth of the person shown to say what another track says. Do you have their consent?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return false;
        return ensureLipSyncModels(parent);
    }
    if (type == "remove_background" || type == "mask.people" || type == "behind_people") {
        if (!mattingAvailable()) {
            QMessageBox::information(parent, QObject::tr("Remove Background"),
                                     QObject::tr("This build of Montage cannot find people: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, mattingModel(), QObject::tr("Remove Background"),
                               QObject::tr("Cutting people out uses MODNet (Apache-2.0), a portrait matting model that runs on this computer."));
    }
    if (type == "tts") {
        if (!ttsAvailable()) return false;
        return ensureModelPack(parent, ttsModel(), QObject::tr("Generate Voiceover"),
                               QObject::tr("Voiceovers from text use Kokoro (Apache-2.0), which runs on this computer."));
    }
    if (type == "rife") {
        if (!rifeAvailable()) {
            QMessageBox::information(parent, QObject::tr("AI Frames"),
                                     QObject::tr("This build of Montage cannot make AI frames: it was built without ONNX Runtime."));
            return false;
        }
        return ensureModelPack(parent, rifeModel(), QObject::tr("AI Frames"),
                               QObject::tr("AI frames for slow motion use RIFE 4.26 (MIT), downloaded from its authors' release. "
                                           "It runs on this computer."));
    }
    if (type == "bandwidth_extension") {
        if (bandwidthExtensionAvailable()) return true;  // (its models depend on the clip: ensureBandwidthModels)
        QMessageBox::information(parent, QObject::tr("Bandwidth Extension"),
                                 QObject::tr("This build of Montage cannot restore speech: it was built without ONNX Runtime."));
        return false;
    }
    if (type != "enhance_speech") return true;
    if (!speechEnhancerAvailable()) {
        QMessageBox::information(parent, QObject::tr("Enhance Speech"),
                                 QObject::tr("This build of Montage cannot enhance speech: it was built without ONNX Runtime."));
        return false;
    }
    return ensureModelPack(parent, speechModel(), QObject::tr("Enhance Speech"),
                           QObject::tr("Enhance Speech uses DeepFilterNet3 (MIT or Apache-2.0), a speech model that runs on this computer."));
}

bool ensureBandwidthModels(QWidget* parent, const Project& p, const Sequence& s, const Clip& c) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || m->path.empty() || !bandwidthExtensionAvailable()) return true;
    const Effect* e = nullptr;
    for (const Effect& x : c.effects)
        if (x.type == "bandwidth_extension") e = &x;
    Effect probe;
    probe.type = "bandwidth_extension";
    if (!bandwidthExtensionApplies(e ? *e : probe, c)) return true;  // (a Music or Effects clip is left alone)
    AudioBufferPtr sound;
    {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        sound = MediaPool::instance().audio(audioKey(m->path, c.channels), s.sampleRate);  // as the clip plays it
        QApplication::restoreOverrideCursor();
    }
    if (!sound) return true;
    const BweResult plan = bandwidthPlanFor(*sound, e ? e->p("cutoff_hz", 0, 0) : 0);
    for (const BweCheckpoint& k : plan.plan)
        if (!ensureModelPack(parent, bweModel(k), QObject::tr("Bandwidth Extension"),
                             QObject::tr("The speech in this clip stops at %1 kHz. Bandwidth Extension rebuilds what is above with "
                                         "AP-BWE (MIT), which runs on this computer, using its %2 to %3 kHz model.")
                                 .arg(plan.cutoff / 1000, 0, 'f', 1)
                                 .arg(k.low / 1000)
                                 .arg(k.high / 1000)))
            return false;
    return true;
}

}  // namespace montage
