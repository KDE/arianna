// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookdatabase.h"
#include "booklistmodel.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "editorsessionstore.h"
#include "maintenancefiles.h"
#include "okularpdfdocument.h"
#include "okularpdfpageitem.h"
#include "referencestore.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <memory>
#include <atomic>
#include <algorithm>
#include <functional>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
class NativeKeyTrace : public QObject
{
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress && qobject_cast<QQuickWindow *>(object)) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Plus || key->key() == Qt::Key_Minus)
                qInfo() << "Native keypad event" << event->type() << object->metaObject()->className()
                        << key->key() << key->modifiers() << key->nativeScanCode() << key->nativeVirtualKey();
        }
        return false;
    }
};
QVariant evaluate(QObject *object, const QString &code)
{
    QQmlExpression expression(qmlContext(object), object, code);
    const QVariant value = expression.evaluate();
    if (expression.hasError())
        QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
    return value;
}

QQuickItem *resultDelegate(QQuickItem *item, const char *role, const QVariant &value)
{
    if (item->property(role) == value)
        return item;
    for (auto *child : item->childItems()) {
        if (auto *result = resultDelegate(child, role, value))
            return result;
    }
    return nullptr;
}

void wheel(QQuickItem *view, QPoint angleDelta, QPoint pixelDelta = {}, Qt::KeyboardModifiers modifiers = Qt::ControlModifier)
{
    auto *window = view->window();
    const QPointF position = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
    QWheelEvent event(position, window->mapToGlobal(position.toPoint()), pixelDelta, angleDelta,
                      Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
}
}

class SearchNavigationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void pdfWheelAndSearchNavigation();
    void epubWheelAndSearchNavigation();
    void epubReferenceUnderlinesAndHover();
    void epubWidthPersistsAcrossReload_data();
    void epubWidthPersistsAcrossReload();
    void epubCrossReferenceReload();
    void nativeReferenceDialogState();
    void nativeReferenceTargetTree();
};

#ifndef Q_MOC_RUN
void SearchNavigationTest::nativeReferenceDialogState()
{
    // Delayed real HTTP replies exercise the native request timers and generation guards.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    struct Pending { QByteArray path; QPointer<QTcpSocket> socket; };
    QList<Pending> pending;
    bool authorized = true;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const QByteArray buffer = socket->property("requestBuffer").toByteArray() + socket->readAll();
                socket->setProperty("requestBuffer", buffer);
                if (!buffer.contains("\r\n\r\n") || socket->property("recorded").toBool()) return;
                socket->setProperty("recorded", true);
                authorized &= buffer.toLower().contains("x-arianna-session-token: native-test");
                pending.append({buffer.split(' ').value(1), socket});
            });
        }
    });
    const auto count = [&](const QByteArray &path) {
        return std::count_if(pending.cbegin(), pending.cend(), [&](const Pending &request) { return request.path == path; });
    };
    const auto reply = [&](const QByteArray &path, const QByteArray &body) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->path != path) continue;
            if (it->socket && it->socket->state() == QAbstractSocket::ConnectedState) {
                it->socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ")
                    + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                it->socket->disconnectFromHost();
            }
            pending.erase(it);
            return true;
        }
        return false;
    };
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testServerUrl"), QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            visible: true; width: 1000; height: 800
            CrossReferenceEditor { serverUrl: testServerUrl; sessionToken: "native-test" }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    auto *editor = window->findChild<QObject *>(QStringLiteral("crossReferenceEditor"));
    QVERIFY(editor);
    const auto begin = [&] { evaluate(editor, QStringLiteral("begin('source','epubcfi(/6/2!/4/2/1:0)','selected',"
        "{targetBookId:'source',targetLocation:'chapter.xhtml#saved',targetPreviewHtml:''})")); };
    begin();
    QTRY_COMPARE(count("/books"), 1);
    QCOMPARE(editor->property("bookState").toString(), QStringLiteral("loading"));
    QVERIFY(!editor->property("canSave").toBool());
    evaluate(editor, QStringLiteral("close()"));
    QTRY_VERIFY(!editor->property("visible").toBool());
    begin();
    QTRY_COMPARE(count("/books"), 2);
    QVERIFY(reply("/books", R"({"books":[{"id":"stale","title":"Old reply"}]})"));
    QCOMPARE(editor->property("bookState").toString(), QStringLiteral("loading"));
    QVERIFY(reply("/books", R"({"books":[
        {"id":"target","title":"Target","author":["Foreign"],"subjects":["Science"]},
        {"id":"source","title":"Source","author":["Writer"],"subjects":["Religion/Bhakti"]},
        {"id":"authorZ","title":"Zeta","author":["Writer"],"subjects":["Religion"]},
        {"id":"topicD","title":"Delta","author":["Other"],"subjects":["Religion/Yoga"]},
        {"id":"authorA","title":"Alpha","author":["Writer"]},
        {"id":"otherA","title":"Aardvark","author":["Foreign"]},
        {"id":"topicB","title":"Beta","author":["Other"],"subjects":["Religion/History"]}
    ]})"));
    QTRY_COMPARE(count("/source/targetLocations"), 1);
    QCOMPARE(evaluate(editor, QStringLiteral("JSON.stringify(bookRows.map(row=>row.value))")).toString(),
             QStringLiteral("[\"source\",\"authorA\",\"authorZ\",\"topicB\",\"topicD\",\"otherA\",\"target\"]"));
    QVERIFY(evaluate(editor, QStringLiteral("bookRows[1].group.includes('Writer') && bookRows[3].group.includes('Religion')")).toBool());
    QCOMPARE(count("/source/refs"), 0);
    auto *bookSearch = editor->findChild<QObject *>(QStringLiteral("crossReferenceBooks"));
    QVERIFY(bookSearch);
    const auto visibleItems = [&](const QString &name) {
        std::function<int(QQuickItem *)> countItems = [&](QQuickItem *item) {
            int count = item->objectName() == name && item->isVisible() ? 1 : 0;
            for (auto *child : item->childItems()) count += countItems(child);
            return count;
        };
        return countItems(qobject_cast<QQuickItem *>(bookSearch));
    };
    evaluate(bookSearch, QStringLiteral("query='Writer'"));
    QCOMPARE(evaluate(bookSearch, QStringLiteral("matches.count")).toInt(), 3);
    const auto groupScreenshot = qEnvironmentVariable("ARIANNA_REFERENCE_GROUP_SCREENSHOT");
    if (!groupScreenshot.isEmpty()) {
        QTest::qWait(250);
        QVERIFY(qobject_cast<QQuickWindow *>(window.get())->grabWindow().save(groupScreenshot));
    }
    QTRY_COMPARE(visibleItems(QStringLiteral("crossReferenceBooksGroupHeading")), 2);
    QCOMPARE(visibleItems(QStringLiteral("crossReferenceBooksGroupSeparator")), 1);
    evaluate(bookSearch, QStringLiteral("query='Alpha'"));
    QTRY_COMPARE(visibleItems(QStringLiteral("crossReferenceBooksGroupHeading")), 1);
    QCOMPARE(visibleItems(QStringLiteral("crossReferenceBooksGroupSeparator")), 0);
    evaluate(bookSearch, QStringLiteral("query=''"));
    if (!groupScreenshot.isEmpty()) {
        QTest::qWait(250);
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow && quickWindow->grabWindow().save(groupScreenshot));
    }
    QCOMPARE(editor->property("targetState").toString(), QStringLiteral("loading"));
    // A pending TOC must preserve an explicit location and a deliberately cleared preview.
    evaluate(editor, QStringLiteral("targetLocation='chapter.xhtml#manual';locationEdited=true;previewHtml='';previewEdited=true;"
                                   "sourceAnchorTitle='My title';autoTitle=false"));
    QVERIFY(reply("/source/targetLocations", R"({"targetLocations":[{"id":"saved","hrefLocation":"chapter.xhtml#saved","previewHtml":"Automatic preview"}]})"));
    QTRY_COMPARE(editor->property("targetState").toString(), QStringLiteral("ready"));
    QCOMPARE(editor->property("targetLocation").toString(), QStringLiteral("chapter.xhtml#manual"));
    QCOMPARE(editor->property("previewHtml").toString(), QString());
    QCOMPARE(editor->property("sourceAnchorTitle").toString(), QStringLiteral("My title"));
    // Changing book cancels the preceding request. A late reply cannot replace its results.
    evaluate(editor, QStringLiteral("loadTargets();chooseBook(bookRows.find(row=>row.value==='target'))"));
    QTRY_COMPARE(count("/source/targetLocations"), 1);
    QTRY_COMPARE(count("/target/targetLocations"), 1);
    QVERIFY(reply("/source/targetLocations", R"({"targetLocations":[{"id":"stale","location":"wrong.xhtml"}]})"));
    QCOMPARE(editor->property("targetState").toString(), QStringLiteral("loading"));
    evaluate(editor, QStringLiteral("targetLocation='chapter.xhtml#manual';locationEdited=true;previewHtml='';previewEdited=true"));
    auto *request = editor->findChild<QObject *>(QStringLiteral("crossReferenceTargetsRequest"));
    QVERIFY(request);
    auto *deadline = request->property("deadline").value<QObject *>();
    QVERIFY(deadline);
    deadline->setProperty("interval", 25);
    QTRY_COMPARE(editor->property("targetState").toString(), QStringLiteral("error"));
    QVERIFY(!editor->property("targetError").toString().isEmpty());
    QVERIFY(!editor->property("canSave").toBool());
    QVERIFY(reply("/target/targetLocations", R"({"targetLocations":[]})"));
    evaluate(editor, QStringLiteral("loadTargets()"));
    QTRY_COMPARE(count("/target/targetLocations"), 1);
    QVERIFY(reply("/target/targetLocations", R"({"targetLocations":[]})"));
    QTRY_COMPARE(editor->property("targetState").toString(), QStringLiteral("ready"));
    QCOMPARE(editor->property("targetLocation").toString(), QStringLiteral("chapter.xhtml#manual"));
    QCOMPARE(editor->property("previewHtml").toString(), QString());
    QVERIFY(editor->property("canSave").toBool());
    QVERIFY(authorized);
    evaluate(editor, QStringLiteral("close()"));
}

void SearchNavigationTest::nativeReferenceTargetTree()
{
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Layouts
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            id: window
            visible: true; width: 900; height: 650
            property string chosen: "manual.xhtml#untouched"
            pageStack.initialPage: Kirigami.Page {
            ReferenceTargetTree {
                objectName: "treeUnderTest"
                anchors.fill: parent
                selectedValue: window.chosen
                onSelected: row => window.chosen = row.value
                rows: [
                    {value:"one.xhtml#verse_18-20",label:"Needleone",searchKey:"needleone part chapter",data:{tocPath:["Part","Chapter"],tocIdPath:["p1","c1"],type:"element"}},
                    {value:"one.xhtml#chapter",label:"Chapter",searchKey:"chapter part",data:{tocPath:["Part","Chapter"],tocIdPath:["p1","c1"],type:"navigation"}},
                    {value:"two.xhtml#verse",label:"Needletwo",searchKey:"needletwo part chapter",data:{tocPath:["Part","Chapter"],tocIdPath:["p2","c2"],type:"element"}}
                ]
            }
            }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    auto *tree = object->findChild<QObject *>(QStringLiteral("treeUnderTest"));
    QVERIFY(tree);
    auto *view = tree->property("treeView").value<QQuickItem *>();
    QVERIFY(view);
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 4); // Roots initially expanded one level.
    evaluate(tree, QStringLiteral("treeView.collapse(0)"));
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 3);
    evaluate(tree, QStringLiteral("query='needleone'"));
    QCOMPARE(tree->property("matchCount").toInt(), 1);
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 3);
    QTRY_VERIFY(evaluate(tree, QStringLiteral("treeView.isExpanded(0) && treeView.isExpanded(1)")).toBool());
    QCOMPARE(object->property("chosen").toString(), QStringLiteral("manual.xhtml#untouched"));
    evaluate(tree, QStringLiteral("query=''"));
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 3);
    QTRY_VERIFY(!evaluate(tree, QStringLiteral("treeView.isExpanded(0)")).toBool()); // Manual collapse restored.
    evaluate(tree, QStringLiteral("query='needleone'"));
    QTRY_VERIFY(evaluate(tree, QStringLiteral("treeView.isExpanded(1)")).toBool());
    QTest::qWait(100); // TreeView reuses delegates during the filter's deferred layout.
    auto *chapter = resultDelegate(view, "targetValue", QStringLiteral("one.xhtml#chapter"));
    QVERIFY(chapter);
    auto *indicator = chapter->property("indicator").value<QQuickItem *>();
    QVERIFY(indicator);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, indicator->mapToScene(QPointF(indicator->width()/2, indicator->height()/2)).toPoint());
    QTRY_VERIFY(!evaluate(tree, QStringLiteral("treeView.isExpanded(1)")).toBool());
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 2);
    QCOMPARE(object->property("chosen").toString(), QStringLiteral("manual.xhtml#untouched")); // Indicator never selects the chapter.
    evaluate(tree, QStringLiteral("query='needleone ';searchField.forceActiveFocus()"));
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 3);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(object->property("chosen").toString(), QStringLiteral("one.xhtml#verse_18-20"));
    evaluate(tree, QStringLiteral("query='absent'"));
    QTRY_COMPARE(evaluate(tree, QStringLiteral("treeView.rows")).toInt(), 0);
    QCOMPARE(object->property("chosen").toString(), QStringLiteral("one.xhtml#verse_18-20"));
    evaluate(tree, QStringLiteral("query=''"));
    QTRY_VERIFY(resultDelegate(view, "targetValue", QStringLiteral("one.xhtml#verse_18-20"))); // Selected target revealed.
    const auto screenshot = qEnvironmentVariable("ARIANNA_REFERENCE_TREE_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshot));
    }
}

