// SPDX-License-Identifier: GPL-3.0-or-later
#include "annotationstore.h"
#include "bookdatabase.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "booktruthstore.h"
#include "categoryentriesmodel.h"
#include "codexclient.h"
#include "config.h"
#include "epubcontainer.h"
#include "noterenderer.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QBuffer>
#include <QCryptographicHash>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInterceptor>
#include <QtWebEngineQuick>

class AiNoteTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    QString path;
    QString bookId = QStringLiteral("ai-note-book");
    QVariantMap note;
    QByteArray bytes()
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }
    QByteArray chapter()
    {
        EPubContainer c(nullptr);
        if (!c.openFile(path))
            return {};
        return c.readData(QStringLiteral("OEBPS/chapter.xhtml"));
    }
    QVariant evaluate(QObject *scope, const QString &code)
    {
        QQmlExpression e(qmlContext(scope), scope, code);
        const auto result = e.evaluate();
        if (e.hasError())
            QTest::qFail(qPrintable(e.error().toString()), __FILE__, __LINE__);
        return result;
    }
    QVariant evaluate(QObject *scope, const char *code)
    {
        return evaluate(scope, QString::fromUtf8(code));
    }
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        // Start with the exact previous annotation schema, including a user note.
        QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-note"));
        db.setDatabaseName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/library.sqlite"));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(
            q.exec(QStringLiteral("CREATE TABLE annotations(bookId varchar,annotationId varchar,cfiRange varchar,anchorId varchar,color varchar,text text,note "
                                  "text,format varchar,location text,created datetime,modified datetime,PRIMARY KEY(bookId,annotationId))")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO annotations VALUES('legacy','user-note','epubcfi(/6/2!/4/2/1:0)','','yellow','quote','User correction','epub','','2020-01-01','')")));
        db.close();
        path = directory.filePath(QStringLiteral("ai-note.epub"));
        KZip zip(path);
        QVERIFY(zip.open(QIODevice::WriteOnly));
        QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
        QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
                              "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='OEBPS/package.opf' "
                              "media-type='application/oebps-package+xml'/></rootfiles></container>"));
        QVERIFY(zip.writeFile(QStringLiteral("OEBPS/package.opf"),
                              "<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'><metadata "
                              "xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>ai-note-book</dc:identifier><dc:title>AI "
                              "Notes</dc:title></metadata><manifest><item id='p' href='chapter.xhtml' "
                              "media-type='application/xhtml+xml'/></manifest><spine><itemref idref='p'/></spine></package>"));
        QVERIFY(
            zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"),
                          "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>AI Notes</title></head><body><p>Hello anchored truth.</p></body></html>"));
        QVERIFY(zip.close());
        BookEntry entry;
        entry.filename = path;
        entry.filetitle = QStringLiteral("ai-note.epub");
        entry.title = QStringLiteral("AI Notes");
        entry.uniqueIdentifier = entry.identifier = bookId;
        BookDatabase::self().addEntry(entry);
        const auto legacy = BookDatabase::self().loadAnnotations(QStringLiteral("legacy"));
        QCOMPARE(legacy.size(), 1);
        QCOMPARE(legacy.first().toMap().value(QStringLiteral("note")).toString(), QStringLiteral("User correction"));
        QCOMPARE(legacy.first().toMap().value(QStringLiteral("annotationId")).toString(), QStringLiteral("user-note"));
    }
    void safeFormatting()
    {
        const auto result =
            NoteRenderer::render(QStringLiteral("# Edited answer\n\n**śrī** चेतना ā\n\n\\(a_1+b\\)\n\n| A | B |\n|---|---|\n| 1 | 2 "
                                                "|\n\n<script>alert(1)</script> [bad](javascript:alert) ![picture](file:///etc/passwd)"));
        const auto html = result.value(QStringLiteral("noteHtml")).toString();
        QVERIFY(html.contains(QStringLiteral("<h1>")));
        QVERIFY(html.contains(QStringLiteral("<strong>śrī</strong>")));
        QVERIFY(html.contains(QStringLiteral("चेतना ā")));
        QVERIFY(html.contains(QStringLiteral("\\(a_1+b\\)")));
        QVERIFY(html.contains(QStringLiteral("<table>")));
        QVERIFY(!html.contains(QStringLiteral("<script")));
        QVERIFY(!html.contains(QStringLiteral("href=\"javascript:")));
        QVERIFY(!html.contains(QStringLiteral("<img")));
    }
    void discussionRendering()
    {
        const QVariantMap target{{QStringLiteral("bookId"), bookId}, {QStringLiteral("cfiRange"), QStringLiteral("selected-cfi")},
                                 {QStringLiteral("expectedStateId"), QStringLiteral("original-state")}};
        QVariantMap turn{{QStringLiteral("question"), QStringLiteral("Why **śrī**? <script>unsafe()</script>")},
                         {QStringLiteral("answer"), QStringLiteral("Because \\(x^2\\). [bad](javascript:unsafe) ![bad](file:///etc/passwd)")},
                         {QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("annotationTarget"), target},
                         {QStringLiteral("context"), QVariantMap{{QStringLiteral("snapshotId"), QStringLiteral("snapshot-1")}}}};
        QVariantMap second{{QStringLiteral("question"), QStringLiteral("And चेतना?")}, {QStringLiteral("answer"), QStringLiteral("Partial answer")},
                           {QStringLiteral("status"), QStringLiteral("interrupted")}};
        const auto result = NoteRenderer::renderDiscussion({turn, second});
        QVERIFY(!result.isEmpty());
        const auto html = result.value(QStringLiteral("noteHtml")).toString();
        QCOMPARE(html.count(QStringLiteral("chat-message")), 4);
        QVERIFY(html.contains(QStringLiteral("<strong>śrī</strong>")));
        QVERIFY(html.contains(QStringLiteral("math-source")));
        QVERIFY(!html.contains(QStringLiteral("<script")) && !html.contains(QStringLiteral("<img")));
        QVERIFY(!html.contains(QStringLiteral("href=\"javascript:")));
        const auto metadata = result.value(QStringLiteral("noteMetadata")).toMap();
        const auto saved = metadata.value(QStringLiteral("turns")).toList();
        QCOMPARE(saved.first().toMap().value(QStringLiteral("selectionCfi")).toString(), QStringLiteral("selected-cfi"));
        QCOMPARE(saved.first().toMap().value(QStringLiteral("selectionStateId")).toString(), QStringLiteral("original-state"));
        QCOMPARE(saved.first().toMap().value(QStringLiteral("contextSnapshotId")).toString(), QStringLiteral("snapshot-1"));
        QVERIFY(!saved.first().toMap().contains(QStringLiteral("context")));
        QCOMPARE(NoteRenderer::renderDiscussion(saved), result);
        QCOMPARE(NoteRenderer::discussionQuestions(metadata).size(), 2);
        QCOMPARE(NoteRenderer::discussionQuestions({{QStringLiteral("origin"), QStringLiteral("ai-discussion")},
                                                    {QStringLiteral("question"), QStringLiteral("Legacy question")}}),
                 QStringList{QStringLiteral("Legacy question")});
        QVERIFY(NoteRenderer::renderDiscussion({}).isEmpty());
        turn.insert(QStringLiteral("status"), QStringLiteral("submitted"));
        QVERIFY(NoteRenderer::renderDiscussion({turn}).isEmpty());
        turn.insert(QStringLiteral("status"), QStringLiteral("completed"));
        turn.insert(QStringLiteral("answer"), QString(256 * 1024, QLatin1Char('a')));
        QVERIFY(NoteRenderer::renderDiscussion({turn}).isEmpty());
        AnnotationStore store;
        const auto before = bytes();
        QVERIFY(store.saveAiDiscussionAnnotation(QStringLiteral("other-book"), target, {turn}).value(QStringLiteral("error")).toBool());
        QCOMPARE(bytes(), before);
        QVERIFY(store.loadAnnotations(bookId).isEmpty());
    }
    void editorCancelDoesNotWrite()
    {
        const auto before = bytes();
        AnnotationStore store;
        const auto target = QVariantMap{{QStringLiteral("bookId"), bookId},
                                        {QStringLiteral("text"), QStringLiteral("anchored")},
                                        {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2,/1:6,/1:14)")},
                                        {QStringLiteral("expectedStateId"), store.currentStateId(bookId)}};
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("target"), target);
        QQmlComponent component(&engine);
        // Use a direct dialog component so its popup can be hosted by a window.
        component.setData(
            "import QtQuick\nimport QtQuick.Controls as QQC2\nimport org.kde.kirigami as Kirigami\nimport org.kde.arianna\nKirigami.ApplicationWindow { "
            "width:900; height:800; visible:true; RichNoteDialog { objectName:'draft'; parent:QQC2.ApplicationWindow.overlay } }",
            QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const std::unique_ptr<QObject> object(component.create());
        QVERIFY(object);
        auto *dialog = object->findChild<QObject *>(QStringLiteral("draft"));
        QVERIFY(dialog);
        evaluate(dialog, "openDraft('Original answer',target,{origin:'ai-discussion'})");
        auto *field = dialog->findChild<QObject *>(QStringLiteral("aiRemarkDraft"));
        QVERIFY(field);
        QVERIFY(field->setProperty("text", QStringLiteral("User edit before saving")));
        QVERIFY(dialog->setProperty("highlightColor", QStringLiteral("#ef5350")));
        QCOMPARE(dialog->property("draft").toString(), QStringLiteral("User edit before saving"));
        evaluate(dialog, "close()");
        QCOMPARE(bytes(), before);
        QVERIFY(store.loadAnnotations(bookId).isEmpty());
    }
    void saveAndEditAnchoredHtml()
    {
        AnnotationStore store;
        const auto state = store.currentStateId(bookId);
        QVariantMap target{{QStringLiteral("bookId"), bookId},
                           {QStringLiteral("text"), QStringLiteral("anchored")},
                           {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2,/1:6,/1:14)")},
                           {QStringLiteral("expectedStateId"), state},
                           {QStringLiteral("color"), QStringLiteral("#ffb74d")},
                           {QStringLiteral("noteMetadata"),
                            QVariantMap{{QStringLiteral("origin"), QStringLiteral("ai-discussion")},
                                        {QStringLiteral("originalAnswer"), QStringLiteral("original AI answer")},
                                        {QStringLiteral("threadId"), QStringLiteral("thread")},
                                        {QStringLiteral("turnId"), QStringLiteral("turn")}}}};
        const auto before = bytes();
        auto stale = target;
        stale.insert(QStringLiteral("expectedStateId"), QUuid::createUuid().toString());
        QVERIFY(store.saveAiAnnotation(bookId, stale, QStringLiteral("Rejected")).value(QStringLiteral("error")).toBool());
        QCOMPARE(bytes(), before);
        note = store.saveAiAnnotation(bookId, target, QStringLiteral("# User edition\n\n**śrī** चेतना ā \\(x^2\\)"));
        QVERIFY2(!note.value(QStringLiteral("error")).toBool(), qPrintable(note.value(QStringLiteral("message")).toString()));
        QVERIFY(!note.value(QStringLiteral("anchorId")).toString().isEmpty());
        QVERIFY(chapter().contains("data-anchor-type=\"annotation\""));
        QVERIFY(!chapter().contains("User edition"));
        QVERIFY(!chapter().contains("original AI answer"));
        auto loaded = store.loadAnnotations(bookId);
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().toMap().value(QStringLiteral("noteHtml")), note.value(QStringLiteral("noteHtml")));
        QCOMPARE(loaded.first().toMap().value(QStringLiteral("noteSource")), note.value(QStringLiteral("noteSource")));
        QCOMPARE(loaded.first().toMap().value(QStringLiteral("color")).toString(), QStringLiteral("#ffb74d"));
        QCOMPARE(loaded.first().toMap().value(QStringLiteral("noteMetadata")).toMap().value(QStringLiteral("turnId")).toString(), QStringLiteral("turn"));
        const auto anchored = bytes();
        const auto anchor = note.value(QStringLiteral("anchorId"));
        note.insert(QStringLiteral("color"), QStringLiteral("#ba68c8"));
        note = store.saveAiAnnotation(bookId, note, QStringLiteral("# Second user edition\n\nEdited answer."));
        QVERIFY2(!note.value(QStringLiteral("error")).toBool(), qPrintable(note.value(QStringLiteral("message")).toString()));
        QCOMPARE(note.value(QStringLiteral("anchorId")), anchor);
        QCOMPARE(note.value(QStringLiteral("color")).toString(), QStringLiteral("#ba68c8"));
        QCOMPARE(store.loadAnnotations(bookId).first().toMap().value(QStringLiteral("color")), note.value(QStringLiteral("color")));
        QCOMPARE(bytes(), anchored);
        QCOMPARE(store.loadAnnotations(bookId).size(), 1);
    }
    void unavailableAnchorDoesNotSave()
    {
        AnnotationStore store;
        const auto before = bytes();
        auto target = note;
        target.remove(QStringLiteral("annotationId"));
        target.insert(QStringLiteral("anchorId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        target.insert(QStringLiteral("expectedStateId"), store.currentStateId(bookId));
        const auto result = store.saveAiAnnotation(bookId, target, QStringLiteral("Keep this draft"));
        QVERIFY(result.value(QStringLiteral("error")).toBool());
        QCOMPARE(result.value(QStringLiteral("noteSource")).toString(), QStringLiteral("Keep this draft"));
        QVERIFY(store.saveAiAnnotation(QStringLiteral("missing-book"), target, QStringLiteral("Keep this draft")).value(QStringLiteral("error")).toBool());
        QCOMPARE(bytes(), before);
        QCOMPARE(store.loadAnnotations(bookId).size(), 1);
    }
    void saveNewDiscussion() {
        // A fresh book exercises the normal direct-save path without a prior answer remark.
        const auto copyPath = directory.filePath(QStringLiteral("discussion.epub"));
        QVERIFY(QFile::copy(path, copyPath));
        BookEntry entry;
        entry.filename = copyPath;
        entry.filetitle = QStringLiteral("discussion.epub");
        entry.title = QStringLiteral("Discussion fixture");
        entry.uniqueIdentifier = entry.identifier = QStringLiteral("discussion-book");
        BookDatabase::self().addEntry(entry);
        AnnotationStore store;
        const auto id = entry.uniqueIdentifier;
        QVariantMap target{{QStringLiteral("bookId"), id}, {QStringLiteral("text"), QStringLiteral("anchored")},
                           {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2,/1:6,/1:14)")},
                           {QStringLiteral("expectedStateId"), store.currentStateId(id)}};
        const QVariantList turns{QVariantMap{{QStringLiteral("question"), QStringLiteral("Explain this passage")},
                                           {QStringLiteral("answer"), QStringLiteral("A saved answer")},
                                           {QStringLiteral("status"), QStringLiteral("completed")},
                                           {QStringLiteral("annotationTarget"), target}}};
        const auto saved = store.saveAiDiscussionAnnotation(id, target, turns);
        QVERIFY2(!saved.value(QStringLiteral("error")).toBool(), qPrintable(saved.value(QStringLiteral("message")).toString()));
        QVERIFY(!saved.value(QStringLiteral("anchorId")).toString().isEmpty());
        const auto loaded = store.loadAnnotations(id);
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().toMap().value(QStringLiteral("noteMetadata")), saved.value(QStringLiteral("noteMetadata")));
        EPubContainer epub(nullptr);
        QVERIFY(epub.openFile(copyPath));
        const auto source = epub.readData(QStringLiteral("OEBPS/chapter.xhtml"));
        QVERIFY(!source.contains("A saved answer") && !source.contains("Explain this passage"));
        QFile fixture(copyPath);
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto before = fixture.readAll();
        fixture.close();
        QVERIFY(!store.saveAiDiscussionAnnotation(id, saved, turns).value(QStringLiteral("error")).toBool());
        QCOMPARE(store.loadAnnotations(id).size(), 1);
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QCOMPARE(fixture.readAll(), before);
    }
    void anchorRangeValidation();
    void anchorSurvivesCfiChange();
    void overlappingReaderTargets_data();
    void overlappingReaderTargets();
};

