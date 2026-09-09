// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfwatermarkfilter.h"
#include "config.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <podofo/main/PdfContents.h>
#include <podofo/main/PdfEncrypt.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>
#include <podofo/main/PdfResourceOperations.h>
#include <podofo/main/PdfResources.h>
#include <podofo/main/PdfXObjectForm.h>

using namespace PoDoFo;

namespace
{
QByteArray nameBytes(const PdfName &name)
{
    return QByteArray(name.GetString().data(), static_cast<qsizetype>(name.GetString().size()));
}

QByteArray invocation(const PdfName &name)
{
    return QByteArrayLiteral("/") + nameBytes(name) + QByteArrayLiteral(" Do\n");
}

void appendContent(PdfPage &page, const QByteArray &content)
{
    page.GetOrCreateContents()
        .CreateStreamForAppending(PdfStreamAppendFlags::NoSaveRestorePrior)
        .SetData(bufferview(content.constData(), static_cast<size_t>(content.size())));
}

PdfName addXObject(PdfResources &resources, const PdfXObjectForm &xObject)
{
    return static_cast<PdfResourceOperations &>(resources).AddResource(PdfResourceType::XObject, xObject.GetObject());
}

void addAdobeWatermarkMarker(PdfXObjectForm &xObject)
{
    PdfDictionary compoundType;
    compoundType.AddKey("Private"_n, PdfObject("WatermarkDemo"_n));

    PdfDictionary pieceInfo;
    pieceInfo.AddKey("ADBE_CompoundType"_n, PdfObject(std::move(compoundType)));
    xObject.GetDictionary().AddKey("PieceInfo"_n, PdfObject(std::move(pieceInfo)));
}

bool hasXObjectResource(PdfPage &page, const QByteArray &resourceName)
{
    for (const auto &[name, object] : page.GetResources().GetResourceIterator(PdfResourceType::XObject)) {
        Q_UNUSED(object)
        if (nameBytes(name) == resourceName) {
            return true;
        }
    }
    return false;
}

QByteArray pageContents(PdfPage &page)
{
    const charbuff contents = page.MustGetContents().GetCopy();
    return QByteArray(contents.data(), static_cast<qsizetype>(contents.size()));
}

void save(PdfMemDocument &document, const QString &fileName)
{
    document.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);
}

PdfWatermarkFilterResult filter(const QString &inputFileName, const QString &outputFileName)
{
    return PdfWatermarkFilter::filterFile(inputFileName, outputFileName, Config::defaultPdfWatermarkFilterPatternValue());
}
}

class PdfWatermarkFilterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void removesAdobePieceInfoWatermarkAndGarbageCollects();
    void doesNotFollowNestedFormResources();
    void handlesMultipleContentStreams();
    void handlesSharedPageResources();
    void rejectsSignedPdf();
    void rejectsEncryptedPdfWithoutOwnerPermissions();
};

void PdfWatermarkFilterTest::removesAdobePieceInfoWatermarkAndGarbageCollects()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("adobe-watermark.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("filtered.pdf"));

    PdfMemDocument document;
    PdfPage &page = document.GetPages().CreatePage(PdfPageSize::A4);
    const auto watermark = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    addAdobeWatermarkMarker(*watermark);
    const PdfName watermarkName = addXObject(page.GetResources(), *watermark);
    appendContent(page, invocation(watermarkName));
    save(document, inputFileName);

    const PdfWatermarkFilterResult result = filter(inputFileName, outputFileName);
    QVERIFY(result.filtered);
    QCOMPARE(result.occurrences, 1);

    PdfMemDocument filteredDocument;
    filteredDocument.Load(outputFileName.toStdString());
    PdfPage &filteredPage = filteredDocument.GetPages().GetPageAt(0);
    QVERIFY(!hasXObjectResource(filteredPage, nameBytes(watermarkName)));
    QVERIFY(!pageContents(filteredPage).contains(invocation(watermarkName).trimmed()));

    QFile outputFile(outputFileName);
    QVERIFY(outputFile.open(QIODevice::ReadOnly));
    QVERIFY(!outputFile.readAll().contains("WatermarkDemo"));
}

void PdfWatermarkFilterTest::doesNotFollowNestedFormResources()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("nested-form.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("filtered.pdf"));

    PdfMemDocument document;
    PdfPage &page = document.GetPages().CreatePage(PdfPageSize::A4);
    const auto watermark = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    addAdobeWatermarkMarker(*watermark);
    const auto outerForm = document.CreateXObjectForm(Rect(0, 0, 40, 40));
    addXObject(outerForm->GetOrCreateResources(), *watermark);
    const PdfName outerFormName = addXObject(page.GetResources(), *outerForm);
    appendContent(page, invocation(outerFormName));
    save(document, inputFileName);

    const PdfWatermarkFilterResult result = filter(inputFileName, outputFileName);
    QVERIFY(!result.filtered);
    QCOMPARE(result.occurrences, 0);
    QVERIFY(result.errorString.isEmpty());
    QVERIFY(!QFileInfo::exists(outputFileName));
}