void SearchNavigationTest::pdfWheelAndSearchNavigation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = dir.filePath(QStringLiteral("search.pdf"));
    {
        QPdfWriter writer(fileName);
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.drawText(80, 150, QStringLiteral("needle first"));
        painter.drawText(80, 600, QStringLiteral("needle second"));
        QVERIFY(writer.newPage());
        painter.drawText(80, 150, QStringLiteral("needle third"));
    }
    OkularPdfDocument document;
    document.setSource(QUrl::fromLocalFile(fileName));
    QVERIFY(document.opened());
    const QVariantList matches = document.searchText(QStringLiteral("needle"));
    QCOMPARE(matches.size(), 3);

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testDocument"), &document);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.arianna
        OkularPdfView {
            width: 600; height: 500
            document: testDocument
            renderScale: 2
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto *view = qobject_cast<QQuickItem *>(object.get());
    QVERIFY(view);
    QQuickWindow window;
    window.resize(600, 500);
    view->setParentItem(window.contentItem());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(view->setProperty("searchString", QStringLiteral("needle")));
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 0);
    QSignalSpy zoom(view, SIGNAL(zoomRequested(double)));
    QSignalSpy section(view, SIGNAL(sectionNavigationRequested(bool)));
    QVERIFY(zoom.isValid());
    QVERIFY(section.isValid());

    // The next match is far down the same enlarged page: it must be brought into view.
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 1);
    QCOMPARE(view->property("currentPage").toInt(), 0);
    auto *page = view->findChild<OkularPdfPageItem *>();
    QVERIFY(page);
    const auto rectangle = matches.at(1).toMap().value(QStringLiteral("rectangles")).toList().first().toMap();
    const auto hitY = [&] {
        return page->mapToItem(view, QPointF(0, (rectangle.value(QStringLiteral("y")).toDouble()
                                                + rectangle.value(QStringLiteral("height")).toDouble() / 2) * page->height())).y();
    };
    QTRY_VERIFY(qAbs(hitY() - view->height() / 2) < 2);
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 2);
    QCOMPARE(view->property("currentPage").toInt(), 1);
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 0);
    wheel(view, {}, QPoint(0, 30), Qt::ShiftModifier);
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 2);
    QCOMPARE(zoom.count(), 0);
    QCOMPARE(section.count(), 0);

    // Clicking a dialog result resets the starting point for subsequent search navigation.
    QVERIFY(QMetaObject::invokeMethod(view, "openSearchDialog"));
    QQuickItem *delegate = nullptr;
    QTRY_VERIFY((delegate = resultDelegate(window.contentItem(), "excerpt", matches.first().toMap().value(QStringLiteral("excerpt")))));
    QVERIFY(QMetaObject::invokeMethod(delegate, "clicked"));
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 0);
    QTest::qWait(300); // Let the popup exit transition finish before sending input to the viewer.
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), 1);

    QSignalSpy spacing(view, SIGNAL(pageSpacingChangeRequested(int)));
    QVERIFY(spacing.isValid());
    wheel(view, QPoint(120, 0)); // Single-page PDF: no gap to adjust.
    QCOMPARE(zoom.count(), 0);
    QCOMPARE(spacing.count(), 0);
    wheel(view, QPoint(0, 120));
    QCOMPARE(zoom.count(), 1);
    QVERIFY(view->setProperty("twoPageMode", true));
    wheel(view, QPoint(120, 0));
    QCOMPARE(spacing.count(), 1);
    QCOMPARE(spacing.last().first().toInt(), 10);
    wheel(view, {}, QPoint(-30, 0));
    QCOMPARE(spacing.last().first().toInt(), -10);
    QCOMPARE(zoom.count(), 1);
    wheel(view, QPoint(120, 0), {}, Qt::NoModifier);
    QCOMPARE(section.count(), 1);
    QVERIFY(view->setProperty("twoPageMode", false));
    QVERIFY(view->setProperty("currentPage", 0));
    wheel(view, QPoint(0, -120), {}, Qt::NoModifier);
    QCOMPARE(view->property("currentPage").toInt(), 1);
    wheel(view, {}, QPoint(0, 30), Qt::NoModifier);
    QCOMPARE(view->property("currentPage").toInt(), 0);
    // Only the navigated outline disappears, in either page layout. A new tick
    // restores the previous match and gives the new target its own half second.
    QVERIFY(view->setProperty("renderScale", 0.4));
    for (const bool twoPages : {false, true}) {
        QVERIFY(view->setProperty("twoPageMode", twoPages));
        evaluate(view, QStringLiteral("goToSearchResult(0)"));
        QQuickItem *first = nullptr;
        QTRY_VERIFY((first = resultDelegate(view, "searchResultIndex", 0)));
        QTRY_VERIFY(first->isVisible());
        wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
        QQuickItem *second = nullptr;
        QTRY_VERIFY((second = resultDelegate(view, "searchResultIndex", 1)));
        QVERIFY(!second->isVisible());
        QVERIFY(first->isVisible());
        QTest::qWait(200);
        QVERIFY(!second->isVisible());
        QElapsedTimer flashTime;
        flashTime.start();
        wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
        QQuickItem *third = nullptr;
        QTRY_VERIFY((third = resultDelegate(view, "searchResultIndex", 2)));
        QVERIFY(!third->isVisible());
        if (twoPages) {
            QTRY_VERIFY((second = resultDelegate(view, "searchResultIndex", 1)));
            QVERIFY(second->isVisible());
        }
        QTest::qWait(200);
        QVERIFY(!third->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(third->isVisible(), 1000);
        QVERIFY(flashTime.elapsed() >= 450);
        QCOMPARE(evaluate(view, QStringLiteral("searchModel.count")).toInt(), 3);
    }
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QVERIFY(view->setProperty("searchString", QStringLiteral("absent")));
    QTest::qWait(600);
    QVERIFY(!resultDelegate(view, "searchResultIndex", 0));
    const int currentPage = view->property("currentPage").toInt();
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    wheel(view, {}, QPoint(0, 30), Qt::ShiftModifier);
    QCOMPARE(evaluate(view, QStringLiteral("searchModel.currentResult")).toInt(), -1);
    QCOMPARE(view->property("currentPage").toInt(), currentPage);
    QCOMPARE(zoom.count(), 1);
    QCOMPARE(section.count(), 1);
}