void AiNoteTest::overlappingReaderTargets_data()
{
    QTest::addColumn<bool>("inverse");
    QTest::addColumn<qreal>("zoom");
    QTest::newRow("system") << false << qreal(1);
    QTest::newRow("inverse-130-percent") << true << qreal(1.3);
}

void AiNoteTest::overlappingReaderTargets()
{
    QFETCH(bool, inverse);
    QFETCH(qreal, zoom);
    const auto oldTheme = Config::readerTheme();
    Config::setReaderTheme(inverse ? Config::EnumReaderTheme::Inverse : Config::EnumReaderTheme::System);
    const auto restoreTheme = qScopeGuard([&] { Config::setReaderTheme(oldTheme); });
    const QString fixture = directory.filePath(QStringLiteral("overlapping-anchors.epub"));
    const QString id = QStringLiteral("overlapping-anchors");
    const QString firstNote = QStringLiteral("01a1de63-b5d6-79c4-8743-72dd8e7ed182");
    const QString firstReference = QStringLiteral("uuid_01a1de6a-992d-76d4-b8b8-afc5f75f7ba9");
    KZip zip(fixture);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArrayLiteral(
        "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='OPS/package.opf' "
        "media-type='application/oebps-package+xml'/></rootfiles></container>")));
    QVERIFY(zip.writeFile(QStringLiteral("OPS/package.opf"), QByteArrayLiteral(
        "<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>"
        "<dc:identifier id='id'>overlapping-anchors</dc:identifier><dc:title>Overlapping anchors</dc:title></metadata><manifest>"
        "<item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='chapter'/></spine></package>")));
    const auto body = QStringLiteral(R"XHTML(
        <html xmlns="http://www.w3.org/1999/xhtml"><head><title>Nested anchors</title>
        <meta http-equiv="Content-Security-Policy" content="img-src 'none'"/></head><body>
        <p>22. <span data-anchor-type="crossref" data-role="anchor" id="%1" title="S.B. Kapitel 1"><span
          data-anchor-type="annotation" data-role="anchor" id="uuid_%2">Pralambari</span> Balarama</span>, 23. Krishna.</p>
        <p><span id="uuid_11111111-1111-4111-8111-111111111111" data-role="anchor" data-anchor-type="annotation"><span id="reverse-ref"
          data-role="anchor" data-anchor-type="crossref">श्री śrī ā</span> note outside</span></p>
        <p><span id="coincident-ref" data-role="anchor" data-anchor-type="crossref"><span id="uuid_22222222-2222-4222-8222-222222222222"
          data-role="anchor" data-anchor-type="annotation">Completely overlapping</span></span></p>
        <p><span id="uuid_33333333-3333-4333-8333-333333333333" data-role="anchor" data-anchor-type="annotation">Standalone remark</span></p>
        <p><span id="single-ref" data-role="anchor" data-anchor-type="crossref">Standalone reference</span></p>
        <p><a id="href-ref" data-role="anchor" data-anchor-type="crossref" href="#destination"><span id="uuid_44444444-4444-4444-8444-444444444444"
          data-role="anchor" data-anchor-type="annotation">Reference with href</span></a></p>
        <p><a id="ordinary-link" href="#destination"><span id="uuid_55555555-5555-4555-8555-555555555555"
          data-role="anchor" data-anchor-type="annotation">Ordinary EPUB link</span></a></p>
        <p id="destination" data-role="anchor" data-anchor-type="target">The naming story.</p>
        </body></html>)XHTML").arg(firstReference, firstNote).toUtf8();
    QVERIFY(zip.writeFile(QStringLiteral("OPS/chapter.xhtml"), body));
    QVERIFY(zip.close());
    BookEntry entry;
    entry.filename = fixture;
    entry.filetitle = QStringLiteral("overlapping-anchors.epub");
    entry.title = QStringLiteral("Overlapping anchors");
    entry.uniqueIdentifier = entry.identifier = id;
    entry.zoomLevel = zoom;
    BookDatabase::self().addEntry(entry);
    const QStringList noteIds{firstNote, QStringLiteral("11111111-1111-4111-8111-111111111111"), QStringLiteral("22222222-2222-4222-8222-222222222222"),
                              QStringLiteral("33333333-3333-4333-8333-333333333333"), QStringLiteral("44444444-4444-4444-8444-444444444444"),
                              QStringLiteral("55555555-5555-4555-8555-555555555555")};
    for (const auto &key : noteIds) {
        auto saved = NoteRenderer::render(QStringLiteral("Remark for ") + key);
        saved.insert(QStringLiteral("annotationId"), key);
        saved.insert(QStringLiteral("anchorId"), key);
        saved.insert(QStringLiteral("color"), QStringLiteral("yellow"));
        saved.insert(QStringLiteral("format"), QStringLiteral("epub"));
        QVERIFY(BookDatabase::self().saveAnnotation(id, saved));
    }
    for (const auto &key : {firstReference, QStringLiteral("reverse-ref"), QStringLiteral("coincident-ref"),
                            QStringLiteral("single-ref"), QStringLiteral("href-ref")}) {
        BookDatabase::self().saveReference({{QStringLiteral("sourceBookId"), id}, {QStringLiteral("sourceAnchorId"), key},
            {QStringLiteral("targetBookId"), id}, {QStringLiteral("targetLocation"), QStringLiteral("OPS/chapter.xhtml#destination")},
            {QStringLiteral("targetPreviewHtml"), QStringLiteral("<p>The naming story.</p>")}});
    }
    QFile original(fixture);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto before = original.readAll();
    original.close();
    QTcpServer port;
    QVERIFY(port.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(port.serverPort());
    port.close();
    BookServer server(QStringLiteral("overlap-test"));
    QVERIFY(server.isRunning());
    class Requests : public QWebEngineUrlRequestInterceptor {
        void interceptRequest(QWebEngineUrlRequestInfo &info) override {
            if (info.requestUrl().host() == QLatin1String("127.0.0.1"))
                info.setHttpHeader("X-Arianna-Session-Token", "overlap-test");
        }
    } requests;
    auto *profile = QWebEngineProfile::defaultProfile();
    profile->setUrlRequestInterceptor(&requests);
    const auto reset = qScopeGuard([&] { profile->setUrlRequestInterceptor(nullptr); });
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testEntry"), QVariant::fromValue(entry));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("overlap-test"));
    QQmlComponent component(&engine);
    component.setData(R"QML(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1000; height: 800; visible: true
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
                    objectName: "reader"
                    entry: testEntry; filename: testEntry.filename; url: testEntry.filename; pageMode: "single"; zoomLevel: testEntry.zoomLevel
                    property bool done: false
                    property var result
                }
            }
        }
    )QML", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *page = object->findChild<QObject *>(QStringLiteral("reader"));
    auto *model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(page && model);
    const auto eval = [&](const QString &code) { return evaluate(model, code); };
    const auto script = [&](const QString &code) {
        const auto encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        eval(QStringLiteral("root.done=false;view.runJavaScript(%1[0],v=>{root.result=v;root.done=true})").arg(encoded));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("done").toBool() && timer.elapsed() < 5000) QTest::qWait(10);
        return page->property("done").toBool() ? page->property("result") : QVariant();
    };
    QTRY_VERIFY_WITH_TIMEOUT(eval(QStringLiteral("view.bookReady")).toBool(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("reader.view.renderer.getContents()[0]?.overlayer?.element.childElementCount >= 6")).toBool(), 15000);
    auto *popup = page->findChild<QObject *>(QStringLiteral("readerTargetPopup"));
    auto *noteDialog = page->findChild<QObject *>(QStringLiteral("richNoteDialog"));
    auto *referenceDialog = page->findChild<QObject *>(QStringLiteral("crossReferenceDialog"));
    auto *web = qobject_cast<QQuickItem *>(eval(QStringLiteral("view")).value<QObject *>());
    QVERIFY(popup && noteDialog && referenceDialog && web && web->window());
    const auto baseline = script(QStringLiteral(R"JS((() => {
        const doc=reader.view.renderer.getContents()[0].doc;
        globalThis.overlapSnapshot=()=>JSON.stringify({text:doc.body.textContent,
            anchors:Array.from(doc.querySelectorAll('[data-role="anchor"]'),el=>[el.id,el.getAttribute('data-anchor-type'),el.textContent]),
            cfi:reader.view.getCFI(0,(()=>{const r=doc.createRange();r.selectNodeContents(doc.getElementById('uuid_%1'));return r})(),reader.view.cfiFilter)});
        return overlapSnapshot();
    })())JS").arg(firstNote)).toString();
    QVERIFY(!baseline.isEmpty());
    const auto point = [&](const QString &anchor, bool lastText = false) {
        const auto encoded = QString::fromUtf8(QJsonDocument(QJsonArray{anchor}).toJson(QJsonDocument::Compact));
        const auto result = script(QStringLiteral(R"JS((() => {
            const doc=reader.view.renderer.getContents()[0].doc, el=doc.getElementById(%1[0]);
            const r=doc.createRange(); %2
            const rect=r.getClientRects()[0], frame=doc.defaultView.frameElement.getBoundingClientRect();
            return {x:frame.x+rect.x+rect.width/2,y:frame.y+rect.y+rect.height/2};
        })())JS").arg(encoded, lastText ? QStringLiteral("r.selectNodeContents(el.lastChild);") : QStringLiteral("r.selectNodeContents(el);"))).toMap();
        return web->mapToScene(QPointF(result.value(QStringLiteral("x")).toReal(), result.value(QStringLiteral("y")).toReal()) * zoom).toPoint();
    };
    const auto choose = [&](int index) {
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), 5000);
        auto *item = qobject_cast<QQuickItem *>(eval(QStringLiteral("readerTargetPopup.itemAt(%1)").arg(index)).value<QObject *>());
        QVERIFY(item && item->window());
        QSignalSpy clicked(item, SIGNAL(clicked()));
        QVERIFY(clicked.isValid());
        QTest::mouseClick(item->window(), Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
        QTRY_COMPARE(clicked.count(), 1);
    };
    const auto click = [&](const QString &anchor, bool lastText = false) {
        const auto p = point(anchor, lastText);
        QTest::mouseMove(web->window(), p);
        QTest::mouseClick(web->window(), Qt::LeftButton, Qt::NoModifier, p);
    };
    const auto closeDialog = [&](QObject *dialog) {
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->property("visible").toBool(), 5000);
    };
    // The user's exact nested anchors: one mouse click, two choices, no second popup.
    click(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), 5000);
    QCOMPARE(popup->property("choices").toList().size(), 2);
    QVERIFY(!noteDialog->property("visible").toBool() && !referenceDialog->property("visible").toBool());
    if (!qEnvironmentVariableIsEmpty("ARIANNA_OVERLAP_SCREENSHOT")) {
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_OVERLAP_SCREENSHOT")));
    }
    choose(0);
    QTRY_VERIFY_WITH_TIMEOUT(noteDialog->property("visible").toBool(), 5000);
    QVERIFY(!referenceDialog->property("visible").toBool());
    closeDialog(noteDialog);
    click(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("visible").toBool());
    choose(1);
    QTRY_VERIFY_WITH_TIMEOUT(referenceDialog->property("visible").toBool(), 5000);
    QCOMPARE(referenceDialog->property("referenceContext").toMap().value(QStringLiteral("ref")).toString(), firstReference);
    QVERIFY(!noteDialog->property("visible").toBool());
    closeDialog(referenceDialog);

    for (const auto &anchor : {QStringLiteral("reverse-ref"), QStringLiteral("uuid_22222222-2222-4222-8222-222222222222"), QStringLiteral("uuid_44444444-4444-4444-8444-444444444444")}) {
        click(anchor);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(), 5000);
        QCOMPARE(popup->property("choices").toList().size(), 2);
        choose(0);
        QTRY_VERIFY_WITH_TIMEOUT(noteDialog->property("visible").toBool(), 5000);
        QVERIFY(!referenceDialog->property("visible").toBool());
        closeDialog(noteDialog);
        click(anchor);
        QTRY_VERIFY(popup->property("visible").toBool());
        choose(1);
        QTRY_VERIFY_WITH_TIMEOUT(referenceDialog->property("visible").toBool(), 5000);
        QVERIFY(!noteDialog->property("visible").toBool());
        closeDialog(referenceDialog);
    }
    // Outside the inner note, Balarama is still a direct reference target.
    click(firstReference, true);
    QTRY_VERIFY_WITH_TIMEOUT(referenceDialog->property("visible").toBool(), 5000);
    QVERIFY(!popup->property("visible").toBool());
    closeDialog(referenceDialog);
    click(QStringLiteral("uuid_33333333-3333-4333-8333-333333333333"));
    QTRY_VERIFY_WITH_TIMEOUT(noteDialog->property("visible").toBool(), 5000);
    QVERIFY(!popup->property("visible").toBool());
    closeDialog(noteDialog);

    script(QStringLiteral("globalThis.nativeLinkCount=0;reader.view.addEventListener('link',e=>{globalThis.nativeLinkCount++;globalThis.nativeLinkTarget=e.detail.href})"));
    click(QStringLiteral("ordinary-link"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("choices").toList().size(), 2);
    choose(0);
    QTRY_VERIFY(noteDialog->property("visible").toBool());
    closeDialog(noteDialog);
    click(QStringLiteral("ordinary-link"));
    QTRY_VERIFY(popup->property("visible").toBool());
    choose(1);
    QTRY_COMPARE(script(QStringLiteral("nativeLinkCount")).toInt(), 1);
    QVERIFY(script(QStringLiteral("nativeLinkTarget")).toString().endsWith(QLatin1String("chapter.xhtml#destination")));
    QVERIFY(!noteDialog->property("visible").toBool());

    // Right-click uses the same overlap chooser, followed by the selected
    // context actions rather than automatically navigating or editing content.
    auto *annotationMenu = page->findChild<QObject *>(QStringLiteral("annotationPopup"));
    auto *referenceMenu = page->findChild<QObject *>(QStringLiteral("referencePopup"));
    auto *linkMenu = page->findChild<QObject *>(QStringLiteral("readerLinkPopup"));
    auto *referenceEditor = page->findChild<QObject *>(QStringLiteral("crossReferenceEditor"));
    QVERIFY(annotationMenu && referenceMenu && linkMenu && referenceEditor);
    const auto rightClick = [&](const QString &anchor, bool lastText = false) {
        const auto p = point(anchor, lastText);
        QTest::mouseMove(web->window(), p);
        QTest::mouseClick(web->window(), Qt::RightButton, Qt::NoModifier, p);
    };
    const auto contextAction = [&](QObject *menu, const QString &name) {
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(), 5000);
        auto *item = menu->findChild<QQuickItem *>(name);
        QVERIFY(item && item->isVisible() && item->isEnabled() && item->window());
        QSignalSpy clicked(item, SIGNAL(clicked()));
        QTest::mouseClick(item->window(), Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
        QTRY_COMPARE(clicked.count(), 1);
    };
    const auto escapeMenu = [&](QObject *menu) {
        auto *content = qobject_cast<QQuickItem *>(menu->property("contentItem").value<QObject *>());
        QVERIFY(content && content->window());
        QTest::keyClick(content->window(), Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
    };
    rightClick(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("opened").toBool());
    QCOMPARE(popup->property("choices").toList().size(), 2);
    QVERIFY(!noteDialog->property("visible").toBool() && !referenceDialog->property("visible").toBool());
    if (!qEnvironmentVariableIsEmpty("ARIANNA_RIGHT_CLICK_SCREENSHOT")) {
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_RIGHT_CLICK_SCREENSHOT") + QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png")));
    }
    choose(0);
    QTRY_VERIFY(annotationMenu->property("opened").toBool());
    QVERIFY(!referenceMenu->property("visible").toBool() && !noteDialog->property("visible").toBool());
    contextAction(annotationMenu, QStringLiteral("openRemarkContextAction"));
    QTRY_VERIFY(noteDialog->property("visible").toBool());
    QVERIFY(!noteDialog->property("editing").toBool());
    closeDialog(noteDialog);
    rightClick(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("opened").toBool());
    choose(0);
    contextAction(annotationMenu, QStringLiteral("editRemarkContextAction"));
    QTRY_VERIFY(noteDialog->property("visible").toBool());
    QVERIFY(noteDialog->property("editing").toBool());
    closeDialog(noteDialog); // Cancel, no writes.
    rightClick(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("opened").toBool());
    choose(1);
    QTRY_VERIFY(referenceMenu->property("opened").toBool());
    QVERIFY(!annotationMenu->property("visible").toBool() && !referenceDialog->property("visible").toBool());
    contextAction(referenceMenu, QStringLiteral("editReferenceContextAction"));
    QTRY_VERIFY(referenceEditor->property("visible").toBool());
    closeDialog(referenceEditor); // Cancel, no writes.
    // Outside the inner remark only the reference menu opens.
    rightClick(firstReference, true);
    QTRY_VERIFY(referenceMenu->property("opened").toBool());
    QVERIFY(!popup->property("visible").toBool());
    escapeMenu(referenceMenu);
    // CFI-less ordinary EPUB links remain reachable under saved remarks too.
    const int linkCount = script(QStringLiteral("nativeLinkCount")).toInt();
    rightClick(QStringLiteral("ordinary-link"));
    QTRY_VERIFY(popup->property("opened").toBool());
    choose(1);
    QTRY_VERIFY(linkMenu->property("opened").toBool());
    contextAction(linkMenu, QStringLiteral("openReaderLinkContextAction"));
    QTRY_COMPARE(script(QStringLiteral("nativeLinkCount")).toInt(), linkCount + 1);
    // Escape from either stage executes no action and clears the pending menu.
    rightClick(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("opened").toBool());
    const int canceledRequest = popup->property("requestId").toInt();
    QTest::keyClick(web->window(), Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    rightClick(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("opened").toBool());
    script(QStringLiteral("selectionAction(1,%1)").arg(canceledRequest));
    QTest::qWait(50);
    QVERIFY(popup->property("visible").toBool());
    choose(0);
    QTRY_VERIFY(annotationMenu->property("opened").toBool());
    escapeMenu(annotationMenu);
    QVERIFY(!noteDialog->property("visible").toBool() && !referenceEditor->property("visible").toBool());

    // A legacy CFI-only note can overlap a reference without a nested wrapper.
    const auto legacyCfi = script(QStringLiteral(R"JS((() => {
        const doc=reader.view.renderer.getContents()[0].doc,range=doc.createRange(),text=doc.getElementById('single-ref').firstChild;
        range.setStart(text,0);range.setEnd(text,text.length);
        return reader.view.getCFI(0,range,reader.view.cfiFilter);
    })())JS")).toString();
    QVERIFY(legacyCfi.startsWith(QLatin1String("epubcfi(")));
    auto legacy = NoteRenderer::render(QStringLiteral("Legacy CFI remark"));
    legacy.insert(QStringLiteral("annotationId"), QStringLiteral("legacy-overlap"));
    legacy.insert(QStringLiteral("cfiRange"), legacyCfi);
    legacy.insert(QStringLiteral("color"), QStringLiteral("yellow"));
    QVERIFY(BookDatabase::self().saveAnnotation(id, legacy));
    eval(QStringLiteral("backend.loadAnnotations()"));
    QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("reader.view.renderer.getContents()[0].overlayer.element.childElementCount >= 7")).toBool(), 5000);
    click(QStringLiteral("single-ref"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("choices").toList().size(), 2);
    choose(0);
    QTRY_VERIFY(noteDialog->property("visible").toBool());
    closeDialog(noteDialog);

    // Noninteractive speech/search/hidden overlays cannot mask the saved note.
    QVERIFY(script(QStringLiteral(R"JS((() => {
        const {doc,overlayer}=reader.view.renderer.getContents()[0],r=doc.createRange();
        r.selectNodeContents(doc.getElementById('uuid_%1'));
        const draw=()=>document.createElementNS('http://www.w3.org/2000/svg','g');
        overlayer.add('test-speech',r,draw,{interactive:false});
        overlayer.add('foliate-search:test',r,draw,{});
        overlayer.add('hidden-test',r,draw,{});overlayer.setHidden('hidden-test',true);
        return true;
    })())JS").arg(firstNote)).toBool());
    // Badges are real reader pixels, independent of CSS cursor-image support or
    // the EPUB's img-src policy. They must also work at the user's 130% zoom.
    QTest::mouseMove(web->window(), web->mapToScene(QPointF(5, 5)).toPoint());
    QTest::qWait(30);
    QTest::mouseMove(web->window(), point(QStringLiteral("uuid_") + firstNote));
    const auto badgeState = [&] {
        return script(QStringLiteral(R"JS((()=>{
            const badge=document.querySelector('[data-arianna-reader-cursor-badge]');
            if(!badge||getComputedStyle(badge).display==='none')return '';
            return badge.getAttribute('data-arianna-reader-cursor-badge');
        })())JS")).toString();
    };
    QTRY_COMPARE(badgeState(), QStringLiteral("annotation reference"));
    const auto themeColors = script(QStringLiteral(R"JS((()=>{
        const badge=document.querySelector('[data-arianna-reader-cursor-badge]'),bodyStyle=getComputedStyle(document.body);
        const rgb=value=>value.match(/[\d.]+/g).slice(0,3).map(Number);
        globalThis.themedBadge=badge;
        return {background:rgb(bodyStyle.backgroundColor),foreground:rgb(bodyStyle.color),
            themed:getComputedStyle(badge.querySelector('rect')).fill===bodyStyle.backgroundColor
                &&getComputedStyle(badge.querySelector('g')).stroke===bodyStyle.color};
    })())JS")).toMap();
    QVERIFY(themeColors.value(QStringLiteral("themed")).toBool());
    const auto badgeRect = script(QStringLiteral(R"JS((()=>{
        const badge=document.querySelector('[data-arianna-reader-cursor-badge]'),r=badge.getBoundingClientRect();
        return {x:r.x,y:r.y,width:r.width,height:r.height};
    })())JS")).toMap();
    // Inspect the rendered badge, not only computed CSS / an internal cursor.
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    QTest::qWait(100);
    const QImage image = window->grabWindow();
    const auto topLeft = web->mapToScene(QPointF(badgeRect.value(QStringLiteral("x")).toReal(), badgeRect.value(QStringLiteral("y")).toReal()) * zoom);
    const QRect badgePixels(qRound(topLeft.x()*image.devicePixelRatio()), qRound(topLeft.y()*image.devicePixelRatio()),
        qRound(badgeRect.value(QStringLiteral("width")).toReal()*zoom*image.devicePixelRatio()),
        qRound(badgeRect.value(QStringLiteral("height")).toReal()*zoom*image.devicePixelRatio()));
    QVERIFY(image.rect().contains(badgePixels));
    const auto background = themeColors.value(QStringLiteral("background")).toList();
    const auto foreground = themeColors.value(QStringLiteral("foreground")).toList();
    QCOMPARE(background.size(), 3);
    QCOMPARE(foreground.size(), 3);
    const auto matches = [](const QColor &pixel, const QVariantList &rgb) {
        return qAbs(pixel.red() - rgb[0].toInt()) < 12 && qAbs(pixel.green() - rgb[1].toInt()) < 12
            && qAbs(pixel.blue() - rgb[2].toInt()) < 12;
    };
    int backgroundPixels = 0, foregroundPixels = 0;
    for (int y = badgePixels.top(); y <= badgePixels.bottom(); ++y) {
        for (int x = badgePixels.left(); x <= badgePixels.right(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (matches(color, background)) ++backgroundPixels;
            if (matches(color, foreground)) ++foregroundPixels;
        }
    }
    QVERIFY(backgroundPixels > 100 && foregroundPixels > 30);
    if (!qEnvironmentVariableIsEmpty("ARIANNA_HOVER_SCREENSHOT"))
        QVERIFY(image.save(qEnvironmentVariable("ARIANNA_HOVER_SCREENSHOT") + QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png")));
    // The badge is in the reader shell, never in the chapter's source DOM.
    QVERIFY(script(QStringLiteral("!reader.view.renderer.getContents()[0].doc.querySelector('[data-arianna-reader-cursor-badge]')")).toBool());
    // Reproduce a desktop path that supplies Mouse Events without Pointer Events.
    script(QStringLiteral(R"JS((()=>{
        const doc=reader.view.renderer.getContents()[0].doc,el=doc.getElementById('uuid_%1'),r=el.getBoundingClientRect();
        el.dispatchEvent(new doc.defaultView.MouseEvent('mousedown',{bubbles:true}));
        el.dispatchEvent(new doc.defaultView.MouseEvent('mousemove',{bubbles:true,clientX:r.x+r.width/2,clientY:r.y+r.height/2}));
    })())JS").arg(firstNote));
    QTRY_COMPARE(badgeState(), QStringLiteral("annotation reference"));
    // Exercise the hider's DOM state and the identical outer/iframe move in one
    // event turn. Compositor-generated hover refreshes must not make this test
    // depend on when a real idle timer happens to expire.
    script(QStringLiteral(R"JS((()=>{
        const doc=reader.view.renderer.getContents()[0].doc;
        globalThis.resumedCursor=null;
        doc.documentElement.style.cursor='none';
        queueMicrotask(()=>{
            const badge=document.querySelector('[data-arianna-reader-cursor-badge]');
            const hidden={cursor:doc.documentElement.style.cursor,badge:badge.style.display};
            const el=doc.getElementById('%1'),r=doc.createRange();r.selectNodeContents(el.lastChild);
            const rect=r.getBoundingClientRect(),position={bubbles:true,screenX:12345,screenY:12345};
            reader.view.dispatchEvent(new MouseEvent('mousemove',position));
            el.dispatchEvent(new doc.defaultView.MouseEvent('mousemove',{...position,clientX:rect.x+rect.width/2,clientY:rect.y+rect.height/2}));
            globalThis.resumedCursor={hiddenCursor:hidden.cursor,hiddenBadge:hidden.badge,
                cursor:doc.documentElement.style.cursor,badge:badge.style.display};
        });
    })())JS").arg(firstReference));
    QTRY_VERIFY(script(QStringLiteral("!!globalThis.resumedCursor")).toBool());
    const auto resumed = script(QStringLiteral("resumedCursor")).toMap();
    QCOMPARE(resumed.value(QStringLiteral("hiddenCursor")).toString(), QStringLiteral("none"));
    QCOMPARE(resumed.value(QStringLiteral("hiddenBadge")).toString(), QStringLiteral("none"));
    QCOMPARE(resumed.value(QStringLiteral("cursor")).toString(), QStringLiteral("auto"));
    QCOMPARE(resumed.value(QStringLiteral("badge")).toString(), QStringLiteral("block"));
    QTest::mouseMove(web->window(), point(firstReference, true));
    QTRY_COMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].doc.documentElement.getAttribute('data-arianna-reader-cursor')")).toString(),
                 QStringLiteral("reference"));
    QTRY_COMPARE(badgeState(), QStringLiteral("reference"));
    QTest::mouseMove(web->window(), point(QStringLiteral("uuid_33333333-3333-4333-8333-333333333333")));
    QTRY_COMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].doc.documentElement.getAttribute('data-arianna-reader-cursor')")).toString(),
                 QStringLiteral("annotation"));
    QTRY_COMPARE(badgeState(), QStringLiteral("annotation"));
    click(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("visible").toBool());
    const int oldRequest = popup->property("requestId").toInt();
    QTest::keyClick(web->window(), Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QTest::qWait(150);
    QVERIFY(!noteDialog->property("visible").toBool() && !referenceDialog->property("visible").toBool());
    click(QStringLiteral("uuid_") + firstNote);
    QTRY_VERIFY(popup->property("visible").toBool());
    QVERIFY(popup->property("requestId").toInt() > oldRequest);
    script(QStringLiteral("selectionAction(1,%1)").arg(oldRequest));
    QTest::qWait(100);
    QVERIFY(popup->property("visible").toBool());
    QVERIFY(!noteDialog->property("visible").toBool() && !referenceDialog->property("visible").toBool());
    QTest::keyClick(web->window(), Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    // A selection gesture retains its text and opens neither overlapping action.
    script(QStringLiteral(R"JS((() => {
        const doc=reader.view.renderer.getContents()[0].doc,el=doc.getElementById('uuid_%1'),r=doc.createRange();
        r.selectNodeContents(el);doc.getSelection().removeAllRanges();doc.getSelection().addRange(r);
        const rect=r.getBoundingClientRect();
        el.dispatchEvent(new doc.defaultView.MouseEvent('click',{bubbles:true,cancelable:true,clientX:rect.x+rect.width/2,clientY:rect.y+rect.height/2}));
    })())JS").arg(firstNote));
    QTest::qWait(100);
    QVERIFY(!popup->property("visible").toBool() && !noteDialog->property("visible").toBool() && !referenceDialog->property("visible").toBool());
    QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].doc.getSelection().toString()")).toString(), QStringLiteral("Pralambari"));
    // A live reader-theme change recolors the existing badge without recreating
    // its artwork or changing the selected targets.
    eval(QStringLiteral("root.setReaderThemeMode(%1)").arg(inverse ? 0 : 1));
    QTRY_COMPARE(script(QStringLiteral("document.body.classList.contains('invert')")).toBool(), !inverse);
    QTRY_VERIFY(script(QStringLiteral(R"JS((()=>{
        const badge=document.querySelector('[data-arianna-reader-cursor-badge]'),style=getComputedStyle(document.body);
        return badge===themedBadge && getComputedStyle(badge.querySelector('rect')).fill===style.backgroundColor
            && getComputedStyle(badge.querySelector('g')).stroke===style.color;
    })())JS")).toBool());
    eval(QStringLiteral("root.setReaderThemeMode(%1)").arg(inverse ? 1 : 2));
    QTRY_COMPARE(script(QStringLiteral("document.body.classList.contains('invert')")).toBool(), inverse);
    QVERIFY(script(QStringLiteral(R"JS((()=>{
        const doc=reader.view.renderer.getContents()[0].doc,el=doc.getElementById('uuid_%1'),r=el.getBoundingClientRect();
        const event=new doc.defaultView.MouseEvent('contextmenu',{bubbles:true,cancelable:true,clientX:r.x+r.width/2,clientY:r.y+r.height/2});
        el.dispatchEvent(event);return !event.defaultPrevented;
    })())JS").arg(firstNote)).toBool());
    QCOMPARE(script(QStringLiteral("overlapSnapshot()")).toString(), baseline);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), before);
    QCOMPARE(BookDatabase::self().loadAnnotations(id).size(), 7);
}

