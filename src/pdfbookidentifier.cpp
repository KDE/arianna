// SPDX-License-Identifier: GPL-2.0-or-later
#include "pdfbookidentifier.h"
#include <KLocalizedString>
#include <podofo/main/PdfCatalog.h>
#include <podofo/main/PdfMemDocument.h>
#include <exception>

QString pdfBookIdentifier(const QString &fileName)
{
    try {
        PoDoFo::PdfMemDocument document;
        document.Load(fileName.toStdString());
        const auto *value = document.GetCatalog().GetDictionary().FindKey("AriannaBookId");
        if (value && value->IsString())
            return QString::fromUtf8(value->GetString().GetString()).trimmed();
    } catch (const std::exception &) {
    }
    return {};
}

QString writePdfBookIdentifier(const QString &input, const QString &output, const QString &identifier)
{
    try {
        PoDoFo::PdfMemDocument document;
        document.Load(input.toStdString());
        if (document.IsEncrypted() && !document.HasOwnerPermissions())
            return i18n("The encrypted PDF cannot be modified without owner permissions.");
        for (const auto *object : document.GetObjects()) {
            if (!object || !object->IsDictionary())
                continue;
            const auto &dictionary = object->GetDictionary();
            const auto *type = dictionary.FindKey("Type");
            if ((type && type->IsName() && type->GetName().GetString() == "Sig") || (dictionary.HasKey("ByteRange") && dictionary.HasKey("Contents")))
                return i18n("The PDF contains a digital signature and cannot be modified.");
        }
        document.GetCatalog().GetDictionary().AddKey("AriannaBookId", PoDoFo::PdfString(identifier.toStdString()));
        document.Save(output.toStdString(), PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        return {};
    } catch (const std::exception &error) {
        return i18n("Unable to change the PDF book identifier: %1", QString::fromUtf8(error.what()));
    }
}