void SearchNavigationTest::epubWheelAndSearchNavigation()
{
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 900; height: 700; visible: true
            property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
            contextDrawer: Kirigami.ContextDrawer {
                signal goTo(var location)
            }
            pageStack.initialPage: EpubViewerPage { objectName: "testPage"; entry: ({ uniqueIdentifier: "" }) }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *view = object->findChild<QQuickItem *>(QStringLiteral("testPage"));
    auto *model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(view);
    QVERIFY(model);
    // Use deterministic results to test the real QML input and selection path independently of book loading.
    evaluate(model, QStringLiteral(R"JS(
        resultFound("needle", [0, 1, 2].map(i => ({
            cfi: "epubcfi(/6/" + (2 + 2 * i) + "!/4/2:0)",
            excerpt: { pre: "", match: "needle", post: " " + i }, section: "Chapter " + i
        })))
    )JS"));
    QSignalSpy selected(model, SIGNAL(resultSelected(QString,bool)));
    QSignalSpy zoom(view, SIGNAL(zoomLevelSaved(double)));
    QVERIFY(selected.isValid());
    QVERIFY(zoom.isValid());
    const int initialWidth = evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt();
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 0);
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 1);
    wheel(view, {}, QPoint(0, 30), Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 0);
    wheel(view, {}, QPoint(0, 30), Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 2);
    QCOMPARE(selected.last().first().toString(), QStringLiteral("epubcfi(/6/6!/4/2:0)"));
    QVERIFY(selected.last().at(1).toBool());
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 0);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), initialWidth);
    QCOMPARE(zoom.count(), 0);

    evaluate(model, QStringLiteral("searchDialog.open()"));
    QQuickItem *delegate = nullptr;
    QTRY_VERIFY((delegate = resultDelegate(window->contentItem(), "cfi", QStringLiteral("epubcfi(/6/4!/4/2:0)"))));
    QVERIFY(QMetaObject::invokeMethod(delegate, "clicked"));
    QCOMPARE(model->property("currentResult").toInt(), 1);
    QVERIFY(!selected.last().at(1).toBool());
    QTest::qWait(300);
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 2);
    wheel(view, QPoint(0, 120));
    QCOMPARE(zoom.count(), 1);
    const int selectedCount = selected.count();
    evaluate(model, QStringLiteral("search('new query')"));
    QVERIFY(model->property("loading").toBool());
    QCOMPARE(model->property("currentResult").toInt(), -1);
    wheel(view, QPoint(0, -120), {}, Qt::ShiftModifier);
    QCOMPARE(selected.count(), selectedCount);
    evaluate(model, QStringLiteral("resultFound('new query', [])"));
    wheel(view, {}, QPoint(0, 30), Qt::ShiftModifier);
    QCOMPARE(selected.count(), selectedCount);
    QCOMPARE(zoom.count(), 1);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), initialWidth);

    // The two modified wheel axes act independently of the active search.
    wheel(view, QPoint(120, 0));
    QCOMPARE(evaluate(model, QStringLiteral("root.pageWidthDistance")).toInt(), initialWidth + 25);
    wheel(view, {}, QPoint(-30, 0));
    QCOMPARE(evaluate(model, QStringLiteral("root.pageWidthDistance")).toInt(), initialWidth);
    QCOMPARE(zoom.count(), 1);

    // Keep the keypad alternative scoped to the reading area.
    evaluate(model, QStringLiteral("focusScope.forceActiveFocus()"));
    QTest::keyClick(window, Qt::Key_Plus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), initialWidth + 25);
    QTest::keyClick(window, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), initialWidth);
    QTest::keyClick(window, Qt::Key_Plus);
    QTest::keyClick(window, Qt::Key_Minus);
    QTest::keyClick(window, Qt::Key_Plus, Qt::ControlModifier | Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), initialWidth);
    evaluate(model, QStringLiteral("root.pageWidthDistance = 110"));
    QTest::keyClick(window, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), 100);
    QTest::keyClick(window, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), 100);
    QCOMPARE(zoom.count(), 1);

    evaluate(model, QStringLiteral("view.visible = true; view.forceActiveFocus()"));
    QTRY_VERIFY(evaluate(model, QStringLiteral("view.activeFocus")).toBool());
    QTest::keyClick(window, Qt::Key_Plus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), 125);
    QTest::keyClick(window, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), 100);

    // Search field input must not resize the document.
    evaluate(model, QStringLiteral("searchDialog.open()"));
    QTRY_VERIFY(!evaluate(model, QStringLiteral("focusScope.activeFocus || view.activeFocus")).toBool());
    QTest::keyClick(window, Qt::Key_Plus, Qt::KeypadModifier);
    QTest::keyClick(window, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(evaluate(model, QStringLiteral("root.effectivePageWidth")).toInt(), 100);
}

