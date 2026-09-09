// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "bookstateimport.h"
#include "bookdatabase.h"
#include "booktruthstore.h"
#include "categoryentriesmodel.h"

#include <KArchiveFile>
#include <KZip>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QScopedPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class BookStateImportTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testSuccessfulImportUpdatesCurrentState();
    void testRepeatedSavesReplaceCurrentState();
    void testNoOpSaveDoesNotChangeState();
    void testConcurrentEditorImportWinsLast();
    void testInvalidEpubPreservesAuthoritativeState();
    void testAtomicFileFailurePreservesAuthoritativeState();
    void testDuplicateImportsCreateSingleState();
    void testAnchorValidationFailuresReportOnly();
};

static QString uniqueBookId(const QString &prefix)
{
    return prefix + QStringLiteral("-") + QUuid::createUuidV7().toString(QUuid::WithoutBraces);
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
    <dc:identifier id="bookid">state-import-book</dc:identifier>
    <dc:title>Book State Import Test</dc:title>
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
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ignored</title></head><body>)")
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

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
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

static EditorSession createSession(const QString &bookId, const QString &workingCopyPath)
{
    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    Q_ASSERT(snapshot.success);
    QFile::remove(workingCopyPath);
    Q_ASSERT(QFile::copy(snapshot.filename, workingCopyPath));

    EditorSession session;
    session.sessionId = QUuid::createUuidV7();
    session.bookId = bookId;
    session.authoritativeFilePath = snapshot.filename;
    session.workingCopyPath = workingCopyPath;
    session.lastImportedFileHash = fileHash(workingCopyPath);
    session.createdAt = QDateTime::currentDateTimeUtc();
    session.lastImportedAt = session.createdAt;
    session.active = true;
    return session;
}

void BookStateImportTest::initTestCase()
{
    const QString dataHome = QDir::temp().filePath(QStringLiteral("arianna-bookstateimporttest-%1").arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dataHome));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));

    QCoreApplication::setOrganizationName(QStringLiteral("KDE"));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-bookstateimporttest"));

    QDir location(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QVERIFY(location.mkpath(QStringLiteral(".")));
    QFile::remove(location.filePath(QStringLiteral("library.sqlite")));
}

void BookStateImportTest::testSuccessfulImportUpdatesCurrentState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("success"));
    const QString epubPath = dir.filePath(QStringLiteral("success.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);
    const QUuid initialStateId = BookDatabase::self().currentBookState(bookId)->stateId;
    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>Edited text.</p>"))));

    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(session);
    QVERIFY(result.status == BookStateImportStatus::Imported);
    QVERIFY(!result.newStateId.isNull());
    QCOMPARE(result.previousStateId, initialStateId);
    QCOMPARE(BookDatabase::self().currentBookState(bookId)->stateId, result.newStateId);
    QCOMPARE(session.lastImportedFileHash, fileHash(workingPath));
    QVERIFY(readChapter(epubPath).contains("Edited text."));
}

void BookStateImportTest::testRepeatedSavesReplaceCurrentState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("repeated"));
    const QString epubPath = dir.filePath(QStringLiteral("repeated.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);

    BookStateImport importer;
    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>First edit.</p>"))));
    const BookStateImportResult first = importer.importEditorWorkingCopy(session);
    QVERIFY(first.status == BookStateImportStatus::Imported);

    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>Second edit.</p>"))));
    const BookStateImportResult second = importer.importEditorWorkingCopy(session);
    QVERIFY(second.status == BookStateImportStatus::Imported);
    QCOMPARE(second.previousStateId, first.newStateId);
    QVERIFY(second.newStateId != first.newStateId);
    QCOMPARE(BookDatabase::self().currentBookState(bookId)->stateId, second.newStateId);
    QVERIFY(readChapter(epubPath).contains("Second edit."));
}

void BookStateImportTest::testNoOpSaveDoesNotChangeState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("noop"));
    const QString epubPath = dir.filePath(QStringLiteral("noop.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);
    const QUuid initialStateId = BookDatabase::self().currentBookState(bookId)->stateId;

    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(session);
    QVERIFY(result.status == BookStateImportStatus::Unchanged);
    QCOMPARE(BookDatabase::self().currentBookState(bookId)->stateId, initialStateId);
}

void BookStateImportTest::testConcurrentEditorImportWinsLast()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("concurrent"));
    const QString epubPath = dir.filePath(QStringLiteral("concurrent.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    const QString otherPath = dir.filePath(QStringLiteral("other.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);

    QVERIFY(writeTestEpub(otherPath, chapterWithBody(QByteArrayLiteral("<p>Authoritative edit.</p>"))));
    BookTruthStore store;
    const BookCommitResult otherCommit = store.commitCandidateBookFile(bookId, otherPath);
    QVERIFY2(otherCommit.success, qPrintable(otherCommit.errorMessage));

    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>Editor edit wins.</p>"))));
    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(session);
    QVERIFY2(result.status == BookStateImportStatus::Imported, qPrintable(result.errorMessage));
    QVERIFY(readChapter(epubPath).contains("Editor edit wins."));
    QVERIFY(!readChapter(epubPath).contains("Authoritative edit."));
}

