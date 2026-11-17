// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookdatabase.h"
#include "bookserver.h"
#include "categoryentriesmodel.h"
#include "config.h"
#include "editorsessionstore.h"
#include "epubcontainer.h"
#include "maintenancefiles.h"
#include "referencestore.h"
#include "workstructure.h"

#include <KLocalizedString>
#include <KZip>
#include <KArchiveFile>
#include <QBuffer>
#include <QDateTime>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QScopedPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class BookServerReferenceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void clearedPreviewSurvivesReloadAndFurtherEdits();
    void copyConflictsAreCheckedOnImport_data();
    void copyConflictsAreCheckedOnImport();
    void imageInversionUsesBackupSetting();
    void nativeReferenceSearchAndTitles();
    void requestsDistinguishTimeoutAndNetworkFailure();
    void fragmentTargetsMustMatchExactly();
    void externalEpubTargetLookup();
    void tocNodeIdentities_data();
    void tocNodeIdentities();

private:
    QJsonObject request(const QString &baseUrl, const QString &path, const QJsonObject &body = {}, int expectedStatus = 200);
    QTemporaryDir m_directory;
};

void BookServerReferenceTest::initTestCase()
{
    QVERIFY(m_directory.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(m_directory.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_directory.path()));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(m_directory.path()));
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_directory.path()));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-bookserverreferencetest"));
    KLocalizedString::setApplicationDomain("arianna");
    QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation), m_directory.path());
}

QJsonObject BookServerReferenceTest::request(const QString &baseUrl, const QString &path, const QJsonObject &body, int expectedStatus)
{
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(baseUrl + path)};
    request.setRawHeader("X-Arianna-Session-Token", "reference-test-token");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QScopedPointer<QNetworkReply> reply(body.isEmpty() ? manager.get(request) : manager.post(request, QJsonDocument(body).toJson()));
    QSignalSpy finished(reply.data(), &QNetworkReply::finished);
    if (!reply->isFinished() && !finished.wait(5000)) {
        QTest::qFail("BookServer request timed out", __FILE__, __LINE__);
        return {};
    }
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != expectedStatus) {
        QTest::qFail(qPrintable(reply->errorString()), __FILE__, __LINE__);
        return {};
    }
    return QJsonDocument::fromJson(reply->readAll()).object();
}