#ifndef Q_MOC_RUN
void SearchNavigationTest::epubReferenceUnderlinesAndHover()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("references.epub"));
    KZip zip(path);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
        R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
    QVERIFY(zip.writeFile(QStringLiteral("content.opf"),
        R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">references-test</dc:identifier><dc:title>References</dc:title><dc:language>en</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), R"(
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>References</title></head><body>
<p><span id="span-ref" data-role="anchor" data-anchor-type="crossref" title="Span tooltip"><em>Span cross-reference</em></span></p>
<p><a id="a-ref" data-role="anchor" data-anchor-type="crossref" title="Anchor tooltip">Anchor cross-reference</a></p>
<p><span id="legacy-ref" class="bookref" data-ref="legacy">Legacy reference</span></p>
<p><a id="internal-ref" data-role="anchor" href="#target">Internal reference</a></p>
<p><a id="ordinary-link" href="#target">Ordinary link</a></p>
<p><span id="annotation" data-role="anchor" data-anchor-type="annotation">Personal annotation</span></p>
<p id="target">Target text</p>
</body></html>)"));
    QVERIFY(zip.close());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString url = QStringLiteral("data:application/epub+zip;base64,") + QString::fromLatin1(file.readAll().toBase64());
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), QStringLiteral("http://127.0.0.1"));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1100; height: 850; visible: true
            property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
            contextDrawer: Kirigami.ContextDrawer {
                property string currentTocId: ""
                property string currentHref: ""
                property var model: QtObject { function importFromJson(json) {} }
                function clearCurrentItem() {}
                signal goTo(var location)
            }
            pageStack.initialPage: Kirigami.Page {}
            Component.onCompleted: pageStack.layers.push(readerPage)
            Component {
                id: readerPage
                EpubViewerPage {
                    objectName: "reader"
                    entry: ({uniqueIdentifier: ""})
                    pageMode: "single"
                    property bool scriptDone: false
                    property var scriptResult
                }
            }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window && QTest::qWaitForWindowExposed(window));
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("reader"));
    auto *model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(page && model);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.readerReady")).toBool(), 15000);
    QVERIFY(page->setProperty("url", QStringLiteral("references.epub")));
    const QString encodedUrl = QString::fromUtf8(QJsonDocument(QJsonArray{url}).toJson(QJsonDocument::Compact));
    evaluate(model, QStringLiteral("view.runJavaScript('openSync(' + JSON.stringify(%1[0]) + ', null, \"\", ' + JSON.stringify(root.currentReaderLayout()) + ')')").arg(encodedUrl));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    auto *webView = evaluate(model, QStringLiteral("view")).value<QQuickItem *>();
    QVERIFY(webView);
    const auto script = [&](const QString &code) {
        const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(model, QStringLiteral("root.scriptDone = false; view.runJavaScript(%1[0], value => {root.scriptResult = value; root.scriptDone = true})").arg(encoded));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000)
            QTest::qWait(10);
        if (!page->property("scriptDone").toBool())
            QTest::qFail("Reader JavaScript timed out", __FILE__, __LINE__);
        return page->property("scriptResult");
    };
    script(QStringLiteral(R"JS(
        globalThis.referenceStyle = id => {
            const doc = reader.view.renderer.getContents()[0].doc;
            const element = doc.getElementById(id);
            const style = getComputedStyle(element);
            const rect = element.getBoundingClientRect();
            const frame = doc.defaultView.frameElement.getBoundingClientRect();
            return {border: style.borderBottomStyle, width: parseFloat(style.borderBottomWidth),
                color: style.borderBottomColor, textColor: style.color,
                icon: getComputedStyle(element, '::before').content,
                hovered: element.matches(':hover'), title: element.title,
                x: frame.left + rect.x + rect.width / 2, y: frame.top + rect.y + rect.height / 2};
        };
    )JS"));
    // Wait for the real reader stylesheet, then check all supported source elements.
    QTRY_COMPARE(script(QStringLiteral("referenceStyle('a-ref').border")).toString(), QStringLiteral("dashed"));
    for (int theme = 0; theme < 3; ++theme) {
        QVERIFY(page->setProperty("currentReaderTheme", theme));
        evaluate(model, QStringLiteral("backend.applyStyle()"));
        QTest::qWait(100);
        for (const auto &id : {QStringLiteral("span-ref"), QStringLiteral("a-ref"), QStringLiteral("legacy-ref"), QStringLiteral("internal-ref")}) {
            QTest::mouseMove(window, QPoint(5, 5));
            const QString stateScript = QStringLiteral("referenceStyle('%1')").arg(id);
            QTRY_COMPARE(script(stateScript + QStringLiteral(".border")).toString(), QStringLiteral("dashed"));
            auto state = script(stateScript).toMap();
            QVERIFY(state.value(QStringLiteral("width")).toDouble() >= 1);
            QCOMPARE(state.value(QStringLiteral("color")), state.value(QStringLiteral("textColor")));
            QCOMPARE(state.value(QStringLiteral("icon")).toString(), QStringLiteral("\"\""));
            const QPointF position(state.value(QStringLiteral("x")).toDouble(), state.value(QStringLiteral("y")).toDouble());
            QVERIFY(webView->contains(position));
            // Enter the WebEngine item first; Qt delivers only HoverEnter on crossing its edge.
            QTest::mouseMove(window, webView->mapToScene(QPointF(10, 10)).toPoint());
            QTest::mouseMove(window, webView->mapToScene(position).toPoint());
            QTRY_VERIFY(script(stateScript + QStringLiteral(".hovered")).toBool());
            QTRY_COMPARE(script(stateScript + QStringLiteral(".border")).toString(), QStringLiteral("solid"));
            if (theme == 0 && id == QLatin1String("span-ref") && !qEnvironmentVariable("ARIANNA_TEST_SCREENSHOT").isEmpty())
                QVERIFY(window->grabWindow().save(qEnvironmentVariable("ARIANNA_TEST_SCREENSHOT")));
            QTest::mouseMove(window, QPoint(5, 5));
            QTRY_COMPARE(script(stateScript + QStringLiteral(".border")).toString(), QStringLiteral("dashed"));
        }
        QCOMPARE(script(QStringLiteral("referenceStyle('ordinary-link').border")).toString(), QStringLiteral("none"));
        QCOMPARE(script(QStringLiteral("referenceStyle('annotation').border")).toString(), QStringLiteral("none"));
        QCOMPARE(script(QStringLiteral("referenceStyle('span-ref').title")).toString(), QStringLiteral("Span tooltip"));
    }
}

#endif

void SearchNavigationTest::epubWidthPersistsAcrossReload_data()
{
    QTest::addColumn<QString>("pageMode");
    QTest::addColumn<double>("zoom");
    QTest::newRow("single") << QStringLiteral("single") << 1.0;
    QTest::newRow("two") << QStringLiteral("two") << 1.0;
    QTest::newRow("two-zoomed") << QStringLiteral("two") << 1.5;
}