void BookStateImportTest::testInvalidEpubPreservesAuthoritativeState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("invalid"));
    const QString epubPath = dir.filePath(QStringLiteral("invalid.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);
    const QUuid initialStateId = BookDatabase::self().currentBookState(bookId)->stateId;

    QFile workingFile(workingPath);
    QVERIFY(workingFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    workingFile.write("not an epub");
    workingFile.close();

    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(session);
    QVERIFY(result.status == BookStateImportStatus::InvalidEpub);
    QCOMPARE(BookDatabase::self().currentBookState(bookId)->stateId, initialStateId);
    QVERIFY(readChapter(epubPath).contains("Original text."));
}

void BookStateImportTest::testAtomicFileFailurePreservesAuthoritativeState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("atomic"));
    const QString epubPath = dir.filePath(QStringLiteral("atomic.epub"));
    const QString workingPath = QDir::temp().filePath(uniqueBookId(QStringLiteral("atomic-working")) + QStringLiteral(".epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);
    const QUuid initialStateId = BookDatabase::self().currentBookState(bookId)->stateId;
    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>Edited text.</p>"))));

    const QFile::Permissions originalPermissions = QFile::permissions(dir.path());
    QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(session);

    QVERIFY(QFile::setPermissions(dir.path(), originalPermissions));
    QFile::remove(workingPath);

    QVERIFY(result.status == BookStateImportStatus::CommitFailed);
    QCOMPARE(BookDatabase::self().currentBookState(bookId)->stateId, initialStateId);
    QVERIFY(readChapter(epubPath).contains("Original text."));
}

void BookStateImportTest::testDuplicateImportsCreateSingleState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("duplicate"));
    const QString epubPath = dir.filePath(QStringLiteral("duplicate.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Original text.</p>"))));
    addBookEntry(bookId, epubPath);
    EditorSession session = createSession(bookId, workingPath);
    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>One saved state.</p>"))));

    BookStateImport importer;
    const BookStateImportResult first = importer.importEditorWorkingCopy(session);
    QVERIFY(first.status == BookStateImportStatus::Imported);
    const BookStateImportResult duplicate = importer.importEditorWorkingCopy(session);
    QVERIFY(duplicate.status == BookStateImportStatus::Unchanged);
    QCOMPARE(BookDatabase::self().currentBookState(bookId)->stateId, first.newStateId);
}

