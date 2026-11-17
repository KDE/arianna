// SPDX-License-Identifier: GPL-2.0-or-later
#include "librarymaintenance.h"
#include "bookdatabase.h"
#include "booklistmodel.h"
#include "booktruthstore.h"
#include "config.h"
#include "editorsessionstore.h"
#include "epubcontainer.h"
#include "maintenancefiles.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtQml/QQmlExtensionPlugin>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>
Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
BookEntry epub(const QString &path, const QString &id, const QByteArray &body = "<span id=\"note\">Text</span>")
{
    KZip zip(path);
    const QByteArray chapter = QByteArrayLiteral("<html><head><title>Test</title></head><body>") + body + QByteArrayLiteral("</body></html>");
    if (!zip.open(QIODevice::WriteOnly) || !zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"))
        || !zip.writeFile(
            QStringLiteral("META-INF/container.xml"),
            QByteArrayLiteral(
                "<container><rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"))
        || !zip.writeFile(
            QStringLiteral("content.opf"),
            QByteArray(QByteArrayLiteral("<package version=\"3.0\" unique-identifier=\"id\"><metadata "
                                         "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Test</dc:title><dc:identifier "
                                         "id=\"id\">")
                       + id.toHtmlEscaped().toUtf8()
                       + QByteArrayLiteral("</dc:identifier></metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" "
                                           "media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"chapter\"/></spine></package>")))
        || !zip.writeFile(QStringLiteral("chapter.xhtml"), chapter) || !zip.close())
        QTest::qFail("Unable to create EPUB", __FILE__, __LINE__);
    BookEntry book;
    book.filename = path;
    book.title = id;
    book.uniqueIdentifier = id;
    book.identifier = id;
    return book;
}
BookEntry epubWithTwoChapters(const QString &path, const QString &id, const QByteArray &first, const QByteArray &second)
{
    KZip zip(path);
    const QByteArray opf = QByteArrayLiteral("<package version=\"3.0\" unique-identifier=\"id\"><metadata "
                                         "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Test</dc:title><dc:identifier id=\"id\">")
        + id.toHtmlEscaped().toUtf8()
        + QByteArrayLiteral("</dc:identifier></metadata><manifest>"
                            "<item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/>"
                            "<item id=\"two\" href=\"two.xhtml\" media-type=\"application/xhtml+xml\"/>"
                            "</manifest><spine><itemref idref=\"one\"/><itemref idref=\"two\"/></spine></package>");
    const auto html = [](const QByteArray &body) -> QByteArray {
        return QByteArrayLiteral("<html><head><title>Test</title></head><body>") + body + QByteArrayLiteral("</body></html>");
    };
    if (!zip.open(QIODevice::WriteOnly) || !zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"))
        || !zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArrayLiteral("<container><rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"))
        || !zip.writeFile(QStringLiteral("content.opf"), opf)
        || !zip.writeFile(QStringLiteral("one.xhtml"), html(first))
        || !zip.writeFile(QStringLiteral("two.xhtml"), html(second)) || !zip.close())
        QTest::qFail("Unable to create two-chapter EPUB", __FILE__, __LINE__);
    BookEntry book;
    book.filename = path;
    book.title = id;
    book.uniqueIdentifier = id;
    book.identifier = id;
    return book;
}
bool hasCode(const QVariantMap &report, const QString &code)
{
    for (const auto &value : report.value(QStringLiteral("results")).toList())
        if (value.toMap().value(QStringLiteral("codes")).toStringList().contains(code))
            return true;
    return false;
}
QQuickItem *visualItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name)
        return root;
    for (auto *child : root->childItems())
        if (auto *item = visualItem(child, name))
            return item;
    return nullptr;
}
QQuickItem *visibleItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name && root->isVisible())
        return root;
    for (auto *child : root->childItems())
        if (auto *item = visibleItem(child, name))
            return item;
    return nullptr;
}
int visibleItemsNamed(QQuickItem *root, const QString &name)
{
    int count = root->objectName() == name && root->isVisible() ? 1 : 0;
    for (auto *child : root->childItems())
        count += visibleItemsNamed(child, name);
    return count;
}
QList<QQuickItem *> duplicateButtons(QQuickItem *root, bool sharedId = false)
{
    QList<QQuickItem *> buttons;
    if (root->objectName() == (sharedId ? QStringLiteral("deleteSharedBookIdFile") : QStringLiteral("deleteDuplicateFile")))
        buttons.append(root);
    for (auto *child : root->childItems())
        buttons.append(duplicateButtons(child, sharedId));
    return buttons;
}
}
class LibraryMaintenanceTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_data;
    QString database()
    {
        return BookDatabase::self().databaseFileName();
    }
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_data.isValid());
        qputenv("XDG_DATA_HOME", QFile::encodeName(m_data.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_data.path()));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_data.path()));
        QCoreApplication::setApplicationName(QStringLiteral("arianna-librarymaintenancetest"));
        KLocalizedString::setApplicationDomain("arianna");
        BookDatabase::self().loadEntries();
    }
    void init()
    {
        QVERIFY(MaintenanceFiles::sessionsRoot().startsWith(m_data.path()));
        QVERIFY(QDir(MaintenanceFiles::sessionsRoot()).removeRecursively());
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        for (const auto &table : {"books", "annotations", "references", "book_current_state", "book_state_history"})
            QVERIFY(query.exec(QStringLiteral("DELETE FROM \"%1\"").arg(QString::fromLatin1(table))));
        query.finish();
        connection.close();
    }
    QString cacheCopy(const QString &source)
    {
        const auto directory = MaintenanceFiles::sessionsRoot() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().mkpath(directory))
            return {};
        const auto path = directory + QLatin1Char('/') + QFileInfo(source).fileName();
        return QFile::copy(source, path) ? path : QString();
    }
    QVariantMap cacheRow(const QString &path)
    {
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("cache"));
        for (const auto &row : report.value(QStringLiteral("results")).toList())
            if (row.toMap().value(QStringLiteral("fileName")).toString() == path)
                return row.toMap();
        return {};
    }
    void missingAnchorImportCanBeDiscardedAndRestarted()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("conflict.epub")), QStringLiteral("conflict"));
        BookDatabase::self().addEntry(book);
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY(!session.value(QStringLiteral("error")).toBool());
        const auto id = session.value(QStringLiteral("sessionId")).toString();
        const auto copy = session.value(QStringLiteral("workingCopyPath")).toString();
        const auto original = digest(book.filename);
        const auto refused = BookTruthStore().createAnchor(book.uniqueIdentifier, QStringLiteral("epubcfi(/6/2!/4/2,/1:0,/1:4)"));
        QVERIFY(!refused.success);
        QVERIFY(refused.errorMessage.contains(copy));
        QCOMPARE(digest(book.filename), original);

        // Reproduce an old session in which the viewer added an anchor after
        // the working copy was created. The cached edit must not erase it.
        epub(copy, book.uniqueIdentifier, QByteArrayLiteral("<span id=\"note\">Edited text</span>"));
        epub(book.filename, book.uniqueIdentifier, QByteArrayLiteral(
            "<span id=\"uuid_019fb5e0-0000-7000-8000-000000000001\" data-role=\"anchor\" data-anchor-type=\"annotation\">Text</span>"));
        QVERIFY(BookTruthStore().commitActiveFileChangeIfNeeded(book.uniqueIdentifier, true).success);
        BookDatabase::self().saveAnnotation(book.uniqueIdentifier,
            {{QStringLiteral("annotationId"), QStringLiteral("new-note")},
             {QStringLiteral("anchorId"), QStringLiteral("019fb5e0-0000-7000-8000-000000000001")},
             {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2,/1:0,/1:4)")}});
        const auto anchoredOriginal = digest(book.filename);
        const auto savedAnnotations = BookDatabase::self().loadAnnotations(book.uniqueIdentifier);
        sessions.retainWorkingCopy(id);
        const auto result = sessions.finishSession(id);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        QVERIFY(!result.value(QStringLiteral("anchorValidationIssues")).toList().isEmpty());
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDiscard")).toBool());
        sessions.releaseWorkingCopy(id);
        QTRY_VERIFY(cacheRow(copy).value(QStringLiteral("canDiscard")).toBool());
        QVERIFY(!cacheRow(copy).value(QStringLiteral("safeToDelete")).toBool());

        LibraryMaintenance checker;
        checker.start(QStringLiteral("cache"));
        QTRY_VERIFY(!checker.running());
        checker.discardWorkingCopy(copy);
        QTRY_VERIFY(!checker.running());
        QCOMPARE(checker.report().value(QStringLiteral("removedFiles")).toInt(), 1);
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(digest(book.filename), anchoredOriginal);
        QCOMPARE(BookDatabase::self().loadAnnotations(book.uniqueIdentifier), savedAnnotations);
        QVERIFY(MaintenanceFiles::conflictingCopies(book).isEmpty());
        const auto fresh = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY2(!fresh.value(QStringLiteral("error")).toBool(), qPrintable(fresh.value(QStringLiteral("message")).toString()));
        const auto freshCopy = fresh.value(QStringLiteral("workingCopyPath")).toString();
        QCOMPARE(digest(freshCopy), anchoredOriginal);
        sessions.finishSession(fresh.value(QStringLiteral("sessionId")).toString());
        QTRY_VERIFY(!QFileInfo::exists(freshCopy));
        QVERIFY(MaintenanceFiles::anchorEditBlockReason(book).isEmpty());
    }

    void explicitDiscardRechecksChangesAndProtectsOpenCopies()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("discard.epub")), QStringLiteral("discard"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto copy = cacheCopy(book.filename);
        // A changed registered source blocks generic cleanup, but must not
        // prevent an explicitly confirmed reset of the editor working copy.
        epub(book.filename, book.uniqueIdentifier, QByteArrayLiteral("<p>Newer original</p>"));
        const auto original = digest(book.filename);
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDelete")).toBool());
        QVERIFY(cacheRow(copy).value(QStringLiteral("canDiscard")).toBool());
        LibraryMaintenance checker;
        checker.start(QStringLiteral("cache"));
        QTRY_VERIFY(!checker.running());
        epub(copy, book.uniqueIdentifier, QByteArrayLiteral("<p>Changed after inspection</p>"));
        checker.discardWorkingCopy(copy);
        QTRY_VERIFY(!checker.running());
        QVERIFY(QFileInfo::exists(copy));
        QCOMPARE(checker.report().value(QStringLiteral("removedFiles")).toInt(), 0);
        QFile editor(copy);
        QVERIFY(editor.open(QIODevice::ReadOnly));
        checker.discardWorkingCopy(copy);
        QTRY_VERIFY(!checker.running());
        QVERIFY(QFileInfo::exists(copy));
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDiscard")).toBool());
        editor.close();
        checker.start(QStringLiteral("cache"));
        QTRY_VERIFY(!checker.running());
        checker.discardWorkingCopy(book.filename);
        QVERIFY(!checker.running());
        QCOMPARE(digest(book.filename), original);
        checker.discardWorkingCopy(copy);
        QTRY_VERIFY(!checker.running());
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(digest(book.filename), original);
    }

    void cacheSessionsBlockDuplicatesAndRecoverLegacyCopies()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("session.epub")), QStringLiteral("session"));
        BookDatabase::self().addEntry(book);
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY2(!session.value(QStringLiteral("error")).toBool(), qPrintable(session.value(QStringLiteral("message")).toString()));
        const auto path = session.value(QStringLiteral("workingCopyPath")).toString();
        const auto id = session.value(QStringLiteral("sessionId")).toString();
        QVERIFY(QFileInfo::exists(MaintenanceFiles::manifestPath(path)));
        const auto reused = sessions.createOrReuseEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY(reused.value(QStringLiteral("reused")).toBool());
        QCOMPARE(reused.value(QStringLiteral("sessionId")).toString(), id);
        QCOMPARE(reused.value(QStringLiteral("workingCopyPath")).toString(), path);
        EditorSessionStore anotherInstance;
        QSignalSpy blocked(&anotherInstance, &EditorSessionStore::editorStartBlocked);
        QVERIFY(anotherInstance.createOrReuseEditorSession(book.uniqueIdentifier, book.filename).value(QStringLiteral("blocked")).toBool());
        QCOMPARE(blocked.size(), 1);
        QCOMPARE(MaintenanceFiles::sessionCopies().size(), 1);
        QVERIFY(anotherInstance.recoverWorkingCopy(book.uniqueIdentifier, path).value(QStringLiteral("blocked")).toBool());
        QVERIFY(sessions.resumeEditorSession(id).value(QStringLiteral("blocked")).toBool());
        QVERIFY(!cacheRow(path).value(QStringLiteral("canDelete")).toBool());
        QCOMPARE(sessions.finishSession(id).value(QStringLiteral("status")).toString(), QStringLiteral("unchanged"));
        QTRY_VERIFY(!QFileInfo::exists(path));

        const auto legacy = cacheCopy(book.filename);
        QVERIFY(!legacy.isEmpty());
        epub(legacy, book.uniqueIdentifier, QByteArrayLiteral("<p>Unimported legacy changes</p>"));
        const auto oldHash = digest(book.filename);
        const auto recovered = anotherInstance.recoverWorkingCopy(book.uniqueIdentifier, legacy);
        QVERIFY2(!recovered.value(QStringLiteral("error")).toBool(), qPrintable(recovered.value(QStringLiteral("message")).toString()));
        QCOMPARE(digest(book.filename), oldHash);
        QVERIFY(!cacheRow(legacy).value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(sessions.createEditorSession(book.uniqueIdentifier, book.filename).value(QStringLiteral("blocked")).toBool());
        const auto result = anotherInstance.importNow(recovered.value(QStringLiteral("sessionId")).toString());
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QCOMPARE(digest(book.filename), digest(legacy));
        QVERIFY(cacheRow(legacy).value(QStringLiteral("safeToDelete")).toBool());
    }
    void cacheCopiesStayAvailableUntilAllImportChecksFinish()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("review.epub")), QStringLiteral("review"));
        BookDatabase::self().addEntry(book);
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        const auto id = session.value(QStringLiteral("sessionId")).toString();
        const auto copy = session.value(QStringLiteral("workingCopyPath")).toString();
        QVERIFY(!copy.isEmpty());
        sessions.retainWorkingCopy(id);
        sessions.retainWorkingCopy(id);
        sessions.finishSession(id);
        QCoreApplication::processEvents();
        QVERIFY(QFileInfo::exists(copy));
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDelete")).toBool());
        sessions.releaseWorkingCopy(id);
        QCoreApplication::processEvents();
        QVERIFY(QFileInfo::exists(copy));
        sessions.releaseWorkingCopy(id);
        QTRY_VERIFY(!QFileInfo::exists(copy));
    }

    void cacheAutomaticCleanupPreservesLibraryEntriesAndBackups()
    {
        QTemporaryDir dir;
        auto book = epub(dir.filePath(QStringLiteral("registered.epub")), QStringLiteral("registered"));
        BookDatabase::self().addEntry(book);
        EditorSessionStore sessions;
        auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        auto copy = session.value(QStringLiteral("workingCopyPath")).toString();
        QVERIFY(!copy.isEmpty());
        auto registeredCopy = book;
        registeredCopy.filename = copy;
        registeredCopy.uniqueIdentifier = QStringLiteral("registered-copy");
        BookDatabase::self().addEntry(registeredCopy);
        sessions.finishSession(session.value(QStringLiteral("sessionId")).toString());
        QCoreApplication::processEvents();
        QVERIFY(QFileInfo::exists(copy));
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDelete")).toBool());

        book = epub(dir.filePath(QStringLiteral("with-backup.epub")), QStringLiteral("with-backup"));
        BookDatabase::self().addEntry(book);
        session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        copy = session.value(QStringLiteral("workingCopyPath")).toString();
        QVERIFY(!copy.isEmpty());
        const auto backup = QFileInfo(copy).dir().filePath(QStringLiteral("with-backup.before_anchoring_test.epub"));
        QVERIFY(QFile::copy(copy, backup));
        QVERIFY(MaintenanceFiles::registerBackup(copy, backup));
        MaintenanceFiles::completeBackup(backup);
        sessions.finishSession(session.value(QStringLiteral("sessionId")).toString());
        QTRY_VERIFY(!QFileInfo::exists(copy));
        QVERIFY(QFileInfo::exists(backup));
        QVERIFY(QFileInfo::exists(MaintenanceFiles::manifestPath(copy)));
        const auto row = cacheRow(backup);
        QCOMPARE(row.value(QStringLiteral("bookId")).toString(), book.uniqueIdentifier);
        QCOMPARE(row.value(QStringLiteral("originalTitle")).toString(), book.title);
        QVERIFY(!row.value(QStringLiteral("createdAt")).toString().isEmpty());
        QCOMPARE(row.value(QStringLiteral("thumbnail")).toString(), book.thumbnail);
        QVERIFY(row.value(QStringLiteral("canDelete")).toBool());
        QVERIFY(!row.value(QStringLiteral("safeToDelete")).toBool());
    }

    void cacheOldKnownVersionsAreSafeButAgeIsNotProof()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("history.epub")), QStringLiteral("history"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto known = cacheCopy(book.filename);
        const auto unknown = cacheCopy(book.filename);
        epub(unknown, book.uniqueIdentifier, QByteArrayLiteral("<p>Lost edits must be kept</p>"));
        QFile old(unknown);
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::currentDateTimeUtc().addYears(-5), QFileDevice::FileModificationTime));
        old.close();
        const auto candidate = epub(dir.filePath(QStringLiteral("candidate.epub")), book.uniqueIdentifier, QByteArrayLiteral("<p>New edition</p>"));
        QVERIFY(BookTruthStore().commitCandidateBookFile(book.uniqueIdentifier, candidate.filename).success);
        const auto dbHash = digest(database());
        QVERIFY(cacheRow(known).value(QStringLiteral("safeToDelete")).toBool());
        const auto unknownRow = cacheRow(unknown);
        QVERIFY(!unknownRow.value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(unknownRow.value(QStringLiteral("canDelete")).toBool());
        QCOMPARE(digest(database()), dbHash);
        epub(book.filename, book.uniqueIdentifier, QByteArrayLiteral("<p>Unregistered changes</p>"));
        QVERIFY(!cacheRow(known).value(QStringLiteral("canDelete")).toBool());
        QVERIFY(!MaintenanceFiles::removeFile(unknownRow.value(QStringLiteral("cleanupPlan")).toMap()));
        QVERIFY(QFileInfo::exists(unknown));
    }
    void cacheOpenLegacyFilesAreProtected()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("open.epub")), QStringLiteral("open"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto copy = cacheCopy(book.filename);
        QFile externalEditor(copy);
        QVERIFY(externalEditor.open(QIODevice::ReadOnly));
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDelete")).toBool());
        EditorSessionStore sessions;
        QVERIFY(sessions.recoverWorkingCopy(book.uniqueIdentifier, copy).value(QStringLiteral("blocked")).toBool());
        externalEditor.close();
        QVERIFY(cacheRow(copy).value(QStringLiteral("safeToDelete")).toBool());
    }

    void cacheCleanupRechecksChangesAndRequiresReview()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("cleanup.epub")), QStringLiteral("cleanup"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto copy = cacheCopy(book.filename);
        const auto originalHash = digest(book.filename);
        LibraryMaintenance checker;
        checker.start(QStringLiteral("cache"));
        QTRY_VERIFY(!checker.running());
        QVERIFY(cacheRow(copy).value(QStringLiteral("safeToDelete")).toBool());
        epub(copy, book.uniqueIdentifier, QByteArrayLiteral("<p>Changed since inspection</p>"));
        checker.cleanCache({copy});
        QTRY_VERIFY(!checker.running());
        QCOMPARE(checker.report().value(QStringLiteral("removedFiles")).toInt(), 0);
        QVERIFY(QFileInfo::exists(copy));
        checker.cleanCache({copy}, false);
        QVERIFY(!checker.running());
        QVERIFY(QFileInfo::exists(copy));
        checker.cleanCache({book.filename}, true);
        QVERIFY(!checker.running());
        checker.cleanCache({copy}, true);
        QTRY_VERIFY(!checker.running());
        QCOMPARE(checker.report().value(QStringLiteral("removedFiles")).toInt(), 1);
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(digest(book.filename), originalHash);
    }
    void cacheBackupsKeepLatestAndFailedOperations()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("backup.epub")), QStringLiteral("backup"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto first = dir.filePath(QStringLiteral("backup.before_anchoring_one.epub"));
        QVERIFY(QFile::copy(book.filename, first));
        QVERIFY(MaintenanceFiles::registerBackup(book.filename, first));
        const auto candidate = epub(dir.filePath(QStringLiteral("candidate.epub")), book.uniqueIdentifier, QByteArrayLiteral("<p>Second edition</p>"));
        QVERIFY(BookTruthStore().commitCandidateBookFile(book.uniqueIdentifier, candidate.filename).success);
        MaintenanceFiles::completeBackup(first);
        const auto latest = book.filename + QStringLiteral(".arianna-anchor-title-latest.bak");
        QVERIFY(QFile::copy(book.filename, latest));
        QVERIFY(MaintenanceFiles::registerBackup(book.filename, latest));
        MaintenanceFiles::completeBackup(latest);
        QFile older(first);
        QVERIFY(older.open(QIODevice::ReadWrite));
        QVERIFY(older.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2), QFileDevice::FileModificationTime));
        older.close();
        QVERIFY(cacheRow(first).value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(!cacheRow(latest).value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(cacheRow(latest).value(QStringLiteral("canDelete")).toBool());
        QFile viewedOriginal(book.filename);
        QVERIFY(viewedOriginal.open(QIODevice::ReadOnly));
        QVERIFY(cacheRow(latest).value(QStringLiteral("canDelete")).toBool());
        QFile openedBackup(latest);
        QVERIFY(openedBackup.open(QIODevice::ReadOnly));
        QVERIFY(!cacheRow(latest).value(QStringLiteral("canDelete")).toBool());
        openedBackup.close();
        viewedOriginal.close();
        QVERIFY(cacheRow(latest + QStringLiteral(".arianna.json")).isEmpty());
        QVERIFY(MaintenanceFiles::registerBackup(book.filename, first)); // Simulate an interrupted operation.
        QVERIFY(!cacheRow(first).value(QStringLiteral("canDelete")).toBool());
    }
    void cachePdfBackupsNeedACompletedJournal()
    {
        QTemporaryDir dir;
        BookEntry book;
        book.filename = dir.filePath(QStringLiteral("pdf.pdf"));
        book.uniqueIdentifier = book.identifier = QStringLiteral("pdf-backup");
        PoDoFo::PdfMemDocument document;
        document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        document.Save(book.filename.toStdString());
        BookDatabase::self().addEntry(book);
        const auto older = book.filename + QStringLiteral(".arianna-anchor-title-old.bak");
        QVERIFY(QFile::copy(book.filename, older));
        QVERIFY(MaintenanceFiles::registerBackup(book.filename, older));
        document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        document.Save(book.filename.toStdString());
        MaintenanceFiles::completeBackup(older);
        const auto latest = book.filename + QStringLiteral(".arianna-anchor-removal-new.bak");
        QVERIFY(QFile::copy(book.filename, latest));
        QFile oldFile(older);
        QVERIFY(oldFile.open(QIODevice::ReadWrite));
        QVERIFY(oldFile.setFileTime(QDateTime::currentDateTimeUtc().addDays(-1), QFileDevice::FileModificationTime));
        oldFile.close();
        QVERIFY(cacheRow(older).value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(!cacheRow(latest).value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(cacheRow(latest).value(QStringLiteral("canDelete")).toBool());
        auto manifest = MaintenanceFiles::readJson(older + QStringLiteral(".arianna.json"));
        manifest.insert(QStringLiteral("afterHash"), QStringLiteral("unconfirmed"));
        QVERIFY(MaintenanceFiles::writeJson(older + QStringLiteral(".arianna.json"), manifest));
        QVERIFY(!cacheRow(older).value(QStringLiteral("safeToDelete")).toBool());
    }
    void cacheHistoryMigratesCurrentStatesAndIsRemovedWithBook()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("migration.epub")), QStringLiteral("migration"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto copy = cacheCopy(book.filename);
        {
            auto db = QSqlDatabase::database();
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("DROP TABLE book_state_history")));
            query.finish();
            db.close();
        }
        BookDatabase::self().loadEntries(); // Simulate upgrading a database with only its current state.
        const auto candidate = epub(dir.filePath(QStringLiteral("candidate.epub")), book.uniqueIdentifier, QByteArrayLiteral("<p>Next state</p>"));
        QVERIFY(BookTruthStore().commitCandidateBookFile(book.uniqueIdentifier, candidate.filename).success);
        QVERIFY(cacheRow(copy).value(QStringLiteral("safeToDelete")).toBool());
        QVERIFY(BookDatabase::self().removeEntry(book));
        auto db = QSqlDatabase::database();
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM book_state_history WHERE book_id='migration'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        query.finish();
        db.close();
    }
    void cacheDamagedLegacyCopiesAndLocksStillBlockEditors()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("damaged.epub")), QStringLiteral("damaged"));
        BookDatabase::self().addEntry(book);
        EditorSessionStore sessions;
        {
            auto lock = MaintenanceFiles::lock();
            QVERIFY(lock);
            QVERIFY(sessions.createEditorSession(book.uniqueIdentifier, book.filename).value(QStringLiteral("blocked")).toBool());
        }
        const auto path = cacheCopy(book.filename);
        QFile broken(path);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("broken archive");
        broken.close();
        QVERIFY(sessions.createEditorSession(book.uniqueIdentifier, book.filename).value(QStringLiteral("blocked")).toBool());
        QVERIFY(sessions.recoverWorkingCopy(book.uniqueIdentifier, path).value(QStringLiteral("error")).toBool());
        QCOMPARE(MaintenanceFiles::sessionCopies().size(), 1);
    }
    void databaseFindsDuplicatesAndOrphansWithoutWriting()
    {
        QTemporaryDir dir;
        auto first = epub(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("shared"));
        auto second = first;
        second.filename = dir.filePath(QStringLiteral("second.epub"));
        QVERIFY(QFile::copy(first.filename, second.filename));
        auto &db = BookDatabase::self();
        db.addEntry(first);
        db.addEntry(second);
        db.saveAnnotation(QStringLiteral("missing"),
                          {{QStringLiteral("annotationId"), QStringLiteral("orphan")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        const auto dbHash = digest(database()), fileHash = digest(first.filename);
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("database"));
        QVERIFY(hasCode(report, QStringLiteral("identical-files")));
        QVERIFY(hasCode(report, QStringLiteral("shared-book-id")));
        QVERIFY(hasCode(report, QStringLiteral("unassigned-record")));
        QVariantMap orphan, shared;
        for (const auto &value : report.value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            const auto codes = row.value(QStringLiteral("codes")).toStringList();
            if (codes.contains(QStringLiteral("unassigned-record")) && row.value(QStringLiteral("recordId")).toString().endsWith(QStringLiteral("orphan")))
                orphan = row;
            if (codes.contains(QStringLiteral("shared-book-id")) && row.value(QStringLiteral("bookId")).toString() == QStringLiteral("shared"))
                shared = row;
        }
        QCOMPARE(orphan.value(QStringLiteral("tableName")).toString(), QStringLiteral("annotations"));
        QCOMPARE(orphan.value(QStringLiteral("recordKeyField")).toString(), QStringLiteral("bookId + annotationId"));
        QCOMPARE(orphan.value(QStringLiteral("fieldName")).toString(), QStringLiteral("bookId"));
        QCOMPARE(shared.value(QStringLiteral("tableName")).toString(), QStringLiteral("books"));
        QCOMPARE(shared.value(QStringLiteral("matchedBookFiles")).toStringList().size(), 2);
        QCOMPARE(digest(database()), dbHash);
        QCOMPARE(digest(first.filename), fileHash);
        QCOMPARE(db.loadAnnotations(QStringLiteral("missing")).size(), 1);
    }
    void databaseCanAssignNewUuidv7ToSelectedBook()
    {
        QTemporaryDir dir;
        auto first = epub(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("shared"));
        auto second = first;
        second.filename = dir.filePath(QStringLiteral("second.epub"));
        QVERIFY(QFile::copy(first.filename, second.filename));
        auto missingId = first;
        missingId.filename = dir.filePath(QStringLiteral("missing-id.epub"));
        missingId.uniqueIdentifier.clear();
        missingId.identifier.clear();
        QVERIFY(QFile::copy(first.filename, missingId.filename));
        auto &db = BookDatabase::self();
        db.addEntry(first);
        db.addEntry(second);
        db.addEntry(missingId);
        db.saveAnnotation(QStringLiteral("shared"),
                          {{QStringLiteral("annotationId"), QStringLiteral("stays-with-old-id")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});

        LibraryMaintenance checker;
        checker.start(QStringLiteral("database"));
        QTRY_VERIFY(!checker.running());
        const auto duplicateRepair = checker.assignNewBookId(second.filename, QStringLiteral("shared"));
        QVERIFY2(duplicateRepair.value(QStringLiteral("success")).toBool(), qPrintable(duplicateRepair.value(QStringLiteral("message")).toString()));
        const auto duplicateId = duplicateRepair.value(QStringLiteral("newIdentifier")).toString();
        QCOMPARE(QUuid(duplicateId).version(), QUuid::UnixEpoch);
        QVERIFY(db.loadEntry(second.filename).has_value());
        QCOMPARE(db.loadEntry(second.filename)->uniqueIdentifier, duplicateId);
        QCOMPARE(db.loadEntry(first.filename)->uniqueIdentifier, QStringLiteral("shared"));
        QCOMPARE(db.loadAnnotations(QStringLiteral("shared")).size(), 1);

        const auto missingRepair = checker.assignNewBookId(missingId.filename, QString());
        QVERIFY2(missingRepair.value(QStringLiteral("success")).toBool(), qPrintable(missingRepair.value(QStringLiteral("message")).toString()));
        QCOMPARE(QUuid(missingRepair.value(QStringLiteral("newIdentifier")).toString()).version(), QUuid::UnixEpoch);
    }
    void referenceMaintenanceKeepsBookIdForUnreadableEpub()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("unreadable.epub")), QStringLiteral("unreadable-book"),
                               QByteArrayLiteral("<p><div>Broken nesting</p>"));
        const auto target = epub(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("target-book"));
        BookDatabase::self().addEntry(book);
        BookDatabase::self().addEntry(target);
        BookDatabase::self().saveReference({{QStringLiteral("sourceBookId"), book.uniqueIdentifier},
                            {QStringLiteral("sourceAnchorId"), QStringLiteral("broken-source")},
                            {QStringLiteral("targetBookId"), target.uniqueIdentifier},
                            {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});

        const auto stored = LibraryMaintenance::inspect(database(), QStringLiteral("references"));
        QVERIFY(hasCode(stored, QStringLiteral("source-anchor-unresolved")));
        QCOMPARE(stored.value(QStringLiteral("results")).toList().size(), 1);
        QCOMPARE(stored.value(QStringLiteral("results")).toList().first().toMap().value(QStringLiteral("bookId")).toString(), book.uniqueIdentifier);
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("references-all"));
        bool foundUnreadable = false;
        for (const auto &value : report.value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("fileName")).toString() != book.filename
                || !row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("incomplete-cross-reference-scan"))) {
                continue;
            }
            QCOMPARE(row.value(QStringLiteral("bookId")).toString(), book.uniqueIdentifier);
            foundUnreadable = true;
        }
        QVERIFY(foundUnreadable);
    }
    void databaseCanDeleteOrphanedEntriesFromBothStateTables()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("history.epub")), QStringLiteral("known-history-book"));
        BookDatabase::self().addEntry(book);

        QSqlQuery insert(QSqlDatabase::database());
        QVERIFY(insert.prepare(QStringLiteral("INSERT INTO book_state_history(book_id, state_id, epub_file_hash, modified) "
                                              "VALUES(:bookId, :stateId, :hash, :modified)")));
        insert.bindValue(QStringLiteral(":hash"), QStringLiteral("hash"));
        insert.bindValue(QStringLiteral(":modified"), QStringLiteral("2026-01-01T00:00:00Z"));
        insert.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-history-book"));
        insert.bindValue(QStringLiteral(":stateId"), QStringLiteral("orphan-history-state"));
        QVERIFY(insert.exec());
        insert.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-history-book-2"));
        insert.bindValue(QStringLiteral(":stateId"), QStringLiteral("orphan-history-state-2"));
        QVERIFY(insert.exec());
        insert.bindValue(QStringLiteral(":bookId"), book.uniqueIdentifier);
        insert.bindValue(QStringLiteral(":stateId"), QStringLiteral("valid-history-state"));
        QVERIFY(insert.exec());
        insert.finish();

        QSqlQuery insertCurrent(QSqlDatabase::database());
        QVERIFY(insertCurrent.prepare(QStringLiteral("INSERT INTO book_current_state(book_id, state_id, text_content_hash, document_state_hash, epub_file_hash, modified) "
                                 "VALUES(:bookId, :stateId, :textHash, :documentHash, :epubHash, :modified)")));
        insertCurrent.bindValue(QStringLiteral(":textHash"), QStringLiteral("text-hash"));
        insertCurrent.bindValue(QStringLiteral(":documentHash"), QStringLiteral("document-hash"));
        insertCurrent.bindValue(QStringLiteral(":epubHash"), QStringLiteral("epub-hash"));
        insertCurrent.bindValue(QStringLiteral(":modified"), QStringLiteral("2026-01-01T00:00:00Z"));
        insertCurrent.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-current-book-1"));
        insertCurrent.bindValue(QStringLiteral(":stateId"), QStringLiteral("orphan-current-state-1"));
        QVERIFY(insertCurrent.exec());
        insertCurrent.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-current-book-2"));
        insertCurrent.bindValue(QStringLiteral(":stateId"), QStringLiteral("orphan-current-state-2"));
        QVERIFY(insertCurrent.exec());
        insertCurrent.bindValue(QStringLiteral(":bookId"), book.uniqueIdentifier);
        insertCurrent.bindValue(QStringLiteral(":stateId"), QStringLiteral("valid-current-state"));
        QVERIFY(insertCurrent.exec());
        insertCurrent.finish();

        LibraryMaintenance checker;
        checker.start(QStringLiteral("database"));
        QTRY_VERIFY(!checker.running());
        bool orphanReported = false;
        for (const auto &value : checker.report().value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("tableName")).toString() == QStringLiteral("book_state_history")
                && row.value(QStringLiteral("bookId")).toString() == QStringLiteral("missing-history-book")
                && row.value(QStringLiteral("stateId")).toString() == QStringLiteral("orphan-history-state")) {
                orphanReported = row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("unassigned-record"));
            }
        }
        QVERIFY(orphanReported);

        const auto result = checker.deleteAllOrphanedBookStates();
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QCOMPARE(result.value(QStringLiteral("deletedCount")).toInt(), 4);
        QTRY_VERIFY(!checker.running());

        QSqlQuery remaining(QSqlDatabase::database());
        QVERIFY(remaining.exec(QStringLiteral("SELECT book_id, state_id FROM book_state_history ORDER BY state_id")));
        QVERIFY(remaining.next());
        QCOMPARE(remaining.value(0).toString(), book.uniqueIdentifier);
        QCOMPARE(remaining.value(1).toString(), QStringLiteral("valid-history-state"));
        QVERIFY(!remaining.next());
        QVERIFY(remaining.exec(QStringLiteral("SELECT book_id, state_id FROM book_current_state")));
        QVERIFY(remaining.next());
        QCOMPARE(remaining.value(0).toString(), book.uniqueIdentifier);
        QCOMPARE(remaining.value(1).toString(), QStringLiteral("valid-current-state"));
        QVERIFY(!remaining.next());
    }
    void deleteAnyIdenticalBookFile_data()
    {
        QTest::addColumn<int>("copies");
        QTest::addColumn<int>("selected");
        QTest::addColumn<bool>("pdf");
        QTest::newRow("two-first") << 2 << 0 << false;
        QTest::newRow("two-second") << 2 << 1 << false;
        QTest::newRow("three-first") << 3 << 0 << false;
        QTest::newRow("three-middle") << 3 << 1 << false;
        QTest::newRow("three-last") << 3 << 2 << false;
        QTest::newRow("pdf-two-second") << 2 << 1 << true;
        QTest::newRow("pdf-three-middle") << 3 << 1 << true;
    }
    void deleteAnyIdenticalBookFile()
    {
        QFETCH(int, copies);
        QFETCH(int, selected);
        QFETCH(bool, pdf);
        QTemporaryDir dir;
        BookEntry book;
        const auto suffix = pdf ? QStringLiteral(".pdf") : QStringLiteral(".epub");
        if (pdf) {
            book.filename = dir.filePath(QStringLiteral("0") + suffix);
            book.title = book.uniqueIdentifier = book.identifier = QStringLiteral("shared");
            PoDoFo::PdfMemDocument document;
            document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
            document.Save(book.filename.toStdString());
        } else {
            book = epub(dir.filePath(QStringLiteral("0") + suffix), QStringLiteral("shared"));
        }
        auto &db = BookDatabase::self();
        QStringList paths;
        for (int i = 0; i < copies; ++i) {
            auto copy = book;
            copy.filename = dir.filePath(QString::number(i) + suffix);
            if (i > 0)
                QVERIFY(QFile::copy(book.filename, copy.filename));
            db.addEntry(copy);
            paths.append(copy.filename);
        }
        db.saveAnnotation(book.uniqueIdentifier,
                          {{QStringLiteral("annotationId"), QStringLiteral("shared-note")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        db.saveAnnotation(paths.at(selected),
                          {{QStringLiteral("annotationId"), QStringLiteral("file-note")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        const QVariantMap reference{{QStringLiteral("sourceBookId"), book.uniqueIdentifier},
                                    {QStringLiteral("sourceAnchorId"), QStringLiteral("ref")},
                                    {QStringLiteral("targetBookId"), book.uniqueIdentifier},
                                    {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}};
        db.saveReference(reference);
        BookListModel model;
        model.componentComplete();
        QTRY_COMPARE(model.count(), copies);
        const auto originalHash = digest(book.filename);
        LibraryMaintenance checker;
        checker.start(QStringLiteral("database"));
        QTRY_VERIFY(!checker.running());
        QVariantMap duplicateRow;
        for (const auto &value : checker.report().value(QStringLiteral("results")).toList()) {
            if (value.toMap().contains(QStringLiteral("duplicateFiles")))
                duplicateRow = value.toMap();
        }
        QCOMPARE(duplicateRow.value(QStringLiteral("duplicateFiles")).toStringList(), paths);
        QCOMPARE(duplicateRow.value(QStringLiteral("contentHash")).toString(), QString::fromLatin1(originalHash.toHex()));
        const auto result = checker.deleteDuplicate(paths.at(selected));
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QTRY_COMPARE(model.count(), copies - 1);
        QCOMPARE(db.loadEntries().size(), copies - 1);
        QCOMPARE(checker.report().value(QStringLiteral("files")).toInt(), copies - 1);
        QCOMPARE(hasCode(checker.report(), QStringLiteral("identical-files")), copies > 2);
        QVERIFY(!QFileInfo::exists(paths.at(selected)));
        QVERIFY(db.loadAnnotations(paths.at(selected)).isEmpty());
        paths.removeAt(selected);
        for (const auto &path : paths)
            QCOMPARE(digest(path), originalHash);
        QCOMPARE(db.loadAnnotations(book.uniqueIdentifier).size(), 1);
        QCOMPARE(db.loadAllReferences().size(), 1);
        for (const auto &entry : db.loadEntries())
            QVERIFY(paths.contains(entry.filename));
        if (copies == 2) {
            QVERIFY(!checker.deleteDuplicate(paths.first()).value(QStringLiteral("success")).toBool());
            QCOMPARE(digest(paths.first()), originalHash);
        }
    }

    void deleteAnySharedBookIdFile_data()
    {
        QTest::addColumn<int>("copies");
        QTest::addColumn<int>("selected");
        QTest::addColumn<QString>("identity");
        QTest::addColumn<bool>("pdf");
        QTest::newRow("two-first") << 2 << 0 << QStringLiteral("plain") << false;
        QTest::newRow("two-second") << 2 << 1 << QStringLiteral("plain") << false;
        QTest::newRow("three-first") << 3 << 0 << QStringLiteral("plain") << false;
        QTest::newRow("three-middle") << 3 << 1 << QStringLiteral("plain") << false;
        QTest::newRow("three-last") << 3 << 2 << QStringLiteral("plain") << false;
        QTest::newRow("normalized-uuid") << 2 << 0 << QStringLiteral("uuid") << false;
        QTest::newRow("shared-alias") << 2 << 1 << QStringLiteral("alias") << false;
        QTest::newRow("pdf-two-first") << 2 << 0 << QStringLiteral("plain") << true;
        QTest::newRow("pdf-three-last") << 3 << 2 << QStringLiteral("plain") << true;
    }
    void deleteAnySharedBookIdFile()
    {
        QFETCH(int, copies);
        QFETCH(int, selected);
        QFETCH(QString, identity);
        QFETCH(bool, pdf);
        QTemporaryDir dir;
        const auto id = identity == QStringLiteral("uuid") ? QStringLiteral("4f03f48b-42bc-46a4-a1ca-6d8314d96d25") : QStringLiteral("shared");
        QStringList paths;
        QHash<QString, QByteArray> originalHashes;
        auto &db = BookDatabase::self();
        for (int i = 0; i < copies; ++i) {
            const auto path = dir.filePath(QString::number(i) + (pdf ? QStringLiteral(".pdf") : QStringLiteral(".epub")));
            BookEntry book;
            if (pdf) {
                PoDoFo::PdfMemDocument document;
                for (int page = 0; page <= i; ++page)
                    document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
                document.Save(path.toStdString());
                book.filename = path;
                book.title = book.uniqueIdentifier = book.identifier = id;
            } else {
                book = epub(path, id, QByteArrayLiteral("<p id=\"note\">Edition ") + QByteArray::number(i) + QByteArrayLiteral("</p>"));
            }
            if (identity == QStringLiteral("uuid")) {
                book.uniqueIdentifier = book.identifier = i == 0 ? QStringLiteral("urn:uuid:%1").arg(id.toUpper()) : QStringLiteral("{%1}").arg(id);
            } else if (identity == QStringLiteral("alias")) {
                book.uniqueIdentifier = QStringLiteral("edition-%1").arg(i);
                book.identifier = book.uniqueIdentifier + QStringLiteral(", ") + id;
            }
            db.addEntry(book);
            paths.append(path);
            originalHashes.insert(path, digest(path));
        }
        db.saveAnnotation(id, {{QStringLiteral("annotationId"), QStringLiteral("shared-note")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        db.saveReference({{QStringLiteral("sourceBookId"), id},
                          {QStringLiteral("sourceAnchorId"), QStringLiteral("ref")},
                          {QStringLiteral("targetBookId"), id},
                          {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});
        LibraryMaintenance checker;
        checker.start(QStringLiteral("database"));
        QTRY_VERIFY(!checker.running());
        QVERIFY(hasCode(checker.report(), QStringLiteral("shared-book-id")));
        QVERIFY(!hasCode(checker.report(), QStringLiteral("identical-files")));
        QVariantMap row;
        for (const auto &value : checker.report().value(QStringLiteral("results")).toList()) {
            if (value.toMap().value(QStringLiteral("bookId")) == id && value.toMap().contains(QStringLiteral("sharedIdFiles")))
                row = value.toMap();
        }
        QCOMPARE(row.value(QStringLiteral("sharedIdFiles")).toStringList(), paths);
        for (const auto &path : paths)
            QCOMPARE(row.value(QStringLiteral("fileHashes")).toMap().value(path).toString(), QString::fromLatin1(originalHashes.value(path).toHex()));
        QVERIFY(!checker.deleteDuplicate(paths.at(selected)).value(QStringLiteral("success")).toBool());
        const auto result = checker.deleteSharedBookIdFile(paths.at(selected), id, true);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QVERIFY(!QFileInfo::exists(paths.at(selected)));
        paths.removeAt(selected);
        for (const auto &path : paths)
            QCOMPARE(digest(path), originalHashes.value(path));
        QCOMPARE(db.loadEntries().size(), copies - 1);
        QCOMPARE(db.loadAnnotations(id).size(), 1);
        QCOMPARE(db.loadAllReferences().size(), 1);
        QCOMPARE(checker.report().value(QStringLiteral("files")).toInt(), copies - 1);
        QCOMPARE(hasCode(checker.report(), QStringLiteral("shared-book-id")), copies > 2);
        if (copies == 2) {
            QVERIFY(!checker.deleteSharedBookIdFile(paths.first(), id, true).value(QStringLiteral("success")).toBool());
            QCOMPARE(digest(paths.first()), originalHashes.value(paths.first()));
        }
    }

    void duplicateDeletionRejectsStaleResults_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("sharedId");
        for (const auto &change : {"selected", "retained", "missing", "unlisted", "symlink", "unchecked", "cancelled"}) {
            QTest::newRow(change) << QString::fromLatin1(change) << false;
            QTest::newRow(qPrintable(QStringLiteral("shared-id-") + QString::fromLatin1(change))) << QString::fromLatin1(change) << true;
        }
        for (const auto &change : {"selected-id", "retained-id", "wrong-id"})
            QTest::newRow(change) << QString::fromLatin1(change) << true;
    }
    void duplicateDeletionRejectsStaleResults()
    {
        QFETCH(QString, change);
        QFETCH(bool, sharedId);
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("shared"));
        auto copy = book;
        copy.filename = dir.filePath(QStringLiteral("second.epub"));
        QVERIFY(QFile::copy(book.filename, copy.filename));
        auto &db = BookDatabase::self();
        db.addEntry(book);
        db.addEntry(copy);
        LibraryMaintenance checker;
        if (change != QStringLiteral("unchecked")) {
            checker.start(QStringLiteral("database"));
            if (sharedId)
                QVERIFY(!checker.deleteSharedBookIdFile(book.filename, book.uniqueIdentifier, true).value(QStringLiteral("success")).toBool());
            if (change == QStringLiteral("cancelled"))
                checker.cancel();
            QTRY_VERIFY(!checker.running());
        }
        if (change == QStringLiteral("selected") || change == QStringLiteral("retained"))
            epub(change == QStringLiteral("selected") ? book.filename : copy.filename, book.uniqueIdentifier, QByteArrayLiteral("<p>Changed</p>"));
        else if (change == QStringLiteral("missing") || change == QStringLiteral("symlink")) {
            QVERIFY(QFile::remove(copy.filename));
            if (change == QStringLiteral("symlink"))
                QVERIFY(QFile::link(book.filename, copy.filename));
        } else if (change == QStringLiteral("unlisted")) {
            QVERIFY(db.removeEntry(copy));
        } else if (change == QStringLiteral("selected-id") || change == QStringLiteral("retained-id")) {
            const auto path = change == QStringLiteral("selected-id") ? book.filename : copy.filename;
            db.updateEntry(path, QStringLiteral("uniqueIdentifier"), QStringLiteral("changed"));
            db.updateEntry(path, QStringLiteral("identifier"), QStringLiteral("changed"));
        }
        const auto originalHash = digest(book.filename), dbHash = digest(database());
        if (sharedId) {
            const auto id = change == QStringLiteral("wrong-id") ? QStringLiteral("unrelated") : book.uniqueIdentifier;
            QVERIFY(!checker.deleteSharedBookIdFile(dir.filePath(QStringLiteral("not-offered.epub")), id, true).value(QStringLiteral("success")).toBool());
            QVERIFY(!checker.deleteSharedBookIdFile(book.filename, id, true).value(QStringLiteral("success")).toBool());
        } else {
            QVERIFY(!checker.deleteDuplicate(dir.filePath(QStringLiteral("not-offered.epub"))).value(QStringLiteral("success")).toBool());
            QVERIFY(!checker.deleteDuplicate(book.filename).value(QStringLiteral("success")).toBool());
        }
        QCOMPARE(digest(book.filename), originalHash);
        QCOMPARE(digest(database()), dbHash);
    }

    void duplicateDeletionRestoresFileOnDatabaseFailure_data()
    {
        QTest::addColumn<bool>("sharedId");
        QTest::newRow("identical-content") << false;
        QTest::newRow("shared-id") << true;
    }
    void duplicateDeletionRestoresFileOnDatabaseFailure()
    {
        QFETCH(bool, sharedId);
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("shared"));
        auto copy = book;
        copy.filename = dir.filePath(QStringLiteral("second.epub"));
        QVERIFY(QFile::copy(book.filename, copy.filename));
        auto &db = BookDatabase::self();
        db.addEntry(book);
        db.addEntry(copy);
        LibraryMaintenance checker;
        checker.start(QStringLiteral("database"));
        QTRY_VERIFY(!checker.running());
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER block_duplicate_deletion BEFORE DELETE ON books BEGIN SELECT RAISE(ABORT, 'test'); END")));
        query.finish();
        const auto originalHash = digest(book.filename);
        const auto result = sharedId ? checker.deleteSharedBookIdFile(book.filename, book.uniqueIdentifier, true) : checker.deleteDuplicate(book.filename);
        connection = QSqlDatabase::database();
        QSqlQuery cleanup(connection);
        QVERIFY(cleanup.exec(QStringLiteral("DROP TRIGGER block_duplicate_deletion")));
        cleanup.finish();
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(book.filename), originalHash);
        QCOMPARE(digest(copy.filename), originalHash);
        QCOMPARE(db.loadEntries().size(), 2);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2);
    }

    void duplicatePageOffersEachFileAndRefreshes_data()
    {
        QTest::addColumn<bool>("sharedId");
        QTest::addColumn<bool>("deleteFile");
        QTest::newRow("identical-content") << false << true;
        QTest::newRow("shared-id-keep-file") << true << false;
        QTest::newRow("shared-id-delete-file") << true << true;
    }
    void duplicatePageOffersEachFileAndRefreshes()
    {
        QFETCH(bool, sharedId);
        QFETCH(bool, deleteFile);
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("0.epub")), QStringLiteral("shared"));
        QStringList paths;
        for (int i = 0; i < 3; ++i) {
            auto copy = book;
            copy.filename = dir.filePath(QString::number(i) + QStringLiteral(".epub"));
            if (sharedId)
                epub(copy.filename, book.uniqueIdentifier, QByteArrayLiteral("<p>Edition ") + QByteArray::number(i) + QByteArrayLiteral("</p>"));
            else if (i > 0)
                QVERIFY(QFile::copy(book.filename, copy.filename));
            BookDatabase::self().addEntry(copy);
            paths.append(copy.filename);
        }
        LibraryMaintenance checker;
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1200; height: 950; visible: true
                MaintaincePage { anchors.fill: parent; checker: testChecker; Component.onCompleted: showCheck("database") }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        QTRY_VERIFY(!checker.running());
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        auto *root = quickWindow->contentItem();
        QTRY_COMPARE(duplicateButtons(root, sharedId).size(), 3);
        const auto buttons = duplicateButtons(root, sharedId);
        QStringList offered;
        for (auto *button : buttons) {
            QVERIFY(button->isVisible());
            QVERIFY(button->isEnabled());
            offered.append(button->property("fileName").toString());
        }
        QCOMPARE(offered, paths);
        auto *confirmation = window->findChild<QObject *>(QStringLiteral("duplicateDeleteConfirmation"));
        QVERIFY(confirmation);
        QVERIFY(QMetaObject::invokeMethod(buttons.at(1), "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(confirmation->property("fileName").toString(), paths.at(1));
        QCOMPARE(confirmation->property("sharedBookId").toString(), sharedId ? book.uniqueIdentifier : QString());
        auto *explanation = visualItem(root, QStringLiteral("bookFileDeleteExplanation"));
        QVERIFY(explanation);
        const auto message = sharedId ? i18n(
                                            "Remove this book from the library? Another file with book ID %1 will remain in the library. Files "
                                            "with the same book ID may have different contents.",
                                            book.uniqueIdentifier)
                                      : i18n("Permanently delete this file and remove its library entry? Another identical copy will be retained.");
        QCOMPARE(explanation->property("text").toString(), sharedId ? message : message + QStringLiteral("\n\n") + paths.at(1));
        auto *deleteFileCheckBox = visualItem(root, QStringLiteral("deleteSharedBookIdFileCheckBox"));
        QVERIFY(deleteFileCheckBox);
        if (sharedId) {
            QVERIFY(deleteFileCheckBox->isVisible());
            QVERIFY(!deleteFileCheckBox->property("checked").toBool());
            QVERIFY(deleteFileCheckBox->setProperty("checked", true));
        }
        QVERIFY(QFileInfo::exists(paths.at(1)));
        QVERIFY(QMetaObject::invokeMethod(confirmation, "reject"));
        QVERIFY(QFileInfo::exists(paths.at(1)));
        QVERIFY(QMetaObject::invokeMethod(buttons.at(1), "clicked"));
        if (sharedId) {
            QVERIFY(!deleteFileCheckBox->property("checked").toBool());
            QVERIFY(deleteFileCheckBox->setProperty("checked", deleteFile));
        }
        QVERIFY(QMetaObject::invokeMethod(confirmation, "accept"));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(QFileInfo::exists(paths.at(1)), !deleteFile);
        QTRY_COMPARE(duplicateButtons(root, sharedId).size(), 2);
        QVERIFY(QMetaObject::invokeMethod(duplicateButtons(root, sharedId).last(), "clicked"));
        QCOMPARE(confirmation->property("fileName").toString(), paths.last());
        if (sharedId) {
            QVERIFY(!deleteFileCheckBox->property("checked").toBool());
            QVERIFY(deleteFileCheckBox->setProperty("checked", deleteFile));
        }
        QVERIFY(QMetaObject::invokeMethod(confirmation, "accept"));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(QFileInfo::exists(paths.last()), !deleteFile);
        QTRY_COMPARE(duplicateButtons(root, sharedId).size(), 0);
        QVERIFY(QFileInfo::exists(paths.first()));
        QCOMPARE(BookDatabase::self().loadEntries().size(), 1);
    }

    void annotationsIncludeMissingBooksAliasesAndInvalidGeometry()
    {
        QTemporaryDir dir;
        auto book = epub(dir.filePath(QStringLiteral("book.epub")), QStringLiteral("book"));
        book.identifier = QStringLiteral("legacy, other");
        BookEntry pdfBook;
        pdfBook.filename = dir.filePath(QStringLiteral("book.pdf"));
        pdfBook.uniqueIdentifier = pdfBook.identifier = QStringLiteral("pdf");
        PoDoFo::PdfMemDocument document;
        document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        document.Save(pdfBook.filename.toStdString());
        auto &db = BookDatabase::self();
        db.addEntry(book);
        db.addEntry(pdfBook);
        db.saveAnnotation(QStringLiteral("legacy"),
                          {{QStringLiteral("annotationId"), QStringLiteral("valid")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        db.saveAnnotation(QStringLiteral("book"),
                          {{QStringLiteral("annotationId"), QStringLiteral("broken")}, {QStringLiteral("anchorId"), QStringLiteral("missing")}});
        db.saveAnnotation(QStringLiteral("unknown"),
                          {{QStringLiteral("annotationId"), QStringLiteral("orphan")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        const QVariantMap rect{{QStringLiteral("x"), 0.2}, {QStringLiteral("y"), 0.3}, {QStringLiteral("width"), 0.4}, {QStringLiteral("height"), 0.1}};
        QVariantMap location{{QStringLiteral("page"), 0}, {QStringLiteral("rects"), QVariantList{rect}}};
        QVariantMap note{{QStringLiteral("annotationId"), QStringLiteral("pdf-valid")},
                         {QStringLiteral("format"), QStringLiteral("pdf")},
                         {QStringLiteral("location"), location}};
        db.saveAnnotation(pdfBook.filename, note);
        note.insert(QStringLiteral("annotationId"), QStringLiteral("pdf-invalid"));
        location.insert(QStringLiteral("page"), 3);
        location.insert(QStringLiteral("rects"), QVariantList{});
        note.insert(QStringLiteral("location"), location);
        db.saveAnnotation(QStringLiteral("pdf"), note);
        const auto before = digest(database());
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("annotations"));
        QCOMPARE(report.value(QStringLiteral("annotations")).toInt(), 5);
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 2);
        QCOMPARE(report.value(QStringLiteral("errors")).toInt(), 3);
        for (const auto &code : {"annotation-book-unresolved", "annotation-anchor-unresolved", "annotation-page-invalid", "annotation-geometry-invalid"})
            QVERIFY(hasCode(report, QString::fromLatin1(code)));
        QCOMPARE(digest(database()), before);
    }
    void storedReferencesIgnoreUnlinkedBooksAndOrphanAnchors()
    {
        QTemporaryDir dir;
        const auto source = epub(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("source"),
            "<span id=\"stored\" data-role=\"anchor\" data-anchor-type=\"crossref\">Stored</span>"
            "<span id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Orphan</span>");
        const auto target = epub(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("target"));
        const auto broken = epub(dir.filePath(QStringLiteral("broken.epub")), QStringLiteral("broken"), "<p><div>Broken</p>");
        const auto missing = epub(dir.filePath(QStringLiteral("missing.epub")), QStringLiteral("missing"));
        auto &db = BookDatabase::self();
        for (const auto &book : {source, target, broken, missing}) db.addEntry(book);
        QVERIFY(QFile::remove(missing.filename));
        db.saveReference({{QStringLiteral("sourceBookId"), source.uniqueIdentifier},
                          {QStringLiteral("sourceAnchorId"), QStringLiteral("stored")},
                          {QStringLiteral("targetBookId"), target.uniqueIdentifier},
                          {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});
        const auto before = digest(database());
        const auto sourceHash = digest(source.filename);
        QStringList paths;
        int total = -1;
        const auto stored = LibraryMaintenance::inspect(database(), QStringLiteral("references"), {}, {},
            [&](int, int count, const QString &path) { total = count; if (!path.isEmpty()) paths.append(path); });
        QCOMPARE(total, 1);
        QCOMPARE(paths, QStringList{source.filename});
        QCOMPARE(stored.value(QStringLiteral("passed")).toInt(), 1);
        QCOMPARE(stored.value(QStringLiteral("errors")).toInt(), 0);
        QCOMPARE(stored.value(QStringLiteral("warnings")).toInt(), 0);
        QCOMPARE(stored.value(QStringLiteral("results")).toList().size(), 1);
        const auto entire = LibraryMaintenance::inspect(database(), QStringLiteral("references-all"));
        QVERIFY(hasCode(entire, QStringLiteral("orphan-source-anchor")));
        QVERIFY(hasCode(entire, QStringLiteral("incomplete-cross-reference-scan")));
        QVERIFY(hasCode(entire, QStringLiteral("book-unreadable")));
        QCOMPARE(digest(database()), before);
        QCOMPARE(digest(source.filename), sourceHash);
        db.removeReference(source.uniqueIdentifier, QStringLiteral("stored"));
        const auto empty = LibraryMaintenance::inspect(database(), QStringLiteral("references"));
        QVERIFY(empty.value(QStringLiteral("results")).toList().isEmpty());
        QVERIFY(empty.value(QStringLiteral("success")).toBool());
    }

    void missingTargetAnchorOffersCorrectEditorInMaintenance()
    {
        QTemporaryDir dir;
        const auto source = epub(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("repair-source"),
            "<span id=\"ref\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</span>");
        const auto target = epub(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("repair-target"), "<p>Target only</p>");
        auto &db = BookDatabase::self();
        db.addEntry(source); db.addEntry(target);
        const QVariantMap reference{{QStringLiteral("sourceBookId"), source.uniqueIdentifier}, {QStringLiteral("sourceAnchorId"), QStringLiteral("ref")},
            {QStringLiteral("targetBookId"), target.uniqueIdentifier}, {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#missing")}};
        db.saveReference(reference);
        const auto beforeSource = digest(source.filename), beforeTarget = digest(target.filename);
        const auto capture = dir.filePath(QStringLiteral("opened.txt")), capturedBook = dir.filePath(QStringLiteral("opened.epub"));
        QFile script(dir.filePath(QStringLiteral("editor.sh")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$1\" >> \"%1\"\ncp -- \"$1\" \"%2\"\n").arg(capture, capturedBook).toUtf8());
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto command = Config::editorCommand();
        const auto locations = Config::bookLocations();
        const auto restore = qScopeGuard([&] { Config::setEditorCommand(command); Config::setBookLocations(locations); });
        Config::setEditorCommand(script.fileName()); Config::setBookLocations({dir.path()});
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow); quickWindow->show();
        auto *checker = window->findChild<LibraryMaintenance *>();
        QVERIFY(checker);
        QVERIFY(QMetaObject::invokeMethod(window.get(), "showMaintenancePage", Q_ARG(QVariant, QStringLiteral("references"))));
        QTRY_VERIFY(!checker->running());
        QQuickItem *open = nullptr;
        QTRY_VERIFY(open = visibleItem(quickWindow->contentItem(), QStringLiteral("openMissingTargetEditor")));
        QVERIFY(!QFileInfo::exists(capture));
        const auto row = checker->report().value(QStringLiteral("results")).toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("targetAnchorRepair")).toMap().value(QStringLiteral("fileName")).toString(), target.filename);
        const auto screenshot = qEnvironmentVariable("ARIANNA_TARGET_REPAIR_SCREENSHOT");
        if (!screenshot.isEmpty()) { QTest::qWait(100); QVERIFY(quickWindow->grabWindow().save(screenshot)); }
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QTRY_VERIFY(QFileInfo::exists(capturedBook));
        QTRY_COMPARE(digest(capturedBook), beforeTarget);
        QFile captured(capture); QVERIFY(captured.open(QIODevice::ReadOnly));
        const auto paths = captured.readAll().trimmed().split('\n');
        QCOMPARE(paths.size(), 1);
        QVERIFY(QString::fromUtf8(paths.first()) != target.filename);
        QCOMPARE(digest(source.filename), beforeSource); QCOMPARE(digest(target.filename), beforeTarget);
        QCOMPARE(db.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("ref")).value(QStringLiteral("targetLocation")), reference.value(QStringLiteral("targetLocation")));
        auto *sessions = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        QTRY_VERIFY(sessions->activeSessionForBook(target.uniqueIdentifier).value(QStringLiteral("error")).toBool());
    }

    void referencesCountOnceAndIncludeCompletelyOrphanedRecords()
    {
        QTemporaryDir dir;
        auto first = epub(dir.filePath(QStringLiteral("first.epub")),
                          QStringLiteral("first"),
                          "<span id=\"ref\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</span>");
        auto second = epub(dir.filePath(QStringLiteral("second.epub")), QStringLiteral("second"));
        auto &db = BookDatabase::self();
        db.addEntry(first);
        db.addEntry(second);
        db.saveReference({{QStringLiteral("sourceBookId"), QStringLiteral("first")},
                          {QStringLiteral("sourceAnchorId"), QStringLiteral("ref")},
                          {QStringLiteral("targetBookId"), QStringLiteral("second")},
                          {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});
        db.saveReference({{QStringLiteral("sourceBookId"), QStringLiteral("missing-source")},
                          {QStringLiteral("sourceAnchorId"), QStringLiteral("ref")},
                          {QStringLiteral("targetBookId"), QStringLiteral("missing-target")},
                          {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});
        db.saveReference({{QStringLiteral("sourceBookId"), QStringLiteral("missing-both")},
                          {QStringLiteral("sourceAnchorId"), QStringLiteral("ref")},
                          {QStringLiteral("targetBookId"), QStringLiteral("missing-both")},
                          {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("references"));
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("errors")).toInt(), 2);
        QCOMPARE(report.value(QStringLiteral("results")).toList().size(), 3);
        QVERIFY(hasCode(report, QStringLiteral("unassigned-reference")));
    }
    void missingFilesSurviveLibraryReload()
    {
        QTemporaryDir dir;
        auto book = epub(dir.filePath(QStringLiteral("missing.epub")), QStringLiteral("missing"));
        auto &db = BookDatabase::self();
        db.addEntry(book);
        db.saveAnnotation(book.uniqueIdentifier,
                          {{QStringLiteral("annotationId"), QStringLiteral("note")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        QVERIFY(QFile::remove(book.filename));
        BookListModel model;
        model.componentComplete();
        QTRY_VERIFY(model.cacheLoaded());
        QVERIFY(model.knownBookFiles().contains(book.filename));
        QVERIFY(db.loadEntry(book.filename));
        QCOMPARE(db.loadAnnotations(book.uniqueIdentifier).size(), 1);
        QVERIFY(hasCode(LibraryMaintenance::inspect(database(), QStringLiteral("database")), QStringLiteral("missing-book-file")));
        const auto notes = LibraryMaintenance::inspect(database(), QStringLiteral("annotations"));
        QCOMPARE(notes.value(QStringLiteral("errors")).toInt(), 0);
        QVERIFY(hasCode(notes, QStringLiteral("annotation-book-unreadable")));
    }
    void structureFindsMissingToolsAndMalformedBooks_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("malformed-xml") << QByteArrayLiteral("<invalid>");
        QTest::newRow("duplicate-id") << QByteArrayLiteral("<p id=\"same\">One</p><p id=\"same\">Two</p>");
    }
    void structureFindsMissingToolsAndMalformedBooks()
    {
        QFETCH(QByteArray, body);
        QTemporaryDir dir;
        auto broken = epub(dir.filePath(QStringLiteral("broken.epub")), QStringLiteral("broken"), body);
        BookDatabase::self().addEntry(broken);
        const auto hash = digest(broken.filename);
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("structure"), QStringLiteral("arianna-nonexistent-check-tool"));
        QVERIFY(hasCode(report, QStringLiteral("basic-book-structure")));
        QVERIFY(hasCode(report, QStringLiteral("validator-unavailable")));
        QCOMPARE(digest(broken.filename), hash);
    }
    void pdfStructureUsesQpdf()
    {
        QTemporaryDir dir;
        BookEntry book;
        book.filename = dir.filePath(QStringLiteral("book.pdf"));
        book.uniqueIdentifier = book.identifier = QStringLiteral("pdf");
        PoDoFo::PdfMemDocument document;
        document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        document.Save(book.filename.toStdString());
        BookDatabase::self().addEntry(book);
        const auto before = digest(book.filename);
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("structure"));
        if (hasCode(report, QStringLiteral("validator-unavailable")))
            QSKIP("qpdf is not installed");
        QVERIFY2(report.value(QStringLiteral("success")).toBool(), QJsonDocument::fromVariant(report).toJson().constData());
        QCOMPARE(digest(book.filename), before);
    }
    void validatorUsesLiteralFileArgument()
    {
        QTemporaryDir dir;
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("spaces ; $book.epub")), QStringLiteral("book")));
        const QString script = dir.filePath(QStringLiteral("validator script.sh"));
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nif [ -f \"$1\" ]; then echo validated; exit 0; else exit 2; fi\n");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("structure"), script);
        QVERIFY2(report.value(QStringLiteral("success")).toBool(), QJsonDocument::fromVariant(report).toJson().constData());
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 1);
    }
    void cancellationStopsValidator()
    {
        QTemporaryDir dir;
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("book.epub")), QStringLiteral("book")));
        const QString script = dir.filePath(QStringLiteral("slow-check"));
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nexec sleep 20\n");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        Config::self()->setEpubCheckCommand(script);
        LibraryMaintenance checker;
        checker.start(QStringLiteral("structure"));
        QTRY_VERIFY(checker.total() == 1);
        checker.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!checker.running(), 5000);
        QVERIFY(checker.report().value(QStringLiteral("cancelled")).toBool());
        QVERIFY(!checker.report().value(QStringLiteral("success")).toBool());
    }
    void reportUiFiltersCopiesAndExports()
    {
        BookDatabase::self().saveAnnotation(QStringLiteral("missing"),
                                            {{QStringLiteral("annotationId"), QStringLiteral("orphan")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        LibraryMaintenance checker;
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(
            "import QtQuick\nimport org.kde.arianna\nWindow { width: 1000; height: 800; visible: true;"
            "MaintaincePage { anchors.fill: parent; checker: testChecker; Component.onCompleted: showCheck('database') } }",
            QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        QTRY_VERIFY(!checker.running() && !checker.report().isEmpty());
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        auto *root = quickWindow->contentItem();
        QQuickItem *filter = nullptr;
        QTRY_VERIFY(filter = visualItem(root, QStringLiteral("maintenanceStatusFilter")));
        QVERIFY(filter->setProperty("currentIndex", 1));
        auto *list = visualItem(root, QStringLiteral("maintenanceResults"));
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 1);
        auto *copy = visualItem(root, QStringLiteral("copyMaintenanceReport"));
        QVERIFY(copy);
        QVERIFY(QMetaObject::invokeMethod(copy, "clicked"));
        QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("orphan")));
        QTemporaryDir dir;
        const auto destination = dir.filePath(QStringLiteral("report.json"));
        QVERIFY(checker.saveReport(QUrl::fromLocalFile(destination)));
        QFile report(destination);
        QVERIFY(report.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(report.readAll()), QJsonDocument::fromVariant(checker.report()));
        checker.start(QStringLiteral("annotations"));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(checker.report().value(QStringLiteral("errors")).toInt(), 1);
    }
    void databaseReadErrorsAreNotEmptySuccess()
    {
        QTemporaryDir dir;
        const auto report = LibraryMaintenance::inspect(dir.filePath(QStringLiteral("absent.sqlite")), QStringLiteral("database"));
        QVERIFY(!report.value(QStringLiteral("success")).toBool());
        QVERIFY(hasCode(report, QStringLiteral("database-unreadable")));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("absent.sqlite"))));
    }

    void closingImportReportReleasesCopyDespiteQueuedChecks()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("queued-review.epub")), QStringLiteral("queued-review"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto copy = cacheCopy(book.filename);
        epub(copy, book.uniqueIdentifier, QByteArrayLiteral("<p>Unimported editor changes</p>"));
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testBookId"), book.uniqueIdentifier);
        engine.rootContext()->setContextProperty(QStringLiteral("testCopy"), copy);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1200; height: 950; visible: true
                property var session: EditorSessionStore.recoverWorkingCopy(testBookId, testCopy)
                EpubImportRepairDialog { id: report; objectName: "importReport" }
                Component.onCompleted: report.showForImport(testBookId, testCopy, session.sessionId)
                function closeDuringRecheck() {
                    report.recheck();
                    report.recheck();
                    report.close();
                }
            }
        )", QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        auto *report = object->findChild<QObject *>(QStringLiteral("importReport"));
        QVERIFY(report);
        QTRY_VERIFY(report->property("opened").toBool());
        QTRY_VERIFY(!report->property("running").toBool());
        QVERIFY(!cacheRow(copy).value(QStringLiteral("canDiscard")).toBool());
        QVERIFY(QMetaObject::invokeMethod(object.get(), "closeDuringRecheck"));
        QTRY_VERIFY(!report->property("visible").toBool());
        QTRY_VERIFY(!report->property("running").toBool());
        QTRY_VERIFY(cacheRow(copy).value(QStringLiteral("canDiscard")).toBool());
        QTest::qWait(300);
        QVERIFY(!report->property("visible").toBool());
        QVERIFY(QFileInfo::exists(copy));
    }

    void cachePageDiscardsWorkingCopyOnlyAfterConfirmation()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("discard-ui.epub")), QStringLiteral("discard-ui"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto copy = cacheCopy(book.filename);
        epub(copy, book.uniqueIdentifier, QByteArrayLiteral("<p>Abandoned editor changes</p>"));
        epub(book.filename, book.uniqueIdentifier, QByteArrayLiteral("<p>New viewer anchor state</p>"));
        const auto original = digest(book.filename);
        LibraryMaintenance checker;
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1200; height: 950; visible: true
                MaintaincePage { anchors.fill: parent; checker: testChecker; Component.onCompleted: showCheck("cache") }
            }
        )", QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QTRY_VERIFY(!checker.running());
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = visualItem(window->contentItem(), QStringLiteral("discardWorkingCopy"))));
        QVERIFY(button->isVisible());
        QVERIFY(button->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto *confirmation = object->findChild<QObject *>(QStringLiteral("discardWorkingCopyConfirmation"));
        QVERIFY(confirmation);
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(confirmation->property("fileName").toString(), copy);
        QVERIFY(QMetaObject::invokeMethod(confirmation, "reject"));
        QVERIFY(QFileInfo::exists(copy));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(confirmation, "accept"));
        QTRY_VERIFY(!checker.running());
        QVERIFY(!QFileInfo::exists(copy));
        QCOMPARE(digest(book.filename), original);
    }

    void cachePageRequiresConfirmationForUnverifiedCopies()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("ui.epub")), QStringLiteral("ui"));
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto safe = cacheCopy(book.filename);
        const auto uncertain = cacheCopy(book.filename);
        epub(uncertain, book.uniqueIdentifier, QByteArrayLiteral("<p>Keep until explicitly confirmed</p>"));
        LibraryMaintenance checker;
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1200; height: 950; visible: true
                MaintaincePage { anchors.fill: parent; checker: testChecker; Component.onCompleted: showCheck("cache") }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        QTRY_VERIFY(!checker.running());
        auto *dialog = window->findChild<QObject *>(QStringLiteral("maintenancePage"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "selectFile", Q_ARG(QVariant, safe), Q_ARG(QVariant, false)));
        QVERIFY(QMetaObject::invokeMethod(dialog, "selectFile", Q_ARG(QVariant, uncertain), Q_ARG(QVariant, true)));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = visualItem(quickWindow->contentItem(), QStringLiteral("deleteCacheFiles")));
        QVERIFY(button->isVisible());
        QVERIFY(button->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto *confirmation = window->findChild<QObject *>(QStringLiteral("cacheDeleteConfirmation"));
        QVERIFY(confirmation);
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QFileInfo::exists(uncertain));
        QVERIFY(QMetaObject::invokeMethod(confirmation, "accept"));
        QTRY_VERIFY(!checker.running());
        QVERIFY(!QFileInfo::exists(uncertain));
        QVERIFY(QFileInfo::exists(safe));
        QVERIFY(QFileInfo::exists(book.filename));
    }

    void legacyCfiRemarkWithTextOpensEditorAtPassage()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("remark-location.epub")), QStringLiteral("remark-location"),
                               QByteArrayLiteral("<span id=\"uuid_valid\" data-role=\"anchor\" data-anchor-type=\"annotation\">Valid</span>"
                                                 "<p>Selected passage</p>"));
        BookDatabase::self().addEntry(book);
        auto &db = BookDatabase::self();
        const QString cfi = QStringLiteral("epubcfi(/6/2!/4/4,/1:0,/1:16)");
        db.saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("legacy-with-text")},
                                                  {QStringLiteral("cfiRange"), cfi},
                                                  {QStringLiteral("text"), QStringLiteral("Selected passage")}});
        db.saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("broken-without-text")},
                                                  {QStringLiteral("anchorId"), QStringLiteral("missing-blank")}});
        db.saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("valid-with-text")},
                                                  {QStringLiteral("anchorId"), QStringLiteral("valid")},
                                                  {QStringLiteral("text"), QStringLiteral("Valid")}});
        const auto report = LibraryMaintenance::inspect(database(), QStringLiteral("annotations"));
        QCOMPARE(report.value(QStringLiteral("errors")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("warnings")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 1);
        QVERIFY(hasCode(report, QStringLiteral("annotation-cfi-unverified")));
        const auto capture = dir.filePath(QStringLiteral("editor-arguments.txt"));
        QFile script(dir.filePath(QStringLiteral("editor.sh")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"");
        script.write(capture.toUtf8());
        script.write("\"\nexit 42\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto previousCommand = Config::self()->editorCommand();
        const auto previousLocations = Config::self()->bookLocations();
        const auto restoreConfig = qScopeGuard([=]() {
            Config::self()->setEditorCommand(previousCommand);
            Config::self()->setBookLocations(previousLocations);
        });
        Config::self()->setEditorCommand(script.fileName());
        Config::self()->setBookLocations({dir.path()});
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        quickWindow->show();
        auto *checker = window->findChild<LibraryMaintenance *>();
        QVERIFY(checker);
        QVERIFY(QMetaObject::invokeMethod(window.get(), "showMaintenancePage", Q_ARG(QVariant, QStringLiteral("annotations"))));
        QTRY_VERIFY(!checker->running());
        QCOMPARE(checker->report().value(QStringLiteral("warnings")).toInt(), 1);
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = visibleItem(quickWindow->contentItem(), QStringLiteral("editRemarkTextLocation")));
        QVERIFY(button->isEnabled());
        QCOMPARE(visibleItemsNamed(quickWindow->contentItem(), QStringLiteral("editRemarkTextLocation")), 1);
        QCOMPARE(button->property("text").toString(), i18n("Edit Text Passage"));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(QFileInfo::exists(capture));
        QFile arguments(capture);
        QVERIFY(arguments.open(QIODevice::ReadOnly));
        const auto args = QString::fromUtf8(arguments.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(args.size(), 3);
        QVERIFY(args.first().endsWith(QStringLiteral("remark-location.epub")));
        QVERIFY(args.first() != book.filename);
        QCOMPARE(args.at(1), QStringLiteral("--select-text"));
        QCOMPARE(args.at(2), QStringLiteral("Selected passage"));
        auto *maintenance = visibleItem(quickWindow->contentItem(), QStringLiteral("maintenancePage"));
        QVERIFY(maintenance);
        QTRY_VERIFY(maintenance->property("exportMessage").toString().contains(QStringLiteral("42")));
        auto *sessions = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        QVERIFY(sessions);
        QTRY_VERIFY(sessions->activeSessionForBook(book.uniqueIdentifier).value(QStringLiteral("error")).toBool());
        QVERIFY(!cacheCopy(book.filename).isEmpty());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(!maintenance->property("exportMessage").toString().isEmpty()
                    && !maintenance->property("exportMessage").toString().contains(QStringLiteral("42")));
    }

    void remarkTextCountAndSourceChoiceCreatesAnchor()
    {
        QTemporaryDir dir;
        const auto book = epubWithTwoChapters(dir.filePath(QStringLiteral("matches.epub")), QStringLiteral("match-book"),
                                              QByteArrayLiteral("<p>First before Selected words first after.</p>"),
                                              QByteArrayLiteral("<p>Second before <em>Selected</em> words second after.</p>"));
        BookDatabase::self().addEntry(book);
        BookDatabase::self().saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("broken-anchor")},
                                                                   {QStringLiteral("anchorId"), QStringLiteral("missing-anchor")},
                                                                   {QStringLiteral("text"), QStringLiteral("Selected words")}});
        BookDatabase::self().saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("absent-text")},
                                                                   {QStringLiteral("anchorId"), QStringLiteral("missing-absent")},
                                                                   {QStringLiteral("text"), QStringLiteral("Never in this EPUB")}});
        LibraryMaintenance checker;
        checker.start(QStringLiteral("annotations"));
        QTRY_VERIFY(!checker.running());
        QVariantMap broken, absent;
        for (const auto &value : checker.report().value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("annotationId")) == QStringLiteral("broken-anchor")) broken = row;
            if (row.value(QStringLiteral("annotationId")) == QStringLiteral("absent-text")) absent = row;
        }
        QCOMPARE(broken.value(QStringLiteral("textMatchCount")).toInt(), 2);
        QCOMPARE(absent.value(QStringLiteral("textMatchCount")).toInt(), 0);
        QVERIFY(!checker.findRemarkText(absent).value(QStringLiteral("success")).toBool());
        const auto found = checker.findRemarkText(broken);
        QVERIFY2(found.value(QStringLiteral("success")).toBool(), qPrintable(found.value(QStringLiteral("message")).toString()));
        const auto matches = found.value(QStringLiteral("matches")).toList();
        QCOMPARE(matches.size(), 2);
        QCOMPARE(matches.at(0).toMap().value(QStringLiteral("documentPath")).toString(), QStringLiteral("one.xhtml"));
        QCOMPARE(matches.at(1).toMap().value(QStringLiteral("documentPath")).toString(), QStringLiteral("two.xhtml"));
        QVERIFY(matches.at(1).toMap().value(QStringLiteral("before")).toString().contains(QStringLiteral("Second before")));
        const auto original = digest(book.filename);
        const auto copy = cacheCopy(book.filename);
        QVERIFY(!copy.isEmpty());
        QVERIFY(!checker.anchorRemarkAtText(broken, matches.at(1).toMap()).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(book.filename), original);
        QVERIFY(QDir(QFileInfo(copy).absolutePath()).removeRecursively());
        const auto foundAgain = checker.findRemarkText(broken);
        QVERIFY2(foundAgain.value(QStringLiteral("success")).toBool(), qPrintable(foundAgain.value(QStringLiteral("message")).toString()));
        const auto anchored = checker.anchorRemarkAtText(broken, matches.at(1).toMap());
        QVERIFY2(anchored.value(QStringLiteral("success")).toBool(), qPrintable(anchored.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        const auto newId = anchored.value(QStringLiteral("anchorId")).toString();
        QVERIFY(!newId.isEmpty());
        QCOMPARE(BookDatabase::self().loadAnnotations(book.uniqueIdentifier).first().toMap().value(QStringLiteral("anchorId")).toString(), newId);
        EPubContainer container(nullptr);
        QVERIFY(container.openFile(book.filename));
        QVERIFY(!container.readData(QStringLiteral("one.xhtml")).contains(newId.toUtf8()));
        QVERIFY(container.readData(QStringLiteral("two.xhtml")).contains(QByteArray(QByteArrayLiteral("uuid_") + newId.toUtf8())));
        QCOMPARE(container.countAnnotationText(QStringLiteral("Selected words")), 2);
        bool repaired = false;
        for (const auto &value : checker.report().value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("annotationId")) == QStringLiteral("broken-anchor"))
                repaired = row.value(QStringLiteral("status")) == QStringLiteral("ok") && row.value(QStringLiteral("textMatchCount")).toInt() == 2;
        }
        QVERIFY(repaired);
    }

    void legacyCfiRemarkCanBeAnchoredByUniqueText()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("legacy-anchor.epub")), QStringLiteral("legacy-anchor-book"),
                               QByteArrayLiteral("<p>Only this saved passage is here.</p>"));
        BookDatabase::self().addEntry(book);
        BookDatabase::self().saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("legacy-note")},
                                                                   {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2,/1:0,/1:8)")},
                                                                   {QStringLiteral("text"), QStringLiteral("saved passage")}});
        LibraryMaintenance checker;
        checker.start(QStringLiteral("annotations"));
        QTRY_VERIFY(!checker.running());
        const auto row = checker.report().value(QStringLiteral("results")).toList().first().toMap();
        QVERIFY(row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("annotation-cfi-unverified")));
        QCOMPARE(row.value(QStringLiteral("textMatchCount")).toInt(), 1);
        const auto found = checker.findRemarkText(row);
        QVERIFY(found.value(QStringLiteral("success")).toBool());
        QCOMPARE(found.value(QStringLiteral("matches")).toList().size(), 1);
        const auto anchored = checker.anchorRemarkAtText(row, found.value(QStringLiteral("matches")).toList().first().toMap());
        QVERIFY2(anchored.value(QStringLiteral("success")).toBool(), qPrintable(anchored.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        const auto saved = BookDatabase::self().loadAnnotations(book.uniqueIdentifier).first().toMap();
        QCOMPARE(saved.value(QStringLiteral("anchorId")).toString(), anchored.value(QStringLiteral("anchorId")).toString());
        QVERIFY(!saved.value(QStringLiteral("cfiRange")).toString().isEmpty());
        QCOMPARE(checker.report().value(QStringLiteral("passed")).toInt(), 1);
    }

    void remarkTextChoiceIsShownInMaintenancePage()
    {
        QTemporaryDir dir;
        const auto book = epubWithTwoChapters(dir.filePath(QStringLiteral("choice.epub")), QStringLiteral("choice-book"),
                                              QByteArrayLiteral("<p>First version of selected phrase.</p>"),
                                              QByteArrayLiteral("<p>Second version of selected phrase.</p>"));
        BookDatabase::self().addEntry(book);
        BookDatabase::self().saveAnnotation(book.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("choice-note")},
                                                                   {QStringLiteral("anchorId"), QStringLiteral("old-missing")},
                                                                   {QStringLiteral("text"), QStringLiteral("selected phrase")}});
        const auto previousLocations = Config::self()->bookLocations();
        const auto restore = qScopeGuard([=]() { Config::self()->setBookLocations(previousLocations); });
        Config::self()->setBookLocations({dir.path()});
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        quickWindow->show();
        auto *checker = window->findChild<LibraryMaintenance *>();
        QVERIFY(checker);
        QVERIFY(QMetaObject::invokeMethod(window.get(), "showMaintenancePage", Q_ARG(QVariant, QStringLiteral("annotations"))));
        QTRY_VERIFY(!checker->running());
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = visibleItem(quickWindow->contentItem(), QStringLiteral("anchorRemarkTextLocation")));
        QVERIFY(button->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto *maintenance = visibleItem(quickWindow->contentItem(), QStringLiteral("maintenancePage"));
        QVERIFY(maintenance);
        auto *dialog = maintenance->findChild<QObject *>(QStringLiteral("anchorTextChoice"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QList<QQuickItem *> choices;
        std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
            if (item->objectName() == QStringLiteral("anchorTextMatch")) choices.append(item);
            for (auto *child : item->childItems()) collect(child);
        };
        collect(quickWindow->contentItem());
        QCOMPARE(choices.size(), 2);
        QVERIFY(choices.at(0)->property("text").toString().contains(QStringLiteral("one.xhtml")));
        QVERIFY(choices.at(1)->property("text").toString().contains(QStringLiteral("two.xhtml")));
        QVERIFY(QMetaObject::invokeMethod(choices.at(1), "clicked"));
        QTRY_VERIFY(!checker->running());
        EPubContainer container(nullptr);
        QVERIFY(container.openFile(book.filename));
        const auto annotations = BookDatabase::self().loadAnnotations(book.uniqueIdentifier);
        QCOMPARE(annotations.size(), 1);
        const auto anchorId = annotations.first().toMap().value(QStringLiteral("anchorId")).toString();
        QVERIFY(anchorId != QStringLiteral("old-missing"));
        QVERIFY(container.readData(QStringLiteral("two.xhtml")).contains(QByteArray(QByteArrayLiteral("uuid_") + anchorId.toUtf8())));
    }

    void maintenancePageRunsTabsAndKeepsReport()
    {
        Config::self()->setBookLocations({m_data.path()});
        QSqlQuery history(QSqlDatabase::database());
        QVERIFY(history.prepare(QStringLiteral("INSERT INTO book_state_history(book_id, state_id, epub_file_hash, modified) "
                                               "VALUES(:bookId, :stateId, :hash, :modified)")));
        history.bindValue(QStringLiteral(":hash"), QStringLiteral("orphan-hash"));
        history.bindValue(QStringLiteral(":modified"), QStringLiteral("2026-01-01T00:00:00Z"));
        history.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-bulk-book-1"));
        history.bindValue(QStringLiteral(":stateId"), QStringLiteral("missing-bulk-state-1"));
        QVERIFY(history.exec());
        history.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-bulk-book-2"));
        history.bindValue(QStringLiteral(":stateId"), QStringLiteral("missing-bulk-state-2"));
        QVERIFY(history.exec());
        history.finish();
        QSqlQuery currentHistory(QSqlDatabase::database());
        QVERIFY(currentHistory.prepare(QStringLiteral("INSERT INTO book_current_state(book_id, state_id, text_content_hash, document_state_hash, epub_file_hash, modified) "
                                "VALUES(:bookId, :stateId, :textHash, :documentHash, :epubHash, :modified)")));
        currentHistory.bindValue(QStringLiteral(":textHash"), QStringLiteral("text-hash"));
        currentHistory.bindValue(QStringLiteral(":documentHash"), QStringLiteral("document-hash"));
        currentHistory.bindValue(QStringLiteral(":epubHash"), QStringLiteral("epub-hash"));
        currentHistory.bindValue(QStringLiteral(":modified"), QStringLiteral("2026-01-01T00:00:00Z"));
        currentHistory.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-bulk-current-1"));
        currentHistory.bindValue(QStringLiteral(":stateId"), QStringLiteral("missing-bulk-current-state-1"));
        QVERIFY(currentHistory.exec());
        currentHistory.bindValue(QStringLiteral(":bookId"), QStringLiteral("missing-bulk-current-2"));
        currentHistory.bindValue(QStringLiteral(":stateId"), QStringLiteral("missing-bulk-current-state-2"));
        QVERIFY(currentHistory.exec());
        currentHistory.finish();
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        quickWindow->resize(1700, 950);
        quickWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
        auto *checker = window->findChild<LibraryMaintenance *>();
        QVERIFY(checker);
        auto *home = visualItem(quickWindow->contentItem(), QStringLiteral("goHomeButton"));
        auto *menu = visualItem(quickWindow->contentItem(), QStringLiteral("maintenancePageButton"));
        auto *imports = visualItem(quickWindow->contentItem(), QStringLiteral("bookImportPageButton"));
        QVERIFY(home && menu && imports);
        QTRY_VERIFY(menu->isEnabled());
        QVERIFY(menu->mapToScene(QPointF()).y() > home->mapToScene(QPointF()).y());
        QVERIFY(imports->mapToScene(QPointF()).y() > menu->mapToScene(QPointF()).y());
        auto click = [quickWindow](QQuickItem *item) {
            QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        click(menu);
        QQuickItem *page = nullptr;
        QTRY_VERIFY(page = visibleItem(quickWindow->contentItem(), QStringLiteral("maintenancePage")));
        auto *stack = window->property("pageStack").value<QObject *>();
        QVERIFY(stack);
        QCOMPARE(stack->property("currentItem").value<QObject *>(), page);
        QVERIFY(!home->property("checked").toBool());
        QVERIFY(menu->property("checked").toBool());
        QTRY_VERIFY(!checker->running() && !checker->report().isEmpty());
        auto *deleteAllHistory = visualItem(page, QStringLiteral("deleteAllOrphanedBookStateHistory"));
        QVERIFY(deleteAllHistory);
        QTRY_VERIFY(deleteAllHistory->isVisible());
        // Opening the database backfills two history rows for the seeded current states.
        QTRY_VERIFY2(deleteAllHistory->property("text").toString().contains(QStringLiteral("(6)")),
                     qPrintable(deleteAllHistory->property("text").toString()));
        QVERIFY(QMetaObject::invokeMethod(page, "requestDeleteAllOrphanedStates"));
        QSqlQuery historyCount(QSqlDatabase::database());
        QVERIFY(historyCount.exec(QStringLiteral("SELECT COUNT(*) FROM book_state_history WHERE book_id LIKE 'missing-bulk-book-%'")));
        QVERIFY(historyCount.next());
        QCOMPARE(historyCount.value(0).toInt(), 2);
        QVERIFY(historyCount.exec(QStringLiteral("SELECT COUNT(*) FROM book_current_state WHERE book_id LIKE 'missing-bulk-current-%'")));
        QVERIFY(historyCount.next());
        QCOMPARE(historyCount.value(0).toInt(), 2);
        auto *tabs = visualItem(page, QStringLiteral("maintenanceTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->property("count").toInt(), 5);
        qreal previousX = -1;
        int index = 0;
        for (const auto &category : {"database", "structure", "annotations", "references", "cache"}) {
            auto *tab = visualItem(page, QStringLiteral("maintenance-") + QString::fromLatin1(category));
            QVERIFY(tab);
            QTRY_VERIFY(tab->isEnabled() && tab->isVisible());
            QVERIFY(tab->mapToScene(QPointF()).x() > previousX);
            previousX = tab->mapToScene(QPointF()).x();
            click(tab);
            QTRY_COMPARE(checker->category(), QString::fromLatin1(category));
            QTRY_VERIFY(!checker->running());
            QCOMPARE(tabs->property("currentIndex").toInt(), index++);
            QVERIFY(!checker->report().value(QStringLiteral("cancelled")).toBool());
            if (QString::fromLatin1(category) == QStringLiteral("references")) {
                auto *stored = visualItem(page, QStringLiteral("checkStoredReferences"));
                auto *entire = visualItem(page, QStringLiteral("checkAllLibraryReferences"));
                QVERIFY(stored && entire && stored->isVisible() && entire->isVisible());
                QCOMPARE(checker->report().value(QStringLiteral("category")).toString(), QStringLiteral("references"));
                click(entire);
                QTRY_COMPARE(checker->category(), QStringLiteral("references-all"));
                QTRY_VERIFY(!checker->running());
                QCOMPARE(tabs->property("currentIndex").toInt(), 3);
                QVERIFY(stored->isVisible() && entire->isVisible());
                click(stored);
                QTRY_COMPARE(checker->category(), QStringLiteral("references"));
                QTRY_VERIFY(!checker->running());
                QCOMPARE(tabs->property("currentIndex").toInt(), 3);
            }
        }
        const auto report = checker->report();
        auto *filter = visualItem(page, QStringLiteral("maintenanceStatusFilter"));
        QVERIFY(filter);
        QVERIFY(filter->setProperty("currentIndex", 2));
        click(home);
        QTRY_VERIFY(!page->isVisible());
        click(menu);
        QTRY_VERIFY(page->isVisible());
        QCOMPARE(stack->property("currentItem").value<QObject *>(), page);
        QCOMPARE(checker->report(), report);
        QCOMPARE(filter->property("currentIndex").toInt(), 2);
        QCOMPARE(tabs->property("currentIndex").toInt(), 4);

        // A blocked editor opens the same page directly on its cache tab.
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("blocked.epub")), QStringLiteral("blocked"));
        BookDatabase::self().addEntry(book);
        QVERIFY(!cacheCopy(book.filename).isEmpty());
        auto *sessions = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        QVERIFY(sessions);
        click(home);
        QTRY_VERIFY(!page->isVisible());
        QVERIFY(sessions->createOrReuseEditorSession(book.uniqueIdentifier, book.filename).value(QStringLiteral("blocked")).toBool());
        auto *notice = window->findChild<QObject *>(QStringLiteral("editorCacheNotice"));
        QVERIFY(notice);
        QTRY_VERIFY(notice->property("visible").toBool());
        auto *review = visibleItem(quickWindow->contentItem(), QStringLiteral("reviewEditorCacheCopies"));
        QVERIFY(review);
        QVERIFY(QMetaObject::invokeMethod(review, "clicked"));
        QTRY_VERIFY(page->isVisible());
        QTRY_VERIFY(!checker->running());
        QCOMPARE(checker->category(), QStringLiteral("cache"));
        QVERIFY(!notice->property("visible").toBool());
        QCOMPARE(filter->property("currentIndex").toInt(), 0);
    }

};
QTEST_MAIN(LibraryMaintenanceTest)
#include "librarymaintenancetest.moc"
