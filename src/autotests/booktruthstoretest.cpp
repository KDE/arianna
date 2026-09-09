// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "booktruthstore.h"
#include "bookdatabase.h"
#include "categoryentriesmodel.h"

#include <KArchiveFile>
#include <KZip>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QScopedPointer>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class BookTruthStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testOpenBookCreatesCurrentState();
    void testTextContentHashIgnoresTechnicalAnchors();
    void testVisibleTextChangeChangesBothHashes();
    void testCreateAnchorUpdatesCurrentState();
    void testFailedAnchorDoesNotChangeStateOrEpub();
    void testAnnotationPersistenceDoesNotExposeInvalidState();
    void testReferencePersistenceHasNoInvalidColumns();
    void testActiveFileChangeRefreshesState();
    void testActiveFileChangeWithInvalidAnchorReportsIssues();
};

static QString uniqueBookId(const QString &prefix)
{
    return prefix + QStringLiteral("-") + QUuid::createUuidV7().toString(QUuid::WithoutBraces);
}

static bool isUuidV7(const QUuid &uuid)
{
    const QString text = uuid.toString(QUuid::WithoutBraces);
    return text.size() > 14 && text.at(14) == QLatin1Char('7');
}

static void writeContainer(KZip &zip)
{
    zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/package.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)"));
}

static QByteArray packageDocument()
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">truth-book</dc:identifier>
    <dc:title>Truth Store Test</dc:title>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)");
}

static QByteArray chapterWithBody(const QByteArray &body)
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ignored title</title></head><body>)")
        + body + QByteArray(R"(</body></html>)");
}

static bool writeTestEpub(const QString &epubPath, const QByteArray &chapterDocument)
{
    QFile::remove(epubPath);
    KZip zip(epubPath);
    if (!zip.open(QIODevice::WriteOnly)) {
        return false;
    }

    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), packageDocument());
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), chapterDocument);

    return zip.close();
}

static QByteArray readChapter(const QString &epubPath)
{
    KZip zip(epubPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        return {};
    }

    const KArchiveFile *chapter = zip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    if (!chapter) {
        return {};
    }

    QScopedPointer<QIODevice> device(chapter->createDevice());
    return device ? device->readAll() : QByteArray();
}

static void addBookEntry(const QString &bookId, const QString &epubPath)
{
    BookEntry entry;
    entry.filename = epubPath;
    entry.filetitle = QFileInfo(epubPath).fileName();
    entry.title = bookId;
    entry.identifier = bookId;
    entry.uniqueIdentifier = bookId;
    entry.created = QDateTime::currentDateTimeUtc();
    BookDatabase::self().addEntry(entry);
}

void BookTruthStoreTest::initTestCase()
{
    const QString dataHome = QDir::temp().filePath(QStringLiteral("arianna-booktruthstoretest-%1").arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dataHome));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));

    QCoreApplication::setOrganizationName(QStringLiteral("KDE"));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-booktruthstoretest"));

    QDir location(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QVERIFY(location.mkpath(QStringLiteral(".")));
    QFile::remove(location.filePath(QStringLiteral("library.sqlite")));
}

void BookTruthStoreTest::testOpenBookCreatesCurrentState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("initial"));
    const QString epubPath = dir.filePath(QStringLiteral("initial.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Initial visible text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));
    QVERIFY(snapshot.state.has_value());
    QVERIFY(isUuidV7(snapshot.state->stateId));
    QCOMPARE(snapshot.state->bookId, bookId);
    QCOMPARE(snapshot.state->textContentHash, snapshot.textContentHash);
    QCOMPARE(snapshot.state->documentStateHash, snapshot.documentStateHash);

    const std::optional<BookState> stored = BookDatabase::self().currentBookState(bookId);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->stateId, snapshot.state->stateId);

    const BookSnapshot secondSnapshot = store.openBook(bookId);
    QVERIFY2(secondSnapshot.success, qPrintable(secondSnapshot.errorMessage));
    QCOMPARE(secondSnapshot.state->stateId, snapshot.state->stateId);

    QSqlDatabase db = QSqlDatabase::database();
    QVERIFY(db.open());
    QVERIFY(db.tables().contains(QStringLiteral("book_current_state"), Qt::CaseInsensitive));
    db.close();
}

