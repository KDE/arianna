// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pdfreferenceindex.h"

#include <KLocalizedString>
#include <QThread>

#include <podofo/auxiliary/StreamDevice.h>
#include <podofo/main/PdfAnnotation.h>
#include <podofo/main/PdfError.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

QString removePdfReferenceAnchor(const QString &workingCopy, int page, int annotation, const QString &anchorId)
{
    try {
        const auto device = std::make_shared<PoDoFo::FileStreamDevice>(workingCopy.toStdString(), PoDoFo::FileMode::Open);
        PoDoFo::PdfMemDocument pdf;
        pdf.Load(device);
        if (pdf.IsEncrypted() && !pdf.HasOwnerPermissions() && !pdf.IsEditNotesAllowed()) {
            return i18n("The encrypted PDF does not allow adding or editing annotations.");
        }
        if (page < 0 || static_cast<unsigned>(page) >= pdf.GetPages().GetCount()) {
            return i18n("The anchor has changed. Run the reference check again.");
        }
        auto &annotations = pdf.GetPages().GetPageAt(static_cast<unsigned>(page)).GetAnnotations();
        if (annotation < 0 || static_cast<unsigned>(annotation) >= annotations.GetCount()) {
            return i18n("The anchor has changed. Run the reference check again.");
        }
        const auto &anchor = annotations.GetAnnotAt(static_cast<unsigned>(annotation));
        if (!isAriannaPdfAnchor(anchor) || ariannaPdfAnchorId(anchor) != anchorId) {
            return i18n("The anchor has changed. Run the reference check again.");
        }
        annotations.RemoveAnnotAt(static_cast<unsigned>(annotation));
        pdf.SaveUpdate(*device, PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        return {};
    } catch (const std::exception &error) {
        return i18n("Unable to remove the PDF anchor: %1", QString::fromUtf8(error.what()));
    }
}

bool isAriannaPdfAnchor(const PoDoFo::PdfAnnotation &annotation)
{
    const auto *marker = annotation.GetDictionary().FindKey("AriannaAnchor");
    bool marked = false;
    return marker && marker->TryGetBool(marked) && marked;
}

QString ariannaPdfAnchorId(const PoDoFo::PdfAnnotation &annotation)
{
    if (!isAriannaPdfAnchor(annotation)) {
        return {};
    }
    const auto *name = annotation.GetDictionary().FindKey("NM");
    PoDoFo::PdfString value;
    if (!name || !name->TryGetString(value)) {
        return {};
    }
    const auto raw = value.GetString();
    return QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size()));
}

PdfReferenceIndex readPdfReferenceIndex(const QString &fileName)
{
    PdfReferenceIndex result;
    try {
        PoDoFo::PdfMemDocument pdf;
        pdf.Load(fileName.toStdString());
        result.pageCount = static_cast<int>(pdf.GetPages().GetCount());
        for (int pageIndex = 0; pageIndex < result.pageCount; ++pageIndex) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                return {};
            }
            const auto &annotations = pdf.GetPages().GetPageAt(static_cast<unsigned>(pageIndex)).GetAnnotations();
            for (unsigned index = 0; index < annotations.GetCount(); ++index) {
                const auto &annotation = annotations.GetAnnotAt(index);
                if (isAriannaPdfAnchor(annotation)) {
                    result.anchors.append({ariannaPdfAnchorId(annotation), pageIndex, static_cast<int>(index)});
                }
            }
        }
        return result;
    } catch (const PoDoFo::PdfError &error) {
        result.needsPassword = error.GetCode() == PoDoFo::PdfErrorCode::InvalidPassword;
        result.error = result.needsPassword ? i18n("The PDF requires a password; its cross-references could not be fully checked: %1", fileName)
                                            : i18n("Unable to inspect PDF references in %1: %2", fileName, QString::fromUtf8(error.what()));
    } catch (const std::exception &error) {
        result.error = i18n("Unable to inspect PDF references in %1: %2", fileName, QString::fromUtf8(error.what()));
    }
    // A partially read file must never look like a complete anchor index.
    result.anchors.clear();
    result.pageCount = -1;
    return result;
}
