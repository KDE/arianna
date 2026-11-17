// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "annotationstore.h"
#include "okularpdfdocument.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QtQml/QQmlExtensionPlugin>

#include <memory>

#include <core/area.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QVariantMap selection(int page)
{
    return {
        {QStringLiteral("page"), page},
        {QStringLiteral("text"), QStringLiteral("Selected PDF text")},
        {QStringLiteral("rects"),
         QVariantList{QVariantMap{{QStringLiteral("x"), 0.2}, {QStringLiteral("y"), 0.3}, {QStringLiteral("width"), 0.4}, {QStringLiteral("height"), 0.05}}}}};
}

QVariant evaluate(QObject *object, const QString &code)
{
    QQmlExpression expression(qmlContext(object), object, code);
    const QVariant result = expression.evaluate();
    if (expression.hasError()) {
        QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
    }
    return result;
}

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

class PdfAnnotationViewTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void persistedNotesAreHighlighted();
    void selectionMenuPassesGeometryToNoteEditor();
    void textSelectionContinuesAcrossTwoPageSpread();
    void notesCanBeManagedFromOverview();

private:
    QTemporaryDir m_dataDirectory;
};

void PdfAnnotationViewTest::initTestCase()
{
    QVERIFY(m_dataDirectory.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(m_dataDirectory.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_dataDirectory.path()));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-pdfannotationviewtest"));
    KLocalizedString::setApplicationDomain("arianna");
}

void PdfAnnotationViewTest::persistedNotesAreHighlighted()
{
    AnnotationStore store;
    const QString bookId = QStringLiteral("pdf-highlight-test");
    store.createUserAnnotation(bookId,
                               {{QStringLiteral("format"), QStringLiteral("pdf")},
                                {QStringLiteral("location"), selection(1)},
                                {QStringLiteral("color"), QStringLiteral("#64b5f6")},
                                {QStringLiteral("note"), QStringLiteral("Saved note")}});

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testAnnotations"), store.loadAnnotations(bookId));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.arianna
        OkularPdfView {
            width: 800; height: 950
            personalAnnotations: testAnnotations
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> view(component.create());
    QVERIFY2(view != nullptr, qPrintable(component.errorString()));
    const QVariantList regions = evaluate(view.get(), QStringLiteral("personalAnnotationsForPage(1)")).toList();
    QCOMPARE(regions.size(), 1);
    const QVariantMap region = regions.constFirst().toMap();
    QCOMPARE(region.value(QStringLiteral("x")).toDouble(), 0.2);
    QCOMPARE(region.value(QStringLiteral("y")).toDouble(), 0.3);
    QCOMPARE(region.value(QStringLiteral("width")).toDouble(), 0.4);
    QCOMPARE(region.value(QStringLiteral("height")).toDouble(), 0.05);
    QCOMPARE(region.value(QStringLiteral("color")).toString(), QStringLiteral("#64b5f6"));
    QVERIFY(evaluate(view.get(), QStringLiteral("personalAnnotationsForPage(0)")).toList().isEmpty());
}

void PdfAnnotationViewTest::selectionMenuPassesGeometryToNoteEditor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("selection.pdf"));
    PoDoFo::PdfMemDocument pdf;
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.Save(fileName.toStdString());
    OkularPdfDocument document;
    document.setSource(QUrl::fromLocalFile(fileName));
    QVERIFY(document.opened());
    auto area = std::make_unique<Okular::RegularAreaRect>();
    area->append(Okular::NormalizedRect(0.2, 0.3, 0.6, 0.35));
    document.setTextSelection(0, std::move(area));
    const QVariantMap selectedGeometry = document.textSelectionGeometry();
    QVERIFY(!selectedGeometry.isEmpty());

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testDocument"), &document);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.arianna
        OkularPdfView {
            width: 800; height: 950
            document: testDocument
            allowPersonalAnnotations: true
            property var requestedSelection: ({})
            onPersonalNoteRequested: selection => requestedSelection = selection
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> view(component.create());
    QVERIFY2(view != nullptr, qPrintable(component.errorString()));
    auto *viewItem = qobject_cast<QQuickItem *>(view.get());
    QVERIFY(viewItem);
    QQuickWindow window;
    window.resize(800, 950);
    viewItem->setParentItem(window.contentItem());
    window.show();
    evaluate(view.get(), QStringLiteral("openSelectionMenu(200, 200)"));
    QQuickItem *addNoteItem = nullptr;
    for (auto *item : view->findChildren<QQuickItem *>()) {
        if (item->property("text").toString() == i18nc("@action:inmenu", "Add Note")) {
            addNoteItem = item;
            break;
        }
    }
    QVERIFY(addNoteItem);
    QTRY_VERIFY(addNoteItem->isVisible());
    QVERIFY(addNoteItem->isEnabled());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, addNoteItem->mapToScene(QPointF(addNoteItem->width() / 2, addNoteItem->height() / 2)).toPoint());
    QTRY_COMPARE(view->property("requestedSelection").toMap(), selectedGeometry);
    QTRY_VERIFY(!document.hasTextSelection());
}