void BookServerReferenceTest::clearedPreviewSurvivesReloadAndFurtherEdits()
{
    const QString epubPath = m_directory.filePath(QStringLiteral("book.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
    QVERIFY(
        zip.writeFile(QStringLiteral("META-INF/container.xml"),
                      QByteArrayLiteral("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles>"
                                        "<rootfile full-path=\"OEBPS/package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>")));
    QVERIFY(zip.writeFile(QStringLiteral("OEBPS/package.opf"),
                          QByteArrayLiteral("<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"3.0\">"
                                            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"bookid\">preview-book</dc:identifier>"
                                            "<dc:title>Preview Test</dc:title></metadata><manifest>"
                                            "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
                                            "</manifest><spine><itemref idref=\"chapter\"/></spine></package>")));
    QVERIFY(zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"),
                          QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p><a id=\"source-anchor\" data-role=\"anchor\">Source</a></p>"
                                            "<p id=\"verse\">Automatic preview</p></body></html>")));
    QVERIFY(zip.close());
    BookEntry entry;
    entry.filename = epubPath;
    entry.filetitle = QFileInfo(epubPath).fileName();
    entry.title = QStringLiteral("Preview Test");
    entry.identifier = QStringLiteral("preview-book");
    entry.uniqueIdentifier = entry.identifier;
    entry.created = QDateTime::currentDateTimeUtc();
    BookDatabase::self().addEntry(entry);

    // Use an available loopback port and an isolated discovery file, not the running reader's server.
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reference-test-token"));
    QVERIFY(server.isRunning());
    QFile discovery(m_directory.filePath(QStringLiteral("arianna-bookserver.json")));
    QVERIFY(discovery.open(QIODevice::ReadOnly));
    const QString baseUrl = QJsonDocument::fromJson(discovery.readAll()).object().value(QStringLiteral("baseUrl")).toString();
    QVERIFY(!baseUrl.isEmpty());

    QJsonObject reference{{QStringLiteral("sourceAnchorId"), QStringLiteral("source-anchor")},
                          {QStringLiteral("sourceAnchorTitle"), QStringLiteral("Reference title")},
                          {QStringLiteral("targetBookId"), entry.uniqueIdentifier},
                          {QStringLiteral("targetLocation"), QStringLiteral("OEBPS/chapter.xhtml#verse")},
                          {QStringLiteral("targetPreviewHtml"), QStringLiteral("")},
                          {QStringLiteral("targetPreviewHtmlEdited"), false}};
    ReferenceStore store;
    const QString postPath = QStringLiteral("/preview-book/ref");
    const QString getPath = QStringLiteral("/preview-book/ref/source-anchor");
    const QJsonObject created = request(baseUrl, postPath, reference);
    QVERIFY(created.value(QStringLiteral("targetPreviewHtml")).toString().contains(QStringLiteral("Automatic preview")));

    reference.insert(QStringLiteral("targetPreviewHtmlEdited"), true);
    const QJsonObject cleared = request(baseUrl, postPath, reference);
    QCOMPARE(cleared.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));
    const QVariantMap stored = store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source-anchor"));
    QCOMPARE(stored.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));
    for (int i = 0; i < 2; ++i) {
        const QJsonObject loaded = request(baseUrl, getPath);
        QCOMPARE(loaded.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));
        QCOMPARE(loaded.value(QStringLiteral("target")).toObject().value(QStringLiteral("previewHtml")).toString(), QStringLiteral(""));
        QCOMPARE(store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source-anchor")), stored);
    }

    // Reopening the dialog and only editing its title must not regenerate the empty preview.
    reference.insert(QStringLiteral("targetPreviewHtmlEdited"), false);
    reference.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Changed title"));
    const QJsonObject edited = request(baseUrl, postPath, reference);
    QCOMPARE(edited.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral(""));
    QCOMPARE(request(baseUrl, getPath).value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("Changed title"));
    QCOMPARE(store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source-anchor")).value(QStringLiteral("targetPreviewHtml")).toString(),
             QStringLiteral(""));

    // A reference with an explicit location and no preview needs no target-book lookup.
    QVariantMap unavailableTarget = store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source-anchor"));
    unavailableTarget.insert(QStringLiteral("targetBookId"), QStringLiteral("unavailable-target"));
    store.saveReference(unavailableTarget);
    QCOMPARE(request(baseUrl, getPath).value(QStringLiteral("targetLocation")).toString(), QStringLiteral("OEBPS/chapter.xhtml#verse"));

    // A stale selection offers a manual span for crossrefs and a native link for internal references.
    for (const bool crossReference : {false, true}) {
        const QJsonObject manual = request(baseUrl,
                                           postPath,
                                           {{QStringLiteral("cfi"), QStringLiteral("epubcfi(/6/2!/2/200,/1:0,/1:5)")},
                                            {QStringLiteral("sourceAnchorTitle"), QStringLiteral("B.S. Vers 5.33 <&>\"")},
                                            {QStringLiteral("text"), QStringLiteral("B.S. 5.33")},
                                            {QStringLiteral("targetBookId"), crossReference ? QStringLiteral("unavailable-target") : entry.uniqueIdentifier},
                                            {QStringLiteral("targetLocation"), QStringLiteral("OEBPS/chapter.xhtml#verse")}},
                                           409);
        QCOMPARE(manual.value(QStringLiteral("error")).toString(), QStringLiteral("manualAnchorRequired"));
        QDomDocument templateDocument;
        QVERIFY(templateDocument.setContent(manual.value(QStringLiteral("anchorOpenTag")).toString() + QStringLiteral("B.S. 5.33")
                                            + manual.value(QStringLiteral("anchorCloseTag")).toString()));
        const auto anchor = templateDocument.documentElement();
        QCOMPARE(anchor.tagName(), crossReference ? QStringLiteral("span") : QStringLiteral("a"));
        QCOMPARE(anchor.attribute(QStringLiteral("title")), QStringLiteral("B.S. Vers 5.33 <&>\""));
        QCOMPARE(anchor.hasAttribute(QStringLiteral("href")), !crossReference);
    }
    // Updating a reference also remains possible while an editor copy exists.
    EditorSessionStore sessions;
    const auto session = sessions.createEditorSession(entry.uniqueIdentifier, entry.filename);
    QVERIFY2(!session.value(QStringLiteral("error")).toBool(), qPrintable(session.value(QStringLiteral("message")).toString()));
    const auto copy = session.value(QStringLiteral("workingCopyPath")).toString();
    const auto copyHash = MaintenanceFiles::digest(copy);
    reference.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Title with working copy"));
    const auto allowed = request(baseUrl, postPath, reference);
    QVERIFY(!allowed.contains(QStringLiteral("error")));
    QCOMPARE(request(baseUrl, getPath).value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("Title with working copy"));
    QCOMPARE(MaintenanceFiles::digest(copy), copyHash);
    sessions.finishSession(session.value(QStringLiteral("sessionId")).toString());
    QTRY_VERIFY(!QFileInfo::exists(copy));
    server.stop();
}

void BookServerReferenceTest::tocNodeIdentities_data()
{
    QTest::addColumn<bool>("ncx");
    QTest::newRow("epub3-navigation") << false;
    QTest::newRow("epub2-ncx") << true;
}