void AiNoteTest::anchorRangeValidation()
{
    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(reader.readAll());
    const auto section = [&](const QString &begin, const QString &end) {
        const auto first = source.indexOf(begin);
        const auto last = source.indexOf(end, first);
        return first >= 0 && last > first ? source.mid(first, last - first) : QString();
    };
    const auto helpers = section(QStringLiteral("const crossReferenceAnchorSelector ="), QStringLiteral("const annotationDrawKeys ="));
    auto index = section(QStringLiteral("async #indexAnnotationAnchors()"), QStringLiteral("#drawAnnotation({"));
    QVERIFY(!helpers.isEmpty() && !index.isEmpty());
    index.replace(QStringLiteral("#indexAnnotationAnchors()"), QStringLiteral("indexAnnotationAnchors()"));
    const auto script = helpers + QStringLiteral(R"JS(
        import('qrc:/qt/qml/org/kde/arianna/qml/foliate-js/epubcfi.js').then(async CFI => {
            const errors = [];
            const check = (value, message) => { if (!value) errors.push(message); };
            const parse = body => new DOMParser().parseFromString(
                '<html xmlns="http://www.w3.org/1999/xhtml"><head/><body>' + body + '</body></html>', 'application/xhtml+xml');
            const wrap = text => '<span id="uuid_valid" data-role="anchor" data-anchor-type="annotation">' + text + '</span>';
            class Harness {
                constructor(docs) {
                    this.book = {sections:docs.map(doc => ({createDocument:async () => doc}))};
                    this.view = {getCFI:(i,r,f) => CFI.joinIndir(CFI.fake.fromIndex(i), CFI.fromRange(r,f))};
                }
    )JS")
        + index + QStringLiteral(R"JS(
            }
            for (const text of ['चेतना कि क्षि', 'śrī ātmā a\u0304']) {
                const doc = parse('<p>Before ' + wrap(text) + ' after.</p>');
                const locations = await new Harness([doc]).indexAnnotationAnchors();
                const parts = CFI.parse(locations.get('valid'));
                (parts.parent ?? parts).shift();
                const range = CFI.toRange(doc, parts, cfiFilter);
                check(range.toString() === text, 'Unicode wrapper boundaries: ' + range.toString());
            }
            const multi = parse('<p>Before <span id="uuid_multi_begin" data-role="anchor" data-anchor-type="annotation-begin"/>first <em>middle</em></p><p>last<span id="uuid_multi_end" data-role="anchor" data-anchor-type="annotation-end"/> after</p>');
            const range = rangeForAnnotationAnchor(multi,{anchorId:'multi'});
            check(range.toString() === 'first middlelast', 'cross-element begin/end range');
            const duplicates = parse('<p>' + wrap('one') + wrap('two') + '</p>');
            check((await new Harness([duplicates]).indexAnnotationAnchors()).get('valid') === null, 'duplicate ID rejected');
            const one = parse('<p>' + wrap('one') + '</p>');
            check((await new Harness([one,one]).indexAnnotationAnchors()).get('valid') === null, 'duplicate across spine files rejected');
            check(!(await new Harness([one]).indexAnnotationAnchors()).has('missing'), 'missing anchor has no CFI fallback');
            globalThis.anchorTestResult = JSON.stringify(errors);
        }).catch(error => globalThis.anchorTestResult = JSON.stringify([String(error)]));
    )JS");
    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral("<html><body></body></html>"), QUrl(QStringLiteral("qrc:/")));
    QVERIFY(loaded.wait(10000));
    page.runJavaScript(script);
    QString result;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                 page.runJavaScript(QStringLiteral("globalThis.anchorTestResult || ''"), [&](const QVariant &value) {
                                     result = value.toString();
                                 });
                                 QTest::qWait(20);
                                 return !result.isEmpty();
                             })(),
                             10000);
    QCOMPARE(result, QStringLiteral("[]"));
}