void PdfAnnotationViewTest::textSelectionContinuesAcrossTwoPageSpread()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("spread-selection.pdf"));
    {
        QPdfWriter writer(fileName);
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.drawText(80, 150, QStringLiteral("First page"));
        QVERIFY(writer.newPage());
        painter.drawText(80, 150, QStringLiteral("Second page"));
    }

    OkularPdfDocument document;
    document.setSource(QUrl::fromLocalFile(fileName));
    QVERIFY(document.opened());

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testDocument"), &document);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.arianna
        OkularPdfView {
            width: 1400; height: 1000
            document: testDocument
            twoPageMode: true
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> view(component.create());
    QVERIFY(view);
    auto *viewItem = qobject_cast<QQuickItem *>(view.get());
    QVERIFY(viewItem);
    QQuickWindow window;
    window.resize(1400, 1000);
    viewItem->setParentItem(window.contentItem());
    window.show();

    QQuickItem *firstPage = nullptr;
    QQuickItem *secondPage = nullptr;
    QTRY_VERIFY([&] {
        for (auto *item : view->findChildren<QQuickItem *>()) {
            if (QString::fromLatin1(item->metaObject()->className()) != QStringLiteral("OkularPdfPageItem")) {
                continue;
            }
            if (item->property("pageNumber").toInt() == 0) {
                firstPage = item;
            } else if (item->property("pageNumber").toInt() == 1) {
                secondPage = item;
            }
        }
        return firstPage && secondPage && firstPage->property("pageValid").toBool() && secondPage->property("pageValid").toBool();
    }());
    QCOMPARE(firstPage->property("selectionPeer").value<QObject *>(), secondPage);

    const QPoint start = window.mapFromGlobal(firstPage->mapToGlobal(QPointF(70, 130)).toPoint());
    const QPoint end = window.mapFromGlobal(secondPage->mapToGlobal(QPointF(300, 300)).toPoint());
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent moveEvent(QEvent::MouseMove, QPointF(end), QPointF(end), window.mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &moveEvent);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);

    QVERIFY(document.hasTextSelection());
    const QString selectedText = document.selectedText();
    QVERIFY(selectedText.contains(QStringLiteral("First page")));
    QVERIFY(selectedText.contains(QStringLiteral("Second page")));
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(850, 500));
    QTRY_VERIFY(!document.hasTextSelection());

    const QPoint reverseStart = window.mapFromGlobal(secondPage->mapToGlobal(QPointF(300, 300)).toPoint());
    const QPoint reverseEnd = window.mapFromGlobal(firstPage->mapToGlobal(QPointF(70, 130)).toPoint());
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, reverseStart);
    QMouseEvent reverseMoveEvent(QEvent::MouseMove,
                                 QPointF(reverseEnd),
                                 QPointF(reverseEnd),
                                 window.mapToGlobal(reverseEnd),
                                 Qt::NoButton,
                                 Qt::LeftButton,
                                 Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &reverseMoveEvent);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, reverseEnd);

    const QString reverseSelectedText = document.selectedText();
    QVERIFY(reverseSelectedText.contains(QStringLiteral("First page")));
    QVERIFY(reverseSelectedText.contains(QStringLiteral("Second page")));
}