void PdfWatermarkFilterTest::handlesMultipleContentStreams()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("multiple-content-streams.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("filtered.pdf"));

    PdfMemDocument document;
    PdfPage &page = document.GetPages().CreatePage(PdfPageSize::A4);
    const auto watermark = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    addAdobeWatermarkMarker(*watermark);
    const auto ordinary = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    const PdfName watermarkName = addXObject(page.GetResources(), *watermark);
    const PdfName ordinaryName = addXObject(page.GetResources(), *ordinary);
    appendContent(page, invocation(watermarkName));
    appendContent(page, invocation(ordinaryName));
    save(document, inputFileName);

    const PdfWatermarkFilterResult result = filter(inputFileName, outputFileName);
    QVERIFY(result.filtered);
    QCOMPARE(result.occurrences, 1);

    PdfMemDocument filteredDocument;
    filteredDocument.Load(outputFileName.toStdString());
    PdfPage &filteredPage = filteredDocument.GetPages().GetPageAt(0);
    QVERIFY(!hasXObjectResource(filteredPage, nameBytes(watermarkName)));
    QVERIFY(hasXObjectResource(filteredPage, nameBytes(ordinaryName)));
    QVERIFY(!pageContents(filteredPage).contains(invocation(watermarkName).trimmed()));
    QVERIFY(pageContents(filteredPage).contains(invocation(ordinaryName).trimmed()));
}

void PdfWatermarkFilterTest::handlesSharedPageResources()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("shared-resources.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("filtered.pdf"));

    PdfMemDocument document;
    PdfPage &firstPage = document.GetPages().CreatePage(PdfPageSize::A4);
    PdfPage &secondPage = document.GetPages().CreatePage(PdfPageSize::A4);
    const auto watermark = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    addAdobeWatermarkMarker(*watermark);

    PdfObject &sharedResourcesObject = document.GetObjects().CreateDictionaryObject();
    std::unique_ptr<PdfResources> sharedResources;
    QVERIFY(PdfResources::TryCreateFromObject(sharedResourcesObject, sharedResources));
    const PdfName watermarkName = addXObject(*sharedResources, *watermark);
    firstPage.GetDictionary().AddKeyIndirect("Resources"_n, sharedResourcesObject);
    secondPage.GetDictionary().AddKeyIndirect("Resources"_n, sharedResourcesObject);
    appendContent(firstPage, invocation(watermarkName));
    appendContent(secondPage, invocation(watermarkName));
    save(document, inputFileName);

    const PdfWatermarkFilterResult result = filter(inputFileName, outputFileName);
    QVERIFY(result.filtered);
    QCOMPARE(result.occurrences, 2);

    PdfMemDocument filteredDocument;
    filteredDocument.Load(outputFileName.toStdString());
    for (unsigned pageIndex = 0; pageIndex < filteredDocument.GetPages().GetCount(); ++pageIndex) {
        PdfPage &page = filteredDocument.GetPages().GetPageAt(pageIndex);
        QVERIFY(!hasXObjectResource(page, nameBytes(watermarkName)));
        QVERIFY(!pageContents(page).contains(invocation(watermarkName).trimmed()));
    }
}

void PdfWatermarkFilterTest::rejectsSignedPdf()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("signed.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("filtered.pdf"));

    PdfMemDocument document;
    PdfPage &page = document.GetPages().CreatePage(PdfPageSize::A4);
    const auto watermark = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    addAdobeWatermarkMarker(*watermark);
    appendContent(page, invocation(addXObject(page.GetResources(), *watermark)));

    PdfObject &signature = document.GetObjects().CreateDictionaryObject();
    signature.GetDictionary().AddKey("Type"_n, PdfObject("Sig"_n));
    document.GetCatalog().GetDictionary().AddKeyIndirect("TestSignature"_n, signature);
    save(document, inputFileName);

    const PdfWatermarkFilterResult result = filter(inputFileName, outputFileName);
    QVERIFY(!result.filtered);
    QCOMPARE(result.occurrences, 0);
    QVERIFY(!result.errorString.isEmpty());
    QVERIFY(!QFileInfo::exists(outputFileName));
}

void PdfWatermarkFilterTest::rejectsEncryptedPdfWithoutOwnerPermissions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString inputFileName = directory.filePath(QStringLiteral("encrypted.pdf"));
    const QString outputFileName = directory.filePath(QStringLiteral("filtered.pdf"));

    PdfMemDocument document;
    PdfPage &page = document.GetPages().CreatePage(PdfPageSize::A4);
    const auto watermark = document.CreateXObjectForm(Rect(0, 0, 20, 20));
    addAdobeWatermarkMarker(*watermark);
    appendContent(page, invocation(addXObject(page.GetResources(), *watermark)));
    document.SetEncrypted({}, "owner", PdfPermissions::None, PdfEncryptionAlgorithm::AESV2, PdfKeyLength::L128);
    save(document, inputFileName);

    const PdfWatermarkFilterResult result = filter(inputFileName, outputFileName);
    QVERIFY(!result.filtered);
    QCOMPARE(result.occurrences, 0);
    QVERIFY(!result.errorString.isEmpty());
    QVERIFY(!QFileInfo::exists(outputFileName));
}

QTEST_MAIN(PdfWatermarkFilterTest)
#include "pdfwatermarkfiltertest.moc"