void BookTruthStoreTest::testTextContentHashIgnoresTechnicalAnchors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString plainBookId = uniqueBookId(QStringLiteral("plain"));
    const QString anchoredBookId = uniqueBookId(QStringLiteral("anchored"));
    const QString plainPath = dir.filePath(QStringLiteral("plain.epub"));
    const QString anchoredPath = dir.filePath(QStringLiteral("anchored.epub"));

    QVERIFY(writeTestEpub(plainPath, chapterWithBody(QByteArrayLiteral("<p>Visible truth text.</p>"))));
    QVERIFY(writeTestEpub(anchoredPath,
                          chapterWithBody(QByteArrayLiteral("<p><span id=\"anchor_begin\" data-role=\"anchor\" data-anchor-type=\"annotation-begin\"></span>"
                                                            "Visible truth text."
                                                            "<span id=\"anchor_end\" data-role=\"anchor\" data-anchor-type=\"annotation-end\"></span></p>"))));
    addBookEntry(plainBookId, plainPath);
    addBookEntry(anchoredBookId, anchoredPath);

    BookTruthStore store;
    const BookSnapshot plain = store.openBook(plainBookId);
    const BookSnapshot anchored = store.openBook(anchoredBookId);
    QVERIFY2(plain.success, qPrintable(plain.errorMessage));
    QVERIFY2(anchored.success, qPrintable(anchored.errorMessage));

    QCOMPARE(anchored.textContentHash, plain.textContentHash);
    QVERIFY(anchored.documentStateHash != plain.documentStateHash);
}

void BookTruthStoreTest::testVisibleTextChangeChangesBothHashes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString firstBookId = uniqueBookId(QStringLiteral("visible-a"));
    const QString secondBookId = uniqueBookId(QStringLiteral("visible-b"));
    const QString firstPath = dir.filePath(QStringLiteral("visible-a.epub"));
    const QString secondPath = dir.filePath(QStringLiteral("visible-b.epub"));

    QVERIFY(writeTestEpub(firstPath, chapterWithBody(QByteArrayLiteral("<p>Original visible text.</p>"))));
    QVERIFY(writeTestEpub(secondPath, chapterWithBody(QByteArrayLiteral("<p>Changed visible text.</p>"))));
    addBookEntry(firstBookId, firstPath);
    addBookEntry(secondBookId, secondPath);

    BookTruthStore store;
    const BookSnapshot first = store.openBook(firstBookId);
    const BookSnapshot second = store.openBook(secondBookId);
    QVERIFY2(first.success, qPrintable(first.errorMessage));
    QVERIFY2(second.success, qPrintable(second.errorMessage));

    QVERIFY(first.textContentHash != second.textContentHash);
    QVERIFY(first.documentStateHash != second.documentStateHash);
}

void BookTruthStoreTest::testCreateAnchorUpdatesCurrentState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("anchor"));
    const QString epubPath = dir.filePath(QStringLiteral("anchor.epub"));
    QVERIFY(writeTestEpub(epubPath,
                          QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>Hello anchored truth.</p></body></html>")));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    const BookCommitResult result = store.createAnchor(bookId, QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:14)"));
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.createdAnchorId.has_value());
    QVERIFY(isUuidV7(*result.createdAnchorId));
    QVERIFY(isUuidV7(result.newStateId));
    QCOMPARE(result.oldStateId, snapshot.state->stateId);
    QVERIFY(result.newStateId != snapshot.state->stateId);
    QCOMPARE(result.textContentHash, snapshot.textContentHash);
    QVERIFY(result.documentStateHash != snapshot.documentStateHash);

    const std::optional<BookState> state = BookDatabase::self().currentBookState(bookId);
    QVERIFY(state.has_value());
    QCOMPARE(state->stateId, result.newStateId);

    const QString anchorElementId = QStringLiteral("uuid_") + result.createdAnchorId->toString(QUuid::WithoutBraces);
    const QString backupPath = dir.filePath(QStringLiteral("anchor.before_anchoring_%1.epub").arg(anchorElementId));
    QVERIFY(QFileInfo::exists(backupPath));
    QVERIFY(!readChapter(backupPath).contains("data-role=\"anchor\""));

    const QString document = QString::fromUtf8(readChapter(epubPath));
    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorElementId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation\"")));
}

