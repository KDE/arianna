// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "okularpdfdocument.h"
#include "okularpdfpageitem.h"
#include "referencestore.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QtQml/QQmlExtensionPlugin>

#include <memory>

#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QQuickItem *findItem(QQuickItem *parent, const QString &name)
{
    if (!parent) {
        return nullptr;
    }
    if (parent->objectName() == name) {
        return parent;
    }
    for (auto *child : parent->childItems()) {
        if (auto *item = findItem(child, name)) {
            return item;
        }
    }
    return nullptr;
}
}

class PdfReferenceViewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void persistedAnchorIsInteractive();
    void createsAndEditsReferencesWithExplicitLocations();

private:
    QTemporaryDir m_dataDirectory;
};

void PdfReferenceViewTest::initTestCase()
{
    QVERIFY(m_dataDirectory.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(m_dataDirectory.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_dataDirectory.path()));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-pdfreferenceviewtest"));
    KLocalizedString::setApplicationDomain("arianna");
}

void PdfReferenceViewTest::persistedAnchorIsInteractive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("source.pdf"));
    PoDoFo::PdfMemDocument pdf;
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.Save(fileName.toStdString());
    const QString anchorId = QStringLiteral("019fb5e0-0000-7000-8000-000000000001");
    const QVariantMap rect{{QStringLiteral("x"), 0.2},
                           {QStringLiteral("y"), 0.2},
                           {QStringLiteral("width"), 0.3},
                           {QStringLiteral("height"), 0.05}};
    OkularPdfDocument document;
    const auto result = document.createPdfAnchorAnnotation({{QStringLiteral("page"), 0}, {QStringLiteral("rects"), QVariantList{rect}}}, anchorId, fileName);
    QVERIFY(result.value(QStringLiteral("success")).toBool());
    document.setSource(QUrl::fromLocalFile(fileName));
    QVERIFY(document.opened());
    const QVariantList persistedAnchors = document.pdfAriannaAnchors(fileName);
    QCOMPARE(persistedAnchors.size(), 1);
    QCOMPARE(persistedAnchors.constFirst().toMap().value(QStringLiteral("anchorId")).toString(), anchorId);

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testDocument"), &document);
    engine.rootContext()->setContextProperty(QStringLiteral("testFileName"), fileName);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.arianna
        OkularPdfView {
            width: 800; height: 950
            document: testDocument
            property string activatedAnchorId: ""
            ariannaAnchors: testDocument.pdfAriannaAnchors(testFileName).map(anchor => ({
                anchorId: anchor.anchorId,
                page: anchor.page,
                rects: anchor.rects,
                hasReference: true
            }))
            onAriannaAnchorActivated: anchorId => activatedAnchorId = anchorId
        }
    )", QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto *view = qobject_cast<QQuickItem *>(object.get());
    QVERIFY(view);
    QTRY_COMPARE(view->property("ariannaAnchors").toList().size(), 1);
    QQuickWindow window;
    window.resize(800, 950);
    view->setParentItem(window.contentItem());
    window.show();
    QTest::qWait(100);
    auto *page = view->findChild<OkularPdfPageItem *>();
    QVERIFY(page);
    QTRY_VERIFY(page->width() > 100);
    const QPoint position = page->mapToItem(view, QPointF(page->width() * 0.3, page->height() * 0.225)).toPoint();
    QTest::mouseMove(&window, position);
    QTRY_COMPARE(view->property("hoveredAriannaAnchorId").toString(), anchorId);
    QTRY_COMPARE(window.cursor().shape(), Qt::BitmapCursor);
    QVERIFY(!window.cursor().pixmap().isNull());
    QCOMPARE(window.cursor().pixmap().deviceIndependentSize(), QSizeF(32, 32));
    QTest::mouseMove(&window, QPoint(1, 1));
    QTRY_COMPARE(view->property("hoveredAriannaAnchorId").toString(), QString());
    QTRY_COMPARE(window.cursor().shape(), Qt::ArrowCursor);
    QVariant regions;
    QVERIFY(QMetaObject::invokeMethod(view,
                                      "ariannaAnchorRegionsForPage",
                                      Q_RETURN_ARG(QVariant, regions),
                                      Q_ARG(QVariant, QVariant(0))));
    QCOMPARE(regions.toList().size(), 1);
    QVariant activated;
    QVERIFY(QMetaObject::invokeMethod(view,
                                      "activateAriannaAnchorAtRootPosition",
                                      Q_RETURN_ARG(QVariant, activated),
                                      Q_ARG(QVariant, QVariant(position.x())),
                                      Q_ARG(QVariant, QVariant(position.y()))));
    QVERIFY(activated.toBool());
    QTRY_COMPARE(view->property("activatedAnchorId").toString(), anchorId);
}

