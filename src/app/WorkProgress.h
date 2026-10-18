// Montage — long work off the interface thread behind a progress dialog that can cancel it (Offload Card, Repair Media).
#pragma once

#include <QEventLoop>
#include <QFutureWatcher>
#include <QMutex>
#include <QProgressDialog>
#include <QTimer>
#include <QtConcurrent>
#include <atomic>
#include <functional>
#include <memory>

namespace montage {

// Runs `work` off the interface thread behind a progress dialog that can cancel it.
template <typename R, typename Work>
inline R withProgress(QWidget* parent, const QString& label, Work work) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto file = std::make_shared<QString>();
    auto lock = std::make_shared<QMutex>();
    QProgressDialog progress(label, QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [cancel] { *cancel = true; });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &progress, [&progress, done, file, lock, label] {
        progress.setValue(done->load());
        QMutexLocker l(lock.get());
        if (!file->isEmpty()) progress.setLabelText(label + QStringLiteral("\n") + *file);
    });
    tick.start(100);
    QFutureWatcher<R> watcher;
    QEventLoop wait;
    QObject::connect(&watcher, &QFutureWatcher<R>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([work, cancel, done, file, lock] {
        return work([cancel, done, file, lock](double f, const QString& at) {
            *done = int(f * 999);
            if (!at.isEmpty()) {
                QMutexLocker l(lock.get());
                *file = at;
            }
            return !cancel->load();
        });
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
    progress.close();
    return watcher.result();
}

}  // namespace montage