void PdfAnnotationViewTest::notesCanBeManagedFromOverview()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("notes.pdf"));
    PoDoFo::PdfMemDocument pdf;
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.Save(fileName.toStdString());
    QFile source(fileName);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray originalPdf = source.readAll();
    source.close();

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testFileName"), fileName);
    engine.rootContext()->setContextProperty(QStringLiteral("testSelection"), selection(1));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1000; height: 1000
            visible: true
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
            pageStack.initialPage: PdfViewerPage {
                objectName: "testPage"
                filename: testFileName
                entry: ({ uniqueIdentifier: "pdf-notes-test" })
                pageMode: "single"
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window != nullptr, qPrintable(component.errorString()));
    auto *page = window->findChild<QQuickItem *>(QStringLiteral("testPage"));
    QVERIFY(page);
    auto *document = page->property("pdfDocument").value<OkularPdfDocument *>();
    QVERIFY(document);
    document->setSource(QUrl::fromLocalFile(fileName));
    QVERIFY(document->opened());

    auto *action = page->findChild<QObject *>(QStringLiteral("personalPdfNotesAction"));
    auto *overview = page->findChild<QObject *>(QStringLiteral("personalPdfNotesDialog"));
    auto *editor = page->findChild<QObject *>(QStringLiteral("personalPdfNoteDialog"));
    auto *noteText = page->findChild<QObject *>(QStringLiteral("personalPdfNoteText"));
    auto *colors = page->findChild<QQuickItem *>(QStringLiteral("personalPdfNoteColors"));
    QVERIFY(action);
    QVERIFY(overview);
    QVERIFY(editor);
    QVERIFY(noteText);
    QVERIFY(colors);
    const auto chooseColor = [&](int index) {
        QTRY_VERIFY(editor->property("opened").toBool());
        auto *swatch = findItem(colors, QStringLiteral("annotationColorSwatch") + QString::number(index));
        QVERIFY(swatch && swatch->isVisible() && swatch->window());
        QSignalSpy clicked(swatch, SIGNAL(clicked()));
        QTest::mouseClick(swatch->window(), Qt::LeftButton, Qt::NoModifier,
                          swatch->mapToScene(QPointF(swatch->width()/2, swatch->height()/2)).toPoint());
        QTRY_COMPARE(clicked.count(), 1);
    };
    auto *overviewContent = overview->property("contentItem").value<QQuickItem *>();
    QVERIFY(overviewContent);
    QVERIFY(!action->property("enabled").toBool());

    AnnotationStore store;
    const QString bookId = QStringLiteral("pdf-notes-test");
    evaluate(page, QStringLiteral("openPersonalPdfNote(testSelection)"));
    QTRY_VERIFY(editor->property("visible").toBool());
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("yellow"));
    chooseColor(5);
    noteText->setProperty("text", QStringLiteral("Discarded note"));
    QVERIFY(QMetaObject::invokeMethod(editor, "reject"));
    QVERIFY(store.loadAnnotations(bookId).isEmpty());

    evaluate(page, QStringLiteral("openPersonalPdfNote(testSelection)"));
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("yellow"));
    chooseColor(5);
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("#64b5f6"));
    // Exactly two rows of eight accessible controls; colors can also be chosen
    // with the keyboard, independently of the note text field.
    for (int i = 0; i < 16; ++i) {
        auto *swatch = findItem(colors, QStringLiteral("annotationColorSwatch") + QString::number(i));
        QVERIFY(swatch && swatch->isVisible());
        auto *rowStart = findItem(colors, QStringLiteral("annotationColorSwatch") + QString::number(i < 8 ? 0 : 8));
        QCOMPARE(swatch->y(), rowStart->y());
    }
    QVERIFY(findItem(colors, QStringLiteral("annotationColorSwatch8"))->y() > findItem(colors, QStringLiteral("annotationColorSwatch0"))->y());
    auto *keyboardSwatch = findItem(colors, QStringLiteral("annotationColorSwatch6"));
    keyboardSwatch->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(keyboardSwatch->window(), Qt::Key_Space);
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("#4dd0e1"));
    chooseColor(5);
    if (!qEnvironmentVariableIsEmpty("ARIANNA_PALETTE_SCREENSHOT")) {
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow && quickWindow->grabWindow().save(qEnvironmentVariable("ARIANNA_PALETTE_SCREENSHOT") + QStringLiteral("-pdf.png")));
    }
    noteText->setProperty("text", QStringLiteral("My PDF note"));
    QVERIFY(QMetaObject::invokeMethod(editor, "accept"));
    const QVariantList saved = store.loadAnnotations(bookId);
    QCOMPARE(saved.size(), 1);
    const QVariantMap annotation = saved.constFirst().toMap();
    QCOMPARE(annotation.value(QStringLiteral("note")).toString(), QStringLiteral("My PDF note"));
    QCOMPARE(annotation.value(QStringLiteral("color")).toString(), QStringLiteral("#64b5f6"));
    QCOMPARE(annotation.value(QStringLiteral("location")).toMap(), selection(1));
    QCOMPARE(page->property("personalPdfAnnotations").toList().size(), 1);
    QVERIFY(action->property("enabled").toBool());
    document->reload();
    QCOMPARE(page->property("personalPdfAnnotations").toList().size(), 1);

    QVERIFY(QMetaObject::invokeMethod(action, "trigger", Q_ARG(QObject *, nullptr)));
    QTRY_VERIFY(overview->property("visible").toBool());
    QTRY_VERIFY(findItem(overviewContent, QStringLiteral("personalPdfNoteDelegate")));
    auto *editButton = findItem(overviewContent, QStringLiteral("editPersonalPdfNoteButton"));
    QVERIFY(editButton);
    QVERIFY(QMetaObject::invokeMethod(editButton, "clicked"));
    QTRY_VERIFY(editor->property("visible").toBool());
    QCOMPARE(noteText->property("text").toString(), QStringLiteral("My PDF note"));
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("#64b5f6"));
    chooseColor(3);
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("#f06292"));
    QSignalSpy closed(editor, SIGNAL(closed()));
    QVERIFY(QMetaObject::invokeMethod(editor, "reject"));
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(store.loadAnnotations(bookId).constFirst().toMap().value(QStringLiteral("color")).toString(), QStringLiteral("#64b5f6"));
    evaluate(page, QStringLiteral("editPersonalPdfAnnotation(personalPdfAnnotations[0].annotationId)"));
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("#64b5f6"));
    chooseColor(7);
    QCOMPARE(colors->property("selectedColor").toString(), QStringLiteral("#81c784"));
    noteText->setProperty("text", QStringLiteral("Edited note"));
    QVERIFY(QMetaObject::invokeMethod(editor, "accept"));
    const QVariantList edited = store.loadAnnotations(bookId);
    QCOMPARE(edited.size(), 1);
    QCOMPARE(edited.constFirst().toMap().value(QStringLiteral("annotationId")), annotation.value(QStringLiteral("annotationId")));
    QCOMPARE(edited.constFirst().toMap().value(QStringLiteral("location")), annotation.value(QStringLiteral("location")));
    QCOMPARE(edited.constFirst().toMap().value(QStringLiteral("note")).toString(), QStringLiteral("Edited note"));
    QCOMPARE(edited.constFirst().toMap().value(QStringLiteral("color")).toString(), QStringLiteral("#81c784"));
    QObject *pdfView = nullptr;
    for (auto *child : page->findChildren<QObject *>()) {
        if (child->metaObject()->indexOfProperty("personalAnnotations") >= 0) { pdfView = child; break; }
    }
    QVERIFY(pdfView);
    QCOMPARE(evaluate(pdfView, QStringLiteral("personalAnnotationsForPage(1)[0].color")).toString(), QStringLiteral("#81c784"));

    QVERIFY(QMetaObject::invokeMethod(action, "trigger", Q_ARG(QObject *, nullptr)));
    QTRY_VERIFY(findItem(overviewContent, QStringLiteral("personalPdfNoteDelegate")));
    auto *delegate = findItem(overviewContent, QStringLiteral("personalPdfNoteDelegate"));
    QVERIFY(QMetaObject::invokeMethod(delegate, "clicked"));
    QTRY_COMPARE(page->property("activePage").toInt(), 1);
    QTRY_VERIFY(!overview->property("visible").toBool());

    // Shift+horizontal wheel follows document order, even when creation order differs.
    const auto earlier = store.createUserAnnotation(bookId,
        {{QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("location"), selection(0)}});
    auto upperLocation = selection(1);
    auto rect = upperLocation.value(QStringLiteral("rects")).toList().first().toMap();
    rect.insert(QStringLiteral("y"), 0.1);
    upperLocation.insert(QStringLiteral("rects"), QVariantList{rect});
    const auto upper = store.createUserAnnotation(bookId,
        {{QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("location"), upperLocation}});
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    QVERIFY(quickWindow);
    QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
    QTest::qWait(300); // Wait for the overview's exit transition.
    const auto remarkWheel = [&](int direction) {
        const QPointF position = page->mapToScene(QPointF(page->width() / 2, page->height() / 2));
        QWheelEvent event(position, quickWindow->mapToGlobal(position.toPoint()), QPoint(direction * 30, 0), {},
                          Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(quickWindow, &event);
    };
    const auto currentNote = [&] { return page->property("currentPersonalPdfAnnotationId").toString(); };
    remarkWheel(1);
    QTRY_COMPARE(currentNote(), upper.value(QStringLiteral("annotationId")).toString());
    QCOMPARE(page->property("activePage").toInt(), 1);
    remarkWheel(1);
    QTRY_COMPARE(currentNote(), earlier.value(QStringLiteral("annotationId")).toString());
    QCOMPARE(page->property("activePage").toInt(), 0);
    remarkWheel(1);
    QTRY_COMPARE(currentNote(), annotation.value(QStringLiteral("annotationId")).toString());
    QCOMPARE(page->property("activePage").toInt(), 1);
    remarkWheel(-1);
    QTRY_COMPARE(currentNote(), earlier.value(QStringLiteral("annotationId")).toString());
    // Deleting the current note still permits navigation among the remaining notes.
    store.removeAnnotation(bookId, earlier.value(QStringLiteral("annotationId")).toString());
    remarkWheel(-1);
    QTRY_COMPARE(currentNote(), upper.value(QStringLiteral("annotationId")).toString());
    store.removeAnnotation(bookId, upper.value(QStringLiteral("annotationId")).toString());
    remarkWheel(-1);
    QTRY_COMPARE(currentNote(), annotation.value(QStringLiteral("annotationId")).toString());
    QCOMPARE(page->property("zoomLevel").toDouble(), 1.0);
    QCOMPARE(page->property("pageWidthDistance").toInt(), -1);

    // Read-only viewers expose the overview, but cannot alter personal notes.
    page->setProperty("readOnly", true);
    QVERIFY(action->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(action, "trigger", Q_ARG(QObject *, nullptr)));
    QTRY_VERIFY(findItem(overviewContent, QStringLiteral("deletePersonalPdfNoteButton")));
    QVERIFY(!findItem(overviewContent, QStringLiteral("deletePersonalPdfNoteButton"))->property("enabled").toBool());
    QVERIFY(!findItem(overviewContent, QStringLiteral("editPersonalPdfNoteButton"))->property("enabled").toBool());
    evaluate(page, QStringLiteral("openPersonalPdfNote(testSelection)"));
    QVERIFY(!editor->property("visible").toBool());
    page->setProperty("readOnly", false);

    // Changes broadcast by the store refresh an already open overview.
    evaluate(page,
             QStringLiteral(
                 "var note = personalPdfAnnotations[0]; note.note = 'Synchronized note'; AnnotationStore.saveAnnotation(personalAnnotationBookId(), note)"));
    QCOMPARE(page->property("personalPdfAnnotations").toList().constFirst().toMap().value(QStringLiteral("note")).toString(),
             QStringLiteral("Synchronized note"));
    QTRY_VERIFY(findItem(overviewContent, QStringLiteral("deletePersonalPdfNoteButton")));
    QVERIFY(QMetaObject::invokeMethod(findItem(overviewContent, QStringLiteral("deletePersonalPdfNoteButton")), "clicked"));
    QTRY_VERIFY(store.loadAnnotations(bookId).isEmpty());
    QTRY_VERIFY(page->property("personalPdfAnnotations").toList().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(overview, "close"));
    QTest::qWait(300);
    const int currentPage = page->property("activePage").toInt();
    remarkWheel(-1);
    remarkWheel(1);
    QCOMPARE(page->property("activePage").toInt(), currentPage);
    QCOMPARE(page->property("zoomLevel").toDouble(), 1.0);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), originalPdf);
    document->setSource(QUrl());
}

QTEST_MAIN(PdfAnnotationViewTest)
#include "pdfannotationviewtest.moc"