void BookTruthStoreTest::testFailedAnchorDoesNotChangeStateOrEpub()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("failed-anchor"));
    const QString epubPath = dir.filePath(QStringLiteral("failed-anchor.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Failed anchor visible text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    const BookCommitResult result = store.createAnchor(bookId, QStringLiteral("epubcfi(/6/2!/2/2/1:6)"));
    QVERIFY(!result.success);
    const std::optional<BookState> state = BookDatabase::self().currentBookState(bookId);
    QVERIFY(state.has_value());
    QCOMPARE(state->stateId, snapshot.state->stateId);
    QVERIFY(readChapter(epubPath).contains("Failed anchor visible text."));
}

void BookTruthStoreTest::testAnnotationPersistenceDoesNotExposeInvalidState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("annotation-db"));
    const QString epubPath = dir.filePath(QStringLiteral("annotation-db.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Annotation database text.</p>"))));
    addBookEntry(bookId, epubPath);

    QVariantMap annotation;
    annotation.insert(QStringLiteral("value"), QStringLiteral("epubcfi(/6/2!/2/2,/1:0,/1:10)"));
    annotation.insert(QStringLiteral("annotationId"), QStringLiteral("annotation-one"));
    annotation.insert(QStringLiteral("anchorId"), QStringLiteral("anchor-one"));
    annotation.insert(QStringLiteral("invalidReason"), QStringLiteral("legacy"));
    BookDatabase::self().saveAnnotation(bookId, annotation);

    const QVariantList annotations = BookDatabase::self().loadAnnotations(bookId);
    QCOMPARE(annotations.size(), 1);
    const QVariantMap loaded = annotations.constFirst().toMap();
    QCOMPARE(loaded.value(QStringLiteral("annotationId")).toString(), QStringLiteral("annotation-one"));
    QCOMPARE(loaded.value(QStringLiteral("anchorId")).toString(), QStringLiteral("anchor-one"));
    QCOMPARE(loaded.value(QStringLiteral("cfiRange")).toString(), annotation.value(QStringLiteral("value")).toString());
    QVERIFY(!loaded.contains(QStringLiteral("invalidReason")));
    QVERIFY(!loaded.value(QStringLiteral("invalid")).toBool());
}

