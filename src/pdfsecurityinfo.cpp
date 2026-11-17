// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfsecurityinfo.h"

#include <QFileInfo>

#include <KLocalizedString>

#include <podofo/main/PdfEncrypt.h>
#include <podofo/main/PdfMemDocument.h>

#include <cstdint>

namespace
{
QString encryptionAlgorithmName(PoDoFo::PdfEncryptionAlgorithm algorithm)
{
    switch (algorithm) {
    case PoDoFo::PdfEncryptionAlgorithm::RC4V1:
        return QStringLiteral("RC4");
    case PoDoFo::PdfEncryptionAlgorithm::RC4V2:
        return QStringLiteral("RC4");
    case PoDoFo::PdfEncryptionAlgorithm::AESV2:
        return QStringLiteral("AES-128");
    case PoDoFo::PdfEncryptionAlgorithm::AESV3R5:
        return QStringLiteral("AES-256 (revision 5)");
    case PoDoFo::PdfEncryptionAlgorithm::AESV3R6:
        return QStringLiteral("AES-256 (revision 6)");
    case PoDoFo::PdfEncryptionAlgorithm::None:
        return QStringLiteral("None");
    }

    return i18n("Unknown");
}

QString permissionText(bool allowed)
{
    return allowed ? i18n("allowed") : i18n("not allowed");
}
}

PdfSecurityInfo::PdfSecurityInfo(QObject *parent)
    : QObject(parent)
{
}

QVariantMap PdfSecurityInfo::inspect(const QString &fileName, const QString &password) const
{
    QVariantMap result;
    result.insert(QStringLiteral("available"), false);

    const QFileInfo fileInfo(fileName);
    if (!fileInfo.isFile()) {
        result.insert(QStringLiteral("error"), i18n("The PDF file does not exist."));
        return result;
    }

    try {
        const QByteArray utf8Password = password.toUtf8();
        PoDoFo::PdfMemDocument document;
        document.Load(fileInfo.absoluteFilePath().toStdString(), utf8Password.toStdString());

        result.insert(QStringLiteral("available"), true);
        result.insert(QStringLiteral("encrypted"), document.IsEncrypted());
        if (!document.IsEncrypted()) {
            result.insert(QStringLiteral("summary"), i18n("The PDF is not encrypted."));
            return result;
        }

        const PoDoFo::PdfEncrypt *encryption = document.GetEncrypt();
        if (!encryption) {
            result.insert(QStringLiteral("available"), false);
            result.insert(QStringLiteral("error"), i18n("The PDF encryption dictionary is unavailable."));
            return result;
        }

        const int permissions = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(encryption->GetPValue())));
        const bool ownerPassword = document.HasOwnerPermissions();
        const bool printLowResolution = encryption->IsPrintAllowed();
        const bool printHighResolution = encryption->IsHighPrintAllowed();
        const bool accessibility = encryption->IsAccessibilityAllowed();
        const bool extract = encryption->IsCopyAllowed();
        const bool documentAssembly = encryption->IsDocAssemblyAllowed();
        const bool forms = encryption->IsFillAndSignAllowed();
        const bool annotations = encryption->IsEditNotesAllowed();
        const bool other = encryption->IsEditAllowed();

        result.insert(QStringLiteral("revision"), static_cast<int>(encryption->GetRevision()));
        result.insert(QStringLiteral("permissions"), permissions);
        result.insert(QStringLiteral("algorithm"), encryptionAlgorithmName(encryption->GetEncryptAlgorithm()));
        result.insert(QStringLiteral("keyLength"), static_cast<int>(encryption->GetKeyLengthBytes() * 8));
        result.insert(QStringLiteral("metadataEncrypted"), encryption->IsMetadataEncrypted());
        result.insert(QStringLiteral("authenticatedAs"), ownerPassword ? QStringLiteral("owner") : QStringLiteral("user"));
        result.insert(QStringLiteral("accessibility"), accessibility);
        result.insert(QStringLiteral("extract"), extract);
        result.insert(QStringLiteral("printLowResolution"), printLowResolution);
        result.insert(QStringLiteral("printHighResolution"), printHighResolution);
        result.insert(QStringLiteral("documentAssembly"), documentAssembly);
        result.insert(QStringLiteral("forms"), forms);
        result.insert(QStringLiteral("annotations"), annotations);
        result.insert(QStringLiteral("other"), other);
        result.insert(QStringLiteral("anything"), printLowResolution && printHighResolution && accessibility && extract && documentAssembly && forms && annotations && other);

        const QStringList lines{
            i18n("Encryption: %1 (%2-bit key)", result.value(QStringLiteral("algorithm")).toString(), result.value(QStringLiteral("keyLength")).toInt()),
            i18n("R = %1", result.value(QStringLiteral("revision")).toInt()),
            i18n("P = %1", permissions),
            ownerPassword ? i18n("Supplied password is owner password") : i18n("Supplied password is user password"),
            i18n("extract for accessibility: %1", permissionText(accessibility)),
            i18n("extract for any purpose: %1", permissionText(extract)),
            i18n("print low resolution: %1", permissionText(printLowResolution)),
            i18n("print high resolution: %1", permissionText(printHighResolution)),
            i18n("modify document assembly: %1", permissionText(documentAssembly)),
            i18n("modify forms: %1", permissionText(forms)),
            i18n("modify annotations: %1", permissionText(annotations)),
            i18n("modify other: %1", permissionText(other)),
            i18n("modify anything: %1", permissionText(result.value(QStringLiteral("anything")).toBool())),
        };
        result.insert(QStringLiteral("summary"), lines.join(QLatin1Char('\n')));
    } catch (const std::exception &error) {
        result.insert(QStringLiteral("error"), i18n("Unable to inspect PDF encryption: %1", QString::fromLocal8Bit(error.what())));
    }

    return result;
}
