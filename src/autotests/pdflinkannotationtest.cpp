// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "annotationstore.h"
#include "okularpdfdocument.h"
#include "referencestore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <podofo/main/PdfAnnotation_Types.h>
#include <podofo/main/PdfContents.h>
#include <podofo/main/PdfEncrypt.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

using namespace PoDoFo;

namespace
{
void appendContent(PdfPage &page, const QByteArray &content)
{
    page.GetOrCreateContents()
        .CreateStreamForAppending(PdfStreamAppendFlags::NoSaveRestorePrior)
        .SetData(bufferview(content.constData(), static_cast<size_t>(content.size())));
}

QByteArray pageContents(PdfPage &page)
{
    const charbuff contents = page.MustGetContents().GetCopy();
    return QByteArray(contents.data(), static_cast<qsizetype>(contents.size()));
}

QVariantMap selection(int page, double x, double y, double width, double height)
{
    QVariantMap rectangle;
    rectangle.insert(QStringLiteral("x"), x);
    rectangle.insert(QStringLiteral("y"), y);
    rectangle.insert(QStringLiteral("width"), width);
    rectangle.insert(QStringLiteral("height"), height);

    QVariantMap result;
    result.insert(QStringLiteral("page"), page);
    result.insert(QStringLiteral("rects"), QVariantList{rectangle});
    return result;
}

QByteArray fileContents(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
}

class PdfLinkAnnotationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void writesNativeInternalLinkWithoutChangingContents();
    void writesAndReadsInvisibleAriannaAnchor();
    void allowsAnnotationWhenEncryptedPdfGrantsEditNotesToUser();
    void rejectsEncryptedPdfWithoutAnnotationPermission();
    void invalidOrCancelledTargetLeavesNoAnnotation();
    void personalNoteUsesAnnotationStoreWithoutChangingPdf();
};

void PdfLinkAnnotationTest::initTestCase()
{
    const QString dataHome = QDir::temp().filePath(QStringLiteral("arianna-pdflinkannotationtest-%1").arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dataHome));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));
    QCoreApplication::setOrganizationName(QStringLiteral("KDE"));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-pdflinkannotationtest"));
}

void PdfLinkAnnotationTest::writesNativeInternalLinkWithoutChangingContents()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("link.pdf"));

    PdfMemDocument created;
    PdfPage &firstPage = created.GetPages().CreatePage(PdfPageSize::A4);
    PdfPage &secondPage = created.GetPages().CreatePage(PdfPageSize::A4);
    appendContent(firstPage, QByteArrayLiteral("q 0 0 10 10 re f Q\n"));
    appendContent(secondPage, QByteArrayLiteral("q 20 20 10 10 re f Q\n"));
    created.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);

    PdfMemDocument before;
    before.Load(fileName.toStdString());
    const QByteArray firstPageContents = pageContents(before.GetPages().GetPageAt(0));
    const QByteArray secondPageContents = pageContents(before.GetPages().GetPageAt(1));

    OkularPdfDocument document;
    const QVariantMap result = document.createPdfLinkAnnotation(selection(0, 0.10, 0.15, 0.20, 0.05), selection(1, 0.40, 0.60, 0.20, 0.05), fileName);
    QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));

    PdfMemDocument updated;
    updated.Load(fileName.toStdString());
    PdfPage &updatedFirstPage = updated.GetPages().GetPageAt(0);
    QCOMPARE(pageContents(updatedFirstPage), firstPageContents);
    QCOMPARE(pageContents(updated.GetPages().GetPageAt(1)), secondPageContents);
    QCOMPARE(updatedFirstPage.GetAnnotations().GetCount(), 1U);

    PdfAnnotation &annotation = updatedFirstPage.GetAnnotations().GetAnnotAt(0);
    QCOMPARE(annotation.GetType(), PdfAnnotationType::Link);
    auto &link = static_cast<PdfAnnotationLink &>(annotation);
    QVERIFY(link.GetDictionary().FindKey("Subtype") != nullptr);
    QVERIFY(link.GetDictionary().FindKey("Dest") != nullptr);
    QVERIFY(link.GetDictionary().FindKey("A") == nullptr);
    QVERIFY(link.GetQuadPoints() != nullptr);
    QCOMPARE(link.GetQuadPoints()->GetSize(), 8U);

    const auto destination = link.GetDestination();
    QVERIFY(destination != nullptr);
    QCOMPARE(destination->GetType(), PdfDestinationType::XYZ);
    QVERIFY(destination->GetPage() != nullptr);
    QCOMPARE(destination->GetPage()->GetIndex(), 1U);
}