void BookServerReferenceTest::tocNodeIdentities()
{
    QFETCH(bool, ncx);
    const QString id = ncx ? QStringLiteral("toc-ncx") : QStringLiteral("toc-nav");
    const QString file = m_directory.filePath(id + QStringLiteral(".epub"));
    KZip zip(file);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
        "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    const auto manifest = ncx ? QStringLiteral("<item id=\"nav\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>")
                              : QStringLiteral("<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>");
    const QString package = QString::fromUtf8("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"%3\" unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">%1</dc:identifier><dc:title>TOC Test</dc:title></metadata><manifest><item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"two\" href=\"two.xhtml\" media-type=\"application/xhtml+xml\"/>%2</manifest><spine toc=\"nav\"><itemref idref=\"one\"/><itemref idref=\"two\"/></spine></package>").arg(id, manifest, ncx ? QStringLiteral("2.0") : QStringLiteral("3.0"));
    QVERIFY(zip.writeFile(QStringLiteral("package.opf"), package.toUtf8()));
    for (const auto &chapter : {QStringLiteral("one.xhtml"), QStringLiteral("two.xhtml")}) {
        QVERIFY(zip.writeFile(chapter, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1 id=\"chapter\">Chapter</h1><p id=\"verse_18-20\">Needle</p></body></html>"));
    }
    if (ncx) {
        QVERIFY(zip.writeFile(QStringLiteral("toc.ncx"), "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\"><navMap>\n            <navPoint id=\"p1\"><navLabel><text>Part</text></navLabel><navPoint id=\"c1\"><navLabel><text>Chapter</text></navLabel><content src=\"one.xhtml#chapter\"/></navPoint></navPoint>\n            <navPoint id=\"p2\"><navLabel><text>Part</text></navLabel><navPoint id=\"c2\"><navLabel><text>Chapter</text></navLabel><content src=\"two.xhtml#chapter\"/></navPoint></navPoint>\n        </navMap></ncx>"));
    } else {
        QVERIFY(zip.writeFile(QStringLiteral("nav.xhtml"), "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body><nav epub:type=\"toc\"><ol>\n            <li><span>Part</span><ol><li><a href=\"one.xhtml#chapter\">Chapter</a></li></ol></li>\n            <li><span>Part</span><ol><li><a href=\"two.xhtml#chapter\">Chapter</a></li></ol></li>\n        </ol></nav></body></html>"));
    }
    QVERIFY(zip.close());
    const auto hash = MaintenanceFiles::digest(file);
    BookEntry entry;
    entry.identifier = entry.uniqueIdentifier = id;
    entry.filename = file;
    entry.title = QStringLiteral("TOC Test");
    BookDatabase::self().addEntry(entry);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reference-test-token"));
    QVERIFY(server.isRunning());
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(Config::bookServerPort());
    const auto response = request(base, u'/' + id + QStringLiteral("/targetLocations"));
    QHash<QString, QJsonObject> byLocation;
    for (const auto &value : response.value(QStringLiteral("targetLocations")).toArray()) {
        const auto row = value.toObject();
        byLocation.insert(row.value(QStringLiteral("hrefLocation")).toString(), row);
    }
    const auto one = byLocation.value(QStringLiteral("one.xhtml#verse_18-20"));
    const auto two = byLocation.value(QStringLiteral("two.xhtml#verse_18-20"));
    QCOMPARE(one.value(QStringLiteral("tocPath")).toArray(), QJsonArray({QStringLiteral("Part"), QStringLiteral("Chapter")}));
    QCOMPARE(one.value(QStringLiteral("tocIdPath")).toArray().size(), 2);
    QCOMPARE(one.value(QStringLiteral("tocIdPath")), byLocation.value(QStringLiteral("one.xhtml#chapter")).value(QStringLiteral("tocIdPath")));
    QVERIFY(one.value(QStringLiteral("tocIdPath")) != two.value(QStringLiteral("tocIdPath")));
    QCOMPARE(two.value(QStringLiteral("tocPath")), one.value(QStringLiteral("tocPath")));
    QVERIFY(!one.value(QStringLiteral("cfiLocation")).toString().isEmpty());
    QCOMPARE(MaintenanceFiles::digest(file), hash);
    server.stop();
}

