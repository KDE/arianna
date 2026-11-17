// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QList>
#include <QString>

namespace PoDoFo
{
class PdfAnnotation;
}

struct PdfReferenceAnchor {
    QString id;
    int page = -1;
    int annotation = -1;
};

struct PdfReferenceIndex {
    int pageCount = -1;
    QList<PdfReferenceAnchor> anchors;
    QString error;
    bool needsPassword = false;
};

bool isAriannaPdfAnchor(const PoDoFo::PdfAnnotation &annotation);
QString ariannaPdfAnchorId(const PoDoFo::PdfAnnotation &annotation);
PdfReferenceIndex readPdfReferenceIndex(const QString &fileName);
// Modifies a temporary working copy only; callers handle validation and replacement of the original.
QString removePdfReferenceAnchor(const QString &workingCopy, int page, int annotation, const QString &anchorId);