void PdfLinkAnnotationTest::writesAndReadsInvisibleAriannaAnchor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("arianna-anchor.pdf"));
    const QString anchorId = QStringLiteral("019fb5e0-0000-7000-8000-000000000001");

    PdfMemDocument created;
    PdfPage &page = created.GetPages().CreatePage(PdfPageSize::A4);
    // /NM is common on ordinary annotations. It must not make this one an
    // Arianna anchor without the explicit /AriannaAnchor marker.
    PdfAnnotationText &foreignAnnotation = page.GetAnnotations().CreateAnnot<PdfAnnotationText>(Rect::FromCorners(10, 10, 20, 20));
    foreignAnnotation.GetDictionary().AddKey(PdfName("NM"), PdfObject(PdfString("foreign-annotation-name")));
    created.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);

    OkularPdfDocument document;
    const QVariantMap result = document.createPdfAnchorAnnotation(selection(0, 0.10, 0.15, 0.20, 0.05), anchorId, fileName);
    QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
    QCOMPARE(result.value(QStringLiteral("anchorId")).toString(), anchorId);

    PdfMemDocument updated;
    updated.Load(fileName.toStdString());
    PdfPage &updatedPage = updated.GetPages().GetPageAt(0);
    QCOMPARE(updatedPage.GetAnnotations().GetCount(), 2U);

    PdfAnnotation *anchorAnnotation = nullptr;
    for (unsigned index = 0; index < updatedPage.GetAnnotations().GetCount(); ++index) {
        PdfAnnotation &annotation = updatedPage.GetAnnotations().GetAnnotAt(index);
        const PdfObject *marker = annotation.GetDictionary().FindKey("AriannaAnchor");
        bool isAriannaAnchor = false;
        if (marker && marker->TryGetBool(isAriannaAnchor) && isAriannaAnchor) {
            anchorAnnotation = &annotation;
            break;
        }
    }
    QVERIFY(anchorAnnotation);
    QCOMPARE(anchorAnnotation->GetType(), PdfAnnotationType::Highlight);

    const PdfDictionary &dictionary = anchorAnnotation->GetDictionary();
    const PdfObject *marker = dictionary.FindKey("AriannaAnchor");
    bool isAriannaAnchor = false;
    QVERIFY(marker && marker->TryGetBool(isAriannaAnchor));
    QVERIFY(isAriannaAnchor);

    QVERIFY(dictionary.FindKey("targetBookId") == nullptr);
    QVERIFY(dictionary.FindKey("targetAnchorId") == nullptr);
    QVERIFY(dictionary.FindKey("referenceId") == nullptr);
    QVERIFY(dictionary.FindKey("A") == nullptr);
    QVERIFY(dictionary.FindKey("Dest") == nullptr);

    const PdfObject *name = dictionary.FindKey("NM");
    PdfString storedAnchorId;
    QVERIFY(name && name->TryGetString(storedAnchorId));
    const std::string_view rawAnchorId = storedAnchorId.GetString();
    QCOMPARE(QString::fromUtf8(rawAnchorId.data(), static_cast<qsizetype>(rawAnchorId.size())), anchorId);

    const uint32_t flags = static_cast<uint32_t>(anchorAnnotation->GetFlags());
    QVERIFY(flags & static_cast<uint32_t>(PdfAnnotationFlags::Hidden));
    QVERIFY(flags & static_cast<uint32_t>(PdfAnnotationFlags::NoView));

    const Rect cropBox = updatedPage.GetCropBox();
    const Rect anchorRect = anchorAnnotation->GetRect();
    QCOMPARE(anchorAnnotation->GetPage()->GetIndex(), 0U);
    QVERIFY(qAbs(anchorRect.GetLeft() - (cropBox.GetLeft() + cropBox.Width * 0.10)) < 0.001);
    QVERIFY(qAbs(anchorRect.GetRight() - (cropBox.GetLeft() + cropBox.Width * 0.30)) < 0.001);
    QVERIFY(qAbs(anchorRect.GetBottom() - (cropBox.GetBottom() + cropBox.Height * 0.80)) < 0.001);
    QVERIFY(qAbs(anchorRect.GetTop() - (cropBox.GetBottom() + cropBox.Height * 0.85)) < 0.001);

    const auto &highlight = static_cast<const PdfAnnotationHighlight &>(*anchorAnnotation);
    QVERIFY(highlight.GetQuadPoints() != nullptr);
    QCOMPARE(highlight.GetQuadPoints()->GetSize(), 8U);

    const QVariantList anchors = document.pdfAriannaAnchors(fileName);
    QCOMPARE(anchors.size(), 1);
    const QVariantMap loadedAnchor = anchors.constFirst().toMap();
    QCOMPARE(loadedAnchor.value(QStringLiteral("anchorId")).toString(), anchorId);
    QCOMPARE(loadedAnchor.value(QStringLiteral("page")).toInt(), 0);
    const QVariantList rectangles = loadedAnchor.value(QStringLiteral("rects")).toList();
    QCOMPARE(rectangles.size(), 1);
    const QVariantMap rectangle = rectangles.constFirst().toMap();
    QVERIFY(qAbs(rectangle.value(QStringLiteral("x")).toDouble() - 0.10) < 0.0001);
    QVERIFY(qAbs(rectangle.value(QStringLiteral("y")).toDouble() - 0.15) < 0.0001);
    QVERIFY(qAbs(rectangle.value(QStringLiteral("width")).toDouble() - 0.20) < 0.0001);
    QVERIFY(qAbs(rectangle.value(QStringLiteral("height")).toDouble() - 0.05) < 0.0001);

    QVariantMap reference;
    const QString sourceBookId =
        QStringLiteral("pdf-source-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    reference.insert(QStringLiteral("sourceBookId"), sourceBookId);
    reference.insert(QStringLiteral("sourceAnchorId"), anchorId);
    reference.insert(QStringLiteral("targetBookId"), QStringLiteral("target-book"));
    reference.insert(QStringLiteral("targetLocation"), QStringLiteral("{\"format\":\"pdf\",\"page\":0}"));
    ReferenceStore referenceStore;
    referenceStore.saveReference(reference);
    QCOMPARE(referenceStore.loadReferenceBySource(sourceBookId, anchorId)
                 .value(QStringLiteral("sourceAnchorId"))
                 .toString(),
             anchorId);
}