void BookTruthStoreTest::testReferencePersistenceHasNoInvalidColumns()
{
    const QString sourceBookId = uniqueBookId(QStringLiteral("reference-source"));
    const QString targetBookId = uniqueBookId(QStringLiteral("reference-target"));

    QVariantMap reference;
    reference.insert(QStringLiteral("sourceBookId"), sourceBookId);
    reference.insert(QStringLiteral("sourceAnchorId"), QStringLiteral("source-anchor"));
    reference.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Source"));
    reference.insert(QStringLiteral("targetBookId"), targetBookId);
    reference.insert(QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#target-anchor"));
    reference.insert(QStringLiteral("invalidReason"), QStringLiteral("legacy"));
    BookDatabase::self().saveReference(reference);

    const QVariantMap loaded = BookDatabase::self().loadReferenceBySource(sourceBookId, QStringLiteral("source-anchor"));
    QCOMPARE(loaded.value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("Source"));
    QVERIFY(!loaded.contains(QStringLiteral("invalidReason")));
    QVERIFY(!loaded.value(QStringLiteral("invalid")).toBool());

    const QVariantList targeting = BookDatabase::self().loadReferencesTargeting(targetBookId);
    QCOMPARE(targeting.size(), 1);
    QVERIFY(!targeting.constFirst().toMap().contains(QStringLiteral("invalidReason")));
}

void BookTruthStoreTest::testActiveFileChangeRefreshesState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("active-file"));
    const QString epubPath = dir.filePath(QStringLiteral("active-file.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original active file text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));
    QVERIFY(!snapshot.epubFileHash.isEmpty());

    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Externally changed active file text.</p>"))));

    const BookCommitResult result = store.commitActiveFileChangeIfNeeded(bookId);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(!result.unchanged);
    QVERIFY(!result.newStateId.isNull());
    QCOMPARE(result.oldStateId, snapshot.state->stateId);
    QVERIFY(result.newStateId != snapshot.state->stateId);
    QVERIFY(result.epubFileHash != snapshot.epubFileHash);
    QVERIFY(result.textContentHash != snapshot.textContentHash);

    const std::optional<BookState> state = BookDatabase::self().currentBookState(bookId);
    QVERIFY(state.has_value());
    QCOMPARE(state->stateId, result.newStateId);

    const BookCommitResult unchanged = store.commitActiveFileChangeIfNeeded(bookId);
    QVERIFY2(unchanged.success, qPrintable(unchanged.errorMessage));
    QVERIFY(unchanged.unchanged);
    QCOMPARE(unchanged.newStateId, result.newStateId);
}

void BookTruthStoreTest::testActiveFileChangeWithInvalidAnchorReportsIssues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("active-invalid-anchor"));
    const QString epubPath = dir.filePath(QStringLiteral("active-invalid-anchor.epub"));
    const QString anchorId = QStringLiteral("uuid_019fa52c-0000-7000-8000-000000000001");
    QVERIFY(writeTestEpub(epubPath,
                          chapterWithBody(QByteArrayLiteral("<p><span id=\"uuid_019fa52c-0000-7000-8000-000000000001\" "
                                                            "data-role=\"anchor\" data-anchor-type=\"annotation\">Anchored</span> text.</p>"))));
    addBookEntry(bookId, epubPath);

    QVariantMap annotation;
    annotation.insert(QStringLiteral("annotationId"), QStringLiteral("annotation-active-invalid"));
    annotation.insert(QStringLiteral("anchorId"), anchorId);
    annotation.insert(QStringLiteral("color"), QStringLiteral("yellow"));
    annotation.insert(QStringLiteral("text"), QStringLiteral("Anchored"));
    BookDatabase::self().saveAnnotation(bookId, annotation);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Anchor removed by an external editor.</p>"))));

    const BookCommitResult blocked = store.commitActiveFileChangeIfNeeded(bookId);
    QVERIFY(!blocked.success);
    QVERIFY(blocked.validationFailed);
    QCOMPARE(blocked.oldStateId, snapshot.state->stateId);
    QCOMPARE(blocked.anchorValidationIssues.size(), 1);

    const BookCommitResult confirmed = store.commitActiveFileChangeIfNeeded(bookId, true);
    QVERIFY2(confirmed.success, qPrintable(confirmed.errorMessage));
    QVERIFY(!confirmed.unchanged);
    QCOMPARE(confirmed.anchorValidationIssues.size(), 1);

    const QVariantList annotations = BookDatabase::self().loadAnnotations(bookId);
    QCOMPARE(annotations.size(), 1);
    const QVariantMap loaded = annotations.constFirst().toMap();
    QVERIFY(!loaded.value(QStringLiteral("invalid")).toBool());
    QVERIFY(!loaded.contains(QStringLiteral("invalidReason")));
}

QTEST_MAIN(BookTruthStoreTest)
#include "booktruthstoretest.moc"