static bool writeReferenceEpub(const QString &path, const QString &id, const QByteArray &body, const bool fullDocument = false,
                               const QByteArray &secondBody = {})
{
    QFile::remove(path);
    KZip zip(path);
    const QByteArray package = QByteArrayLiteral("<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"3.0\">"
                                               "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"bookid\">")
        + id.toUtf8() + QByteArrayLiteral("</dc:identifier><dc:title>Copy Test</dc:title></metadata><manifest>"
                                         "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>")
        + (secondBody.isEmpty() ? QByteArray() : QByteArrayLiteral("<item id=\"second\" href=\"second.xhtml\" media-type=\"application/xhtml+xml\"/>"))
        + QByteArrayLiteral("</manifest><spine><itemref idref=\"chapter\"/>")
        + (secondBody.isEmpty() ? QByteArray() : QByteArrayLiteral("<itemref idref=\"second\"/>"))
        + QByteArrayLiteral("</spine></package>");
    const QByteArray chapter = fullDocument ? body
        : QByteArray(QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Test</title></head><body>")
                     + body + QByteArrayLiteral("</body></html>"));
    return zip.open(QIODevice::WriteOnly)
        && zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"))
        && zip.writeFile(QStringLiteral("META-INF/container.xml"),
                         QByteArrayLiteral("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
                                           "<rootfile full-path=\"OPS/package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"))
        && zip.writeFile(QStringLiteral("OPS/package.opf"), package)
        && zip.writeFile(QStringLiteral("OPS/chapter.xhtml"), chapter)
        && (secondBody.isEmpty() || zip.writeFile(QStringLiteral("OPS/second.xhtml"),
            QByteArray(QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>") + secondBody + QByteArrayLiteral("</body></html>"))))
        && zip.close();
}

void BookServerReferenceTest::copyConflictsAreCheckedOnImport_data()
{
    QTest::addColumn<QString>("copyKind");
    QTest::addColumn<bool>("separateTarget");
    QTest::newRow("active-copies-and-backups") << QStringLiteral("active") << false;
    QTest::newRow("active-copies-separate-target") << QStringLiteral("active") << true;
    QTest::newRow("orphaned-copies-and-backups") << QStringLiteral("orphaned") << false;
    QTest::newRow("backups-only") << QStringLiteral("none") << false;
}

void BookServerReferenceTest::copyConflictsAreCheckedOnImport()
{
    QFETCH(QString, copyKind);
    QFETCH(bool, separateTarget);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const bool previousBackup = Config::backupOnAnchoring();
    const auto restoreBackup = qScopeGuard([previousBackup] { Config::setBackupOnAnchoring(previousBackup); });
    Config::setBackupOnAnchoring(true);
    EditorSessionStore sessions;
    QList<BookEntry> books;
    QStringList copies, sessionIds, backups;
    QList<QByteArray> copyHashes, backupHashes, manifestHashes;
    const auto body = QByteArrayLiteral("<p>Selected passage.</p><p><span id=\"kept-note\" data-role=\"anchor\">Note text.</span></p>");
    for (const auto &role : {QStringLiteral("source"), QStringLiteral("target")}) {
        BookEntry book;
        book.uniqueIdentifier = book.identifier = QString::fromLatin1(QTest::currentDataTag()) + QLatin1Char('-') + role;
        book.filename = directory.filePath(role + QStringLiteral(".epub"));
        book.filetitle = QFileInfo(book.filename).fileName();
        book.title = role;
        book.created = QDateTime::currentDateTimeUtc();
        QVERIFY(writeReferenceEpub(book.filename, book.uniqueIdentifier, body));
        BookDatabase::self().addEntry(book);
        const QString annotationId = role + QStringLiteral("-note");
        QVERIFY(BookDatabase::self().saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), annotationId},
                                                                          {QStringLiteral("anchorId"), QStringLiteral("kept-note")},
                                                                          {QStringLiteral("note"), QStringLiteral("Saved remark")}}));
        books.append(book);
        const auto backup = book.filename + QStringLiteral(".before_anchoring_existing.epub");
        QVERIFY(QFile::copy(book.filename, backup));
        QVERIFY(MaintenanceFiles::registerBackup(book.filename, backup));
        MaintenanceFiles::completeBackup(backup);
        backups.append(backup);
        backupHashes.append(MaintenanceFiles::digest(backup));
        if (copyKind == QStringLiteral("none"))
            continue;
        QString copy;
        if (copyKind == QStringLiteral("active")) {
            const auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
            QVERIFY2(!session.value(QStringLiteral("error")).toBool(), qPrintable(session.value(QStringLiteral("message")).toString()));
            copy = session.value(QStringLiteral("workingCopyPath")).toString();
            sessionIds.append(session.value(QStringLiteral("sessionId")).toString());
        } else {
            // A legacy/abandoned copy has no session manifest and is recovered in Maintenance later.
            const QDir dir(MaintenanceFiles::sessionsRoot() + QLatin1Char('/') + QUuid::createUuidV7().toString(QUuid::WithoutBraces));
            QVERIFY(QDir().mkpath(dir.path()));
            copy = dir.filePath(book.filetitle);
            QVERIFY(QFile::copy(book.filename, copy));
        }
        copies.append(copy);
        copyHashes.append(MaintenanceFiles::digest(copy));
        manifestHashes.append(MaintenanceFiles::digest(MaintenanceFiles::manifestPath(copy)));
        QVERIFY(MaintenanceFiles::conflictingCopies(book).contains(copy));
    }
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    const QString baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reference-test-token"));
    QVERIFY(server.isRunning());
    const QString cfi = QStringLiteral("epubcfi(/6/2!/4/2,/1:0,/1:8)");
    QJsonObject reference{{QStringLiteral("cfi"), cfi},
                          {QStringLiteral("targetBookId"), books.at(1).uniqueIdentifier},
                          {QStringLiteral("targetCFI"), cfi}};
    const QString path = QLatin1Char('/') + books.first().uniqueIdentifier + QStringLiteral("/ref");
    {
        auto lock = MaintenanceFiles::lock();
        QVERIFY(lock);
        const auto blocked = request(baseUrl, path, reference, 409);
        QCOMPARE(blocked.value(QStringLiteral("error")).toString(), QStringLiteral("editorWorkingCopyExists"));
        QVERIFY(BookDatabase::self().loadReferences(books.first().uniqueIdentifier).isEmpty());
    }
    if (separateTarget) {
        const auto target = request(baseUrl, QLatin1Char('/') + books.at(1).uniqueIdentifier + QStringLiteral("/targetLocations"),
                                    {{QStringLiteral("cfi"), cfi}});
        QVERIFY(target.value(QStringLiteral("anchorCreated")).toBool());
        reference.remove(QStringLiteral("targetCFI"));
        reference.insert(QStringLiteral("targetLocation"), target.value(QStringLiteral("hrefLocation")));
    }
    const auto created = request(baseUrl, path, reference);
    QVERIFY(created.value(QStringLiteral("sourceAnchorCreated")).toBool());
    QCOMPARE(created.value(QStringLiteral("targetAnchorCreated")).toBool(), !separateTarget);
    const auto sourceAnchor = created.value(QStringLiteral("sourceAnchorId")).toString();
    const auto targetLocation = created.value(QStringLiteral("targetLocation")).toString();
    QVERIFY(!sourceAnchor.isEmpty());
    QVERIFY(targetLocation.contains(QLatin1Char('#')));
    for (int i = 0; i < books.size(); ++i) {
        const auto &book = books.at(i);
        QCOMPARE(MaintenanceFiles::digest(backups.at(i)), backupHashes.at(i));
        if (copies.isEmpty())
            continue;
        QCOMPARE(MaintenanceFiles::digest(copies.at(i)), copyHashes.at(i));
        QCOMPARE(MaintenanceFiles::digest(MaintenanceFiles::manifestPath(copies.at(i))), manifestHashes.at(i));
        const auto originalHash = MaintenanceFiles::digest(book.filename);
        const auto references = BookDatabase::self().loadAllReferences();
        const auto annotations = BookDatabase::self().loadAnnotations(book.uniqueIdentifier);
        // Saving an old copy must report both the new reference anchor and a lost remark.
        QVERIFY(writeReferenceEpub(copies.at(i), book.uniqueIdentifier, QByteArrayLiteral("<p>Editor text.</p><p>Note text.</p>")));
        if (copyKind == QStringLiteral("orphaned")) {
            const auto recovered = sessions.recoverWorkingCopy(book.uniqueIdentifier, copies.at(i));
            QVERIFY2(!recovered.value(QStringLiteral("error")).toBool(), qPrintable(recovered.value(QStringLiteral("message")).toString()));
            sessionIds.append(recovered.value(QStringLiteral("sessionId")).toString());
        }
        const auto workingHash = MaintenanceFiles::digest(copies.at(i));
        const auto refused = sessions.importNow(sessionIds.at(i));
        QCOMPARE(refused.value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        QStringList issues;
        for (const auto &issue : refused.value(QStringLiteral("anchorValidationIssues")).toList())
            issues.append(issue.toMap().value(QStringLiteral("type")).toString());
        QVERIFY(issues.contains(QStringLiteral("annotation")));
        QVERIFY(issues.contains(i == 0 ? QStringLiteral("reference-source") : QStringLiteral("reference-target-location")));
        QCOMPARE(MaintenanceFiles::digest(book.filename), originalHash);
        QCOMPARE(MaintenanceFiles::digest(copies.at(i)), workingHash);
        QCOMPARE(BookDatabase::self().loadAllReferences(), references);
        QCOMPARE(BookDatabase::self().loadAnnotations(book.uniqueIdentifier), annotations);
        // Correcting the copy to retain all anchors allows a subsequent normal import.
        EPubContainer current(nullptr);
        QVERIFY(current.openFile(book.filename));
        auto corrected = current.readData(QStringLiteral("OPS/chapter.xhtml"));
        QVERIFY(corrected.contains("Selected"));
        corrected.replace("passage.", "revised passage.");
        // Reuse the real chapter including the generated source/target anchor IDs.
        QVERIFY(writeReferenceEpub(copies.at(i), book.uniqueIdentifier, corrected, true));
        const auto accepted = sessions.importNow(sessionIds.at(i));
        QVERIFY2(accepted.value(QStringLiteral("status")).toString() == QStringLiteral("imported"),
                 qPrintable(accepted.value(QStringLiteral("message")).toString()));
        QCOMPARE(MaintenanceFiles::digest(book.filename), MaintenanceFiles::digest(copies.at(i)));
        QCOMPARE(BookDatabase::self().loadAllReferences(), references);
        QCOMPARE(BookDatabase::self().loadAnnotations(book.uniqueIdentifier), annotations);
        sessions.retainWorkingCopy(sessionIds.at(i));
        sessions.finishSession(sessionIds.at(i));
        sessions.releaseWorkingCopy(sessionIds.at(i));
        QTRY_VERIFY(!QFileInfo::exists(copies.at(i)));
    }
    server.stop();
}