void PdfLinkAnnotationTest::allowsAnnotationWhenEncryptedPdfGrantsEditNotesToUser()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("editable-notes.pdf"));

    PdfMemDocument created;
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.SetEncrypted("reader", "owner", PdfPermissions::EditNotes, PdfEncryptionAlgorithm::AESV2, PdfKeyLength::L128);
    created.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);

    OkularPdfDocument document;
    document.setPassword(QStringLiteral("reader"));
    const QVariantMap result = document.createPdfLinkAnnotation(selection(0, 0.10, 0.15, 0.20, 0.05), selection(1, 0.40, 0.60, 0.20, 0.05), fileName);
    QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));

    PdfMemDocument updated;
    updated.Load(fileName.toStdString(), "reader");
    QVERIFY(!updated.HasOwnerPermissions());
    QVERIFY(updated.IsEditNotesAllowed());
    QCOMPARE(updated.GetPages().GetPageAt(0).GetAnnotations().GetCount(), 1U);
    QCOMPARE(updated.GetPages().GetPageAt(0).GetAnnotations().GetAnnotAt(0).GetType(), PdfAnnotationType::Link);
}

void PdfLinkAnnotationTest::rejectsEncryptedPdfWithoutAnnotationPermission()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("protected-notes.pdf"));

    PdfMemDocument created;
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.SetEncrypted("reader", "owner", PdfPermissions::None, PdfEncryptionAlgorithm::AESV2, PdfKeyLength::L128);
    created.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);
    const QByteArray before = fileContents(fileName);

    OkularPdfDocument document;
    document.setPassword(QStringLiteral("reader"));
    const QVariantMap result = document.createPdfLinkAnnotation(selection(0, 0.10, 0.15, 0.20, 0.05), selection(1, 0.40, 0.60, 0.20, 0.05), fileName);
    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(fileContents(fileName), before);
}

void PdfLinkAnnotationTest::invalidOrCancelledTargetLeavesNoAnnotation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("no-half-link.pdf"));

    PdfMemDocument created;
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);
    const QByteArray before = fileContents(fileName);

    OkularPdfDocument document;
    const QVariantMap result = document.createPdfLinkAnnotation(selection(0, 0.10, 0.15, 0.20, 0.05), QVariantMap(), fileName);
    QVERIFY(!result.value(QStringLiteral("success")).toBool());
    QCOMPARE(fileContents(fileName), before);

    PdfMemDocument unchanged;
    unchanged.Load(fileName.toStdString());
    QCOMPARE(unchanged.GetPages().GetPageAt(0).GetAnnotations().GetCount(), 0U);
}

void PdfLinkAnnotationTest::personalNoteUsesAnnotationStoreWithoutChangingPdf()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("personal-note.pdf"));

    PdfMemDocument created;
    created.GetPages().CreatePage(PdfPageSize::A4);
    created.Save(fileName.toStdString(), PdfSaveOptions::NoMetadataUpdate);
    const QByteArray before = fileContents(fileName);

    const QString bookId = QStringLiteral("pdf-note-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    AnnotationStore store;
    QVariantMap annotation;
    annotation.insert(QStringLiteral("format"), QStringLiteral("pdf"));
    annotation.insert(QStringLiteral("location"), selection(0, 0.10, 0.15, 0.20, 0.05));
    annotation.insert(QStringLiteral("color"), QStringLiteral("yellow"));
    annotation.insert(QStringLiteral("text"), QStringLiteral("A footnote marker"));
    annotation.insert(QStringLiteral("note"), QStringLiteral("Personal reminder"));
    const QVariantMap saved = store.createUserAnnotation(bookId, annotation);
    QVERIFY(!saved.value(QStringLiteral("annotationId")).toString().isEmpty());
    QCOMPARE(fileContents(fileName), before);

    const QVariantList annotations = store.loadAnnotations(bookId);
    QCOMPARE(annotations.size(), 1);
    const QVariantMap loaded = annotations.constFirst().toMap();
    QCOMPARE(loaded.value(QStringLiteral("format")).toString(), QStringLiteral("pdf"));
    QCOMPARE(loaded.value(QStringLiteral("note")).toString(), QStringLiteral("Personal reminder"));
    QCOMPARE(loaded.value(QStringLiteral("location")).toMap().value(QStringLiteral("page")).toInt(), 0);
}

QTEST_MAIN(PdfLinkAnnotationTest)
#include "pdflinkannotationtest.moc"
