#include "PatchGenerator.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QTcpSocket>
#include <QTimer>

#include "DisplayCalibration.h"
#include "ReferenceOutputPanel.h"
#include "render/Processing.h"

namespace montage {

PatchWindow::PatchWindow(QWidget* parent) : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("calibrationPatch"));
    setWindowTitle(tr("Montage — Calibration Patch"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::BlankCursor);
}

void PatchWindow::setPatch(const CalibrationPatch& patch, std::shared_ptr<const Lut3D> lut) {
    patch_ = patch;
    lut_ = std::move(lut);
    has_ = true;
    update();
}

QImage PatchWindow::picture() const {
    const qreal dpr = devicePixelRatioF();
    const int w = std::max(1, int(std::lround(width() * dpr))), h = std::max(1, int(std::lround(height() * dpr)));
    if (!has_) {
        QImage black(w, h, QImage::Format_RGBA8888);
        black.fill(Qt::black);
        return black;
    }
    QImage img = patchImage(patch_, w, h, lut_.get());
    img.setDevicePixelRatio(dpr);
    return img;
}

void PatchWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.drawImage(rect(), picture());  // (drawn pixel for pixel: no smoothing to blend the edges)
}

void PatchWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) close();
    else QWidget::keyPressEvent(e);
}

void PatchWindow::closeEvent(QCloseEvent* e) {
    emit closed();
    QWidget::closeEvent(e);
}

// ---------------------------------------------------------------------------

PatchGenerator::PatchGenerator(ReferenceOutputController* output, QWidget* parentWindow, QObject* parent)
    : QObject(parent), output_(output), parentWindow_(parentWindow) {
    retry_ = new QTimer(this);
    retry_->setInterval(1000);
    connect(retry_, &QTimer::timeout, this, &PatchGenerator::connectNow);
}

PatchGenerator::~PatchGenerator() {
    stop();
    delete window_.data();
}

void PatchGenerator::setScreen(QScreen* screen) { screen_ = screen; }
void PatchGenerator::setReferenceOutput(bool on) { toOutput_ = on; }
void PatchGenerator::setThroughCalibration(bool on) { through_ = on; }

bool PatchGenerator::start(const QString& host, int port, QString* error) {
    stop();
    if (toOutput_ && (!output_ || !output_->running())) {
        if (error) *error = tr("Start the reference output first (Playback › Video Output › Reference Output…)");
        return false;
    }
    host_ = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    port_ = port;
    active_ = true;
    count_ = 0;
    error_.clear();
    stream_ = PatchStream();
    if (!toOutput_) {
        // Black until the first patch, filling the screen.
        if (!window_) {
            window_ = new PatchWindow(parentWindow_);
            connect(window_, &PatchWindow::closed, this, [this] {
                if (active_) stop();
            });
        }
        QScreen* s = screen_ ? screen_.data() : parentWindow_ ? parentWindow_->screen() : QGuiApplication::primaryScreen();
        if (s) {
            window_->setScreen(s);
            window_->setGeometry(s->geometry());
        }
        window_->showFullScreen();
        window_->raise();
    } else if (output_ && output_->scheduler()) {
        const output::Mode& m = output_->scheduler()->mode();
        Image black(m.width, m.height, Image::Uninitialized{});
        black.fill(0, 0, 0, 1);
        output_->scheduler()->setPatch(std::make_shared<const Image>(std::move(black)));
    }
    connectNow();
    emit statusChanged();
    return true;
}

void PatchGenerator::stop() {
    const bool was = active_;
    active_ = false;
    retry_->stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    clearShown();
    if (was) emit statusChanged();
}

void PatchGenerator::clearShown() {
    if (window_ && window_->isVisible()) window_->hide();
    if (output_ && output_->scheduler()) output_->scheduler()->setPatch(nullptr);
}

bool PatchGenerator::connected() const { return socket_ && socket_->state() == QAbstractSocket::ConnectedState; }

void PatchGenerator::connectNow() {
    if (!active_ || connected() || (socket_ && socket_->state() != QAbstractSocket::UnconnectedState)) return;
    if (socket_) socket_->deleteLater();
    socket_ = new QTcpSocket(this);
    QTcpSocket* sock = socket_;
    connect(sock, &QTcpSocket::connected, this, [this] {
        retry_->stop();
        error_.clear();
        emit statusChanged();
    });
    connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
        const QByteArray data = sock->readAll();
        for (const CalibrationPatch& p : stream_.feed(data.constData(), size_t(data.size()))) show(p);
        if (!stream_.error().empty()) error_ = QString::fromStdString(stream_.error());
        // Out of step: connected again, to start the stream afresh.
        if (stream_.broken()) sock->abort();
    });
    // Gone or not there yet: tried again shortly.
    auto again = [this, sock] {
        if (sock != socket_ || !active_) return;
        if (sock->error() != QAbstractSocket::UnknownSocketError && sock->error() != QAbstractSocket::RemoteHostClosedError)
            error_ = sock->errorString();
        stream_ = PatchStream();
        if (!retry_->isActive()) retry_->start();
        emit statusChanged();
    };
    connect(sock, &QTcpSocket::disconnected, this, again);
    connect(sock, &QTcpSocket::errorOccurred, this, again);
    sock->connectToHost(host_, quint16(port_));
}

void PatchGenerator::show(const CalibrationPatch& p) {
    last_ = p;
    ++count_;
    if (toOutput_) {
        auto sched = output_ ? output_->scheduler() : nullptr;
        if (!sched || !sched->running()) {
            error_ = tr("The reference output stopped");
        } else {
            const output::Mode& m = sched->mode();
            const std::shared_ptr<const Lut3D> lut = through_ ? sched->calibration() : nullptr;
            sched->setPatch(std::make_shared<const Image>(patchFrame(p, m.width, m.height, lut.get())));
        }
    } else if (window_) {
        window_->setPatch(p, through_ ? DisplayCalibration::instance().screenLut(window_->screen()) : nullptr);
    }
    emit patchShown();
    emit statusChanged();
}

QString PatchGenerator::status() const {
    if (!active_) return tr("Off");
    const QString where = QStringLiteral("%1:%2").arg(host_).arg(port_);
    if (!connected()) {
        QString text = tr("Waiting for the calibration software at %1").arg(where);
        if (!error_.isEmpty()) text += QStringLiteral(" (%1)").arg(error_);
        return text;
    }
    if (count_ == 0) return tr("Connected to %1: waiting for the first patch").arg(where);
    const auto& s = last_.shapes.front();
    QString text = tr("Connected to %1: %n patch(es), last R %2 G %3 B %4 (%5-bit)", "", count_)
                       .arg(where)
                       .arg(s.rgb[0])
                       .arg(s.rgb[1])
                       .arg(s.rgb[2])
                       .arg(last_.bits);
    if (!error_.isEmpty()) text += QStringLiteral(" (%1)").arg(error_);
    return text;
}

}  // namespace montage
