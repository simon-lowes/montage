#include "LicencesDialog.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

extern "C" {
#include <libavutil/avutil.h>
}

namespace montage {

namespace {

QString readText(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QTextBrowser* plainPage(const QString& name, const QString& text) {
    auto* page = new QTextBrowser;
    page->setObjectName(name);
    page->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    page->setLineWrapMode(QTextEdit::NoWrap);
    page->setPlainText(text);
    return page;
}

}  // namespace

QString packagedLicencesFolder() {
    const QDir dir(QCoreApplication::applicationDirPath());
    for (const QString& rel : {QStringLiteral("licenses"), QStringLiteral("../Resources/licenses")}) {
        const QString path = dir.absoluteFilePath(rel);
        if (QFileInfo::exists(path + "/BUNDLED.txt")) return QDir::cleanPath(path);
    }
    return {};
}

QString ffmpegLicence() { return QString::fromUtf8(avutil_license()); }

QString ffmpegVersion() { return QString::fromUtf8(av_version_info()); }

LicencesDialog::LicencesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("licencesDialog");
    setWindowTitle(tr("Licences"));
    resize(820, 640);
    const QString folder = packagedLicencesFolder();

    auto* tabs = new QTabWidget(this);
    tabs->setObjectName("licencesTabs");

    auto* montagePage = new QTextBrowser;
    montagePage->setObjectName("licenceMontage");
    montagePage->setOpenExternalLinks(true);
    montagePage->setHtml(
        tr("<h3>Montage %1</h3>"
           "<p>Montage is free software: you can redistribute it and/or modify it under the terms of the GNU General "
           "Public License as published by the Free Software Foundation, either version 3 of the License, or (at your "
           "option) any later version.</p>"
           "<p>Montage is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the "
           "implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License "
           "(the next tab) for more details.</p>"
           "<p>Its source code is at <a href=\"https://github.com/simon-lowes/montage\">github.com/simon-lowes/montage</a>, "
           "and the source for each release, including every library its packages bundle, is attached to the "
           "release.</p>"
           "<p>This copy runs with FFmpeg %2 (%3) and Qt %4 (LGPL-3.0).</p>")
                .arg(QStringLiteral(MONTAGE_VERSION), ffmpegVersion().toHtmlEscaped(), ffmpegLicence().toHtmlEscaped(),
                     QString::fromLatin1(qVersion())) +
        "<pre>" + readText(":/montage/licences/LICENSE-EXCEPTION").toHtmlEscaped() + "</pre>");
    tabs->addTab(montagePage, tr("Montage"));
    tabs->addTab(plainPage("licenceGpl", readText(":/montage/licences/LICENSE")), tr("GNU GPL"));

    auto* notices = new QTextBrowser;
    notices->setObjectName("licenceNotices");
    notices->setOpenLinks(false);
    notices->setMarkdown(readText(":/montage/licences/THIRD_PARTY_NOTICES.md"));
    // Web links open in the browser; the notices' links to licence files open the packaged copy, where there is one.
    connect(notices, &QTextBrowser::anchorClicked, this, [folder](const QUrl& url) {
        if (!url.isRelative()) QDesktopServices::openUrl(url);
        else if (!folder.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(folder + "/montage/" + url.path()));
    });
    tabs->addTab(notices, tr("Third-Party Notices"));

    const QString bundled = folder.isEmpty()
                                ? tr("This copy of Montage was not packaged: it uses the Qt, FFmpeg and other libraries "
                                     "installed on this computer, under their own licences.")
                                : readText(folder + "/BUNDLED.txt");
    tabs->addTab(plainPage("licenceBundled", bundled), tr("Bundled Libraries"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* open = buttons->addButton(tr("Open Licences Folder"), QDialogButtonBox::ActionRole);
    open->setObjectName("openLicencesFolder");
    open->setEnabled(!folder.isEmpty());
    open->setToolTip(folder.isEmpty() ? tr("Only a packaged copy of Montage has a licences folder") : folder);
    connect(open, &QPushButton::clicked, this, [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

}  // namespace montage
