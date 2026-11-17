// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookserver.h"
#include "bookserverconfig.h"
#include "codexclient.h"
#include "config.h"
#include "fakespeechdispatcher.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>
#include <memory>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QQuickItem *namedItem(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name)
        return item;
    for (auto *child : item->childItems()) {
        if (auto *found = namedItem(child, name))
            return found;
    }
    return nullptr;
}
}

class CodexQmlTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void settingsAndTestDialog()
    {
        Config::setCodexPath(QString::fromUtf8(CODEX_TEST_PEER_PATH));
        Config::self()->save();
        qputenv("ARIANNA_FAKE_CODEX", "stream");
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 1000; height: 1000; visible: true
                pageStack.initialPage: SettingsPage { objectName: "settingsPage" }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        auto *tabs = object->findChild<QObject *>(QStringLiteral("settingsTabs"));
        QVERIFY(tabs);
        QVERIFY(object->findChild<QObject *>(QStringLiteral("aiAssistanceSettingsTabButton")));
        QVERIFY(tabs->setProperty("currentIndex", 4));
        auto *pathSetting = namedItem(window->contentItem(), QStringLiteral("codexPathSetting"));
        QVERIFY(pathSetting);
        QTRY_VERIFY(pathSetting->isVisible());
        QCOMPARE(pathSetting->property("text").toString(), Config::codexPath());
        const QString invalidPath = QStringLiteral("/missing/codex");
        QVERIFY(pathSetting->setProperty("text", invalidPath));
        QVERIFY(QMetaObject::invokeMethod(pathSetting, "editingFinished"));
        QCOMPARE(Config::codexPath(), invalidPath);
        Config::self()->load();
        QCOMPARE(Config::codexPath(), invalidPath);
        Config::setCodexPath(QString::fromUtf8(CODEX_TEST_PEER_PATH));
        Config::self()->save();
        auto *testButton = namedItem(window->contentItem(), QStringLiteral("codexTestConnectionButton"));
        QVERIFY(testButton);
        QVERIFY(QMetaObject::invokeMethod(testButton, "clicked"));
        auto *client = engine.singletonInstance<CodexClient *>("org.kde.arianna", "CodexClient");
        QVERIFY(client);
        QTRY_COMPARE(client->state(), CodexClient::Ready);
        auto *dialog = object->findChild<QObject *>(QStringLiteral("codexTestDialog"));
        QVERIFY(dialog);
        auto *input = dialog->findChild<QObject *>(QStringLiteral("codexInput"));
        auto *status = dialog->findChild<QObject *>(QStringLiteral("codexConnectionStatus"));
        auto *send = dialog->findChild<QObject *>(QStringLiteral("codexSendButton"));
        auto *output = dialog->findChild<QObject *>(QStringLiteral("codexOutput"));
        QVERIFY(input && status && send && output);
        QVERIFY(status->property("text").toString().contains(client->version()));
        QVERIFY(input->setProperty("text", QStringLiteral("Discuss this passage.")));
        QTRY_VERIFY(send->property("enabled").toBool());
        QSignalSpy finished(client, &CodexClient::turnFinished);
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(output->property("text").toString(), QStringLiteral("Grüße 世界"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(client->state(), CodexClient::Disconnected);
    }

    void realEpubSelection();

    void readerDiscussion_data()
    {
        QTest::addColumn<bool>("selectionHasToc");
        QTest::addColumn<bool>("metadataFallback");
        QTest::addColumn<bool>("languageMapTitle");
        QTest::addColumn<QByteArray>("failureScenario");
        QTest::newRow("selected passage TOC") << true << false << false << QByteArray();
        QTest::newRow("reader position TOC fallback") << false << false << false << QByteArray();
        QTest::newRow("structured metadata fallback") << true << true << false << QByteArray();
        QTest::newRow("multilingual metadata title") << true << true << true << QByteArray();
        QTest::newRow("RPC failure history") << true << false << false << QByteArray("rpc-error");
        QTest::newRow("process crash history") << true << false << false << QByteArray("crash");
    }

    void readerDiscussion()
    {
        QFETCH(bool, selectionHasToc);
        QFETCH(bool, metadataFallback);
        QFETCH(bool, languageMapTitle);
        QFETCH(QByteArray, failureScenario);
        Config::setCodexPath(QString::fromUtf8(CODEX_TEST_PEER_PATH));
        qputenv("ARIANNA_FAKE_CODEX", failureScenario.isEmpty() ? QByteArray("cancel") : failureScenario);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), QStringLiteral("http://127.0.0.1:1"));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("codex-test"));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 1000; height: 1000; visible: true
                property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
                pageStack.initialPage: EpubViewerPage {
                    objectName: "readerPage"
                    readerSuspended: true
                    entry: ({title: "Test book", author: "Test author", uniqueIdentifier: "sourceA", locations: "[]"})
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *page = object->findChild<QObject *>(QStringLiteral("readerPage"));
        QVERIFY(page);
        auto *dialog = object->findChild<QObject *>(QStringLiteral("aiDiscussionDialog"));
        QVERIFY(dialog);
        if (metadataFallback) {
            QVERIFY(page->setProperty("entry",
                                      QVariantMap{{QStringLiteral("title"), QVariantList{}},
                                                  {QStringLiteral("author"), QVariantList{}},
                                                  {QStringLiteral("uniqueIdentifier"), QStringLiteral("sourceA")},
                                                  {QStringLiteral("locations"), QStringLiteral("[]")}}));
        }
        QQmlExpression stubAction(qmlContext(dialog), dialog, QStringLiteral("view.runJavaScript(\"globalThis.selectionAction = () => {}\")"));
        stubAction.evaluate();
        QQmlExpression selection(qmlContext(dialog),
                                 dialog,
                                 QStringLiteral("backend.metadata = {title: [], creator: [], author: [{name: 'Metadata author'}, {name: 'Coauthor'}]}; "
                                                "backend.location = {tocItem: {label: 'Reader chapter'}}; "
                                                "backend.selection = {content: 'चेतनश्चेतनानाम् — cetanaś cetanānām', bookId: 'sourceA', lang: 'sa', value: "
                                                "'epubcfi(/6/2)', readerContext: {paragraph: 'Surrounding paragraph', section: 'Larger section'}%1}; %2")
                                     .arg(selectionHasToc ? QStringLiteral(", tocItem: {label: 'Selected chapter'}") : QString(),
                                          metadataFallback ? (languageMapTitle ? QStringLiteral("backend.metadata.title = {en: 'Metadata book'}")
                                                                               : QStringLiteral("backend.metadata.title = [{value: 'Metadata book'}]"))
                                                           : QString()));
        selection.evaluate();
        QVERIFY2(!selection.hasError(), qPrintable(selection.error().toString()));
        auto *menuItem = object->findChild<QObject *>(QStringLiteral("aiDiscussionMenuItem"));
        QVERIFY(menuItem);
        QTRY_VERIFY(menuItem->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(menuItem, "clicked"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        auto *client = engine.singletonInstance<CodexClient *>("org.kde.arianna", "CodexClient");
        QVERIFY(client);
        QTRY_COMPARE(client->state(), CodexClient::Ready);
        auto *status = dialog->findChild<QObject *>(QStringLiteral("codexConnectionStatus"));
        auto *send = dialog->findChild<QObject *>(QStringLiteral("codexSendButton"));
        auto *cancel = dialog->findChild<QObject *>(QStringLiteral("codexCancelButton"));
        QVERIFY(status && send && cancel);
        QVERIFY(status->property("text").toString().contains(client->version()));
        // A relocation or metadata refresh must not replace this discussion's context.
        QVERIFY(page->setProperty("entry",
                                  QVariantMap{{QStringLiteral("title"), QStringLiteral("Other book")},
                                              {QStringLiteral("author"), QStringLiteral("Other author")},
                                              {QStringLiteral("uniqueIdentifier"), QStringLiteral("sourceB")},
                                              {QStringLiteral("locations"), QStringLiteral("[]")}}));
        QQmlExpression relocated(qmlContext(dialog),
                                 dialog,
                                 QStringLiteral("backend.metadata = {title: 'Other metadata', author: 'Other author'}; "
                                                "backend.location = {tocItem: {label: 'Other chapter'}}"));
        relocated.evaluate();
        QVERIFY2(!relocated.hasError(), qPrintable(relocated.error().toString()));
        QTRY_VERIFY(send->property("enabled").toBool());
        QSignalSpy submitted(client, &CodexClient::notificationReceived);
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        if (!failureScenario.isEmpty()) {
            QTRY_VERIFY(!client->errorString().isEmpty());
            QQmlExpression history(qmlContext(dialog), dialog, QStringLiteral("JSON.stringify(aiDiscussionDialog.submissions)"));
            const auto entries = QJsonDocument::fromJson(history.evaluate().toString().toUtf8()).array();
            QCOMPARE(entries.size(), 1);
            const auto entry = entries.first().toObject();
            QCOMPARE(entry.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
            QVERIFY(!entry.value(QStringLiteral("error")).toString().isEmpty());
            QVERIFY(!entry.value(QStringLiteral("context")).toObject().value(QStringLiteral("snapshotId")).toString().isEmpty());
            QVERIFY(!entry.value(QStringLiteral("threadId")).toString().isEmpty());
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            QTRY_VERIFY(client->state() == CodexClient::Disconnected || client->state() == CodexClient::Failed);
            return;
        }
        QTRY_COMPARE(client->state(), CodexClient::Running);
        QJsonArray submittedInput;
        for (const auto &notification : submitted) {
            if (notification.at(0).toString() == QLatin1String("test/submittedInput"))
                submittedInput = notification.at(1).value<QJsonObject>().value(QStringLiteral("input")).toArray();
        }
        QCOMPARE(submittedInput.size(), 2);
        const auto contextText = submittedInput.first().toObject().value(QStringLiteral("text")).toString();
        const auto context = QJsonDocument::fromJson(contextText.mid(contextText.indexOf(QLatin1Char('\n')) + 1).toUtf8()).object();
        QVERIFY(!context.isEmpty());
        const auto bibliography = context.value(QStringLiteral("bibliography")).toObject();
        QCOMPARE(bibliography.value(QStringLiteral("title")).toString(), metadataFallback ? QStringLiteral("Metadata book") : QStringLiteral("Test book"));
        const auto authors = bibliography.value(QStringLiteral("authors")).toArray();
        QCOMPARE(authors.first().toString(), metadataFallback ? QStringLiteral("Metadata author") : QStringLiteral("Test author"));
        const auto location = context.value(QStringLiteral("location")).toObject();
        QCOMPARE(location.value(QStringLiteral("chapter")).toString(), selectionHasToc ? QStringLiteral("Selected chapter") : QStringLiteral("Reader chapter"));
        QCOMPARE(location.value(QStringLiteral("selectionCfi")).toString(), QStringLiteral("epubcfi(/6/2)"));
        QCOMPARE(context.value(QStringLiteral("selection")).toObject().value(QStringLiteral("text")).toString(),
                 QStringLiteral("चेतनश्चेतनानाम् — cetanaś cetanānām"));
        QVERIFY(!contextText.contains(QLatin1String("Other book")));
        QVERIFY(!contextText.contains(QLatin1String("Other author")));
        QVERIFY(!contextText.contains(QLatin1String("Other chapter")));
        QVERIFY(!context.value(QStringLiteral("surrounding")).toObject().contains(QStringLiteral("section")));
        auto *pane = dialog->findChild<QObject *>(QStringLiteral("readerContextPane"));
        auto *inspector = dialog->findChild<QObject *>(QStringLiteral("readerContextJson"));
        auto *refresh = dialog->findChild<QObject *>(QStringLiteral("readerContextRefreshButton"));
        auto *question = dialog->findChild<QObject *>(QStringLiteral("aiDiscussionQuestionPrompt"));
        QVERIFY(pane && inspector && refresh && question);
        QCOMPARE(inspector->property("text").toString(), contextText.mid(contextText.indexOf(QLatin1Char('\n')) + 1));
        QVERIFY(submittedInput.last().toObject().value(QStringLiteral("text")).toString().startsWith(QLatin1String("User question:\n")));
        auto *copy = dialog->findChild<QObject *>(QStringLiteral("aiDiscussionCopyPromptButton"));
        QVERIFY(copy);
        QVERIFY(QMetaObject::invokeMethod(copy, "clicked"));
        const auto clipboard = QGuiApplication::clipboard()->text();
        QCOMPARE(clipboard, contextText + QStringLiteral("\n") + submittedInput.last().toObject().value(QStringLiteral("text")).toString());
        QVERIFY(!refresh->property("enabled").toBool()); // old selection, different book, active turn.
        QTRY_VERIFY(cancel->property("enabled").toBool());
        QSignalSpy finished(client, &CodexClient::turnFinished);
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("interrupted"));
        // A follow-up stays in the original thread and retains exactly this context.
        submitted.clear();
        QVERIFY(question->setProperty("text", QStringLiteral("What does cetanaś mean?")));
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        QTRY_COMPARE(client->state(), CodexClient::Running);
        for (const auto &notification : submitted) {
            if (notification.at(0).toString() == QLatin1String("test/submittedInput")) {
                const auto input = notification.at(1).value<QJsonObject>().value(QStringLiteral("input")).toArray();
                QCOMPARE(input.first().toObject().value(QStringLiteral("text")).toString(), contextText);
                QCOMPARE(input.last().toObject().value(QStringLiteral("text")).toString(), QStringLiteral("User question:\nWhat does cetanaś mean?"));
            }
        }
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(finished.first().first().toString(), finished.last().first().toString());
        QQmlExpression history(qmlContext(dialog), dialog, QStringLiteral("JSON.stringify(aiDiscussionDialog.submissions)"));
        const auto entries = QJsonDocument::fromJson(history.evaluate().toString().toUtf8()).array();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().toObject().value(QStringLiteral("context")).toObject(), context);
        QCOMPARE(entries.last().toObject().value(QStringLiteral("context")).toObject(), context);
        QVERIFY(!entries.first().toObject().value(QStringLiteral("turnId")).toString().isEmpty());
        QVERIFY(!refresh->property("enabled").toBool()); // old book selection remains unavailable.
        // Only explicit refresh captures the new book/selection, while preserving history.
        QQmlExpression newSelection(qmlContext(dialog),
                                    dialog,
                                    QStringLiteral("backend.dispatch({type: 'show-selection', payload: {type: 'selection', bookId: 'sourceB', "
                                                   "content: 'New selected text', lang: 'en', value: 'epubcfi(/6/4)', "
                                                   "readerContext: {paragraph: 'New surrounding paragraph'}}})"));
        newSelection.evaluate();
        QVERIFY2(!newSelection.hasError(), qPrintable(newSelection.error().toString()));
        QTRY_VERIFY(refresh->property("enabled").toBool());
        QCOMPARE(pane->property("contextJson").toString(), inspector->property("text").toString());
        QVERIFY(QMetaObject::invokeMethod(refresh, "clicked"));
        const auto updated = QJsonDocument::fromJson(pane->property("contextJson").toString().toUtf8()).object();
        QCOMPARE(updated.value(QStringLiteral("bibliography")).toObject().value(QStringLiteral("title")).toString(), QStringLiteral("Other metadata"));
        QCOMPARE(updated.value(QStringLiteral("selection")).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("New selected text"));
        QVERIFY(updated.value(QStringLiteral("snapshotId")) != context.value(QStringLiteral("snapshotId")));
        QVERIFY(pane->setProperty("expanded", true));
        QQmlExpression exclude(qmlContext(dialog), dialog, QStringLiteral("readerContextPane.setIncluded('surrounding.paragraph', false)"));
        exclude.evaluate();
        QVERIFY2(!exclude.hasError(), qPrintable(exclude.error().toString()));
        submitted.clear();
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        QTRY_COMPARE(client->state(), CodexClient::Running);
        bool observed = false;
        for (const auto &notification : submitted) {
            if (notification.at(0).toString() != QLatin1String("test/submittedInput"))
                continue;
            observed = true;
            const auto input = notification.at(1).value<QJsonObject>().value(QStringLiteral("input")).toArray();
            const auto text = input.first().toObject().value(QStringLiteral("text")).toString();
            const auto json = text.mid(text.indexOf(QLatin1Char('\n')) + 1);
            QCOMPARE(json, inspector->property("text").toString());
            QVERIFY(!QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("surrounding")).toObject().contains(QStringLiteral("paragraph")));
        }
        QVERIFY(observed);
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_COMPARE(finished.size(), 3);
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(client->state(), CodexClient::Disconnected);
    }
};

