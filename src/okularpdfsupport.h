// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>
#include <QUrl>
#include <core/document.h>

namespace Arianna
{
void initializeOkularPdfSupport();
Okular::Document::OpenResult openOkularPdfDocument(Okular::Document &document, const QString &fileName, const QUrl &url, const QString &password = {});
}