void BookServerReferenceTest::fragmentTargetsMustMatchExactly()
{
    const auto path = m_directory.filePath(QStringLiteral("fragment-targets.epub"));
    QVERIFY(writeReferenceEpub(path, QStringLiteral("fragment-book"),
                              QByteArrayLiteral("<p><span id=\"source\" data-role=\"anchor\">Source</span></p>"
                                                "<h2 id=\"verse_18-20\">SB 9.4.18-20</h2>"
                                                "<a name=\"named-target\">Named target</a><p id=\"rāma\">Unicode target</p>"
                                                "<p id=\"duplicate\">One</p><p id=\"duplicate\">Two</p>"), false,
                              QByteArrayLiteral("<h2 id=\"verse_18-20\">Same ID in another document</h2>")));
    BookEntry entry;
    entry.filename = path;
    entry.filetitle = QFileInfo(path).fileName();
    entry.identifier = entry.uniqueIdentifier = QStringLiteral("fragment-book");
    entry.title = QStringLiteral("Fragment targets");
    entry.genres = QStringList{QStringLiteral("Religion/Bhakti"), QStringLiteral("Philosophy/Yoga")};
    entry.created = QDateTime::currentDateTimeUtc();
    BookDatabase::self().addEntry(entry);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    const auto baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reference-test-token"));
    QVERIFY(server.isRunning());
    ReferenceStore store;
    const auto listedBooks = request(baseUrl, QStringLiteral("/books")).value(QStringLiteral("books")).toArray();
    const auto listed = std::find_if(listedBooks.cbegin(), listedBooks.cend(), [&](const QJsonValue &book) {
        return book.toObject().value(QStringLiteral("id")).toString() == entry.uniqueIdentifier;
    });
    QVERIFY(listed != listedBooks.cend());
    QCOMPARE(listed->toObject().value(QStringLiteral("subjects")).toArray(), QJsonArray::fromStringList(entry.genres));
    const QString postPath = QStringLiteral("/fragment-book/ref");
    const QString getPath = QStringLiteral("/fragment-book/ref/source");
    const QJsonObject base{{QStringLiteral("sourceAnchorId"), QStringLiteral("source")},
                           {QStringLiteral("targetBookId"), entry.uniqueIdentifier},
                           {QStringLiteral("targetPreviewHtml"), QStringLiteral("Saved preview")}};
    const auto sourcePath = m_directory.filePath(QStringLiteral("fragment-source.epub"));
    QVERIFY(writeReferenceEpub(sourcePath, QStringLiteral("fragment-source"), QByteArrayLiteral("<p id=\"source\">Source</p>")));
    BookEntry sourceEntry = entry;
    sourceEntry.filename = sourcePath;
    sourceEntry.identifier = sourceEntry.uniqueIdentifier = QStringLiteral("fragment-source");
    BookDatabase::self().addEntry(sourceEntry);
    auto qualified = base;
    qualified.insert(QStringLiteral("targetLocation"), QStringLiteral("OPS/second.xhtml#verse_18-20"));
    const auto exact = request(baseUrl, QStringLiteral("/fragment-source/ref"), qualified);
    QVERIFY(!exact.contains(QStringLiteral("error")));
    QCOMPARE(request(baseUrl, QStringLiteral("/fragment-source/ref/source")).value(QStringLiteral("targetLocation")).toString(),
             QStringLiteral("OPS/second.xhtml#verse_18-20"));
    for (const auto &fragment : {QStringLiteral("verse_18-20"), QStringLiteral("named-target"), QStringLiteral("r%C4%81ma")}) {
        auto reference = base;
        const QString location = QStringLiteral("OPS/chapter.xhtml#") + fragment;
        reference.insert(QStringLiteral("targetLocation"), location);
        const auto saved = request(baseUrl, postPath, reference);
        QVERIFY(!saved.contains(QStringLiteral("error")));
        const auto loaded = request(baseUrl, getPath);
        QVERIFY(!loaded.contains(QStringLiteral("targetNavigationError")));
        QCOMPARE(loaded.value(QStringLiteral("targetLocation")).toString(), location);
    }
    for (const auto &pair : {qMakePair(QStringLiteral("OPS/chapter.xhtml#vers_18-20"), QStringLiteral("missing-target-anchor")),
                             qMakePair(QStringLiteral("OPS/absent.xhtml#verse_18-20"), QStringLiteral("missing-target-document")),
                             qMakePair(QStringLiteral("OPS/chapter.xhtml#duplicate"), QStringLiteral("duplicate-target-anchor"))}) {
        auto reference = base;
        reference.insert(QStringLiteral("targetLocation"), pair.first);
        const auto previous = store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source"));
        const auto before = MaintenanceFiles::digest(path);
        const auto rejected = request(baseUrl, postPath, reference, 409);
        QCOMPARE(rejected.value(QStringLiteral("error")).toString(), QStringLiteral("invalidReferenceTarget"));
        QCOMPARE(rejected.value(QStringLiteral("reason")).toString(), pair.second);
        const auto repair = rejected.value(QStringLiteral("targetAnchorRepair")).toObject();
        QCOMPARE(!repair.isEmpty(), pair.second == QStringLiteral("missing-target-anchor"));
        if (!repair.isEmpty()) {
            QCOMPARE(repair.value(QStringLiteral("bookId")).toString(), entry.uniqueIdentifier);
            QCOMPARE(repair.value(QStringLiteral("fileName")).toString(), path);
            QCOMPARE(repair.value(QStringLiteral("documentFile")).toString(), QStringLiteral("OPS/chapter.xhtml"));
            QCOMPARE(repair.value(QStringLiteral("anchorId")).toString(), QStringLiteral("vers_18-20"));
            QDomDocument html;
            QVERIFY(html.setContent(repair.value(QStringLiteral("htmlSource")).toString()));
            QCOMPARE(html.documentElement().attribute(QStringLiteral("id")), QStringLiteral("vers_18-20"));
        }
        QCOMPARE(store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source")), previous);
        QCOMPARE(MaintenanceFiles::digest(path), before);
        // Simulate an invalid reference already present in a legacy database.
        auto legacy = previous;
        legacy.insert(QStringLiteral("targetLocation"), pair.first);
        store.saveReference(legacy);
        const auto loaded = request(baseUrl, getPath);
        QCOMPARE(loaded.value(QStringLiteral("targetNavigationErrorCode")).toString(), pair.second);
        QCOMPARE(loaded.value(QStringLiteral("targetAnchorRepair")).toObject(), repair);
        QVERIFY(!loaded.value(QStringLiteral("targetNavigationError")).toString().isEmpty());
        QCOMPARE(loaded.value(QStringLiteral("targetLocation")).toString(), pair.first);
        QCOMPARE(loaded.value(QStringLiteral("targetPreviewHtml")).toString(), QStringLiteral("Saved preview"));
        QCOMPARE(store.loadReferenceBySource(entry.uniqueIdentifier, QStringLiteral("source")), legacy);
    }
    server.stop();
}

void BookServerReferenceTest::nativeReferenceSearchAndTitles()
{
    const auto node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty()) QSKIP("Node.js is needed for the shared formatting helpers");
    const auto module = QUrl::fromLocalFile(QFINDTESTDATA("../qml/reference-utils.mjs")).toString();
    const QString script = QStringLiteral("import assert from 'node:assert/strict';\nimport * as helpers from '") + module + QStringLiteral("';\n") + QStringLiteral(R"JS(
        assert.equal(helpers.referenceSearchKey('Śrī Kṛṣṇa'), 'sri krsna');
        assert.notEqual(helpers.referenceSearchKey('कृष्ण'), helpers.referenceSearchKey('कृष्णा'));
        const rows = Array.from({length: 100}, (_, i) => ({value: String(i), searchKey: 'sri krsna कृष्ण'}));
        assert.equal(helpers.matchingRows(rows, 'sri krsna').rows.length, 40);
        assert.equal(helpers.matchingRows(rows, 'sri krsna').count, 100);
        assert.equal(helpers.matchingRows(rows, 'कृष्णा').count, 0);
        assert.equal(helpers.matchingRows(rows, 'कृष्ण').count, 100);
        assert.equal(helpers.suggestedSourceAnchorTitle({title: 'Srimad Bhagavatam'}, {}, 'S.B. 9.4.18'), 'S.B. Vers 9.4.18');
        assert.ok(helpers.suggestedSourceAnchorTitle({title: 'Bhagavad Gita'}, {semanticPath:[{iast:'adhyāya',number:2}]}).includes('adhyāya 2'));
        const books = [
            {id:'otherZ',title:'Zebra',author:['Foreign'],subjects:['Science']},
            {id:'authorZ',title:'Zeta',author:['Writer'],subjects:['Religion/Yoga']},
            {id:'topicD',title:'Delta',author:['Other'],subjects:[' Religion / Yoga']},
            {id:'source',title:'Source',author:['Writer'],subjects:['Religion/Bhakti','religion/Yoga']},
            {id:'authorA',title:'Alpha',author:['Writer'],subjects:['Science']},
            {id:'otherA',title:'Aardvark',author:['Foreign']},
            {id:'topicB',title:'Beta',author:['Other'],subjects:['RELIGION/History']}
        ];
        assert.deepEqual(helpers.mainSubjectsForBook(books[3]), ['Religion']);
        assert.deepEqual(helpers.mainSubjectsForBook({subjects:['/invalid',' ','Philosophy/Yoga','Religion/Bhakti']}), ['Philosophy','Religion']);
        const grouped = helpers.groupedReferenceBooks(books, 'source', ['This book','Writer','Religion','Other']);
        assert.deepEqual(grouped.map(row=>row.value), ['source','authorA','authorZ','topicB','topicD','otherA','otherZ']);
        assert.deepEqual(grouped.map(row=>row.order), [0,1,1,2,2,3,3]);
        assert.equal(grouped.find(row=>row.value==='authorZ').label, 'Zeta');
        assert.equal(helpers.matchingRows(grouped,'Writer').count, 3);
        const withoutMetadata = helpers.groupedReferenceBooks([{id:'source',title:'Source'},{id:'other',title:'Other'}], 'source', ['This','','','Other']);
        assert.deepEqual(withoutMetadata.map(row=>row.order), [0,3]);
    )JS");
    QProcess process;
    process.start(node, {QStringLiteral("--input-type=module")});
    QVERIFY(process.waitForStarted());
    process.write(script.toUtf8());
    process.closeWriteChannel();
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

void BookServerReferenceTest::requestsDistinguishTimeoutAndNetworkFailure()
{
    const auto node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (node.isEmpty()) QSKIP("Node.js is needed for the XHR event-order regression");
    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const auto source = QString::fromUtf8(reader.readAll());
    const auto start = source.indexOf(QStringLiteral("const requestJson ="));
    const auto end = source.indexOf(QStringLiteral("\nconst dispatchBookReloadNeeded ="), start);
    QVERIFY(start >= 0 && end > start);
    const QString script = QStringLiteral("import assert from 'node:assert/strict';\n") + source.mid(start, end - start) + QStringLiteral(R"JS(
        const dispatch = () => {};
        let event = 'timeout';
        let pending;
        globalThis.XMLHttpRequest = class {
            open() {}
            setRequestHeader() {}
            send() {
                pending = this;
                if (event === 'pending') return;
                this.readyState = 4;
                this.status = event === 'success' ? 200 : event === 'unauthorized' ? 401 : 0;
                this.responseText = this.status === 200 ? '{"loaded":true}' : '';
                // Browsers send DONE/status 0 before the actual failure event.
                this.onreadystatechange();
                if (event === 'timeout') this.ontimeout();
                if (event === 'network') this.onerror();
                this.onloadend();
            }
            abort() {
                this.readyState = 4; this.status = 0;
                this.onreadystatechange(); this.onabort(); this.onloadend();
            }
        };
        await assert.rejects(requestJson('GET', 'http://test', null, 60000), error => error.code === 'timeout');
        assert.equal(pending.timeout, 60000);
        event = 'network';
        await assert.rejects(requestJson('GET', 'http://test'), error => error.code === 'network');
        event = 'unauthorized';
        await assert.rejects(requestJson('GET', 'http://test'), error => error.status === 401);
        event = 'pending';
        const controller = new AbortController();
        const cancelled = requestJson('GET', 'http://test', null, 60000, controller.signal);
        controller.abort();
        await assert.rejects(cancelled, error => error.code === 'aborted');
        event = 'success';
        assert.deepEqual(await requestJson('GET', 'http://test'), {loaded:true});
    )JS");
    QProcess process;
    process.start(node, {QStringLiteral("--input-type=module")});
    QVERIFY(process.waitForStarted());
    process.write(script.toUtf8());
    process.closeWriteChannel();
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

void BookServerReferenceTest::externalEpubTargetLookup()
{
    const auto path = qEnvironmentVariable("ARIANNA_TARGET_TEST_EPUB");
    if (path.isEmpty()) QSKIP("Set ARIANNA_TARGET_TEST_EPUB to a disposable EPUB copy for the large-book integration check");
    QElapsedTimer timer;
    timer.start();
    EPubContainer book(nullptr);
    QVERIFY(book.openFile(path));
    qInfo() << "EPUB open ms:" << timer.restart();
    const auto phase = qEnvironmentVariable("ARIANNA_TARGET_TEST_PHASE");
    if (phase == QStringLiteral("structure")) {
        const auto snapshot = WorkStructure::snapshot(book, QStringLiteral("large-target-book"));
        qInfo() << "Structure snapshot ms:" << timer.elapsed() << "evidence:" << snapshot.value(QStringLiteral("evidence")).toArray().size();
        return;
    }
    if (phase == QStringLiteral("targets")) {
        const auto targets = book.referenceableTargets();
        qInfo() << "Referenceable targets ms:" << timer.elapsed() << "targets:" << targets.size();
        QVERIFY(!targets.isEmpty());
        return;
    }
    BookEntry entry;
    entry.filename = path;
    entry.filetitle = QFileInfo(path).fileName();
    entry.identifier = entry.uniqueIdentifier = QStringLiteral("large-target-book");
    entry.title = QStringLiteral("Large target book");
    entry.created = QDateTime::currentDateTimeUtc();
    BookDatabase::self().addEntry(entry);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    const auto baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reference-test-token"));
    QVERIFY(server.isRunning());
    const auto before = MaintenanceFiles::digest(path);
    timer.restart();
    const auto state = request(baseUrl, QStringLiteral("/large-target-book/workStructure"));
    qInfo() << "HTTP stored-structure lookup ms:" << timer.restart() << "validity:" << state.value(QStringLiteral("validity")).toString();
    const auto response = request(baseUrl, QStringLiteral("/large-target-book/targetLocations"));
    qInfo() << "HTTP target lookup ms:" << timer.elapsed() << "targets:" << response.value(QStringLiteral("targetLocations")).toArray().size();
    QVERIFY(!response.value(QStringLiteral("targetLocations")).toArray().isEmpty());
    const auto location = qEnvironmentVariable("ARIANNA_TARGET_TEST_LOCATION");
    if (!location.isEmpty()) {
        ReferenceStore references;
        references.saveReference({{QStringLiteral("sourceBookId"), entry.uniqueIdentifier},
                                  {QStringLiteral("sourceAnchorId"), QStringLiteral("external-test-source")},
                                  {QStringLiteral("targetBookId"), entry.uniqueIdentifier},
                                  {QStringLiteral("targetLocation"), location}});
        timer.restart();
        const auto target = request(baseUrl, QStringLiteral("/large-target-book/ref/external-test-source"));
        qInfo() << "HTTP fragment validation ms:" << timer.elapsed() << "location:" << location
                << "result:" << target.value(QStringLiteral("targetNavigationErrorCode")).toString();
        QCOMPARE(target.value(QStringLiteral("targetLocation")).toString(), location);
        QCOMPARE(target.value(QStringLiteral("targetNavigationErrorCode")).toString(), qEnvironmentVariable("ARIANNA_TARGET_TEST_ERROR"));
    }
    QCOMPARE(MaintenanceFiles::digest(path), before);
    server.stop();
}

void BookServerReferenceTest::imageInversionUsesBackupSetting()
{
    QCOMPARE(Config::backupOnImageInversion(), false);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(probe.serverPort());
    probe.close();
    BookServer server(QStringLiteral("reference-test-token"));
    QVERIFY(server.isRunning());
    QFile discovery(m_directory.filePath(QStringLiteral("arianna-bookserver.json")));
    QVERIFY(discovery.open(QIODevice::ReadOnly));
    const QString baseUrl = QJsonDocument::fromJson(discovery.readAll()).object().value(QStringLiteral("baseUrl")).toString();
    QVERIFY(!baseUrl.isEmpty());
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    const QList<bool> modes{false, true, false};
    for (qsizetype i = 0; i < modes.size(); ++i) {
        if (i > 0) Config::setBackupOnImageInversion(modes.at(i));
        const QString identifier = QStringLiteral("inversion-book-%1").arg(i);
        const QString epubPath = m_directory.filePath(identifier + QStringLiteral(".epub"));
        KZip zip(epubPath);
        QVERIFY(zip.open(QIODevice::WriteOnly));
        QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
        QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
            QByteArrayLiteral("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
                              "<rootfile full-path=\"OEBPS/package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>")));
        QVERIFY(zip.writeFile(QStringLiteral("OEBPS/package.opf"), QStringLiteral(
            "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"3.0\">"
            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"bookid\">%1</dc:identifier>"
            "<dc:title>Image inversion test</dc:title></metadata><manifest>"
            "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
            "<item id=\"photo\" href=\"photo.png\" media-type=\"image/png\"/></manifest>"
            "<spine><itemref idref=\"chapter\"/></spine></package>").arg(identifier).toUtf8()));
        QVERIFY(zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArrayLiteral(
            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>Before <img id=\"photo\" src=\"photo.png\"/> after</p></body></html>")));
        QVERIFY(zip.writeFile(QStringLiteral("OEBPS/photo.png"), png));
        QVERIFY(zip.close());
        QFile original(epubPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = original.readAll();
        original.close();
        BookEntry entry;
        entry.filename = epubPath;
        entry.filetitle = QFileInfo(epubPath).fileName();
        entry.title = QStringLiteral("Image inversion test");
        entry.identifier = identifier;
        entry.uniqueIdentifier = identifier;
        entry.created = QDateTime::currentDateTimeUtc();
        BookDatabase::self().addEntry(entry);
        const QString path = QLatin1Char('/') + identifier + QStringLiteral("/image/not-inverse");
        const QString imageCfi = i == 0 ? QStringLiteral("epubcfi(/6/2!/2/99)")
                                        : QStringLiteral("epubcfi(/6/2!/2,/2:1,/2:2)");
        const QJsonObject body{{QStringLiteral("cfi"), imageCfi},
                              {QStringLiteral("src"), QStringLiteral("photo.png")}};
        const QJsonObject changed = request(baseUrl, path, body);
        QCOMPARE(changed.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        QVERIFY(changed.value(QStringLiteral("changed")).toBool());
        KZip updated(epubPath);
        QVERIFY(updated.open(QIODevice::ReadOnly));
        const auto *chapter = updated.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
        QVERIFY(chapter);
        QVERIFY(chapter->data().contains("class=\"not_inverse\""));
        QVERIFY(updated.close());
        const QString backupPath = m_directory.filePath(identifier + QStringLiteral(".before.image-not-inverse_photo.epub"));
        QCOMPARE(QFileInfo::exists(backupPath), modes.at(i));
        QCOMPARE(QFileInfo::exists(backupPath + QStringLiteral(".arianna.json")), modes.at(i));
        if (modes.at(i)) {
            QFile backup(backupPath);
            QVERIFY(backup.open(QIODevice::ReadOnly));
            QCOMPARE(backup.readAll(), originalBytes);
            QFile manifest(backupPath + QStringLiteral(".arianna.json"));
            QVERIFY(manifest.open(QIODevice::ReadOnly));
            QVERIFY(QJsonDocument::fromJson(manifest.readAll()).object().value(QStringLiteral("completed")).toBool());
        }
        const QStringList backups = QDir(m_directory.path()).entryList({identifier + QStringLiteral(".before.image-not-inverse_*.epub")}, QDir::Files);
        const QJsonObject repeated = request(baseUrl, path, body);
        QVERIFY(!repeated.value(QStringLiteral("changed")).toBool());
        QCOMPARE(repeated.value(QStringLiteral("status")).toString(), QStringLiteral("unchanged"));
        QCOMPARE(QDir(m_directory.path()).entryList({identifier + QStringLiteral(".before.image-not-inverse_*.epub")}, QDir::Files), backups);
    }
}

QTEST_GUILESS_MAIN(BookServerReferenceTest)
#include "bookserverreferencetest.moc"
