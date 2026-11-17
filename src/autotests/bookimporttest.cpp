// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookdatabase.h"
#include "booklistmodel.h"
#include "config.h"
#include "epubcontainer.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtQml/QQmlExtensionPlugin>
#include <functional>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>
Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QString epubFile(const QString &path,
                 const QString &id,
                 const QStringList &aliases = {},
                 const QByteArray &body = QByteArrayLiteral("<p id=\"note\">Text</p>"),
                 const QMap<QString, QByteArray> &extraFiles = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QByteArray identifiers = "<dc:identifier id=\"book-id\">" + id.toHtmlEscaped().toUtf8() + "</dc:identifier>";
    for (const auto &alias : aliases)
        identifiers += "<dc:identifier>" + alias.toHtmlEscaped().toUtf8() + "</dc:identifier>";
    const QByteArray opf = QByteArrayLiteral("<package version=\"3.0\" unique-identifier=\"book-id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">")
        + identifiers + "<dc:title>" + QFileInfo(path).completeBaseName().toHtmlEscaped().toUtf8()
        + QByteArrayLiteral("</dc:title></metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
                            "<spine><itemref idref=\"chapter\"/></spine></package>");
    KZip zip(path);
    bool success = zip.open(QIODevice::WriteOnly) && zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"))
        && zip.writeFile(QStringLiteral("META-INF/container.xml"),
                         QByteArrayLiteral("<container><rootfiles><rootfile full-path=\"content.opf\" "
                                           "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"))
        && zip.writeFile(QStringLiteral("content.opf"), opf)
        && zip.writeFile(QStringLiteral("chapter.xhtml"),
                         QByteArray(QByteArrayLiteral("<html><head><title>Test</title></head><body>") + body + QByteArrayLiteral("</body></html>")));
    for (auto it = extraFiles.cbegin(); it != extraFiles.cend(); ++it)
        success = success && zip.writeFile(it.key(), it.value());
    success = success && zip.close();
    if (!success)
        QTest::qFail("Unable to create EPUB fixture", __FILE__, __LINE__);
    return path;
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

void discover(ContentList *content, const QString &path)
{
    QVERIFY(QMetaObject::invokeMethod(content, "fileFound", Q_ARG(QString, path), Q_ARG(QVariantMap, QVariantMap{})));
}

QQuickItem *visualItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name)
        return root;
    for (auto *child : root->childItems())
        if (auto *found = visualItem(child, name))
            return found;
    return nullptr;
}
}

class BookImportTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_data;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_data.isValid());
        qputenv("XDG_DATA_HOME", QFile::encodeName(m_data.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_data.path()));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_data.path()));
        QCoreApplication::setApplicationName(QStringLiteral("arianna-bookimporttest"));
        KLocalizedString::setApplicationDomain("arianna");
        BookDatabase::self().loadEntries();
    }
    void init()
    {
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        for (const auto &table : {"books", "annotations", "references", "book_current_state", "book_state_history"})
            QVERIFY(query.exec(QStringLiteral("DELETE FROM \"%1\"").arg(QString::fromLatin1(table))));
        query.finish();
        connection.close();
        Config::self()->setBookLocations({});
    }

    void topicDetailsKeepsDraftAndUsesPersistedMetadata()
    {
        QTemporaryDir dir;
        const auto path = epubFile(dir.filePath(QStringLiteral("el-shaddai.epub")), QStringLiteral("topic-draft"));
        const QStringList oldTopics{QStringLiteral("Christ"),
                                    QStringLiteral("Christianity"),
                                    QStringLiteral("Jesus"),
                                    QStringLiteral("God"),
                                    QStringLiteral("El Shaddai")};
        {
            EPubContainer epub(nullptr);
            QVERIFY(epub.openFile(path));
            QVERIFY(epub.updateSubjects(oldTopics).success);
        }
        BookListModel model;
        const auto original = model.addBookFromFile(path, false);
        QVERIFY(!original.filename.isEmpty());
        QCOMPARE(original.genres, oldTopics);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testLibrary"), &model);
        engine.rootContext()->setContextProperty(QStringLiteral("testBook"), QVariant::fromValue(original));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                visible: true; width: 1000; height: 900
                property var bookListModel: testLibrary
                pageStack.initialPage: BookDetailsPage {
                    objectName: "topicDetails"
                    metadata: testBook
                    bookListModel: testLibrary
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *details = window->findChild<QObject *>(QStringLiteral("topicDetails"));
        QVERIFY(details);
        const auto evaluate = [](QObject *object, const QString &code) {
            QQmlExpression expression(qmlContext(object), object, code);
            const auto result = expression.evaluate();
            if (expression.hasError())
                QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
            return result;
        };
        const std::function<QObject *(QObject *)> findField = [&](QObject *node) -> QObject * {
            if (node->property("label").toString() == i18n("Topic Tree String"))
                return node;
            for (auto *child : node->children())
                if (auto *found = findField(child))
                    return found;
            return nullptr;
        };
        auto *field = findField(details);
        QVERIFY(field);
        QCOMPARE(field->property("text").toString(), oldTopics.join(QStringLiteral(", ")));
        const QString requested = QStringLiteral("Spirituality/Christianity");
        field->setProperty("text", requested);
        QVERIFY(QMetaObject::invokeMethod(field, "textEdited"));
        // Reader progress can update the entry while the editor still has focus.
        model.setBookData(path, QStringLiteral("currentProgress"), QStringLiteral("42"));
        QCOMPARE(field->property("text").toString(), requested);
        QCOMPARE(details->property("topicDraft").toString(), requested);
        evaluate(details, QStringLiteral("saveTopics()"));
        QCOMPARE(evaluate(details, QStringLiteral("topicsText()")).toString(), requested);
        QCOMPARE(field->property("text").toString(), requested);
        QCOMPARE(BookDatabase::self().loadEntry(path)->genres, QStringList({QStringLiteral("Spirituality"), requested}));
        {
            EPubContainer updated(nullptr);
            QVERIFY(updated.openFile(path));
            QCOMPARE(updated.metadata(QStringLiteral("subject")), QStringList({QStringLiteral("Spirituality"), requested}));
        }
        // Deliberately reopen with the delegate's stale pre-save value.
        window.reset();
        window.reset(component.create());
        QVERIFY(window);
        details = window->findChild<QObject *>(QStringLiteral("topicDetails"));
        QVERIFY(details);
        field = findField(details);
        QVERIFY(field);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(), requested, 1000);
        // A failed save must keep the draft visible and report the failure.
        QVERIFY(QFile::remove(path));
        field->setProperty("text", QStringLiteral("Spirituality/History"));
        QVERIFY(QMetaObject::invokeMethod(field, "textEdited"));
        evaluate(details, QStringLiteral("saveTopics()"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("Spirituality/History"));
        QVERIFY(!details->property("topicSaveError").toString().isEmpty());
        QCOMPARE(BookDatabase::self().loadEntry(path)->genres, QStringList({QStringLiteral("Spirituality"), requested}));
    }

    void singleImportRejectsRegisteredIds_data()
    {
        QTest::addColumn<QString>("storedId");
        QTest::addColumn<QString>("incomingId");
        QTest::addColumn<QStringList>("storedAliases");
        QTest::addColumn<QStringList>("incomingAliases");
        QTest::newRow("same-id-different-contents") << QStringLiteral("shared") << QStringLiteral("shared") << QStringList{} << QStringList{};
        QTest::newRow("uuid") << QStringLiteral("urn:uuid:4F03F48B-42BC-46A4-A1CA-6D8314D96D25") << QStringLiteral("{4f03f48b-42bc-46a4-a1ca-6d8314d96d25}")
                              << QStringList{} << QStringList{};
        QTest::newRow("registered-alias") << QStringLiteral("original") << QStringLiteral("shared") << QStringList{QStringLiteral("shared")} << QStringList{};
        QTest::newRow("incoming-alias") << QStringLiteral("shared") << QStringLiteral("new") << QStringList{} << QStringList{QStringLiteral("shared")};
        QTest::newRow("shared-secondary-id") << QStringLiteral("original") << QStringLiteral("new") << QStringList{QStringLiteral("shared")}
                                             << QStringList{QStringLiteral("shared")};
    }
    void singleImportRejectsRegisteredIds()
    {
        QFETCH(QString, storedId);
        QFETCH(QString, incomingId);
        QFETCH(QStringList, storedAliases);
        QFETCH(QStringList, incomingAliases);
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), storedId, storedAliases);
        const auto incoming = epubFile(dir.filePath(QStringLiteral("incoming.epub")), incomingId, incomingAliases);
        const auto originalHash = digest(original), incomingHash = digest(incoming);
        BookListModel firstModel;
        QVERIFY(!firstModel.addBookFromFile(original, false).filename.isEmpty());
        BookDatabase::self().saveAnnotation(
            storedId,
            {{QStringLiteral("annotationId"), QStringLiteral("saved-note")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        QCOMPARE(BookDatabase::self().loadAnnotations(storedId).size(), 1);
        // A separate, unloaded model must still check the authoritative database.
        BookListModel importer;
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        QVERIFY(importer.addBookFromFile(QUrl::fromLocalFile(incoming).toString(), false).filename.isEmpty());
        QCOMPARE(importer.count(), 0);
        QCOMPARE(rejected.size(), 1);
        const auto notice = rejected.first().first().toMap();
        QCOMPARE(notice.value(QStringLiteral("status")).toString(), QStringLiteral("duplicate-book-id"));
        QCOMPARE(notice.value(QStringLiteral("fileName")).toString(), incoming);
        QCOMPARE(notice.value(QStringLiteral("existingFile")).toString(), original);
        QVERIFY(!notice.value(QStringLiteral("bookId")).toString().isEmpty());
        QVERIFY(notice.value(QStringLiteral("message")).toString().contains(original));
        QVERIFY(notice.value(QStringLiteral("message")).toString().contains(incoming));
        QCOMPARE(BookDatabase::self().loadEntries().size(), 1);
        QCOMPARE(BookDatabase::self().loadAnnotations(storedId).size(), 1);
        QCOMPARE(digest(original), originalHash);
        QCOMPARE(digest(incoming), incomingHash);
    }

    void pdfAndEpubCannotClaimTheSameId_data()
    {
        QTest::addColumn<bool>("incomingPdf");
        QTest::newRow("reject-epub") << false;
        QTest::newRow("reject-pdf") << true;
    }
    void pdfAndEpubCannotClaimTheSameId()
    {
        QFETCH(bool, incomingPdf);
        QTemporaryDir dir;
        const auto pdf = dir.filePath(QStringLiteral("book.pdf"));
        PoDoFo::PdfMemDocument document;
        document.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        document.Save(pdf.toStdString());
        BookListModel setup;
        const auto registeredPdf = setup.addBookFromFile(pdf, false);
        QVERIFY(!registeredPdf.uniqueIdentifier.isEmpty());
        const auto epub = epubFile(dir.filePath(QStringLiteral("book.epub")), registeredPdf.uniqueIdentifier);
        if (incomingPdf) {
            QVERIFY(BookDatabase::self().removeEntry(registeredPdf));
            QVERIFY(!setup.addBookFromFile(epub, false).filename.isEmpty());
        }
        const auto pdfHash = digest(pdf), epubHash = digest(epub);
        BookListModel importer;
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        QVERIFY(importer.addBookFromFile(incomingPdf ? pdf : epub, false).filename.isEmpty());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected.first().first().toMap().value(QStringLiteral("bookId")).toString(), registeredPdf.uniqueIdentifier);
        QCOMPARE(BookDatabase::self().loadEntries().size(), 1);
        QCOMPARE(digest(pdf), pdfHash);
        QCOMPARE(digest(epub), epubHash);
    }

    void unavailableRegisteredFileStillReservesItsId()
    {
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), QStringLiteral("shared"));
        const auto incoming = epubFile(dir.filePath(QStringLiteral("incoming.epub")), QStringLiteral("shared"));
        BookListModel model;
        QVERIFY(!model.addBookFromFile(original, false).filename.isEmpty());
        QVERIFY(QFile::remove(original));
        QVERIFY(model.addBookFromFile(incoming, false).filename.isEmpty());
        QCOMPARE(BookDatabase::self().loadEntries().size(), 1);
        QCOMPARE(model.count(), 1);
    }

    void knownPathKeepsStoredMetadataAndEmptyIdsDoNotConflict()
    {
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), QStringLiteral("shared"));
        BookListModel first;
        auto entry = first.addBookFromFile(original, false);
        entry.currentProgress = 42;
        entry.currentLocation = QStringLiteral("keep-location");
        BookDatabase::self().updateEntry(entry);
        BookListModel importer;
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        const auto reopened = importer.addBookFromFile(original, false);
        QCOMPARE(reopened.currentProgress, 42);
        QCOMPARE(reopened.currentLocation, QStringLiteral("keep-location"));
        QCOMPARE(importer.addBookFromFile(original, false).filename, original);
        const auto one = epubFile(dir.filePath(QStringLiteral("no-id-one.epub")), {});
        const auto two = epubFile(dir.filePath(QStringLiteral("no-id-two.epub")), {});
        QVERIFY(!importer.addBookFromFile(one, false).filename.isEmpty());
        QVERIFY(!importer.addBookFromFile(two, false).filename.isEmpty());
        QCOMPARE(BookDatabase::self().loadEntries().size(), 3);
        QCOMPARE(importer.count(), 3);
        QVERIFY(rejected.isEmpty());
    }

    void batchRejectsExistingAndEarlierBatchIds()
    {
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), QStringLiteral("existing"));
        const auto conflict = epubFile(dir.filePath(QStringLiteral("conflict.epub")), QStringLiteral("existing"));
        const auto first = epubFile(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("new-id"));
        const auto second = epubFile(dir.filePath(QStringLiteral("second.epub")), QStringLiteral("new-id"));
        const auto other = epubFile(dir.filePath(QStringLiteral("other.epub")), QStringLiteral("other-id"));
        BookListModel importer;
        QVERIFY(!importer.addBookFromFile(original, false).filename.isEmpty());
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        const auto result = importer.importBookFiles({conflict, first, second, QUrl::fromLocalFile(other).toString(), original}, false);
        QCOMPARE(result.value(QStringLiteral("scanned")).toInt(), 5);
        QCOMPARE(result.value(QStringLiteral("imported")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("duplicates")).toInt(), 3);
        QCOMPARE(result.value(QStringLiteral("duplicateBookIds")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("failed")).toInt(), 0);
        QCOMPARE(result.value(QStringLiteral("rejections")).toList().size(), 2);
        QCOMPARE(rejected.size(), 2);
        QCOMPARE(rejected.last().first().toMap().value(QStringLiteral("existingFile")).toString(), first);
        QCOMPARE(importer.count(), 3);
        QCOMPARE(BookDatabase::self().loadEntries().size(), 3);
    }

    void calibreUsesTheSameGuard()
    {
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("registered.epub")), QStringLiteral("existing"));
        const auto folder = dir.filePath(QStringLiteral("Calibre"));
        epubFile(folder + QStringLiteral("/Author/A (1)/a.epub"), QStringLiteral("existing"));
        epubFile(folder + QStringLiteral("/Author/B (2)/b.epub"), QStringLiteral("existing"));
        const auto first = epubFile(folder + QStringLiteral("/Author/C (3)/c.epub"), QStringLiteral("new-id"));
        epubFile(folder + QStringLiteral("/Author/D (4)/d.epub"), QStringLiteral("new-id"));
        epubFile(folder + QStringLiteral("/Author/E (5)/e.epub"), QStringLiteral("other-id"));
        BookListModel importer;
        importer.componentComplete();
        QTRY_VERIFY(importer.cacheLoaded());
        QVERIFY(!importer.addBookFromFile(original, false).filename.isEmpty());
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        const auto result = importer.importCalibreLibrary(folder, false);
        QCOMPARE(result.value(QStringLiteral("scanned")).toInt(), 5);
        QCOMPARE(result.value(QStringLiteral("imported")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("duplicates")).toInt(), 3);
        QCOMPARE(result.value(QStringLiteral("duplicateBookIds")).toInt(), 3);
        QCOMPARE(result.value(QStringLiteral("failed")).toInt(), 0);
        QCOMPARE(rejected.size(), 3);
        QCOMPARE(rejected.last().first().toMap().value(QStringLiteral("existingFile")).toString(), first);
        QCOMPARE(BookDatabase::self().loadEntries().size(), 3);
        QCOMPARE(importer.importCalibreLibrary(folder, false).value(QStringLiteral("imported")).toInt(), 0);
        QCOMPARE(BookDatabase::self().loadEntries().size(), 3);
    }

    void discoveredContentUsesTheSameGuard()
    {
        QTemporaryDir dir;
        const auto first = epubFile(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("shared"));
        const auto second = epubFile(dir.filePath(QStringLiteral("second.epub")), QStringLiteral("shared"));
        const auto other = epubFile(dir.filePath(QStringLiteral("other.epub")), QStringLiteral("other"));
        ContentList content;
        BookListModel importer;
        importer.setContentModel(&content);
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        discover(&content, first);
        discover(&content, second);
        discover(&content, other);
        QCOMPARE(importer.count(), 2);
        QCOMPARE(BookDatabase::self().loadEntries().size(), 2);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected.first().first().toMap().value(QStringLiteral("existingFile")).toString(), first);
    }

    void databaseFailureDoesNotAddToModel()
    {
        QTemporaryDir dir;
        const auto path = epubFile(dir.filePath(QStringLiteral("blocked.epub")), QStringLiteral("new"));
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER block_import BEFORE INSERT ON books BEGIN SELECT RAISE(ABORT, 'test'); END")));
        query.finish();
        BookListModel importer;
        QSignalSpy rejected(&importer, &BookListModel::importRejected);
        const auto entry = importer.addBookFromFile(path, false);
        connection = QSqlDatabase::database();
        QSqlQuery cleanup(connection);
        QVERIFY(cleanup.exec(QStringLiteral("DROP TRIGGER block_import")));
        cleanup.finish();
        QVERIFY(entry.filename.isEmpty());
        QCOMPARE(importer.count(), 0);
        QVERIFY(BookDatabase::self().loadEntries().isEmpty());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected.first().first().toMap().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    }

    void identifierChangeChecksAnchorsInTheIncomingEpub_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("allowed");
        for (const auto &kind : {"none", "note-elsewhere", "source-elsewhere", "target-elsewhere", "target-fragment-elsewhere", "unrelated-id"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << true;
        for (const auto &kind :
             {"note-here", "note-range", "note-cfi", "source-here", "target-here", "target-document", "secondary-id", "malformed", "signed", "encrypted"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << false;
    }
    void identifierChangeChecksAnchorsInTheIncomingEpub()
    {
        QFETCH(QString, kind);
        QFETCH(bool, allowed);
        QTemporaryDir dir;
        QStringList aliases;
        if (kind == QStringLiteral("secondary-id"))
            aliases.append(QStringLiteral("shared-alias"));
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), QStringLiteral("shared"), aliases);
        QByteArray body = QByteArrayLiteral("<p id=\"note\">Text</p>");
        if (kind == QStringLiteral("note-range"))
            body = QByteArrayLiteral("<span id=\"uuid_note_begin\"/><p>Text</p><span id=\"uuid_note_end\"/>");
        else if (kind == QStringLiteral("malformed"))
            body = QByteArrayLiteral("<p>Not closed");
        QMap<QString, QByteArray> extras;
        if (kind == QStringLiteral("signed"))
            extras.insert(QStringLiteral("META-INF/signatures.xml"), QByteArrayLiteral("<signatures/>"));
        if (kind == QStringLiteral("encrypted"))
            extras.insert(QStringLiteral("META-INF/encryption.xml"), QByteArrayLiteral("<encryption/>"));
        const auto incoming = epubFile(dir.filePath(QStringLiteral("incoming.epub")), QStringLiteral("shared"), aliases, body, extras);
        BookListModel model;
        QVERIFY(!model.addBookFromFile(original, false).filename.isEmpty());
        auto &db = BookDatabase::self();
        if (kind.startsWith(QStringLiteral("note-")) || kind == QStringLiteral("secondary-id") || kind == QStringLiteral("unrelated-id")) {
            const auto owner = kind == QStringLiteral("secondary-id") ? QStringLiteral("shared-alias")
                : kind == QStringLiteral("unrelated-id")              ? QStringLiteral("other-book")
                                                                      : QStringLiteral("shared");
            QVariantMap note{{QStringLiteral("annotationId"), QStringLiteral("saved-note")},
                             {QStringLiteral("anchorId"), kind == QStringLiteral("note-elsewhere") ? QStringLiteral("missing") : QStringLiteral("note")}};
            if (kind == QStringLiteral("note-cfi")) {
                note.remove(QStringLiteral("anchorId"));
                note.insert(QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2/1:0)"));
            }
            db.saveAnnotation(owner, note);
        } else if (kind.startsWith(QStringLiteral("source-")) || kind.startsWith(QStringLiteral("target-"))) {
            const bool source = kind.startsWith(QStringLiteral("source-"));
            const auto location = kind == QStringLiteral("target-document") ? QStringLiteral("chapter.xhtml")
                : kind == QStringLiteral("target-fragment-elsewhere")       ? QStringLiteral("#missing")
                : kind == QStringLiteral("target-elsewhere")                ? QStringLiteral("chapter.xhtml#missing")
                                                                            : QStringLiteral("chapter.xhtml#note");
            db.saveReference(
                {{QStringLiteral("sourceBookId"), source ? QStringLiteral("shared") : QStringLiteral("other")},
                 {QStringLiteral("sourceAnchorId"), kind == QStringLiteral("source-elsewhere") ? QStringLiteral("missing") : QStringLiteral("note")},
                 {QStringLiteral("targetBookId"), source ? QStringLiteral("other") : QStringLiteral("shared")},
                 {QStringLiteral("targetLocation"), location}});
        }
        const auto notes = db.loadAnnotations(QStringLiteral("shared"));
        const auto references = db.loadAllReferences();
        const auto originalHash = digest(original), incomingHash = digest(incoming);
        QSignalSpy rejected(&model, &BookListModel::importRejected);
        QVERIFY(model.addBookFromFile(incoming, false).filename.isEmpty());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected.first().first().toMap().value(QStringLiteral("canChangeIdentifier")).toBool(), allowed);
        const auto result = model.changeRejectedBookIdentifier(incoming);
        QCOMPARE(result.value(QStringLiteral("success")).toBool(), allowed);
        QCOMPARE(db.loadEntries().size(), allowed ? 2 : 1);
        QCOMPARE(digest(original), originalHash);
        QCOMPARE(db.loadAnnotations(QStringLiteral("shared")), notes);
        QCOMPARE(db.loadAllReferences(), references);
        if (allowed) {
            const auto entry = db.loadEntry(incoming);
            QVERIFY(entry);
            QVERIFY(entry->uniqueIdentifier != QStringLiteral("shared"));
            QVERIFY(!QUuid(entry->uniqueIdentifier).isNull());
            EPubContainer epub(nullptr);
            QVERIFY(epub.openFile(incoming));
            QCOMPARE(epub.metadata(QStringLiteral("unique-identifier")).value(0), entry->uniqueIdentifier);
            QVERIFY(epub.readData(QStringLiteral("chapter.xhtml")).contains(body));
            QCOMPARE(model.count(), 2);
            QVERIFY(!model.changeRejectedBookIdentifier(incoming).value(QStringLiteral("success")).toBool());
        } else {
            QCOMPARE(digest(incoming), incomingHash);
        }
    }

    void identifierChangeUpdatesConflictingAliases_data()
    {
        QTest::addColumn<bool>("secondaryOnly");
        QTest::newRow("primary-and-secondary") << false;
        QTest::newRow("only-secondary") << true;
    }
    void identifierChangeUpdatesConflictingAliases()
    {
        QFETCH(bool, secondaryOnly);
        QTemporaryDir dir;
        const auto isbn = QStringLiteral("9780131103627");
        const auto alias = QStringLiteral("4f32d7af-9c18-4b66-a032-c613a7a03209");
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), isbn, {alias});
        const auto incoming = dir.filePath(QStringLiteral("incoming.epub"));
        const auto primary = secondaryOnly ? QStringLiteral("incoming-primary") : isbn;
        const auto incomingAlias = QStringLiteral("URN:UUID:") + alias.toUpper();
        const auto opf = QStringLiteral(
                             "<opf:package xmlns:opf=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"primary\">"
                             "<opf:metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                             "<dc:identifier id=\"primary\" opf:scheme=\"ISBN\">%1</dc:identifier>"
                             "<opf:meta refines=\"#primary\" property=\"identifier-type\" scheme=\"onix:codelist5\">15</opf:meta>"
                             "<dc:identifier id=\"secondary\">%2</dc:identifier><dc:identifier>retained-alias</dc:identifier>"
                             "<dc:title>Alias test</dc:title></opf:metadata><opf:manifest>"
                             "<opf:item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
                             "<opf:item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
                             "</opf:manifest><opf:spine toc=\"ncx\"><opf:itemref idref=\"chapter\"/></opf:spine></opf:package>")
                             .arg(primary, incomingAlias)
                             .toUtf8();
        const auto ncx = QStringLiteral(
                             "<ncx:ncx xmlns:ncx=\"http://www.daisy.org/z3986/2005/ncx/\"><ncx:head><ncx:meta name=\"dtb:uid\" content=\"%1\"/>"
                             "</ncx:head><ncx:navMap/></ncx:ncx>")
                             .arg(primary)
                             .toUtf8();
        const auto chapter = QByteArrayLiteral("<html><body><p>Unchanged text</p></body></html>");
        {
            KZip zip(incoming);
            QVERIFY(zip.open(QIODevice::WriteOnly));
            QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
            QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
                                  QByteArrayLiteral("<container><rootfiles><rootfile full-path=\"content.opf\" "
                                                    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>")));
            QVERIFY(zip.writeFile(QStringLiteral("content.opf"), opf));
            QVERIFY(zip.writeFile(QStringLiteral("toc.ncx"), ncx));
            QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), chapter));
            QVERIFY(zip.close());
        }
        BookListModel model;
        QVERIFY(!model.addBookFromFile(original, false).filename.isEmpty());
        QVERIFY(model.addBookFromFile(incoming, false).filename.isEmpty());
        const auto result = model.changeRejectedBookIdentifier(incoming);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        const auto newId = result.value(QStringLiteral("bookId")).toString();
        const auto expectedPrimary = secondaryOnly ? primary : newId;
        const auto entry = BookDatabase::self().loadEntry(incoming);
        QVERIFY(entry);
        QCOMPARE(entry->uniqueIdentifier, expectedPrimary);
        QStringList ids;
        for (const auto &id : entry->identifier.split(QLatin1Char(',')))
            ids.append(id.trimmed());
        QVERIFY(ids.contains(newId));
        QVERIFY(ids.contains(QStringLiteral("retained-alias")));
        QVERIFY(!ids.contains(incomingAlias));
        EPubContainer epub(nullptr);
        QVERIFY(epub.openFile(incoming));
        QCOMPARE(epub.readData(QStringLiteral("chapter.xhtml")), chapter);
        QDomDocument document;
        QVERIFY(document.setContent(epub.readData(QStringLiteral("toc.ncx")), QDomDocument::ParseOption::UseNamespaceProcessing));
        QCOMPARE(document.elementsByTagName(QStringLiteral("meta")).at(0).toElement().attribute(QStringLiteral("content")), expectedPrimary);
        QVERIFY(document.setContent(epub.readData(QStringLiteral("content.opf")), QDomDocument::ParseOption::UseNamespaceProcessing));
        QCOMPARE(document.elementsByTagName(QStringLiteral("meta")).size(), secondaryOnly ? 1 : 0);
        const auto firstId = document.elementsByTagName(QStringLiteral("identifier")).at(0).toElement();
        QCOMPARE(firstId.hasAttributeNS(QStringLiteral("http://www.idpf.org/2007/opf"), QStringLiteral("scheme")), secondaryOnly);
    }

    void identifierChangeRechecksBeforeWriting_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto &change : {"note", "source", "target", "file", "database-failure"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void identifierChangeRechecksBeforeWriting()
    {
        QFETCH(QString, change);
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), QStringLiteral("shared"));
        const auto incoming = epubFile(dir.filePath(QStringLiteral("incoming.epub")), QStringLiteral("shared"));
        BookListModel model;
        QVERIFY(!model.addBookFromFile(original, false).filename.isEmpty());
        QSignalSpy rejected(&model, &BookListModel::importRejected);
        QVERIFY(model.addBookFromFile(incoming, false).filename.isEmpty());
        QVERIFY(rejected.first().first().toMap().value(QStringLiteral("canChangeIdentifier")).toBool());
        auto &db = BookDatabase::self();
        if (change == QStringLiteral("note"))
            db.saveAnnotation(QStringLiteral("shared"),
                              {{QStringLiteral("annotationId"), QStringLiteral("late")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
        else if (change == QStringLiteral("source") || change == QStringLiteral("target"))
            db.saveReference({{QStringLiteral("sourceBookId"), change == QStringLiteral("source") ? QStringLiteral("shared") : QStringLiteral("other")},
                              {QStringLiteral("sourceAnchorId"), QStringLiteral("note")},
                              {QStringLiteral("targetBookId"), change == QStringLiteral("target") ? QStringLiteral("shared") : QStringLiteral("other")},
                              {QStringLiteral("targetLocation"), QStringLiteral("chapter.xhtml#note")}});
        else if (change == QStringLiteral("file"))
            epubFile(incoming, QStringLiteral("shared"), {}, QByteArrayLiteral("<p>Changed contents</p>"));
        else {
            auto connection = QSqlDatabase::database();
            QSqlQuery query(connection);
            QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER block_id_import BEFORE INSERT ON books BEGIN SELECT RAISE(ABORT, 'test'); END")));
        }
        const auto originalHash = digest(original), incomingHash = digest(incoming);
        const auto result = model.changeRejectedBookIdentifier(incoming);
        if (change == QStringLiteral("database-failure")) {
            auto connection = QSqlDatabase::database();
            QSqlQuery query(connection);
            QVERIFY(query.exec(QStringLiteral("DROP TRIGGER block_id_import")));
        }
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(original), originalHash);
        QCOMPARE(digest(incoming), incomingHash);
        QCOMPARE(db.loadEntries().size(), 1);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2);
    }

    void identifierChangeButton_data()
    {
        QTest::addColumn<bool>("anchored");
        QTest::newRow("anchor-in-this-epub") << true;
        QTest::newRow("anchor-only-in-existing-edition") << false;
    }
    void identifierChangeButton()
    {
        QFETCH(bool, anchored);
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("original.epub")), QStringLiteral("shared"));
        const auto incoming = epubFile(dir.filePath(QStringLiteral("incoming.epub")),
                                       QStringLiteral("shared"),
                                       {},
                                       anchored ? QByteArrayLiteral("<p id=\"note\">Text</p>") : QByteArrayLiteral("<p id=\"other\">Different edition</p>"));
        {
            BookListModel setup;
            QVERIFY(!setup.addBookFromFile(original, false).filename.isEmpty());
        }
        BookDatabase::self().saveAnnotation(QStringLiteral("shared"),
                                            {{QStringLiteral("annotationId"), QStringLiteral("saved")}, {QStringLiteral("anchorId"), QStringLiteral("note")}});
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
        auto *model = window->findChild<BookListModel *>();
        QVERIFY(model);
        QTRY_VERIFY(model->cacheLoaded());
        QVERIFY(QMetaObject::invokeMethod(window.get(), "importBookEntry", Q_ARG(QVariant, incoming)));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("importRejectionDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = visualItem(quickWindow->contentItem(), QStringLiteral("changeRejectedBookIdentifier")));
        QCOMPARE(button->isVisible(), !anchored);
        if (!anchored) {
            QCOMPARE(button->property("fileName").toString(), incoming);
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QCOMPARE(BookDatabase::self().loadEntries().size(), 2);
            QCOMPARE(model->count(), 2);
        }
        QCOMPARE(BookDatabase::self().loadAnnotations(QStringLiteral("shared")).size(), 1);
    }

    void mainShowsAllRejectedFiles_data()
    {
        QTest::addColumn<QString>("route");
        for (const auto &route : {"single", "files", "baloo", "calibre"})
            QTest::newRow(route) << QString::fromLatin1(route);
    }
    void mainShowsAllRejectedFiles()
    {
        QFETCH(QString, route);
        QTemporaryDir dir;
        const auto original = epubFile(dir.filePath(QStringLiteral("registered.epub")), QStringLiteral("shared"));
        {
            BookListModel setup;
            QVERIFY(!setup.addBookFromFile(original, false).filename.isEmpty());
        }
        const auto folder = dir.filePath(QStringLiteral("incoming"));
        const auto first = epubFile(folder + QStringLiteral("/a.epub"), QStringLiteral("shared"));
        const auto second = epubFile(folder + QStringLiteral("/b.epub"), QStringLiteral("shared"));
        const auto other = epubFile(folder + QStringLiteral("/c.epub"), QStringLiteral("other"));
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
        auto *model = window->findChild<BookListModel *>();
        QVERIFY(model);
        QTRY_VERIFY(model->cacheLoaded());
        if (route == QStringLiteral("single")) {
            QVERIFY(QMetaObject::invokeMethod(window.get(), "importBookEntry", Q_ARG(QVariant, first)));
        } else if (route == QStringLiteral("files")) {
            QVERIFY(QMetaObject::invokeMethod(window.get(), "importBookFiles", Q_ARG(QVariant, QVariant::fromValue(QStringList{first, second, other}))));
        } else if (route == QStringLiteral("calibre")) {
            Config::self()->setCalibreLibraryFolder(folder);
            QVERIFY(QMetaObject::invokeMethod(window.get(), "startCalibreLibraryImport"));
        } else {
            auto *content = window->findChild<ContentList *>(QStringLiteral("balooImportModel"));
            QVERIFY(content);
            discover(content, first);
            discover(content, second);
            discover(content, other);
            QVERIFY(QMetaObject::invokeMethod(window.get(), "importBalooSearchResults"));
        }
        auto *dialog = window->findChild<QObject *>(QStringLiteral("importRejectionDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        const auto rows = dialog->property("rejections").value<QJSValue>().toVariant().toList();
        QCOMPARE(rows.size(), route == QStringLiteral("single") ? 1 : 2);
        for (const auto &value : rows) {
            const auto row = value.toMap();
            QCOMPARE(row.value(QStringLiteral("bookId")).toString(), QStringLiteral("shared"));
            QCOMPARE(row.value(QStringLiteral("existingFile")).toString(), original);
            QVERIFY(row.value(QStringLiteral("message")).toString().contains(row.value(QStringLiteral("fileName")).toString()));
        }
        QCOMPARE(BookDatabase::self().loadEntries().size(), route == QStringLiteral("single") ? 1 : 2);
        QCOMPARE(model->count(), route == QStringLiteral("single") ? 1 : 2);
        QVERIFY(QFileInfo::exists(first));
        QVERIFY(QFileInfo::exists(second));
    }

    void importPageNavigation()
    {
        QTemporaryDir dir;
        const auto balooBook = epubFile(dir.filePath(QStringLiteral("baloo.epub")), QStringLiteral("baloo-page-book"));
        const auto calibreFolder = dir.filePath(QStringLiteral("Calibre"));
        epubFile(calibreFolder + QStringLiteral("/calibre.epub"), QStringLiteral("calibre-page-book"));
        Config::self()->setCalibreLibraryFolder(calibreFolder);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        quickWindow->resize(1500, 900);
        quickWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
        auto *model = window->findChild<BookListModel *>();
        QVERIFY(model);
        QTRY_VERIFY(model->cacheLoaded());
        auto *menu = visualItem(quickWindow->contentItem(), QStringLiteral("bookImportPageButton"));
        auto *home = visualItem(quickWindow->contentItem(), QStringLiteral("goHomeButton"));
        QVERIFY(menu && home);
        auto click = [quickWindow](QQuickItem *item) {
            QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        QTRY_VERIFY(menu->isEnabled());
        click(menu);
        auto *stack = window->property("pageStack").value<QObject *>();
        QVERIFY(stack);
        QQuickItem *page = nullptr;
        QTRY_VERIFY((page = qobject_cast<QQuickItem *>(stack->property("currentItem").value<QObject *>())) && page->property("importPage").toBool());
        QTRY_VERIFY(page->isVisible());
        QVERIFY(menu->property("checked").toBool());
        QVERIFY(!home->property("checked").toBool());
        QCOMPARE(window->property("title").toString(), i18nc("@title:window", "%1 - Arianna", i18nc("@title:group", "Book Import")));
        auto *tabs = visualItem(page, QStringLiteral("bookImportTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->property("count").toInt(), 3);
        QCOMPARE(tabs->property("currentIndex").toInt(), 0);
        auto *filesButton = visualItem(page, QStringLiteral("importFilesButton"));
        QVERIFY(filesButton);
        QTRY_VERIFY(filesButton->isVisible() && filesButton->isEnabled());
        auto *fileDialog = window->findChild<QObject *>(QStringLiteral("addBooksDialog"));
        QVERIFY(fileDialog);
        click(filesButton);
        QTRY_VERIFY(fileDialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(fileDialog, "close"));
        QTRY_VERIFY(!fileDialog->property("visible").toBool());
        QCOMPARE(model->count(), 0);

        // Switching tabs never imports automatically; each action uses the
        // existing importer, and leaving/reopening the page retains its tab.
        auto *balooTab = visualItem(page, QStringLiteral("bookImport-baloo"));
        auto *calibreTab = visualItem(page, QStringLiteral("bookImport-calibre"));
        QVERIFY(balooTab && calibreTab);
        click(balooTab);
        QTRY_COMPARE(tabs->property("currentIndex").toInt(), 1);
        auto *content = window->findChild<ContentList *>(QStringLiteral("balooImportModel"));
        QVERIFY(content);
        discover(content, balooBook);
        auto *balooImport = visualItem(page, QStringLiteral("importBalooBooksButton"));
        QVERIFY(balooImport);
        QTRY_VERIFY(balooImport->isVisible() && balooImport->isEnabled());
        QCOMPARE(model->count(), 0);
        click(balooImport);
        QTRY_COMPARE(model->count(), 1);
        QCOMPARE(content->count(), 0);
        QVERIFY(!window->property("bookImportStatus").toString().isEmpty());
        QCOMPARE(stack->property("currentItem").value<QObject *>(), page);
        click(calibreTab);
        QTRY_COMPARE(tabs->property("currentIndex").toInt(), 2);
        auto *folder = visualItem(page, QStringLiteral("calibreImportFolder"));
        QVERIFY(folder);
        QCOMPARE(folder->property("text").toString(), calibreFolder);
        QCOMPARE(model->count(), 1);
        auto *calibreImport = visualItem(page, QStringLiteral("importCalibreBooksButton"));
        QVERIFY(calibreImport);
        QTRY_VERIFY(calibreImport->isVisible() && calibreImport->isEnabled());
        click(calibreImport);
        QTRY_COMPARE(model->count(), 2);
        QTRY_VERIFY(!window->property("calibreImportRunning").toBool());
        QCOMPARE(stack->property("currentItem").value<QObject *>(), page);
        click(home);
        QTRY_VERIFY(!page->isVisible());
        click(menu);
        QTRY_VERIFY(page->isVisible());
        QCOMPARE(stack->property("currentItem").value<QObject *>(), page);
        QCOMPARE(tabs->property("currentIndex").toInt(), 2);

        auto *maintenanceMenu = visualItem(quickWindow->contentItem(), QStringLiteral("maintenancePageButton"));
        QVERIFY(maintenanceMenu);
        click(maintenanceMenu);
        QQuickItem *maintenancePage = nullptr;
        QTRY_VERIFY((maintenancePage = qobject_cast<QQuickItem *>(stack->property("currentItem").value<QObject *>()))
                    && maintenancePage->property("maintenancePage").toBool());
        auto *maintenanceTabs = visualItem(maintenancePage, QStringLiteral("maintenanceTabs"));
        QVERIFY(maintenanceTabs);
        QCOMPARE(maintenanceTabs->property("count").toInt(), 5);
        click(menu);
        QTRY_VERIFY(page->isVisible());
        QCOMPARE(stack->property("currentItem").value<QObject *>(), page);
        QCOMPARE(tabs->property("currentIndex").toInt(), 2);
    }
};
QTEST_MAIN(BookImportTest)
#include "bookimporttest.moc"