void BookStateImportTest::testAnchorValidationFailuresReportOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString anchorId = QStringLiteral("uuid_019fb5e0-0000-7000-8000-000000000001");
    const QString bookId = uniqueBookId(QStringLiteral("anchor-validation"));
    const QString epubPath = dir.filePath(QStringLiteral("anchor-validation.epub"));
    const QString workingPath = dir.filePath(QStringLiteral("working.epub"));
    QVERIFY(writeTestEpub(epubPath,
                          chapterWithBody(QByteArrayLiteral("<p><span id=\"uuid_019fb5e0-0000-7000-8000-000000000001\" "
                                                            "data-role=\"anchor\" data-anchor-type=\"annotation\">Anchored</span></p>"))));
    addBookEntry(bookId, epubPath);

    QVariantMap annotation;
    annotation.insert(QStringLiteral("annotationId"), QStringLiteral("annotation-one"));
    annotation.insert(QStringLiteral("anchorId"), QStringLiteral("019fb5e0-0000-7000-8000-000000000001"));
    annotation.insert(QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/2/2,/1:0,/1:8)"));
    annotation.insert(QStringLiteral("color"), QStringLiteral("yellow"));
    annotation.insert(QStringLiteral("text"), QStringLiteral("Anchored"));
    BookDatabase::self().saveAnnotation(bookId, annotation);

    QVariantMap sourceReference;
    sourceReference.insert(QStringLiteral("sourceBookId"), bookId);
    sourceReference.insert(QStringLiteral("sourceAnchorId"), anchorId);
    sourceReference.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("S.B. Vers 1.1.1"));
    sourceReference.insert(QStringLiteral("targetBookId"), QStringLiteral("other-target-book"));
    sourceReference.insert(QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#other-target-anchor"));
    BookDatabase::self().saveReference(sourceReference);

    QVariantMap targetReference;
    targetReference.insert(QStringLiteral("sourceBookId"), QStringLiteral("other-source-book"));
    targetReference.insert(QStringLiteral("sourceAnchorId"), QStringLiteral("other-source-anchor"));
    targetReference.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Other Source Title"));
    targetReference.insert(QStringLiteral("targetBookId"), bookId);
    targetReference.insert(QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#%1").arg(anchorId));
    BookDatabase::self().saveReference(targetReference);

    BookStateImport importer;

    EditorSession removedSession = createSession(bookId, workingPath);
    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p>Anchored</p>"))));
    const BookStateImportResult removed = importer.importEditorWorkingCopy(removedSession);
    QVERIFY(removed.status == BookStateImportStatus::AnchorValidationFailed);
    QVERIFY(!removed.anchorValidationIssues.isEmpty());

    const BookStateImportResult confirmed = importer.importEditorWorkingCopy(removedSession, true);
    QVERIFY2(confirmed.status == BookStateImportStatus::Imported, qPrintable(confirmed.errorMessage));
    QVERIFY(!confirmed.anchorValidationIssues.isEmpty());
    QVERIFY(readChapter(epubPath).contains("Anchored"));
    QVERIFY(!readChapter(epubPath).contains("data-role=\"anchor\""));

    const QVariantList annotations = BookDatabase::self().loadAnnotations(bookId);
    QCOMPARE(annotations.size(), 1);
    QVERIFY(!annotations.constFirst().toMap().value(QStringLiteral("invalid")).toBool());

    const QVariantList sourceReferences = BookDatabase::self().loadReferences(bookId);
    QCOMPARE(sourceReferences.size(), 1);
    QCOMPARE(sourceReferences.constFirst().toMap().value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("S.B. Vers 1.1.1"));
    QVERIFY(!sourceReferences.constFirst().toMap().value(QStringLiteral("invalid")).toBool());
    QVERIFY(!sourceReferences.constFirst().toMap().contains(QStringLiteral("invalidReason")));

    const QVariantList targetReferences = BookDatabase::self().loadReferencesTargeting(bookId);
    QCOMPARE(targetReferences.size(), 1);
    QCOMPARE(targetReferences.constFirst().toMap().value(QStringLiteral("sourceAnchorTitle")).toString(), QStringLiteral("Other Source Title"));
    QVERIFY(!targetReferences.constFirst().toMap().value(QStringLiteral("invalid")).toBool());

    EditorSession duplicateSession = createSession(bookId, workingPath);
    QVERIFY(writeTestEpub(workingPath,
                          chapterWithBody((QStringLiteral("<p><span id=\"%1\" data-role=\"anchor\">A</span>"
                                                          "<span id=\"%1\" data-role=\"anchor\">B</span></p>")
                                               .arg(anchorId)
                                               .toUtf8()))));
    const BookStateImportResult duplicate = importer.importEditorWorkingCopy(duplicateSession);
    QVERIFY(duplicate.status == BookStateImportStatus::AnchorValidationFailed);

    EditorSession malformedSession = createSession(bookId, workingPath);
    QVERIFY(writeTestEpub(workingPath, chapterWithBody(QByteArrayLiteral("<p><span id=\"uuid_not-valid\" data-role=\"anchor\">Broken</span></p>"))));
    const BookStateImportResult malformed = importer.importEditorWorkingCopy(malformedSession);
    QVERIFY(malformed.status == BookStateImportStatus::AnchorValidationFailed);
}

QTEST_MAIN(BookStateImportTest)
#include "bookstateimporttest.moc"
