// Montage — Help › Licences…: Montage's licence (GPL-3.0-or-later and its additional permission), the third-party
// notices, and the licences of the libraries a package bundles (its licenses folder, which scripts/package-*.sh fill).
#pragma once

#include <QDialog>
#include <QString>

namespace montage {

// The packaged licenses folder: beside the program on Windows, in the bundle's Resources on macOS; "" when there is
// none (a build run from its build folder uses the libraries installed on the system).
QString packagedLicencesFolder();

// The licence of the FFmpeg Montage runs with, as FFmpeg reports it ("GPL version 3 or later"...), and its version.
QString ffmpegLicence();
QString ffmpegVersion();

class LicencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit LicencesDialog(QWidget* parent = nullptr);
};

}  // namespace montage
