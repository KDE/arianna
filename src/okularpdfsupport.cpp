// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "okularpdfsupport.h"

#include <QCoreApplication>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>

#include <settings_core.h>

namespace
{
void addOkularPluginPath()
{
#ifdef ARIANNA_OKULAR_PLUGIN_DIR
    const QString pluginPath = QString::fromUtf8(ARIANNA_OKULAR_PLUGIN_DIR);
    if (!pluginPath.isEmpty() && !QCoreApplication::libraryPaths().contains(pluginPath)) {
        QCoreApplication::addLibraryPath(pluginPath);
    }
#endif
}

QMimeType pdfMimeTypeForUrl(const QUrl &url)
{
    QMimeDatabase mimeDatabase;
    QMimeType mimeType = mimeDatabase.mimeTypeForUrl(url);
    if (!mimeType.isValid() || mimeType.name() == QLatin1String("application/octet-stream")) {
        mimeType = mimeDatabase.mimeTypeForName(QStringLiteral("application/pdf"));
    }

    return mimeType;
}
}

namespace Arianna
{
void initializeOkularPdfSupport()
{
    addOkularPluginPath();

    static const bool initialized = [] {
        Okular::SettingsCore::instance(QStringLiteral("okularproviderrc"));
        return true;
    }();
    Q_UNUSED(initialized)
}

Okular::Document::OpenResult openOkularPdfDocument(Okular::Document &document, const QString &fileName, const QUrl &url, const QString &password)
{
    initializeOkularPdfSupport();
    return document.openDocument(fileName, url, pdfMimeTypeForUrl(url), password);
}
}
