// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfversionupdater.h"

#include <QFile>
#include <QFileInfo>

#include <KLocalizedString>

#include <podofo/main/PdfMemDocument.h>

#include <exception>

namespace
{
QString pdfHeaderVersion(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray header = file.read(1024);
    const qsizetype markerIndex = header.indexOf("%PDF-");
    if (markerIndex < 0) {
        return {};
    }

    const qsizetype versionStart = markerIndex + 5;
    qsizetype versionEnd = versionStart;
    while (versionEnd < header.size()) {
        const char character = header.at(versionEnd);
        if ((character < '0' || character > '9') && character != '.') {
            break;
        }
        ++versionEnd;
    }

    return QString::fromLatin1(header.mid(versionStart, versionEnd - versionStart)).trimmed();
}

const PoDoFo::PdfObject *dereferenceObject(const PoDoFo::PdfObject &object, const PoDoFo::PdfMemDocument &document)
{
    PoDoFo::PdfReference reference;
    if (!object.TryGetReference(reference) || !reference.IsIndirect()) {
        return &object;
    }

    return document.GetObjects().GetObject(reference);
}

bool hasDigitalSignature(const PoDoFo::PdfMemDocument &document)
{
    for (const PoDoFo::PdfObject *object : document.GetObjects()) {
        if (object == nullptr || !object->IsDictionary()) {
            continue;
        }

        const PoDoFo::PdfObject *type = object->GetDictionary().GetKey("Type");
        const PoDoFo::PdfObject *resolvedType = type ? dereferenceObject(*type, document) : nullptr;
        if (resolvedType != nullptr && resolvedType->IsName() && resolvedType->GetName().GetString() == "Sig") {
            return true;
        }

        const PoDoFo::PdfObject *byteRange = object->GetDictionary().GetKey("ByteRange");
        const PoDoFo::PdfObject *contents = object->GetDictionary().GetKey("Contents");
        const PoDoFo::PdfObject *resolvedByteRange = byteRange ? dereferenceObject(*byteRange, document) : nullptr;
        if (resolvedByteRange != nullptr && resolvedByteRange->IsArray() && contents != nullptr) {
            return true;
        }
    }

    return false;
}
}

PdfVersionUpdateResult PdfVersionUpdater::setVersion20(const QString &inputFileName, const QString &outputFileName)
{
    PdfVersionUpdateResult result;
    if (!QFileInfo::exists(inputFileName)) {
        result.errorString = i18n("The input PDF does not exist.");
        return result;
    }
    if (inputFileName == outputFileName) {
        result.errorString = i18n("The updated PDF must be written to a different file.");
        return result;
    }
    if (pdfHeaderVersion(inputFileName) == QLatin1String("2.0")) {
        result.alreadyVersion20 = true;
        return result;
    }

    try {
        PoDoFo::PdfMemDocument document;
        document.Load(inputFileName.toStdString());
        if (document.IsEncrypted() && !document.HasOwnerPermissions()) {
            result.errorString = i18n("The encrypted PDF cannot be modified without owner permissions.");
            return result;
        }
        if (hasDigitalSignature(document)) {
            result.errorString = i18n("The PDF contains a digital signature and cannot be modified.");
            return result;
        }

        document.GetMetadata().SetPdfVersion(PoDoFo::PdfVersion::V2_0);
        document.Save(outputFileName.toStdString(), PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        result.updated = QFileInfo(outputFileName).isFile() && pdfHeaderVersion(outputFileName) == QLatin1String("2.0");
        if (!result.updated) {
            result.errorString = i18n("PoDoFo did not create a PDF with version 2.0.");
        }
    } catch (const std::exception &error) {
        result.errorString = QString::fromUtf8(error.what());
    }

    return result;
}