#ifndef Q_MOC_RUN
void CodexQmlTest::realEpubSelection()
{
    Config::setCodexPath(QString::fromUtf8(CODEX_TEST_PEER_PATH));
    qputenv("ARIANNA_FAKE_CODEX", "stream");
    BookServer server(QStringLiteral("codex-test"));
    QVERIFY(server.isRunning());
    QTemporaryDir fixture;
    KZip zip(fixture.filePath(QStringLiteral("context.epub")));
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
    QVERIFY(zip.writeFile(
        QStringLiteral("META-INF/container.xml"),
        R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
    QVERIFY(zip.writeFile(
        QStringLiteral("package.opf"),
        R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id" prefix="schema: https://schema.org/"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Kaṭha-Upaniṣad</dc:title><dc:identifier id="id">context-fixture</dc:identifier><dc:language>sa</dc:language><dc:creator id="author">Real Author</dc:creator><dc:contributor id="translator">Real Translator</dc:contributor><meta refines="#translator" property="role" scheme="marc:relators">trl</meta><dc:contributor id="editor">Real Editor</dc:contributor><meta refines="#editor" property="role" scheme="marc:relators">edt</meta><meta property="schema:bookEdition">Test edition</meta></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="nav" href="nav.xhtml" properties="nav" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    QVERIFY(zip.writeFile(
        QStringLiteral("nav.xhtml"),
        R"(<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>TOC</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml#chapter">Selected TOC chapter</a></li></ol></nav></body></html>)"));
    const auto chapter =
        QStringLiteral(
            "<html xmlns='http://www.w3.org/1999/xhtml' xml:lang='sa'><head><title>Chapter</title></head><body><h2 id='chapter'>Actual heading</h2><p id='p' "
            "data-verse='2.2.13'>Before. <span lang='sa-Latn'>cetanaś cetanānām</span> <span lang='sa-Deva'>चेतनश्चेतनानाम्</span> After.</p></body></html>")
            .toUtf8();
    QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), chapter));
    QVERIFY(zip.close());
    QFile file(zip.fileName());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll();
    const QString url = QStringLiteral("data:application/epub+zip;base64,") + QString::fromLatin1(original.toBase64());
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("codex-test"));
    QQmlComponent component(&engine);
    component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 1100; height: 1000; visible: true
                property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
                contextDrawer: Kirigami.ContextDrawer {
                    property string currentTocId: ""
                    property string currentHref: ""
                    property var model: QtObject { function importFromJson(json) {} }
                    function clearCurrentItem() {}
                    signal goTo(var location)
                }
                pageStack.initialPage: Kirigami.Page {}
                Component.onCompleted: pageStack.layers.push(readerComponent)
                Component {
                    id: readerComponent
                    EpubViewerPage {
                        objectName: "readerPage"
                        entry: ({uniqueIdentifier: "context-fixture", locations: "[]"})
                        pageMode: "single"
                        property bool scriptDone: false
                        property var scriptResult
                    }
                }
            }
        )",
                      QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *page = object->findChild<QObject *>(QStringLiteral("readerPage"));
    auto *dialog = object->findChild<QObject *>(QStringLiteral("aiDiscussionDialog"));
    QVERIFY(page && dialog);
    const auto evaluate = [&](const QString &code) {
        QQmlExpression expression(qmlContext(dialog), dialog, code);
        const auto result = expression.evaluate();
        if (expression.hasError())
            QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
        return result;
    };
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(QStringLiteral("view.readerReady")).toBool(), 15000);
    const auto encoded = QString::fromUtf8(QJsonDocument(QJsonArray{url}).toJson(QJsonDocument::Compact));
    evaluate(QStringLiteral(
                 "view.runJavaScript('openSync(' + JSON.stringify(%1[0]) + ', null, \"context-fixture\", ' + JSON.stringify(root.currentReaderLayout()) + ')')")
                 .arg(encoded));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(QStringLiteral("view.bookReady")).toBool(), 15000);
    const auto script = [&](const QString &code) {
        const auto json = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(QStringLiteral("root.scriptDone=false; view.runJavaScript(%1[0], value => {root.scriptResult=value;root.scriptDone=true})").arg(json));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000)
            QTest::qWait(10);
        return page->property("scriptResult");
    };
    QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("reader.view.renderer.getContents().some(c => c.doc?.getElementById('p'))")).toBool(), 15000);
    script(QStringLiteral(R"JS((() => {
            const doc = reader.view.renderer.getContents().find(c => c.doc?.getElementById('p')).doc;
            globalThis.contextDoc = doc; globalThis.originalHtml = doc.body.innerHTML;
            const p = doc.getElementById('p'), r = doc.createRange();
            r.setStart(p.querySelector('[lang="sa-Latn"]').firstChild, 0);
            r.setEnd(p.querySelector('[lang="sa-Deva"]').firstChild, p.querySelector('[lang="sa-Deva"]').textContent.length);
            const selection = doc.getSelection(); selection.removeAllRanges(); selection.addRange(r);
            p.dispatchEvent(new doc.defaultView.PointerEvent('pointerup', {bubbles:true}));
            return selection.toString();
        })())JS"));
    auto *menu = object->findChild<QObject *>(QStringLiteral("aiDiscussionMenuItem"));
    QVERIFY(menu);
    QTRY_VERIFY_WITH_TIMEOUT(menu->property("enabled").toBool(), 15000);
    QVERIFY(QMetaObject::invokeMethod(menu, "clicked"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    auto *client = engine.singletonInstance<CodexClient *>("org.kde.arianna", "CodexClient");
    QTRY_COMPARE(client->state(), CodexClient::Ready);
    auto *pane = dialog->findChild<QObject *>(QStringLiteral("readerContextPane"));
    auto *send = dialog->findChild<QObject *>(QStringLiteral("codexSendButton"));
    QVERIFY(pane && send);
    const auto context = QJsonDocument::fromJson(pane->property("contextJson").toString().toUtf8()).object();
    const auto b = context.value(QStringLiteral("bibliography")).toObject();
    QCOMPARE(b.value(QStringLiteral("title")).toString(), QStringLiteral("Kaṭha-Upaniṣad"));
    QCOMPARE(b.value(QStringLiteral("authors")).toArray().first().toString(), QStringLiteral("Real Author"));
    QCOMPARE(b.value(QStringLiteral("translators")).toArray().first().toString(), QStringLiteral("Real Translator"));
    QCOMPARE(b.value(QStringLiteral("editors")).toArray().first().toString(), QStringLiteral("Real Editor"));
    QCOMPARE(b.value(QStringLiteral("edition")).toString(), QStringLiteral("Test edition"));
    const auto l = context.value(QStringLiteral("location")).toObject();
    QCOMPARE(l.value(QStringLiteral("chapter")).toString(), QStringLiteral("Selected TOC chapter"));
    QCOMPARE(l.value(QStringLiteral("heading")).toString(), QStringLiteral("Actual heading"));
    QCOMPARE(l.value(QStringLiteral("verse")).toString(), QStringLiteral("2.2.13"));
    QVERIFY(l.value(QStringLiteral("selectionCfi")).toString().startsWith(QLatin1String("epubcfi(")));
    const auto selection = context.value(QStringLiteral("selection")).toObject();
    QCOMPARE(selection.value(QStringLiteral("language")).toString(), QStringLiteral("mul"));
    QCOMPARE(selection.value(QStringLiteral("text")).toString(), QStringLiteral("cetanaś cetanānām चेतनश्चेतनानाम्"));
    QVERIFY(context.value(QStringLiteral("surrounding")).toObject().value(QStringLiteral("paragraph")).toString().contains(QLatin1String("Before.")));
    QSignalSpy finished(client, &CodexClient::turnFinished);
    QSignalSpy submitted(client, &CodexClient::notificationReceived);
    QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
    QTRY_COMPARE(finished.size(), 1);
    bool observed = false;
    for (const auto &notification : submitted) {
        if (notification.at(0).toString() != QLatin1String("test/submittedInput"))
            continue;
        observed = true;
        const auto input = notification.at(1).value<QJsonObject>().value(QStringLiteral("input")).toArray();
        const auto text = input.first().toObject().value(QStringLiteral("text")).toString();
        QCOMPARE(text.mid(text.indexOf(QLatin1Char('\n')) + 1), pane->property("contextJson").toString());
    }
    QVERIFY(observed);
    QVERIFY(script(QStringLiteral("contextDoc.body.innerHTML === originalHtml")).toBool());
    QVERIFY(file.seek(0));
    QCOMPARE(file.readAll(), original);
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QTRY_COMPARE(client->state(), CodexClient::Disconnected);
}

#endif

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid())
        return 1;
    const QByteArray path = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", path);
    qputenv("XDG_CONFIG_HOME", path);
    qputenv("XDG_CACHE_HOME", path);
    qputenv("XDG_RUNTIME_DIR", path);
    FakeSpeechDispatcher speech(environment.filePath(QStringLiteral("speech.sock")));
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(environment.filePath(QStringLiteral("speech.sock")))));
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    if (!speech.startServer())
        return 1;
    KLocalizedString::setApplicationDomain("arianna");
    QTcpServer port;
    if (!port.listen(QHostAddress::LocalHost, 0))
        return 1;
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(port.serverPort());
    port.close();
    CodexQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "codexqmltest.moc"