void AiNoteTest::anchorSurvivesCfiChange()
{
    // Move the original anchored paragraph beneath a new structural wrapper.
    EPubContainer container(nullptr);
    QVERIFY(container.openFile(path));
    auto source = chapter();
    source.replace("<body>", "<body><p>Inserted before the remark.</p><section>");
    source.replace("</body>", "</section></body>");
    const auto candidate = container.createMetadataCandidate({{QStringLiteral("OEBPS/chapter.xhtml"), source}});
    QVERIFY(!candidate.isEmpty());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(candidate), candidate.size());
    f.close();
    QTcpServer port;
    QVERIFY(port.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(port.serverPort());
    port.close();
    BookServer server(QStringLiteral("note-test"));
    QVERIFY(server.isRunning());
    class Requests : public QWebEngineUrlRequestInterceptor
    {
        void interceptRequest(QWebEngineUrlRequestInfo &info) override
        {
            if (info.requestUrl().host() == QLatin1String("127.0.0.1"))
                info.setHttpHeader("X-Arianna-Session-Token", "note-test");
        }
    } requests;
    auto *profile = QWebEngineProfile::defaultProfile();
    profile->setUrlRequestInterceptor(&requests);
    const auto reset = qScopeGuard([&] {
        profile->setUrlRequestInterceptor(nullptr);
    });
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("testNote"), note);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("testEntry"), QVariant::fromValue(*BookDatabase::self().loadEntryByUniqueIdentifier(bookId)));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("note-test"));
    QQmlComponent component(&engine);
    component.setData(R"QML(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1000; height: 800; visible: true
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
                    objectName: "reader"
                    entry: testEntry
                    filename: testEntry.filename
                    url: testEntry.filename
                    pageMode: 'single'
                    property bool done: false
                    property var result
                }
            }
        }
    )QML",
                      QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> object(component.create());
    QVERIFY(object);
    auto *page = object->findChild<QObject *>(QStringLiteral("reader"));
    QVERIFY(page);
    auto *model = object->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(model);
    const auto eval = [&](const char *code) {
        return evaluate(model, code);
    };
    const auto noteScript = [&](const QString &code) {
        const auto argument = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(model, QStringLiteral("root.done=false;noteBrowser.runJavaScript(%1[0],v=>{root.result=v;root.done=true})").arg(argument));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("done").toBool() && timer.elapsed() < 5000)
            QTest::qWait(10);
        return page->property("done").toBool() ? page->property("result") : QVariant();
    };
    QTRY_VERIFY_WITH_TIMEOUT(eval("view.readerReady").toBool(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(eval("view.bookReady").toBool(), 15000);
    eval(
        "root.done=false;view.runJavaScript('reader.goToAnnotation('+JSON.stringify(testNote)+').then(()=>({key:reader.currentAnnotationKey,cfi:reader.view."
        "lastLocation.cfi}))',value=>{root.result=value;root.done=true})");
    QTRY_VERIFY_WITH_TIMEOUT(page->property("done").toBool(), 10000);
    // runJavaScript does not await a JS Promise; inspect the resolved runtime position separately.
    QTRY_VERIFY_WITH_TIMEOUT(eval("backend.annotationForValue(testNote.annotationId)?.runtimeCfi?.length > 0").toBool(), 10000);
    const auto cfi = eval("backend.annotationForValue(testNote.annotationId).runtimeCfi").toString();
    QVERIFY(cfi != note.value(QStringLiteral("cfiRange")).toString());
    engine.rootContext()->setContextProperty(QStringLiteral("resolvedCfi"), cfi);
    eval(
        "root.done=false;view.runJavaScript('(() => {const {anchor}=reader.view.resolveCFI('+JSON.stringify(resolvedCfi)+',reader.view.cfiFilter);const "
        "doc=reader.view.renderer.getContents()[0].doc;return anchor(doc).toString()})()',value=>{root.result=value;root.done=true})");
    QTRY_VERIFY_WITH_TIMEOUT(page->property("done").toBool(), 5000);
    QCOMPARE(page->property("result").toString(), QStringLiteral("anchored"));
    // Click the actual highlighted range in the reader document.
    eval(
        "view.runJavaScript('(() => {const doc=reader.view.renderer.getContents()[0].doc;const "
        "range=reader.view.resolveCFI('+JSON.stringify(resolvedCfi)+',reader.view.cfiFilter).anchor(doc);const "
        "r=range.getBoundingClientRect();range.startContainer.parentElement.dispatchEvent(new "
        "doc.defaultView.MouseEvent(\"click\",{bubbles:true,cancelable:true,clientX:r.x+r.width/2,clientY:r.y+r.height/2}))})()')");
    auto *dialog = page->findChild<QObject *>(QStringLiteral("richNoteDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(!dialog->property("editing").toBool());
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    auto *browser = dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser"));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), browser);
    QTRY_VERIFY_WITH_TIMEOUT(eval("noteBrowser.loading === false").toBool(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        noteScript(QStringLiteral("document.getElementById('note')?.textContent || ''")).toString().contains(QStringLiteral("Second user edition")),
        10000);
    eval("richNoteDialog.close()");
    Config::setCodexPath(QString::fromUtf8(CODEX_TEST_PEER_PATH));
    qputenv("ARIANNA_FAKE_CODEX", "stream");
    eval(
        "view.runJavaScript('(() => {const doc=reader.view.renderer.getContents()[0].doc;const "
        "r=reader.view.resolveCFI('+JSON.stringify(resolvedCfi)+',reader.view.cfiFilter).anchor(doc);doc.getSelection().removeAllRanges();doc.getSelection()."
        "addRange(r);doc.dispatchEvent(new doc.defaultView.PointerEvent(\"pointerup\",{bubbles:true}))})()')");
    auto *discuss = page->findChild<QObject *>(QStringLiteral("aiDiscussionMenuItem"));
    QVERIFY(discuss);
    QTRY_VERIFY_WITH_TIMEOUT(discuss->property("enabled").toBool(), 10000);
    QVERIFY(QMetaObject::invokeMethod(discuss, "clicked"));
    auto *client = engine.singletonInstance<CodexClient *>("org.kde.arianna", "CodexClient");
    QVERIFY(client);
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), CodexClient::Ready, 10000);
    auto *send = page->findChild<QObject *>(QStringLiteral("codexSendButton"));
    QVERIFY(send);
    QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
    auto *saveAnswer = page->findChild<QObject *>(QStringLiteral("aiDiscussionSaveRemarkButton"));
    QVERIFY(saveAnswer);
    QTRY_VERIFY_WITH_TIMEOUT(saveAnswer->property("enabled").toBool(), 10000);
    const auto beforeDraft = bytes();
    QVERIFY(QMetaObject::invokeMethod(saveAnswer, "clicked"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    auto *draft = dialog->findChild<QObject *>(QStringLiteral("aiRemarkDraft"));
    QVERIFY(draft);
    QCOMPARE(dialog->property("highlightColor").toString(), QStringLiteral("yellow"));
    QVERIFY(dialog->setProperty("highlightColor", QStringLiteral("#64b5f6")));
    QVERIFY(draft->setProperty("text", QStringLiteral("# Personally edited AI answer\n\n**śrī** चेतना \\(x^2\\)\n\n| A | B |\n|---|---|\n| visible | table |")));
    QCOMPARE(bytes(), beforeDraft);
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 1);
    auto *tabs = dialog->findChild<QObject *>(QStringLiteral("aiRemarkTabs"));
    QVERIFY(tabs && tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("document.querySelector('td')?.textContent.includes('visible') || false")).toBool(), 10000);
    QCOMPARE(bytes(), beforeDraft);
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 1);
    auto *save = dialog->findChild<QObject *>(QStringLiteral("aiRemarkSaveButton"));
    QVERIFY(save && save->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(BookDatabase::self().loadAnnotations(bookId).size(), 2, 10000);
    const auto saved = BookDatabase::self().loadAnnotations(bookId).last().toMap();
    QCOMPARE(saved.value(QStringLiteral("color")).toString(), QStringLiteral("#64b5f6"));
    QVERIFY(saved.value(QStringLiteral("noteSource")).toString().contains(QStringLiteral("Personally edited")));
    QVERIFY(saved.value(QStringLiteral("noteHtml")).toString().contains(QStringLiteral("<strong>śrī</strong>")));
    QVERIFY(!saved.value(QStringLiteral("anchorId")).toString().isEmpty());
    QVERIFY(!chapter().contains("Personally edited"));
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    browser = dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser"));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), browser);
    QTRY_VERIFY_WITH_TIMEOUT(
        noteScript(QStringLiteral("document.getElementById('note')?.textContent || ''")).toString().contains(QStringLiteral("Personally edited")),
        10000);
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("!!document.querySelector('mjx-container')")).toBool(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(eval("view.bookReady").toBool(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(eval("backend.annotations.every(note=>!note.invalid && !!note.runtimeCfi)").toBool(), 10000);
    QCOMPARE(eval("aiDiscussionDialog.latestSubmission.annotationId").toString(), saved.value(QStringLiteral("annotationId")).toString());
    const auto savedEpub = bytes();
    // Reopening the same answer edits its saved version, without another anchor.
    eval("richNoteDialog.close()");
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(saveAnswer, "clicked"));
    QTRY_VERIFY(dialog->property("editing").toBool());
    QCOMPARE(dialog->property("draft").toString(), saved.value(QStringLiteral("noteSource")).toString());
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 2);
    QCOMPARE(bytes(), savedEpub);
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("typeof showNote === 'function'")).toBool(), 10000);
    noteScript(
        QStringLiteral("showNote('<script>globalThis.unsafeNote=true</script><p onclick=\"unsafeNote=true\">safe content</p>"
                       "<a href=\"javascript:unsafeNote=true\">unsafe link</a><iframe></iframe><img src=\"data:,bad\">')"));
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("document.getElementById('note')?.textContent.includes('safe content')")).toBool(), 10000);
    QVERIFY(noteScript(QStringLiteral("!globalThis.unsafeNote && !document.querySelector('#note script,#note iframe,#note img,#note [onclick],#note a[href]')"))
                .toBool());
    // Save a real two-turn discussion independently of the edited single answer.
    eval("richNoteDialog.close()");
    QTRY_VERIFY(!dialog->property("visible").toBool());
    const QString firstQuestion = eval("aiDiscussionDialog.submissions[0].question").toString();
    const QString originalAnswer = eval("aiDiscussionDialog.submissions[0].answer").toString();
    auto *question = page->findChild<QObject *>(QStringLiteral("aiDiscussionQuestionPrompt"));
    QVERIFY(question && question->setProperty("text", QStringLiteral("What follows from this passage?")));
    QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
    auto *saveDiscussion = page->findChild<QObject *>(QStringLiteral("aiDiscussionSaveConversationButton"));
    QVERIFY(saveDiscussion);
    QTRY_VERIFY_WITH_TIMEOUT(saveDiscussion->property("enabled").toBool(), 10000);
    QCOMPARE(eval("aiDiscussionDialog.submissions.length").toInt(), 2);
    QVERIFY(QMetaObject::invokeMethod(saveDiscussion, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(BookDatabase::self().loadAnnotations(bookId).size(), 3, 10000);
    auto discussion = eval("aiDiscussionDialog.savedDiscussion").toMap();
    QVERIFY2(!discussion.isEmpty() && !discussion.value(QStringLiteral("error")).toBool(),
             qPrintable(eval("aiDiscussionDialog.saveError").toString()));
    const auto discussionId = discussion.value(QStringLiteral("annotationId")).toString();
    QVERIFY(!discussionId.isEmpty() && discussionId != saved.value(QStringLiteral("annotationId")).toString());
    QCOMPARE(discussion.value(QStringLiteral("anchorId")), saved.value(QStringLiteral("anchorId")));
    QCOMPARE(bytes(), savedEpub);
    const auto turns = discussion.value(QStringLiteral("noteMetadata")).toMap().value(QStringLiteral("turns")).toList();
    QCOMPARE(turns.size(), 2);
    QCOMPARE(turns.first().toMap().value(QStringLiteral("question")).toString(), firstQuestion);
    QCOMPARE(turns.first().toMap().value(QStringLiteral("answer")).toString(), originalAnswer);
    QCOMPARE(turns.last().toMap().value(QStringLiteral("question")).toString(), QStringLiteral("What follows from this passage?"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(dialog->property("discussion").toBool());
    QVERIFY(!dialog->property("editing").toBool());
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    QTRY_COMPARE_WITH_TIMEOUT(noteScript(QStringLiteral("document.querySelectorAll('#note .chat-message').length")).toInt(), 4, 10000);
    QCOMPARE(noteScript(QStringLiteral("document.querySelectorAll('#note .chat-user').length")).toInt(), 2);
    QVERIFY(noteScript(QStringLiteral("document.getElementById('note').textContent.includes('What follows from this passage?')")).toBool());
    QVERIFY(dialog->findChild<QObject *>(QStringLiteral("aiRemarkEditButton"))->property("visible").toBool());
    engine.rootContext()->setContextProperty(QStringLiteral("discussionId"), discussionId);
    QVERIFY(eval("(()=>{for(let i=0;i<notesModel.count;i++){const n=notesModel.get(i);if(n.annotationId===discussionId) return n.ai && n.discussion && n.questions.includes('What follows from this passage?') && n.questions.includes(aiDiscussionDialog.submissions[0].question)}return false})()").toBool());
    // Repeated saving updates the same remark and never duplicates the anchor.
    eval("richNoteDialog.close()");
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(saveDiscussion, "clicked"));
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 3);
    QCOMPARE(eval("aiDiscussionDialog.savedDiscussion.annotationId").toString(), discussionId);
    QCOMPARE(bytes(), savedEpub);
    eval("richNoteDialog.close();aiDiscussionDialog.close()");
    QTRY_COMPARE(client->state(), CodexClient::Disconnected);
    // The ordinary highlight still opens the discussion; the reader has no AI icon.
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(eval("backend.annotationForValue(discussionId)?.runtimeCfi?.length > 0").toBool(), 10000);
    const auto markerScript = QStringLiteral(R"JS((() => {
        const c=reader.view.renderer.getContents()[0];
        if(c.overlayer.element.querySelector('[data-ai-remark]')) return false;
        const range=reader.view.resolveCFI(%1[0],reader.view.cfiFilter).anchor(c.doc);
        const r=range.getBoundingClientRect();
        c.doc.getSelection().removeAllRanges();
        range.startContainer.parentElement.dispatchEvent(new c.doc.defaultView.MouseEvent('click', {bubbles:true,
            clientX:r.x+r.width/2,clientY:r.y+r.height/2}));
        return true;
    })())JS").arg(QString::fromUtf8(QJsonDocument(QJsonArray{cfi}).toJson(QJsonDocument::Compact)));
    engine.rootContext()->setContextProperty(QStringLiteral("discussionMarkerScript"), markerScript);
    eval("root.done=false;view.runJavaScript(discussionMarkerScript,v=>{root.result=v;root.done=true})");
    QTRY_VERIFY_WITH_TIMEOUT(page->property("done").toBool(), 5000);
    QVERIFY(page->property("result").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(dialog->property("visible").toBool(), 10000);
    QCOMPARE(dialog->property("annotation").toMap().value(QStringLiteral("annotationId")).toString(), discussionId);
    QVERIFY(dialog->property("discussion").toBool());
    QCOMPARE(client->state(), CodexClient::Disconnected);
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    QTRY_COMPARE_WITH_TIMEOUT(noteScript(QStringLiteral("document.querySelectorAll('.chat-message').length")).toInt(), 4, 10000);
    if (!qEnvironmentVariableIsEmpty("ARIANNA_NOTE_SCREENSHOT")) {
        QTest::qWait(300);
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_NOTE_SCREENSHOT")));
    }
    QCOMPARE(bytes(), savedEpub);
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 3);
    eval("richNoteDialog.close();notesDialog.open();notesListView.positionViewAtEnd();notesListView.forceLayout()");
    QTRY_VERIFY(eval(R"QML((()=>{
        const find=(item,name)=>{
            if(item.objectName===name) return item;
            for(const child of item.children || []) {const found=find(child,name);if(found) return found}
            return null;
        };
        for(let i=0;i<notesModel.count;i++) {
            if(notesModel.get(i).annotationId!==discussionId) continue;
            const item=notesListView.itemAtIndex(i);
            if(!item) return false;
            const label=find(item,'aiRemarkOverviewQuestions');
            const icon=find(item,'aiRemarkOverviewIcon');
            return !!label?.visible && label.text.includes('What follows from this passage?') && !!icon?.visible;
        }
        return false;
    })())QML").toBool());
    if (!qEnvironmentVariableIsEmpty("ARIANNA_NOTE_SCREENSHOT")) {
        QTest::qWait(300);
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_NOTE_SCREENSHOT") + QStringLiteral("-overview.png")));
    }
    // Repeater delegates belong to the visual item tree, not necessarily the QObject tree.
    const auto discussionField = [&](const QString &name) -> QObject * {
        const auto find = [&](const auto &self, QQuickItem *item) -> QObject * {
            if (!item) return nullptr;
            if (item->objectName() == name) return item;
            for (auto *child : item->childItems()) {
                if (auto *found = self(self, child)) return found;
            }
            return nullptr;
        };
        return find(find, dialog->property("contentItem").value<QQuickItem *>());
    };
    // The overview's edit action opens the structured Markdown draft offline.
    const auto editFromOverview = [&] {
        return eval(R"QML((()=>{
            const find=(item,name)=>{
                if(item.objectName===name) return item;
                for(const child of item.children || []) {const found=find(child,name);if(found) return found}
                return null;
            };
            for(let i=0;i<notesModel.count;i++) {
                if(notesModel.get(i).annotationId!==discussionId) continue;
                const button=find(notesListView.itemAtIndex(i),'aiRemarkOverviewEditButton');
                if(!button?.visible || !button.enabled) return false;
                button.clicked();
                return true;
            }
            return false;
        })())QML").toBool();
    };
    QVERIFY(editFromOverview());
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(dialog->property("editing").toBool());
    auto *questionDraft = discussionField(QStringLiteral("aiDiscussionRemarkQuestion0"));
    auto *answerDraft = discussionField(QStringLiteral("aiDiscussionRemarkAnswer1"));
    QVERIFY(questionDraft && answerDraft);
    QCOMPARE(questionDraft->property("text").toString(), firstQuestion);
    QVERIFY(questionDraft->setProperty("text", QStringLiteral("Cancelled question")));
    QVERIFY(answerDraft->setProperty("text", QStringLiteral("Cancelled answer")));
    const auto originalDiscussionColor = discussion.value(QStringLiteral("color")).toString();
    QVERIFY(dialog->setProperty("highlightColor", QStringLiteral("#ef5350")));
    // Opening/editing/cancelling cannot mutate the saved metadata through aliases.
    QCOMPARE(dialog->property("annotation").toMap().value(QStringLiteral("noteMetadata")).toMap().value(QStringLiteral("turns")).toList(), turns);
    eval("richNoteDialog.close()");
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(AnnotationStore().loadAnnotations(bookId).last().toMap().value(QStringLiteral("noteMetadata")).toMap().value(QStringLiteral("turns")).toList(), turns);
    QCOMPARE(AnnotationStore().loadAnnotations(bookId).last().toMap().value(QStringLiteral("color")).toString(), originalDiscussionColor);
    QCOMPARE(bytes(), savedEpub);
    eval("notesDialog.open();notesListView.positionViewAtEnd();notesListView.forceLayout()");
    QTRY_VERIFY(editFromOverview());
    QTRY_VERIFY(dialog->property("visible").toBool());
    questionDraft = discussionField(QStringLiteral("aiDiscussionRemarkQuestion0"));
    answerDraft = discussionField(QStringLiteral("aiDiscussionRemarkAnswer1"));
    QVERIFY(questionDraft && answerDraft);
    QCOMPARE(questionDraft->property("text").toString(), firstQuestion);
    QVERIFY(questionDraft->setProperty("text", QString()));
    QVERIFY(!save->property("enabled").toBool());
    const QString editedQuestion = QStringLiteral("My edited **question** about śrī?");
    const QString editedAnswer = QStringLiteral("My edited **answer** with चेतना and \\(x^2\\).");
    QVERIFY(questionDraft->setProperty("text", editedQuestion));
    QVERIFY(answerDraft->setProperty("text", editedAnswer));
    QVERIFY(dialog->setProperty("highlightColor", QStringLiteral("#81c784")));
    QVERIFY(save->property("enabled").toBool());
    tabs = dialog->findChild<QObject *>(QStringLiteral("aiRemarkTabs"));
    QVERIFY(tabs && tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    engine.rootContext()->setContextProperty(QStringLiteral("noteBrowser"), dialog->findChild<QObject *>(QStringLiteral("noteHtmlBrowser")));
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("document.getElementById('note')?.textContent.includes('My edited question')")).toBool(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("document.getElementById('note')?.textContent.includes('My edited answer')")).toBool(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(noteScript(QStringLiteral("!!document.querySelector('mjx-container')")).toBool(), 15000);
    if (!qEnvironmentVariableIsEmpty("ARIANNA_NOTE_SCREENSHOT")) {
        QTest::qWait(300);
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_NOTE_SCREENSHOT") + QStringLiteral("-edited-preview.png")));
    }
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_VERIFY(!dialog->property("editing").toBool());
    const auto edited = AnnotationStore().loadAnnotations(bookId).last().toMap();
    QCOMPARE(edited.value(QStringLiteral("color")).toString(), QStringLiteral("#81c784"));
    const auto editedTurns = edited.value(QStringLiteral("noteMetadata")).toMap().value(QStringLiteral("turns")).toList();
    QCOMPARE(editedTurns.size(), 2);
    for (int i=0; i<turns.size(); ++i) {
        auto expected = turns[i].toMap();
        if (i == 0) expected.insert(QStringLiteral("question"), editedQuestion);
        if (i == 1) expected.insert(QStringLiteral("answer"), editedAnswer);
        QCOMPARE(editedTurns[i].toMap(), expected);
    }
    QCOMPARE(edited.value(QStringLiteral("annotationId")), discussion.value(QStringLiteral("annotationId")));
    QCOMPARE(edited.value(QStringLiteral("anchorId")), discussion.value(QStringLiteral("anchorId")));
    QCOMPARE(edited.value(QStringLiteral("created")), discussion.value(QStringLiteral("created")));
    QVERIFY(edited.value(QStringLiteral("noteHtml")).toString().contains(QStringLiteral("<strong>answer</strong>")));
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 3);
    QCOMPARE(bytes(), savedEpub);
    QCOMPARE(client->state(), CodexClient::Disconnected);
    // Reopening uses persisted turns; the overview immediately reflects the edit.
    eval("richNoteDialog.close();backend.loadAnnotations();notesDialog.open();notesListView.positionViewAtEnd();notesListView.forceLayout()");
    QTRY_VERIFY(eval("(()=>{for(let i=0;i<notesModel.count;i++){const n=notesModel.get(i);if(n.annotationId===discussionId) return n.questions.includes('My edited **question**')}return false})()").toBool());
    QTRY_VERIFY(editFromOverview());
    questionDraft = discussionField(QStringLiteral("aiDiscussionRemarkQuestion0"));
    answerDraft = discussionField(QStringLiteral("aiDiscussionRemarkAnswer1"));
    QVERIFY(questionDraft && answerDraft);
    QCOMPARE(questionDraft->property("text").toString(), editedQuestion);
    QCOMPARE(answerDraft->property("text").toString(), editedAnswer);
    if (!qEnvironmentVariableIsEmpty("ARIANNA_NOTE_SCREENSHOT")) {
        QTest::qWait(300);
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_NOTE_SCREENSHOT") + QStringLiteral("-editor.png")));
    }
    eval("richNoteDialog.close();notesDialog.close()");
    QTRY_VERIFY(!dialog->property("visible").toBool());
    // A new ordinary highlight has the same palette. Cancel must not insert
    // an EPUB anchor; confirmed colors persist and redraw through the reader.
    const auto readerScript = [&](const QString &code) {
        const auto encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(model, QStringLiteral("root.done=false;view.runJavaScript(%1[0],value=>{root.result=value;root.done=true})").arg(encoded));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("done").toBool() && timer.elapsed() < 5000) QTest::qWait(10);
        return page->property("done").toBool() ? page->property("result") : QVariant();
    };
    auto *plainDialog = page->findChild<QObject *>(QStringLiteral("epubAnnotationDialog"));
    auto *plainColors = page->findChild<QObject *>(QStringLiteral("epubAnnotationColors"));
    auto *plainNote = page->findChild<QObject *>(QStringLiteral("epubAnnotationNote"));
    auto *highlight = page->findChild<QObject *>(QStringLiteral("epubHighlightMenuItem"));
    QVERIFY(plainDialog && plainColors && plainNote && highlight);
    const auto selectNewPassage = [&] {
        readerScript(QStringLiteral(R"JS((()=>{
            const doc=reader.view.renderer.getContents()[0].doc,range=doc.createRange();
            const text=doc.querySelector('p').firstChild;
            range.setStart(text,0);range.setEnd(text,text.length);
            doc.getSelection().removeAllRanges();doc.getSelection().addRange(range);
            doc.dispatchEvent(new doc.defaultView.PointerEvent('pointerup',{bubbles:true}));
        })())JS"));
        QTRY_VERIFY(eval("selectionPopup.opened && backend.canAnnotateSelection()").toBool());
        QVERIFY(QMetaObject::invokeMethod(highlight, "clicked"));
        QTRY_VERIFY(plainDialog->property("opened").toBool());
    };
    selectNewPassage();
    QCOMPARE(plainColors->property("selectedColor").toString(), QStringLiteral("yellow"));
    QVERIFY(plainColors->setProperty("selectedColor", QStringLiteral("#ef5350")));
    QSignalSpy closed(plainDialog, SIGNAL(closed()));
    QVERIFY(QMetaObject::invokeMethod(plainDialog, "reject"));
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(bytes(), savedEpub);
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 3);
    selectNewPassage();
    QVERIFY(plainColors->setProperty("selectedColor", QStringLiteral("#ffb74d")));
    QVERIFY(plainNote->setProperty("text", QStringLiteral("An orange ordinary remark")));
    if (!qEnvironmentVariableIsEmpty("ARIANNA_PALETTE_SCREENSHOT")) {
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window && window->grabWindow().save(qEnvironmentVariable("ARIANNA_PALETTE_SCREENSHOT") + QStringLiteral("-epub.png")));
    }
    QVERIFY(QMetaObject::invokeMethod(plainDialog, "accept"));
    QTRY_COMPARE(BookDatabase::self().loadAnnotations(bookId).size(), 4);
    const auto plain = BookDatabase::self().loadAnnotations(bookId).last().toMap();
    QCOMPARE(plain.value(QStringLiteral("color")).toString(), QStringLiteral("#ffb74d"));
    QCOMPARE(plain.value(QStringLiteral("note")).toString(), QStringLiteral("An orange ordinary remark"));
    const auto anchoredEpub = bytes();
    QTRY_VERIFY_WITH_TIMEOUT(eval("view.bookReady && backend.annotations.every(note=>!note.invalid && !!note.runtimeCfi)").toBool(), 10000);
    QTRY_VERIFY(readerScript(QStringLiteral("reader.view.renderer.getContents()[0].overlayer.element.querySelector('[fill=\"#ffb74d\"] rect') !== null")).toBool());
    engine.rootContext()->setContextProperty(QStringLiteral("plainNoteId"), plain.value(QStringLiteral("annotationId")));
    eval("backend.openAnnotationEditor(plainNoteId)");
    QTRY_VERIFY(plainDialog->property("opened").toBool());
    QCOMPARE(plainColors->property("selectedColor").toString(), QStringLiteral("#ffb74d"));
    QVERIFY(plainColors->setProperty("selectedColor", QStringLiteral("#f06292")));
    QVERIFY(QMetaObject::invokeMethod(plainDialog, "accept"));
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).last().toMap().value(QStringLiteral("anchorId")), plain.value(QStringLiteral("anchorId")));
    QCOMPARE(BookDatabase::self().loadAnnotations(bookId).last().toMap().value(QStringLiteral("color")).toString(), QStringLiteral("#f06292"));
    QTRY_VERIFY(readerScript(QStringLiteral("reader.view.renderer.getContents()[0].overlayer.element.querySelector('[fill=\"#f06292\"] rect') !== null")).toBool());
    QCOMPARE(bytes(), anchoredEpub);
}

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    for (const char *name : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
        qputenv(name, QFile::encodeName(environment.path()));
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("arianna-ai-note-test"));
    KLocalizedString::setApplicationDomain("arianna");
    AiNoteTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ainotetest.moc"