void PdfReferenceViewTest::createsAndEditsReferencesWithExplicitLocations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("edit-source.pdf"));
    PoDoFo::PdfMemDocument pdf;
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.Save(fileName.toStdString());
    const QString sourceBookId = QStringLiteral("pdf-edit-source");
    const QString anchorId = QStringLiteral("019fb5e0-0000-7000-8000-000000000002");
    const QVariantMap rect{{QStringLiteral("x"), 0.2}, {QStringLiteral("y"), 0.2},
                          {QStringLiteral("width"), 0.3}, {QStringLiteral("height"), 0.05}};
    OkularPdfDocument writer;
    QVERIFY(writer.createPdfAnchorAnnotation({{QStringLiteral("page"), 1}, {QStringLiteral("rects"), QVariantList{rect}}}, anchorId, fileName)
                .value(QStringLiteral("success")).toBool());
    QFile sourceFile(fileName);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalPdf = sourceFile.readAll();
    sourceFile.close();
    ReferenceStore store;
    store.saveReference({{QStringLiteral("sourceBookId"), sourceBookId},
                         {QStringLiteral("sourceAnchorId"), anchorId},
                         {QStringLiteral("sourceAnchorTitle"), QStringLiteral("Original title")},
                         {QStringLiteral("targetBookId"), QStringLiteral("target-a")},
                         {QStringLiteral("targetLocation"), QStringLiteral("epubcfi(/6/8!/4/2:12)")},
                         {QStringLiteral("targetPreviewHtml"), QStringLiteral("<p>Custom preview</p>")}});
    const QVariantMap originalReference = store.loadReferenceBySource(sourceBookId, anchorId);

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testFileName"), fileName);
    engine.rootContext()->setContextProperty(QStringLiteral("testFileUrl"), QUrl::fromLocalFile(fileName));
    engine.rootContext()->setContextProperty(QStringLiteral("testAnchorId"), anchorId);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1000; height: 1000
            visible: true
            property string openedLocation: ""
            property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
            contextDrawer: Kirigami.ContextDrawer {
                property string currentTocId: ""
                property string currentHref: ""
                property var model: QtObject { function importFromJson(json) {} }
                function clearCurrentItem() {}
                function setCurrentItem(id) {}
                signal goTo(var location)
                signal removePdfTableOfContentsItem(string tocId)
                signal editPdfTableOfContentsItem(string tocId, string title)
            }
            function pdfWatermarkFilterMode() { return "off" }
            function fileUrlFromLocalPath(fileName) { return testFileUrl }
            function openReferencePage(filename, location, entry, readOnly, locations, sourceTitle) {
                openedLocation = location;
            }
            pageStack.initialPage: PdfViewerPage {
                objectName: "testPage"
                filename: testFileName
                entry: ({ uniqueIdentifier: "pdf-edit-source" })
                pageMode: "single"
                property var requests: []
                function requestBookServerJson(path, success, failure) {
                    requests.push({ path: path, success: success, failure: failure });
                }
                function reply(index, data) { requests[index].success(data); }
            }
        }
    )", QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("testPage"));
    QVERIFY(page);
    auto evaluate = [&](const QString &code) {
        QQmlExpression expression(qmlContext(page), page, code);
        const QVariant result = expression.evaluate();
        if (expression.hasError()) {
            QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
        }
        return result;
    };
    auto *document = page->property("pdfDocument").value<OkularPdfDocument *>();
    QVERIFY(document);
    page->setProperty("url", QUrl::fromLocalFile(fileName).toString());
    QVERIFY(document->opened());

    // The toolbar overview navigates to the persisted source geometry, including
    // in read-only books, without opening the reference's external target.
    auto *referencesAction = page->findChild<QObject *>(QStringLiteral("pdfReferencesAction"));
    auto *referencesDialog = page->findChild<QObject *>(QStringLiteral("pdfReferencesDialog"));
    QVERIFY(referencesAction);
    QVERIFY(referencesDialog);
    QVERIFY(referencesAction->property("enabled").toBool());
    page->setProperty("readOnly", true);
    QVERIFY(QMetaObject::invokeMethod(referencesAction, "trigger", Q_ARG(QObject *, nullptr)));
    QTRY_VERIFY(referencesDialog->property("visible").toBool());
    auto *referencesContent = referencesDialog->property("contentItem").value<QQuickItem *>();
    QVERIFY(referencesContent);
    QTRY_VERIFY(findItem(referencesContent, QStringLiteral("pdfReferenceDelegate")));
    auto *referenceItem = findItem(referencesContent, QStringLiteral("pdfReferenceDelegate"));
    QVERIFY(referenceItem->isVisible());
    const QVariantMap anchor = referenceItem->property("modelData").toMap();
    QCOMPARE(anchor.value(QStringLiteral("anchorId")).toString(), anchorId);
    QCOMPARE(anchor.value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("Original title"));
    QCOMPARE(anchor.value(QStringLiteral("page")).toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(referenceItem, "clicked"));
    QTRY_COMPARE(page->property("activePage").toInt(), 1);
    QTRY_VERIFY(!referencesDialog->property("visible").toBool());
    QVERIFY(object->property("openedLocation").toString().isEmpty());
    QCOMPARE(evaluate(QStringLiteral("JSON.parse(pdfHistoryCurrentLocation).x")).toDouble(), 0.2);
    QCOMPARE(evaluate(QStringLiteral("JSON.parse(pdfHistoryCurrentLocation).y")).toDouble(), 0.2);
    evaluate(QStringLiteral("goBack()"));
    QTRY_COMPARE(page->property("activePage").toInt(), 0);
    page->setProperty("readOnly", false);

    evaluate(QStringLiteral("openPdfAriannaAnchor(testAnchorId)"));
    auto *previewDialog = page->findChild<QObject *>(QStringLiteral("pdfReferenceDialog"));
    QVERIFY(previewDialog);
    QVERIFY(previewDialog->property("visible").toBool());
    auto *editButton = page->findChild<QObject *>(QStringLiteral("editPdfReferenceButton"));
    QVERIFY(editButton);
    QVERIFY(QMetaObject::invokeMethod(editButton, "clicked"));
    auto *editDialog = page->findChild<QObject *>(QStringLiteral("externalBookReferenceDialog"));
    QVERIFY(editDialog);
    QVERIFY(editDialog->property("visible").toBool());
    evaluate(QStringLiteral("reply(0, {books: [{id: 'target-a', title: 'Target A'}, {id: 'target-b', title: 'Target B'}]})"));
    evaluate(QStringLiteral("reply(1, {refs: [{targetBookId: 'target-a'}]})"));
    evaluate(QStringLiteral("reply(2, {targetLocations: [{location: 'chapter.xhtml', title: 'Chapter'}]})"));
    QCOMPARE(evaluate(QStringLiteral("selectedExternalReferenceBook().bookId")).toString(), QStringLiteral("target-a"));
    QCOMPARE(evaluate(QStringLiteral("selectedExternalReferenceTargetLocation().targetLocation")).toString(),
             originalReference.value(QStringLiteral("targetLocation")).toString());
    auto *titleField = page->findChild<QObject *>(QStringLiteral("externalReferenceSourceAnchorTitle"));
    auto *previewField = page->findChild<QObject *>(QStringLiteral("externalReferencePreviewHtml"));
    auto *locationField = page->findChild<QObject *>(QStringLiteral("externalReferenceTargetLocationInput"));
    QVERIFY(titleField);
    QVERIFY(previewField);
    QVERIFY(locationField);
    QCOMPARE(locationField->property("text").toString(), originalReference.value(QStringLiteral("targetLocation")).toString());
    QCOMPARE(titleField->property("text").toString(), QStringLiteral("Original title"));
    QCOMPARE(previewField->property("text").toString(), QStringLiteral("<p>Custom preview</p>"));

    // Cancelling must leave the stored reference untouched.
    titleField->setProperty("text", QStringLiteral("Discard me"));
    QVERIFY(QMetaObject::invokeMethod(editDialog, "close"));
    QCOMPARE(store.loadReferenceBySource(sourceBookId, anchorId), originalReference);
    evaluate(QStringLiteral("editPdfAriannaAnchor(testAnchorId)"));
    evaluate(QStringLiteral("reply(3, {books: [{id: 'target-a', title: 'Target A'}, {id: 'target-b', title: 'Target B'}]})"));
    evaluate(QStringLiteral("reply(4, {refs: [{targetBookId: 'target-a'}]})"));

    // Changing books while the initial request is still pending must ignore
    // its late response, including the stored location/preview preselection.
    auto *bookCombo = page->findChild<QObject *>(QStringLiteral("externalReferenceBookCombo"));
    QVERIFY(bookCombo);
    int targetIndex = -1;
    QVERIFY(QMetaObject::invokeMethod(bookCombo, "find", Q_RETURN_ARG(int, targetIndex), Q_ARG(QString, QStringLiteral("Target B"))));
    QVERIFY(targetIndex >= 0);
    bookCombo->setProperty("currentIndex", targetIndex);
    QCOMPARE(evaluate(QStringLiteral("selectedExternalReferenceBook().bookId")).toString(), QStringLiteral("target-b"));
    evaluate(QStringLiteral("loadExternalReferenceTargetLocations()"));
    evaluate(QStringLiteral("reply(6, {targetLocations: [{location: 'canto.xhtml#verse', title: 'Verse', previewHtml: 'New target preview'}]})"));
    evaluate(QStringLiteral("reply(5, {targetLocations: [{location: 'wrong.xhtml', title: 'Stale'}]})"));
    auto *locationCombo = page->findChild<QObject *>(QStringLiteral("externalReferenceTargetLocationCombo"));
    QVERIFY(locationCombo);
    locationCombo->setProperty("currentIndex", 1);
    QVERIFY(QMetaObject::invokeMethod(locationCombo, "activated", Q_ARG(int, 1)));
    QCOMPARE(evaluate(QStringLiteral("selectedExternalReferenceTargetLocation().targetLocation")).toString(), QStringLiteral("canto.xhtml#verse"));
    QCOMPARE(locationField->property("text").toString(), QStringLiteral("canto.xhtml#verse"));
    auto *saveButton = page->findChild<QObject *>(QStringLiteral("saveExternalBookReferenceButton"));
    QVERIFY(saveButton);
    locationField->setProperty("text", QStringLiteral("   "));
    QVERIFY(QMetaObject::invokeMethod(locationField, "textEdited"));
    QVERIFY(!saveButton->property("enabled").toBool());
    evaluate(QStringLiteral("saveExternalBookReference()"));
    QCOMPARE(store.loadReferenceBySource(sourceBookId, anchorId), originalReference);
    locationField->setProperty("text", QStringLiteral("canto.xhtml#manual-verse"));
    QVERIFY(QMetaObject::invokeMethod(locationField, "textEdited"));
    QCOMPARE(locationCombo->property("currentIndex").toInt(), 0);
    QVERIFY(saveButton->property("enabled").toBool());
    titleField->setProperty("text", QStringLiteral("Updated title"));
    previewField->setProperty("text", QStringLiteral("<p>Updated preview</p>"));
    evaluate(QStringLiteral("saveExternalBookReference()"));
    QVERIFY(!editDialog->property("visible").toBool());
    const QVariantMap saved = store.loadReferenceBySource(sourceBookId, anchorId);
    QCOMPARE(store.loadReferences(sourceBookId).size(), 1);
    QCOMPARE(saved.value(QStringLiteral("sourceAnchorId")).toString(), anchorId);
    QCOMPARE(saved.value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("Updated title"));
    QCOMPARE(saved.value(QStringLiteral("targetBookId")).toString(), QStringLiteral("target-b"));
    QCOMPARE(saved.value(QStringLiteral("targetLocation")).toString(), QStringLiteral("canto.xhtml#manual-verse"));
    QCOMPARE(saved.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral("<p>Updated preview</p>"));
    QCOMPARE(saved.value(QStringLiteral("created")), originalReference.value(QStringLiteral("created")));
    QVERIFY(!saved.value(QStringLiteral("modified")).toString().isEmpty());
    QCOMPARE(evaluate(QStringLiteral("pdfAriannaAnchors[0].sourceAnchorTitle")).toString(), QStringLiteral("Updated title"));
    QVERIFY(previewDialog->property("visible").toBool());
    QCOMPARE(previewDialog->property("title").toString(), QStringLiteral("Updated title"));
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), originalPdf);
    QCOMPARE(document->pdfAriannaAnchors(fileName).size(), 1);

    // Opening the edited target still uses the existing reference resolver.
    evaluate(QStringLiteral("followPdfAriannaAnchor(testAnchorId)"));
    evaluate(QStringLiteral("reply(7, {targetLocation: 'canto.xhtml#manual-verse', target: {entry: {filename: '/test/target.epub'}}})"));
    QCOMPARE(object->property("openedLocation").toString(), QStringLiteral("canto.xhtml#manual-verse"));
    page->setProperty("readOnly", true);
    evaluate(QStringLiteral("openPdfAriannaAnchor(testAnchorId)"));
    QVERIFY(!editButton->property("enabled").toBool());
    evaluate(QStringLiteral("editPdfAriannaAnchor(testAnchorId)"));
    QVERIFY(!editDialog->property("visible").toBool());

    // A new reference can also use a manual location before its TOC loads.
    page->setProperty("readOnly", false);
    QVERIFY(QMetaObject::invokeMethod(previewDialog, "close"));
    evaluate(QStringLiteral("openExternalBookReferenceDialog({page: 0, rects: [{x: 0.4, y: 0.4, width: 0.2, height: 0.05}], text: 'New reference'})"));
    evaluate(QStringLiteral("reply(8, {books: [{id: 'target-b', title: 'Target B'}]})"));
    evaluate(QStringLiteral("reply(9, {refs: [{targetBookId: 'target-b'}]})"));
    QVERIFY(QMetaObject::invokeMethod(bookCombo, "find", Q_RETURN_ARG(int, targetIndex), Q_ARG(QString, QStringLiteral("Target B"))));
    QVERIFY(targetIndex >= 0);
    bookCombo->setProperty("currentIndex", targetIndex);
    evaluate(QStringLiteral("loadExternalReferenceTargetLocations()"));
    QCOMPARE(locationField->property("text").toString(), QString());
    locationField->setProperty("text", QStringLiteral("epubcfi(/6/10!/4/2:8)"));
    QVERIFY(QMetaObject::invokeMethod(locationField, "textEdited"));
    QVERIFY(page->property("externalReferenceLocationsLoading").toBool());
    QVERIFY(saveButton->property("enabled").toBool());
    evaluate(QStringLiteral("saveExternalBookReference()"));
    QVERIFY(!editDialog->property("visible").toBool());
    const QVariantList allReferences = store.loadReferences(sourceBookId);
    QCOMPARE(allReferences.size(), 2);
    const QVariantMap created = allReferences[0].toMap().value(QStringLiteral("sourceAnchorId")).toString() == anchorId
        ? allReferences[1].toMap() : allReferences[0].toMap();
    QCOMPARE(created.value(QStringLiteral("targetLocation")).toString(), QStringLiteral("epubcfi(/6/10!/4/2:8)"));
    QCOMPARE(created.value(QStringLiteral("targetBookId")).toString(), QStringLiteral("target-b"));
    QCOMPARE(document->pdfAriannaAnchors(fileName).size(), 2);
    evaluate(QStringLiteral("reply(10, {targetLocations: [{location: 'late.xhtml', title: 'Late'}]})"));
    QCOMPARE(store.loadReferenceBySource(sourceBookId, created.value(QStringLiteral("sourceAnchorId")).toString()), created);

    // Clearing a preview while the saved target's TOC is loading must survive the late reply.
    evaluate(QStringLiteral("editPdfAriannaAnchor(testAnchorId)"));
    evaluate(QStringLiteral("reply(11, {books: [{id: 'target-b', title: 'Target B'}]})"));
    evaluate(QStringLiteral("reply(12, {refs: [{targetBookId: 'target-b'}]})"));
    QCOMPARE(previewField->property("text").toString(), QStringLiteral("<p>Updated preview</p>"));
    previewField->setProperty("text", QStringLiteral(""));
    evaluate(QStringLiteral("reply(13, {targetLocations: [{location: 'canto.xhtml#manual-verse', title: 'Verse', previewHtml: 'TOC preview'}]})"));
    QCOMPARE(previewField->property("text").toString(), QStringLiteral(""));
    QCOMPARE(locationField->property("text").toString(), QStringLiteral("canto.xhtml#manual-verse"));
    evaluate(QStringLiteral("saveExternalBookReference()"));
    QCOMPARE(store.loadReferenceBySource(sourceBookId, anchorId).value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));
    QCOMPARE(previewDialog->property("reference").toMap().value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));

    // Reopen and edit just the title: the stored empty preview stays empty.
    evaluate(QStringLiteral("editPdfAriannaAnchor(testAnchorId)"));
    evaluate(QStringLiteral("reply(14, {books: [{id: 'target-b', title: 'Target B'}]})"));
    evaluate(QStringLiteral("reply(15, {refs: [{targetBookId: 'target-b'}]})"));
    evaluate(QStringLiteral("reply(16, {targetLocations: [{location: 'canto.xhtml#manual-verse', title: 'Verse', previewHtml: 'TOC preview'}]})"));
    QCOMPARE(previewField->property("text").toString(), QStringLiteral(""));
    titleField->setProperty("text", QStringLiteral("No preview"));
    evaluate(QStringLiteral("saveExternalBookReference()"));
    const QVariantMap withoutPreview = store.loadReferenceBySource(sourceBookId, anchorId);
    QCOMPARE(withoutPreview.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));
    QCOMPARE(withoutPreview.value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("No preview"));
    QCOMPARE(store.loadReferences(sourceBookId).size(), 2);

    // An open overview follows title edits signalled by ReferenceStore.
    QVERIFY(QMetaObject::invokeMethod(referencesAction, "trigger", Q_ARG(QObject *, nullptr)));
    QTRY_VERIFY(referencesDialog->property("visible").toBool());
    evaluate(QStringLiteral("var reference = ReferenceStore.loadReferenceBySource(bookServerIdentifier(), testAnchorId); "
                            "reference.sourceAnchorTitle = 'Renamed source'; ReferenceStore.saveReference(reference)"));
    QCOMPARE(evaluate(QStringLiteral("pdfAriannaAnchors.find(anchor => anchor.anchorId === testAnchorId).sourceAnchorTitle")).toString(),
             QStringLiteral("Renamed source"));
    QCOMPARE(page->property("pdfAriannaAnchors").toList().size(), 2);
    page->setProperty("url", QString());
    QTRY_VERIFY(!referencesDialog->property("visible").toBool());
    QVERIFY(!referencesAction->property("enabled").toBool());
}

QTEST_MAIN(PdfReferenceViewTest)
#include "pdfreferenceviewtest.moc"