void SearchNavigationTest::epubWidthPersistsAcrossReload()
{
    QFETCH(QString, pageMode);
    QFETCH(double, zoom);
    QTemporaryDir directory;
    const QString epubPath = directory.filePath(QStringLiteral("width.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArrayLiteral(
        "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles>"
        "<rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>")));
    QVERIFY(zip.writeFile(QStringLiteral("content.opf"), QByteArrayLiteral(
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">width-test</dc:identifier>"
        "<dc:title>Width Test</dc:title><dc:language>en</dc:language></metadata><manifest>"
        "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"other\" href=\"other.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"chapter\"/><itemref idref=\"other\"/></spine></package>")));
    QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), QByteArrayLiteral(
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Width</title></head><body>"
        "<p id=\"text\">A needle, another needle and a third needle for checking the actual EPUB page width after a keypad shortcut.</p>"
        "<p id=\"note2\" style=\"break-before:column\">Second remark.</p>"
        "<p id=\"note3\" style=\"break-before:column\">Third remark.</p>"
        "</body></html>")));
    QVERIFY(zip.writeFile(QStringLiteral("other.xhtml"), QByteArrayLiteral(
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Other</title></head><body>"
        "<p>A needle in another section.</p></body></html>")));
    QVERIFY(zip.close());
    QFile epub(epubPath);
    QVERIFY(epub.open(QIODevice::ReadOnly));

    BookEntry book;
    book.filename = epubPath;
    book.pageMode = pageMode;
    book.zoomLevel = zoom;
    book.pageWidthDistance = 250;
    BookDatabase::self().addEntry(book);
    BookListModel library;
    library.componentComplete();
    QTRY_VERIFY(library.cacheLoaded());
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibrary"), &library);
    engine.rootContext()->setContextProperty(QStringLiteral("testBook"), QVariant::fromValue(book));
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testEpubUrl"),
        QStringLiteral("data:application/epub+zip;base64,%1").arg(QString::fromLatin1(epub.readAll().toBase64())));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), QStringLiteral("http://127.0.0.1"));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1000; height: 800; visible: true
            property var bookListModel: testLibrary
            contextDrawer: Kirigami.ContextDrawer {
                property string currentTocId: ""
                property string currentHref: ""
                property var model: QtObject { function importFromJson(json) {} }
                function clearCurrentItem() {}
                signal goTo(var location)
            }
            pageStack.initialPage: Kirigami.Page {}
            Component.onCompleted: pageStack.layers.push(readerPage)
            Component {
                id: readerPage
                EpubViewerPage {
                    objectName: "testPage"
                    url: "width.epub"
                    entry: testBook
                    filename: testBook.filename
                    pageMode: testBook.pageMode
                    zoomLevel: testBook.zoomLevel
                    onPageWidthDistanceSaved: value => testLibrary.setBookData(filename, "pageWidthDistance", value)
                    onZoomLevelSaved: value => testLibrary.setBookData(filename, "zoomLevel", value)
                    property bool scriptDone: false
                    property var scriptResult
                }
            }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("testPage"));
    auto *model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(window && page && model);
    QVERIFY(page->setProperty("pageMode", pageMode));
    evaluate(model, QStringLiteral("view.zoomFactor = %1").arg(zoom));
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.readerReady")).toBool(), 15000);
    evaluate(model, QStringLiteral("view.runJavaScript('openSync(' + JSON.stringify(testEpubUrl) + ', null, \"\", ' + JSON.stringify(root.currentReaderLayout()) + ')')"));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);

    const auto script = [&](const QString &code) -> QVariant {
        const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(model, QStringLiteral("root.scriptDone = false; view.runJavaScript(%1[0], value => {root.scriptResult = value; root.scriptDone = true})").arg(encoded));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000)
            QTest::qWait(10);
        if (!page->property("scriptDone").toBool())
            QTest::qFail("Reader JavaScript timed out", __FILE__, __LINE__);
        return page->property("scriptResult");
    };
    const auto pageWidth = [&] {
        return script(QStringLiteral("reader.view.renderer.getContents()[0]?.doc.getElementById('text').getBoundingClientRect().width")).toDouble();
    };
    QTRY_VERIFY(pageWidth() > 100);
    // Exercise actual EPUB search outlines through QML, WebEngine and the SVG
    // overlay, for single pages and spreads (including a zoomed spread).
    evaluate(model, QStringLiteral("search('needle')"));
    QTRY_VERIFY_WITH_TIMEOUT(!model->property("loading").toBool(), 10000);
    QCOMPARE(model->property("count").toInt(), 4);
    script(QStringLiteral(R"JS(
        globalThis.outlines = () => Array.from(reader.view.renderer.getContents()[0]
            .overlayer.element.children).filter(el => el.getAttribute('stroke'));
        globalThis.hiddenOutlines = () => outlines().filter(el => getComputedStyle(el).visibility === 'hidden');
        globalThis.hiddenTarget = cfi => {
            const { index, anchor } = reader.view.resolveNavigation(cfi);
            const content = reader.view.renderer.getContents().find(item => item.index === index);
            if (!content || hiddenOutlines().length !== 1) return false;
            const rect = anchor(content.doc).getClientRects()[0];
            const outline = hiddenOutlines()[0].querySelector('rect');
            return Math.abs(Number(outline.getAttribute('x')) - rect.x) < 1
                && Math.abs(Number(outline.getAttribute('y')) - rect.y) < 1;
        };
    )JS"));
    const auto hiddenMatchesCurrent = [&] {
        const QString cfi = evaluate(model, QStringLiteral("get(currentResult).cfi")).toString();
        const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{cfi}).toJson(QJsonDocument::Compact));
        return script(QStringLiteral("hiddenTarget(%1[0])").arg(encoded)).toBool();
    };
    QTRY_COMPARE(script(QStringLiteral("outlines().length")).toInt(), 3);
    wheel(page, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 0);
    QTRY_VERIFY(hiddenMatchesCurrent());
    QCOMPARE(script(QStringLiteral("outlines().length")).toInt(), 3);
    QTest::qWait(150);
    QVERIFY(hiddenMatchesCurrent());
    // A redraw must keep the current target hidden; rapid ticks restore the
    // previous target immediately and must never hide two results at once.
    script(QStringLiteral("reader.view.renderer.getContents()[0].overlayer.redraw()"));
    QVERIFY(hiddenMatchesCurrent());
    wheel(page, QPoint(0, -120), {}, Qt::ShiftModifier);
    wheel(page, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 2);
    QTRY_VERIFY(hiddenMatchesCurrent());
    QElapsedTimer flashTime;
    flashTime.start();
    QTest::qWait(200);
    QVERIFY(hiddenMatchesCurrent());
    QTRY_COMPARE_WITH_TIMEOUT(script(QStringLiteral("hiddenOutlines().length")).toInt(), 0, 1000);
    QVERIFY(flashTime.elapsed() >= 400);
    QCOMPARE(script(QStringLiteral("outlines().length")).toInt(), 3);
    // A section load completes before the half-second indication starts.
    wheel(page, QPoint(0, -120), {}, Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 3);
    QTRY_VERIFY(hiddenMatchesCurrent());
    QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].index")).toInt(), 1);
    QTest::qWait(200);
    QVERIFY(hiddenMatchesCurrent());
    QTRY_COMPARE_WITH_TIMEOUT(script(QStringLiteral("hiddenOutlines().length")).toInt(), 0, 1000);
    // Reverse navigation works across sections as well.
    wheel(page, {}, QPoint(0, 30), Qt::ShiftModifier);
    QTRY_COMPARE(model->property("currentResult").toInt(), 2);
    QTRY_VERIFY(hiddenMatchesCurrent());
    // Starting a new search during the flash must not resurrect stale outlines.
    evaluate(model, QStringLiteral("search('absent')"));
    QTRY_VERIFY(!model->property("loading").toBool());
    QTest::qWait(600);
    QCOMPARE(script(QStringLiteral("outlines().length")).toInt(), 0);
    QCOMPARE(model->property("count").toInt(), 0);
    // Generate valid CFI targets from the rendered EPUB, deliberately out of document order.
    const QString notes = script(QStringLiteral(R"JS(JSON.stringify(['note3', 'text', 'note2'].map(id => {
        const doc = reader.view.renderer.getContents()[0].doc;
        const range = doc.createRange();
        range.selectNodeContents(doc.getElementById(id));
        return { annotationId: id, value: reader.view.getCFI(0, range), note: id };
    })))JS")).toString();
    QVERIFY(!notes.isEmpty());
    evaluate(model, QStringLiteral("backend.annotations = %1; backend.annotations.push(Object.assign({}, backend.annotations[2], {annotationId: 'invalid', invalid: true})); backend.updateNotesModel(); backend.goToAnnotation(backend.annotations[2])").arg(notes));
    QTRY_COMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("note2"));
    wheel(page, QPoint(-120, 0), {}, Qt::ShiftModifier);
    QTRY_COMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("note3"));
    QTRY_VERIFY(script(QStringLiteral("reader.view.lastLocation.range.intersectsNode(reader.view.renderer.getContents()[0].doc.getElementById('note3'))")).toBool());
    wheel(page, {}, QPoint(-30, 0), Qt::ShiftModifier);
    QTRY_COMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("text"));
    wheel(page, QPoint(120, 0), {}, Qt::ShiftModifier);
    QTRY_COMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("note3"));
    // Fast wheel ticks must visit both next entries, without losing a step.
    wheel(page, QPoint(-120, 0), {}, Qt::ShiftModifier);
    wheel(page, QPoint(-120, 0), {}, Qt::ShiftModifier);
    QTRY_COMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("note2"));
    QTRY_VERIFY(script(QStringLiteral("reader.view.lastLocation.range.intersectsNode(reader.view.renderer.getContents()[0].doc.getElementById('note2'))")).toBool());
    // Missing/deleted remarks must not turn pages or zoom.
    evaluate(model, QStringLiteral("backend.annotations = []"));
    wheel(page, QPoint(-120, 0), {}, Qt::ShiftModifier);
    QTest::qWait(50);
    QCOMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("note2"));
    evaluate(model, QStringLiteral("backend.annotations = %1; backend.goToAnnotation(backend.annotations[1])").arg(notes));
    QTRY_COMPARE(script(QStringLiteral("reader.currentAnnotationKey")).toString(), QStringLiteral("text"));
    QTRY_VERIFY(script(QStringLiteral("reader.view.lastLocation.range.intersectsNode(reader.view.renderer.getContents()[0].doc.getElementById('text'))")).toBool());
    QCOMPARE(page->property("pageWidthDistance").toInt(), 250);
    QVERIFY(qAbs(page->property("zoomLevel").toDouble() - zoom) < 0.001);
    const double before = pageWidth();
    const int globalWidth = evaluate(model, QStringLiteral("Config.maxWidth")).toInt();
    wheel(page, QPoint(120, 0));
    QTRY_COMPARE(page->property("pageWidthDistance").toInt(), 275);
    QTRY_VERIFY(pageWidth() > before + 10);
    wheel(page, {}, QPoint(-30, 0));
    QTRY_COMPARE(page->property("pageWidthDistance").toInt(), 250);
    QTRY_VERIFY(qAbs(pageWidth() - before) < 2);
    QCOMPARE(evaluate(model, QStringLiteral("Config.maxWidth")).toInt(), globalWidth);
    wheel(page, QPoint(0, 120));
    QTRY_VERIFY(qAbs(BookDatabase::self().loadEntry(epubPath)->zoomLevel - (zoom + 0.1)) < 0.001);
    QCOMPARE(page->property("pageWidthDistance").toInt(), 250);
    wheel(page, {}, QPoint(0, -30));
    QTRY_VERIFY(qAbs(BookDatabase::self().loadEntry(epubPath)->zoomLevel - zoom) < 0.001);
    QTRY_VERIFY(qAbs(pageWidth() - before) < 2);
    // Verify page vs. section routing through the live WebEngine bridge.
    script(QStringLiteral("globalThis.wheelCalls = []; for (const name of ['prev', 'next', 'prevSection', 'nextSection']) reader.view[name] = async () => wheelCalls.push(name)"));
    wheel(page, QPoint(120, 0), {}, Qt::NoModifier);
    wheel(page, QPoint(-120, 0), {}, Qt::NoModifier);
    wheel(page, QPoint(0, 120), {}, Qt::NoModifier);
    wheel(page, {}, QPoint(0, -30), Qt::NoModifier);
    QTRY_COMPARE(script(QStringLiteral("wheelCalls.join(',')")).toString(), QStringLiteral("prevSection,nextSection,prev,next"));
    // Exercise the WebEngine path directly, independently of QML's shortcut map.
    QVERIFY(!script(QStringLiteral(R"JS((() => {
        const doc = reader.view.renderer.getContents()[0].doc;
        return doc.body.dispatchEvent(new KeyboardEvent('keydown', {
            key: '+', code: 'NumpadAdd', location: 3, bubbles: true, cancelable: true
        }));
    })())JS")).toBool());
    QTRY_COMPARE(evaluate(model, QStringLiteral("root.pageWidthDistance")).toInt(), 275);
    QTRY_VERIFY(pageWidth() > before + 10);
    script(QStringLiteral(R"JS((() => {
        const doc = reader.view.renderer.getContents()[0].doc;
        doc.body.dispatchEvent(new KeyboardEvent('keydown', {
            key: '-', code: 'NumpadSubtract', location: 3, bubbles: true, cancelable: true
        }));
    })())JS"));
    QTRY_VERIFY(qAbs(pageWidth() - before) < 2);
    QVERIFY(script(QStringLiteral(R"JS((() => {
        const doc = reader.view.renderer.getContents()[0].doc;
        const input = doc.createElement('input');
        doc.body.append(input);
        const allowed = input.dispatchEvent(new KeyboardEvent('keydown', {
            key: '+', code: 'NumpadAdd', location: 3, bubbles: true, cancelable: true
        }));
        input.remove();
        return allowed;
    })())JS")).toBool());
    QCOMPARE(evaluate(model, QStringLiteral("root.pageWidthDistance")).toInt(), 250);
    NativeKeyTrace keyTrace;
    if (qEnvironmentVariableIsSet("ARIANNA_TEST_NATIVE_KEYS"))
        qApp->installEventFilter(&keyTrace);
    const auto keypad = [&](Qt::Key key) {
        if (!qEnvironmentVariableIsSet("ARIANNA_TEST_NATIVE_KEYS")) {
            QTest::keyClick(window, key, Qt::KeypadModifier);
            return;
        }
        qInfo() << "Native keypad focus" << window->isActive() << window->activeFocusItem()
                << evaluate(model, QStringLiteral("[view.activeFocus, focusScope.activeFocus, root.visible]"));
        QProcess focus;
        focus.start(QStringLiteral("xdotool"), {QStringLiteral("getwindowfocus")});
        QVERIFY(focus.waitForFinished(5000));
        QCOMPARE(QString::fromUtf8(focus.readAllStandardOutput()).trimmed().toULongLong(), quint64(window->winId()));
        QProcess input;
        input.start(QStringLiteral("xdotool"), {QStringLiteral("key"), QStringLiteral("--clearmodifiers"),
                    key == Qt::Key_Plus ? QStringLiteral("KP_Add") : QStringLiteral("KP_Subtract")});
        QVERIFY(input.waitForFinished(5000));
        QCOMPARE(input.exitCode(), 0);
    };
    // Click inside the rendered book, without forcing either of the outer QML focus scopes.
    const QPoint center = page->mapToScene(QPointF(page->width()/2, page->height()/2)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
    keypad(Qt::Key_Plus);
    QTRY_COMPARE(evaluate(model, QStringLiteral("root.pageWidthDistance")).toInt(), 275);
    QTRY_COMPARE(script(QStringLiteral("reader.view.renderer.getAttribute('max-inline-size')")).toString(), QStringLiteral("275px"));
    QTRY_VERIFY(pageWidth() > before + 10);
    keypad(Qt::Key_Minus);
    QTRY_VERIFY(qAbs(pageWidth() - before) < 2);

    evaluate(model, QStringLiteral("root.pageWidthDistance = 1400; backend.applyStyle()"));
    QTRY_VERIFY(pageWidth() > before + 30);
    const double fullWidth = pageWidth();
    wheel(page, QPoint(-120, 0));
    QTRY_VERIFY(pageWidth() < fullWidth - 10);
    const int savedWidth = page->property("pageWidthDistance").toInt();
    const double renderedWidth = pageWidth();
    QCOMPARE(BookDatabase::self().loadEntry(epubPath)->pageWidthDistance, savedWidth);
    object.reset();
    engine.rootContext()->setContextProperty(QStringLiteral("testBook"), QVariant::fromValue(*BookDatabase::self().loadEntry(epubPath)));
    object.reset(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    page = object->findChild<QQuickItem *>(QStringLiteral("testPage"));
    model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(page && model);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.readerReady")).toBool(), 15000);
    evaluate(model, QStringLiteral("view.runJavaScript('openSync(' + JSON.stringify(testEpubUrl) + ', null, \"\", ' + JSON.stringify(root.currentReaderLayout()) + ')')"));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    QCOMPARE(page->property("pageWidthDistance").toInt(), savedWidth);
    QTRY_VERIFY(qAbs(pageWidth() - renderedWidth) < 2);
}

void SearchNavigationTest::epubCrossReferenceReload()
{
    QTemporaryDir dir;
    // A fixture failure must never activate a real desktop editor.
    const auto editorCommand = Config::editorCommand();
    Config::setEditorCommand(QString());
    const auto restoreEditor = qScopeGuard([&] { Config::setEditorCommand(editorCommand); });
    BookEntry source;
    for (const QString &id : {QStringLiteral("reload-source"), QStringLiteral("reload-target")}) {
        BookEntry entry;
        entry.identifier = entry.uniqueIdentifier = entry.title = id;
        if (id == QStringLiteral("reload-target")) {
            entry.title = QStringLiteral("Śrī Kṛṣṇa");
            entry.author = QStringList{QStringLiteral("Śaṅkara")};
        }
        entry.filename = dir.filePath(id + QStringLiteral(".epub"));
        KZip zip(entry.filename);
        QVERIFY(zip.open(QIODevice::WriteOnly));
        QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
        QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
            R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
        QVERIFY(zip.writeFile(QStringLiteral("package.opf"), QStringLiteral(R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">%1</dc:identifier><dc:title>%2</dc:title><dc:language>en</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest><spine><itemref idref="chapter"/></spine></package>)").arg(id, entry.title).toUtf8()));
        QVERIFY(zip.writeFile(QStringLiteral("nav.xhtml"), R"(<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><span>Part One</span><ol><li><a href="chapter.xhtml">The chapter</a></li></ol></li></ol></nav></body></html>)"));
        const QString body = id == QStringLiteral("reload-source")
            ? QStringLiteral("<p>Earlier text.</p>").repeated(100) + QStringLiteral("<p id=\"source\">Reference text</p>")
            : QString(QStringLiteral("<p>Earlier target text.</p>").repeated(100)
                + QStringLiteral("<p id=\"target\" data-role=\"anchor\">Target text %1Śrī Kṛṣṇa कृष्ण</p>"
                                 "<p id=\"other\" data-role=\"anchor\">Other target</p>").arg(QStringLiteral("long preview ").repeated(15)));
        QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"),
            QStringLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Reload</title></head><body>%1</body></html>").arg(body).toUtf8()));
        QVERIFY(zip.close());
        BookDatabase::self().addEntry(entry);
        if (id == QStringLiteral("reload-source")) source = entry;
    }
    // The real reader must save the reference even while both books have editor copies.
    EditorSessionStore sessions;
    QStringList sessionIds, copies;
    QList<QByteArray> copyHashes;
    for (const auto &id : {QStringLiteral("reload-source"), QStringLiteral("reload-target")}) {
        const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(id);
        QVERIFY(entry);
        const auto session = sessions.createEditorSession(id, entry->filename);
        QVERIFY2(!session.value(QStringLiteral("error")).toBool(), qPrintable(session.value(QStringLiteral("message")).toString()));
        sessionIds.append(session.value(QStringLiteral("sessionId")).toString());
        copies.append(session.value(QStringLiteral("workingCopyPath")).toString());
        copyHashes.append(MaintenanceFiles::digest(copies.last()));
    }
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reload-test"));
    QVERIFY(server.isRunning());
    class SessionRequests : public QWebEngineUrlRequestInterceptor {
    public:
        std::atomic_bool failNextTargets{false};
        void interceptRequest(QWebEngineUrlRequestInfo &info) override {
            if (info.requestUrl().path().endsWith(QStringLiteral("/targetLocations")) && failNextTargets.exchange(false)) {
                info.block(true);
                return;
            }
            if (BookServerConfig::matchesBookServer(info.requestUrl())) info.setHttpHeader("X-Arianna-Session-Token", "reload-test");
        }
    } requests;
    auto *profile = QWebEngineProfile::defaultProfile();
    profile->setUrlRequestInterceptor(&requests);
    const auto resetProfile = qScopeGuard([&] { profile->setUrlRequestInterceptor(nullptr); });
    BookListModel library;
    library.componentComplete();
    QTRY_VERIFY(library.cacheLoaded());
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testLibrary"), &library);
    engine.rootContext()->setContextProperty(QStringLiteral("testBook"), QVariant::fromValue(source));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("reload-test"));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1000; height: 800; visible: true
            property var bookListModel: testLibrary
            property string openedReferenceLocation: ""
            function openReferencePage(filename, location, entry, readOnly, locations, sourceTitle) {
                openedReferenceLocation = location;
                pageStack.layers.currentItem.suspendReaderForReference("cross-reference-test");
                pageStack.layers.push(referenceReader, {filename: filename, url: filename, entry: entry, currentLocation: location});
            }
            contextDrawer: Kirigami.ContextDrawer {
                property string currentTocId: ""
                property string currentHref: ""
                property var model: QtObject { function importFromJson(json) {} }
                function clearCurrentItem() {}
                signal goTo(var location)
            }
            pageStack.initialPage: Kirigami.Page {}
            Component.onCompleted: pageStack.layers.push(readerPage)
            Component {
                id: referenceReader
                EpubViewerPage {
                    objectName: "referenceTargetReader"
                    pageMode: "single"
                    property bool scriptDone: false
                    property var scriptResult
                }
            }
            Component {
                id: readerPage
                EpubViewerPage {
                    objectName: "reloadReader"
                    entry: testBook
                    filename: testBook.filename
                    url: testBook.filename
                    currentLocation: "chapter.xhtml#source"
                    pageMode: "single"
                    property bool scriptDone: false
                    property var scriptResult
                }
            }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *page = object->findChild<QQuickItem *>(QStringLiteral("reloadReader"));
    auto *model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(page && model);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    QTRY_VERIFY(page->property("currentLocation").toString().startsWith(QStringLiteral("epubcfi(")));
    QString position = page->property("currentLocation").toString();
    QVERIFY(page->property("currentProgress").toDouble() > 0.5);
    const QString version = page->property("bookDeliveryVersion").toString();
    const auto runScript = [&](QObject *scriptModel, QObject *scriptPage, const QString &code) {
        const auto encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(scriptModel, QStringLiteral("root.scriptDone=false;view.runJavaScript(%1[0],value=>{root.scriptResult=value;root.scriptDone=true})").arg(encoded));
        QElapsedTimer timer;
        timer.start();
        while (!scriptPage->property("scriptDone").toBool() && timer.elapsed() < 5000)
            QTest::qWait(10);
        if (!scriptPage->property("scriptDone").toBool())
            QTest::qFail("Reader script timed out", __FILE__, __LINE__);
        return scriptPage->property("scriptResult");
    };
    const auto script = [&](const QString &code) { return runScript(model, page, code); };
    // Use the actual selection and reference dialog, not a synthetic reload event.
    script(QStringLiteral(R"JS(
        const doc = reader.view.renderer.getContents()[0].doc;
        const range = doc.createRange();range.selectNodeContents(doc.getElementById('source').firstChild);
        doc.getSelection().removeAllRanges();doc.getSelection().addRange(range);
        doc.dispatchEvent(new doc.defaultView.PointerEvent('pointerup', {bubbles:true}));
    )JS"));
    QTRY_VERIFY(evaluate(model, QStringLiteral("backend.selection?.type === 'selection'")).toBool());
    auto *create = evaluate(model, QStringLiteral("selectionPopup.itemAt(1)")).value<QObject *>();
    QVERIFY(create && QMetaObject::invokeMethod(create, "clicked"));
    auto *editor = object->findChild<QObject *>(QStringLiteral("crossReferenceEditor"));
    QVERIFY(editor);
    QTRY_VERIFY(editor->property("visible").toBool());
    QTRY_COMPARE(editor->property("bookState").toString(), QStringLiteral("ready"));
    QTRY_COMPARE(editor->property("targetState").toString(), QStringLiteral("ready"));
    auto *books = editor->findChild<QObject *>(QStringLiteral("crossReferenceBooks"));
    auto *targets = editor->findChild<QObject *>(QStringLiteral("crossReferenceTargets"));
    QVERIFY(books && targets);
    evaluate(books, QStringLiteral("query='no such book'"));
    QCOMPARE(evaluate(books, QStringLiteral("matches.count")).toInt(), 0);
    QCOMPARE(editor->property("targetBookId").toString(), source.uniqueIdentifier);
    evaluate(books, QStringLiteral("query='SRI sankara';searchField.forceActiveFocus()"));
    QCOMPARE(evaluate(books, QStringLiteral("matches.count")).toInt(), 1);
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(editor->property("targetBookId").toString(), QStringLiteral("reload-target"));
    QTRY_COMPARE(editor->property("targetState").toString(), QStringLiteral("ready"));
    evaluate(targets, QStringLiteral("query='कृष्णा'"));
    QCOMPARE(evaluate(targets, QStringLiteral("matchCount")).toInt(), 0);
    evaluate(targets, QStringLiteral("query='कृष्ण'"));
    QVERIFY(evaluate(targets, QStringLiteral("targetModel.indexForValue('chapter.xhtml#target').valid")).toBool());
    evaluate(targets, QStringLiteral("query='Krsna'"));
    QCOMPARE(evaluate(targets, QStringLiteral("targetModel.data(targetModel.parent(targetModel.indexForValue('chapter.xhtml#target')))")).toString(), QStringLiteral("The chapter"));
    QCOMPARE(evaluate(targets, QStringLiteral("targetModel.data(targetModel.parent(targetModel.parent(targetModel.indexForValue('chapter.xhtml#target'))))")).toString(), QStringLiteral("Part One"));
    QTRY_COMPARE(evaluate(targets, QStringLiteral("treeView.rows")).toInt(), 3);
    QVERIFY(!evaluate(targets, QStringLiteral("targetModel.indexForValue('chapter.xhtml#other').valid")).toBool());
    auto *targetList = targets->property("treeView").value<QQuickItem *>();
    QVERIFY(targetList);
    QTRY_VERIFY(resultDelegate(targetList, "targetValue", QStringLiteral("chapter.xhtml#target")));
    auto *targetResult = resultDelegate(targetList, "targetValue", QStringLiteral("chapter.xhtml#target"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      targetResult->mapToScene(QPointF(targetResult->width() / 2, targetResult->height() / 2)).toPoint());
    QCOMPARE(editor->property("targetLocation").toString(), QStringLiteral("chapter.xhtml#target"));
    const auto screenshot = qEnvironmentVariable("ARIANNA_REFERENCE_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTest::qWait(250); // Let the popup's enter transition finish before visual inspection.
        QVERIFY(window->grabWindow().save(screenshot));
    }
    const auto preview = editor->property("previewHtml").toString();
    evaluate(targets, QStringLiteral("query='no matching target'"));
    QCOMPARE(editor->property("targetLocation").toString(), QStringLiteral("chapter.xhtml#target"));
    QCOMPARE(editor->property("previewHtml").toString(), preview);
    QVERIFY(editor->property("visible").toBool());
    editor->setProperty("autoTitle", false);
    editor->setProperty("sourceAnchorTitle", QStringLiteral("Reload reference"));
    position = page->property("currentLocation").toString();
    evaluate(editor, QStringLiteral("save();save()")); // A double click submits once.
    QTRY_VERIFY_WITH_TIMEOUT(page->property("bookDeliveryVersion").toString() != version, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    QTRY_COMPARE(page->property("currentLocation").toString(), position);
    const auto anchor =
        script(QStringLiteral("(() => {const a=reader.view.renderer.getContents()[0].doc.querySelector('[data-anchor-type=\"crossref\"]');return a ? "
                              "{text:a.textContent,title:a.title,id:a.id} : null})()"))
            .toMap();
    QCOMPARE(anchor.value(QStringLiteral("text")).toString(), QStringLiteral("Reference text"));
    QCOMPARE(anchor.value(QStringLiteral("title")).toString(), QStringLiteral("Reload reference"));
    QVERIFY(!anchor.value(QStringLiteral("id")).toString().isEmpty());
    QCOMPARE(ReferenceStore().loadReferences(source.uniqueIdentifier).size(), 1);
    // A legacy invalid target is displayed in native QML and remains editable.
    ReferenceStore references;
    auto saved = references.loadReferenceBySource(source.uniqueIdentifier, anchor.value(QStringLiteral("id")).toString());
    QVERIFY(!saved.isEmpty());
    saved.insert(QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#missing-target"));
    references.saveReference(saved);
    const auto clickReference = [&] {
        script(QStringLiteral("reader.view.renderer.getContents()[0].doc.querySelector('[data-anchor-type=\"crossref\"]').click()"));
    };
    clickReference();
    auto *dialog = object->findChild<QObject *>(QStringLiteral("crossReferenceDialog"));
    auto *open = object->findChild<QObject *>(QStringLiteral("crossReferenceOpen"));
    auto *edit = object->findChild<QObject *>(QStringLiteral("crossReferenceEdit"));
    QVERIFY(dialog && open && edit);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(evaluate(dialog, QStringLiteral("!!referenceContext.targetNavigationError")).toBool());
    QVERIFY(!open->property("enabled").toBool());
    auto *targetEditorOffer = dialog->findChild<QObject *>(QStringLiteral("openMissingTargetEditor"));
    QVERIFY(targetEditorOffer);
    QTRY_VERIFY(targetEditorOffer->property("visible").toBool());
    QCOMPARE(evaluate(dialog, QStringLiteral("referenceContext.targetAnchorRepair.bookId")).toString(), QStringLiteral("reload-target"));
    QCOMPARE(evaluate(dialog, QStringLiteral("referenceContext.targetAnchorRepair.documentFile")).toString(), QStringLiteral("chapter.xhtml"));
    QVERIFY(object->property("openedReferenceLocation").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(edit, "clicked"));
    QTRY_VERIFY(editor->property("visible").toBool());
    QTRY_COMPARE(editor->property("targetState").toString(), QStringLiteral("ready"));
    QCOMPARE(editor->property("targetLocation").toString(), QStringLiteral("chapter.xhtml#missing-target"));
    evaluate(editor, QStringLiteral("save()"));
    QTRY_VERIFY(!editor->property("errorMessage").toString().isEmpty());
    QVERIFY(editor->property("visible").toBool());
    QCOMPARE(evaluate(editor, QStringLiteral("targetAnchorRepair.bookId")).toString(), QStringLiteral("reload-target"));
    QCOMPARE(evaluate(editor, QStringLiteral("targetAnchorRepair.anchorId")).toString(), QStringLiteral("missing-target"));
    auto *repairOpen = editor->findChild<QObject *>(QStringLiteral("openMissingTargetEditor"));
    QVERIFY(repairOpen);
    QVERIFY(QMetaObject::invokeMethod(repairOpen, "clicked"));
    auto *repairStatus = editor->findChild<QObject *>(QStringLiteral("missingTargetEditorStatus"));
    QVERIFY(repairStatus && !repairStatus->property("text").toString().isEmpty());
    QCOMPARE(references.loadReferenceBySource(source.uniqueIdentifier, anchor.value(QStringLiteral("id")).toString())
                 .value(QStringLiteral("targetLocation")).toString(), QStringLiteral("chapter.xhtml#missing-target"));
    evaluate(targets, QStringLiteral("query='Krsna';selected(targetModel.rowForValue('chapter.xhtml#target'))"));
    QVERIFY(evaluate(editor, QStringLiteral("!targetAnchorRepair.fileName")).toBool());
    const auto editedVersion = page->property("bookDeliveryVersion").toString();
    evaluate(editor, QStringLiteral("save()"));
    QTRY_VERIFY(!editor->property("visible").toBool());
    QTRY_VERIFY(page->property("bookDeliveryVersion").toString() != editedVersion);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    clickReference();
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(!evaluate(dialog, QStringLiteral("!!referenceContext.targetNavigationError")).toBool());
    QVERIFY(open->property("enabled").toBool());
    // Canceling the native deletion confirmation does not change stored data.
    evaluate(dialog, QStringLiteral("confirmingDeletion=true;close()"));
    QVERIFY(!references.loadReferenceBySource(source.uniqueIdentifier, anchor.value(QStringLiteral("id")).toString()).isEmpty());
    clickReference();
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
    QTRY_COMPARE(object->property("openedReferenceLocation").toString(), QStringLiteral("chapter.xhtml#target"));
    auto *targetPage = object->findChild<QQuickItem *>(QStringLiteral("referenceTargetReader"));
    QVERIFY(targetPage);
    auto *targetModel = targetPage->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(targetModel);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(targetModel, QStringLiteral("view.bookReady")).toBool(), 15000);
    QTRY_VERIFY(targetPage->property("currentProgress").toDouble() > 0.5);
    QVERIFY(runScript(targetModel, targetPage, QStringLiteral("(() => {const doc=reader.view.renderer.getContents()[0].doc;"
        "const range=doc.createRange();range.selectNodeContents(doc.getElementById('target'));"
        "return reader.view.renderer.isRangeVisible(range)})()")).toBool());
    evaluate(object.get(), QStringLiteral("pageStack.layers.pop()"));
    QTRY_VERIFY(page->property("visible").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    clickReference();
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(!dialog->property("confirmingDeletion").toBool());
    auto *remove = dialog->findChild<QObject *>(QStringLiteral("crossReferenceDelete"));
    QVERIFY(remove && QMetaObject::invokeMethod(remove, "clicked"));
    QVERIFY(dialog->property("confirmingDeletion").toBool());
    const auto deleteVersion = page->property("bookDeliveryVersion").toString();
    QVERIFY(QMetaObject::invokeMethod(remove, "clicked"));
    // Deletion retains the server's existing working-copy guard; its error is visible.
    QTRY_VERIFY(!dialog->property("errorMessage").toString().isEmpty());
    QVERIFY(!references.loadReferences(source.uniqueIdentifier).isEmpty());
    for (int i = 0; i < copies.size(); ++i) {
        QCOMPARE(MaintenanceFiles::digest(copies.at(i)), copyHashes.at(i));
        sessions.finishSession(sessionIds.at(i));
        QTRY_VERIFY(!QFileInfo::exists(copies.at(i)));
    }
    QVERIFY(QMetaObject::invokeMethod(remove, "clicked"));
    QTRY_VERIFY2(references.loadReferences(source.uniqueIdentifier).isEmpty(), qPrintable(dialog->property("errorMessage").toString()));
    QTRY_VERIFY(page->property("bookDeliveryVersion").toString() != deleteVersion);
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(model, QStringLiteral("view.bookReady")).toBool(), 15000);
    QTRY_VERIFY(script(QStringLiteral("!reader.view.renderer.getContents()[0].doc.querySelector('[data-anchor-type=\"crossref\"]') && "
                                     "reader.view.renderer.getContents()[0].doc.body.textContent.includes('Reference text')")).toBool());
}

#endif

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid())
        return 1;
    const QByteArray directory = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", directory);
    qputenv("XDG_CONFIG_HOME", directory);
    qputenv("XDG_CACHE_HOME", directory);
    qputenv("XDG_RUNTIME_DIR", directory);
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    SearchNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "searchnavigationtest.moc"
