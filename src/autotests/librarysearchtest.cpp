// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookdatabase.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "okularpdfpageitem.h"
#include "categoryentriesmodel.h"
#include "epubcontainer.h"
#include "librarysearchmodel.h"
#include "pdfsearchprocess.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlPropertyMap>
#include <QTcpServer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <csignal>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
class TestScriptState : public QQmlPropertyMap
{
public:
    TestScriptState()
        : QQmlPropertyMap(this, nullptr)
    {
    }
};

class BookServerTestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit BookServerTestInterceptor(const QString &token) : m_token(token.toUtf8())
    {
        QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(this);
    }
    ~BookServerTestInterceptor() override
    {
        QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(nullptr);
    }
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (BookServerConfig::matchesBookServer(info.requestUrl()))
            info.setHttpHeader(QByteArrayLiteral("X-Arianna-Session-Token"), m_token);
    }
private:
    const QByteArray m_token;
};

QByteArray chapter(const QByteArray &body)
{
    return QByteArrayLiteral(
               "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter</title>"
               "<style>needle</style></head><body>\n")
        + body + QByteArrayLiteral("\n</body></html>");
}

BookEntry epub(const QString &path, const QString &title, const QList<QByteArray> &chapters)
{
    KZip zip(path);
    if (!zip.open(QIODevice::WriteOnly))
        qFatal("Cannot create test EPUB");
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    zip.writeFile(QStringLiteral("META-INF/container.xml"),
                  QByteArrayLiteral(
                      "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"OPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    QByteArray manifest, spine;
    for (int i = 0; i < chapters.size(); ++i) {
        const QByteArray id = QByteArray::number(i);
        manifest += "<item id=\"c" + id + "\" href=\"chapter" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>";
        spine += "<itemref idref=\"c" + id + "\"/>";
        zip.writeFile(QStringLiteral("OPS/chapter%1.xhtml").arg(i), chapters.at(i));
    }
    zip.writeFile(
        QStringLiteral("OPS/content.opf"),
        QByteArray(
            QByteArrayLiteral("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Book</dc:title></metadata><manifest>")
            + manifest + "</manifest><spine>" + spine + "</spine></package>"));
    if (!zip.close())
        qFatal("Cannot close test EPUB");
    BookEntry book;
    book.filename = path;
    book.title = title;
    book.author = QStringList{QStringLiteral("Test Author")};
    return book;
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

QVariant evaluate(QObject *object, const QString &code)
{
    QQmlExpression expression(qmlContext(object), object, code);
    auto value = expression.evaluate();
    if (expression.hasError())
        QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
    return value;
}

QQuickItem *visualItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name)
        return root;
    for (auto *child : root->childItems()) {
        if (auto *found = visualItem(child, name))
            return found;
    }
    return nullptr;
}
}

class LibrarySearchTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        BookDatabase::self().loadEntries();
        auto database = QSqlDatabase::database();
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DELETE FROM books")));
        query.finish();
        database.close();
    }

    void epubTextAndSpineOrder()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("test.epub")),
                               QStringLiteral("Test"),
                               {chapter(QByteArrayLiteral("<p>Needle <em>über</em>\n  zwei &amp; drei.</p><p>nee<!-- comment -->dle</p>"
                                                          "<script>needle</script><p hidden=\"hidden\">needle</p>")),
                                chapter(QByteArrayLiteral("<p>Third NEEDLE.</p>"))});
        const auto original = digest(book.filename);
        EPubContainer container(nullptr);
        QVERIFY(container.openFile(book.filename));
        QString error;
        const auto hits = container.searchText(QStringLiteral(" needle "), {}, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(hits.size(), 3);
        QVERIFY(hits[0].location.startsWith(QStringLiteral("epubcfi(/6/2!")));
        QVERIFY(hits[1].location.startsWith(QStringLiteral("epubcfi(/6/2!")));
        QVERIFY(hits[2].location.startsWith(QStringLiteral("epubcfi(/6/4!")));
        QCOMPARE(hits[0].text, QStringLiteral("Needle"));
        QCOMPARE(hits[1].text, QStringLiteral("needle"));
        QCOMPARE(hits[0].section, QStringLiteral("Chapter"));
        const auto phrase = container.searchText(QStringLiteral("ÜBER zwei & drei"));
        QCOMPARE(phrase.size(), 1);
        QCOMPARE(phrase[0].text, QStringLiteral("über zwei & drei"));
        QVERIFY(container.searchText(QStringLiteral("  \n ")).isEmpty());
        QVERIFY(container
                    .searchText(QStringLiteral("needle"),
                                [] {
                                    return true;
                                })
                    .isEmpty());
        QCOMPARE(digest(book.filename), original);
    }

    void epubMalformedChapterIsReported()
    {
        QTemporaryDir dir;
        const auto book = epub(dir.filePath(QStringLiteral("broken.epub")),
                               QStringLiteral("Broken"),
                               {QByteArrayLiteral("<html><body><p>needle"), chapter(QByteArrayLiteral("<p>needle</p>"))});
        EPubContainer container(nullptr);
        QVERIFY(container.openFile(book.filename));
        QString error;
        QCOMPARE(container.searchText(QStringLiteral("needle"), {}, &error).size(), 1);
        QVERIFY(!error.isEmpty());
    }

    void libraryProgressOrderPdfAndRestart()
    {
        QTemporaryDir dir;
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("z.epub")), QStringLiteral("Zebra"), {chapter("<p>needle needle</p>")}));
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("a.epub")), QStringLiteral("Alpha"), {chapter("<p>needle &amp; &lt;tag&gt;</p>")}));
        BookEntry pdf;
        pdf.filename = dir.filePath(QStringLiteral("b.pdf"));
        pdf.title = QStringLiteral("Beta");
        {
            QPdfWriter writer(pdf.filename);
            QPainter painter(&writer);
            painter.drawText(100, 300, QStringLiteral("needle in PDF"));
            writer.newPage();
            painter.drawText(100, 300, QStringLiteral("second needle"));
        }
        BookDatabase::self().addEntry(pdf);
        BookEntry missing;
        missing.filename = dir.filePath(QStringLiteral("missing.epub"));
        missing.title = QStringLiteral("Missing");
        BookDatabase::self().addEntry(missing);
        LibrarySearchModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        bool progressive = false;
        connect(&model, &LibrarySearchModel::changed, &model, [&] {
            if (model.running() && model.completed() == 1 && model.rowCount() > 0)
                progressive = true;
        });
        model.start(QStringLiteral("needle"));
        QCOMPARE(model.total(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(!model.running(), 15000);
        QVERIFY(progressive);
        QCOMPARE(model.completed(), 4);
        QCOMPARE(model.failedCount(), 1);
        QCOMPARE(model.matchCount(), 5);
        QCOMPARE(model.bookCount(), 3); // the unreadable book is not a matching book
        QCOMPARE(model.rowCount(), 9); // four book headings, five hits
        auto row = [&](int i) {
            return model.data(model.index(i), Qt::UserRole).toMap();
        };
        QCOMPARE(row(0).value(QStringLiteral("title")).toString(), QStringLiteral("Alpha"));
        QCOMPARE(row(0).value(QStringLiteral("author")).toString(), QStringLiteral("Test Author"));
        QVERIFY(row(1).value(QStringLiteral("markup")).toString().contains(QStringLiteral("&lt;tag&gt;")));
        QCOMPARE(row(2).value(QStringLiteral("title")).toString(), QStringLiteral("Beta"));
        QCOMPARE(QJsonDocument::fromJson(row(4).value(QStringLiteral("location")).toString().toUtf8()).object().value(QStringLiteral("page")).toInt(), 1);
        QCOMPARE(row(5).value(QStringLiteral("title")).toString(), QStringLiteral("Zebra"));
        QCOMPARE(row(8).value(QStringLiteral("title")).toString(), QStringLiteral("Missing"));
        for (const auto &[index, separator] : {std::pair{0, 0}, {2, 1}, {5, 1}, {8, 2}}) {
            QCOMPARE(row(index).value(QStringLiteral("separator")).toInt(), separator);
        }
        QCOMPARE(row(8).value(QStringLiteral("number")).toInt(), 4);
        model.start(QStringLiteral("not present"));
        QTRY_VERIFY(!model.running());
        QCOMPARE(model.matchCount(), 0);
        QCOMPARE(model.rowCount(), 1); // missing book remains visible as an error
        QCOMPARE(model.bookCount(), 0);
        model.start(QString());
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.total(), 0);
        QCOMPARE(model.bookCount(), 0);
        QVERIFY(model.query().isEmpty());
    }

    void bookGroupsCollapseIndependently()
    {
        QTemporaryDir dir;
        const auto alpha = epub(dir.filePath(QStringLiteral("a.epub")), QStringLiteral("Alpha"), {chapter("<p>needle needle needle</p>")});
        const auto beta = epub(dir.filePath(QStringLiteral("b.epub")), QStringLiteral("Beta"), {chapter("<p>needle needle needle needle</p>")});
        BookDatabase::self().addEntry(alpha);
        BookDatabase::self().addEntry(beta);
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("c.epub")), QStringLiteral("Gamma"), {chapter("<p>needle needle</p>")}));
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("d.epub")), QStringLiteral("Zeta"), {chapter("<p>needle</p>")}));
        LibrarySearchModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto row = [&](int index) {
            return model.data(model.index(index), Qt::UserRole).toMap();
        };
        const auto heading = [&](const QString &title) {
            for (int i = 0; i < model.rowCount(); ++i)
                if (row(i).value(QStringLiteral("title")).toString() == title)
                    return i;
            return -1;
        };
        bool openedDuringSearch = false;
        connect(&model, &LibrarySearchModel::changed, &model, [&] {
            if (!openedDuringSearch && model.running() && model.completed() == 1) {
                QCOMPARE(model.rowCount(), 1); // Three hits start collapsed.
                QVERIFY(row(0).value(QStringLiteral("collapsed")).toBool());
                model.toggleBook(0);
                openedDuringSearch = true;
            }
        });
        model.start(QStringLiteral("needle"));
        QTRY_VERIFY(!model.running());
        QVERIFY(openedDuringSearch);
        QCOMPARE(model.bookCount(), 4);
        QCOMPARE(model.matchCount(), 10);
        QCOMPARE(model.rowCount(), 10); // Alpha remains open as later books arrive.
        QVERIFY(!row(0).value(QStringLiteral("collapsed")).toBool());
        QCOMPARE(heading(QStringLiteral("Beta")), 4);
        model.toggleBook(4);
        QCOMPARE(model.rowCount(), 14);
        const auto betaHit = row(5);
        QCOMPARE(betaHit.value(QStringLiteral("filename")).toString(), beta.filename);
        QVERIFY(betaHit.value(QStringLiteral("location")).toString().startsWith(QStringLiteral("epubcfi(")));
        model.toggleBook(0);
        QCOMPARE(model.rowCount(), 11);
        QCOMPARE(heading(QStringLiteral("Beta")), 1);
        QCOMPARE(row(2), betaHit); // Collapsing an earlier book preserves this hit and its destination.
        model.toggleBook(1);
        QCOMPARE(model.rowCount(), 7);
        const int gamma = heading(QStringLiteral("Gamma"));
        QVERIFY(gamma >= 0);
        QVERIFY(!row(gamma).value(QStringLiteral("collapsed")).toBool());
        model.toggleBook(gamma); // One/two matches remain visible, including after unrelated toggles.
        model.toggleBook(-1);
        model.toggleBook(model.rowCount());
        model.toggleBook(gamma + 1); // A hit is not a heading.
        QCOMPARE(model.rowCount(), 7);
        QCOMPARE(model.matchCount(), 10);
        QCOMPARE(model.bookCount(), 4);
        model.toggleBook(0);
        QCOMPARE(model.rowCount(), 10);
        model.start(QStringLiteral("needle"));
        QTRY_VERIFY(!model.running());
        QCOMPARE(model.rowCount(), 7); // A new search restores the threshold's defaults.
        QCOMPARE(model.matchCount(), 10);
        QVERIFY(row(0).value(QStringLiteral("collapsed")).toBool());
        QVERIFY(row(1).value(QStringLiteral("collapsed")).toBool());
    }

    void cancellationAndDestruction()
    {
        QTemporaryDir dir;
        const auto book =
            epub(dir.filePath(QStringLiteral("large.epub")), QStringLiteral("Large"), {chapter(QByteArrayLiteral("<p>needle text</p>").repeated(200000))});
        BookDatabase::self().addEntry(book);
        LibrarySearchModel model;
        model.start(QStringLiteral("needle"));
        model.cancel();
        QVERIFY(model.cancelled());
        QTRY_VERIFY_WITH_TIMEOUT(!model.running(), 10000);
        QCOMPARE(model.completed(), 0);
        model.start(QStringLiteral("missing"));
        QVERIFY(!model.cancelled());
        QTRY_VERIFY_WITH_TIMEOUT(!model.running(), 10000);
        QCOMPARE(model.completed(), 1);
        QCOMPARE(model.matchCount(), 0);
        // Cancelling after one completed book retains that book's results.
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("Alpha"), {chapter("<p>needle</p>")}));
        connect(&model, &LibrarySearchModel::changed, &model, [&] {
            if (model.running() && model.completed() == 1)
                model.cancel();
        });
        model.start(QStringLiteral("needle"));
        QTRY_VERIFY_WITH_TIMEOUT(!model.running(), 10000);
        QVERIFY(model.cancelled());
        QCOMPARE(model.completed(), 1);
        QCOMPARE(model.matchCount(), 1);
        QCOMPARE(model.rowCount(), 2);

        auto pending = std::make_unique<LibrarySearchModel>();
        pending->start(QStringLiteral("needle"));
        pending.reset(); // joins safely even with queued results
    }

    void isolatedPdfFailures_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("expectedError");
        const auto crashed = i18n("Text extraction stopped unexpectedly for this PDF. The remaining books will still be searched.");
        const auto invalid = i18n("PDF search returned an invalid result.");
        QTest::newRow("native-segfault") << QStringLiteral("crash") << crashed;
        QTest::newRow("nonzero-exit") << QStringLiteral("exit") << crashed;
        QTest::newRow("hung-parser") << QStringLiteral("hang") << i18n("The time limit for searching this PDF was exceeded.");
        QTest::newRow("oversized-output") << QStringLiteral("flood") << i18n("The PDF search produced too much output. Try a more specific search text.");
        QTest::newRow("invalid-json") << QStringLiteral("invalid") << invalid;
        QTest::newRow("invalid-hit") << QStringLiteral("invalid-hit") << invalid;
    }

    void isolatedPdfFailures()
    {
        QFETCH(QString, mode);
        QFETCH(QString, expectedError);
        PdfSearchProcessOptions options;
        options.program = QCoreApplication::applicationFilePath();
        options.arguments = {QStringLiteral("--pdf-search-test-") + mode};
        options.timeoutMs = mode == QStringLiteral("hang") ? 200 : 5000;
        options.maximumOutputBytes = 4096;
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = searchPdfInProcess(QStringLiteral("unused.pdf"), QStringLiteral("needle"), {}, options);
        QVERIFY(!result.cancelled);
        QCOMPARE(result.error, expectedError);
        QVERIFY(result.hits.isEmpty());
        QVERIFY(elapsed.elapsed() < options.timeoutMs + 2000);
    }

    void isolatedPdfCancelStartFailureAndWarnings()
    {
        PdfSearchProcessOptions options;
        options.program = QCoreApplication::applicationFilePath();
        options.arguments = {QStringLiteral("--pdf-search-test-hang")};
        QElapsedTimer elapsed;
        elapsed.start();
        const auto cancelled = searchPdfInProcess(
            QStringLiteral("unused.pdf"),
            QStringLiteral("needle"),
            [&] {
                return elapsed.elapsed() > 100;
            },
            options);
        QVERIFY(cancelled.cancelled);
        QVERIFY(cancelled.error.isEmpty());
        QVERIFY(elapsed.elapsed() < 3000);
        options.arguments = {QStringLiteral("--pdf-search-test-warnings")};
        const auto warnings = searchPdfInProcess(QStringLiteral("unused.pdf"), QStringLiteral("needle"), {}, options);
        QVERIFY2(warnings.error.isEmpty(), qPrintable(warnings.error));
        QVERIFY(!warnings.cancelled);
        QTemporaryDir dir;
        options.program = dir.filePath(QStringLiteral("does-not-exist"));
        QVERIFY(!searchPdfInProcess(QStringLiteral("unused.pdf"), QStringLiteral("needle"), {}, options).error.isEmpty());
    }

    void unreadablePdfDoesNotStopLibrary()
    {
        QTemporaryDir dir;
        BookEntry broken;
        broken.title = QStringLiteral("Alpha broken PDF");
        broken.filename = dir.filePath(QStringLiteral("broken.pdf"));
        QFile file(broken.filename);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("%PDF-1.7\nThis is not a PDF document.\n");
        file.close();
        BookDatabase::self().addEntry(broken);
        BookEntry missing;
        missing.title = QStringLiteral("Beta missing EPUB");
        missing.filename = dir.filePath(QStringLiteral("missing.epub"));
        BookDatabase::self().addEntry(missing);
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("gamma.epub")), QStringLiteral("Gamma"), {chapter("<p>needle needle needle</p>")}));
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("zeta.epub")), QStringLiteral("Zeta"), {chapter("<p>needle</p>")}));
        LibrarySearchModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto row = [&](int index) {
            return model.data(model.index(index), Qt::UserRole).toMap();
        };
        model.start(QStringLiteral("needle"));
        QTRY_VERIFY_WITH_TIMEOUT(!model.running(), 15000);
        QCOMPARE(model.completed(), 4);
        QCOMPARE(model.failedCount(), 2);
        QCOMPARE(model.bookCount(), 2);
        QCOMPARE(model.matchCount(), 4);
        QCOMPARE(model.rowCount(), 5);
        QCOMPARE(row(0).value(QStringLiteral("title")).toString(), QStringLiteral("Gamma"));
        QCOMPARE(row(1).value(QStringLiteral("title")).toString(), QStringLiteral("Zeta"));
        QCOMPARE(row(3).value(QStringLiteral("title")).toString(), broken.title);
        QCOMPARE(row(4).value(QStringLiteral("title")).toString(), missing.title);
        for (const auto &[index, separator] : {std::pair{0, 0}, {1, 1}, {3, 2}, {4, 1}}) {
            QCOMPARE(row(index).value(QStringLiteral("separator")).toInt(), separator);
            QCOMPARE(row(index).value(QStringLiteral("number")).toInt(), index == 0 ? 1 : index == 1 ? 2 : index == 3 ? 3 : 4);
        }
        QVERIFY(!row(3).value(QStringLiteral("error")).toString().isEmpty());
        QVERIFY(!row(4).value(QStringLiteral("error")).toString().isEmpty());
        model.toggleBook(0);
        QCOMPARE(model.rowCount(), 8);
        QCOMPARE(row(6).value(QStringLiteral("title")).toString(), broken.title);
        QCOMPARE(row(6).value(QStringLiteral("separator")).toInt(), 2);
        model.toggleBook(0);
        QCOMPARE(model.rowCount(), 5);

        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/LibrarySearchPage.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> page(component.createWithInitialProperties({{QStringLiteral("searchModel"), QVariant::fromValue(&model)}}));
        auto *pageItem = qobject_cast<QQuickItem *>(page.get());
        QVERIFY2(pageItem, qPrintable(component.errorString()));
        QQuickWindow window;
        window.resize(1000, 900);
        pageItem->setParentItem(window.contentItem());
        pageItem->setSize(QSizeF(1000, 900));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *list = page->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchResults"));
        QVERIFY(list);
        const auto visibleHeadings = [&] {
            QList<QQuickItem *> found;
            const auto collect = [&](const auto &self, QQuickItem *parent) -> void {
                for (auto *child : parent->childItems()) {
                    if (child->isVisible() && child->objectName() == QStringLiteral("libraryFullTextSearchBook"))
                        found.append(child);
                    self(self, child);
                }
            };
            collect(collect, list);
            return found;
        };
        QTRY_COMPARE(visibleHeadings().size(), 4);
        for (auto *heading : visibleHeadings()) {
            const int separator = heading->property("result").toMap().value(QStringLiteral("separator")).toInt();
            auto *rule = heading->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchSeparator"));
            QVERIFY(rule);
            QCOMPARE(rule->isVisible(), separator != 0);
            QCOMPARE(rule->childItems().size(), 2);
            QCOMPARE(rule->childItems().at(1)->isVisible(), separator == 2);
            if (separator == 2)
                QVERIFY(rule->height() > 1);
        }
    }

    void realProblemPdfDoesNotStopLibrary()
    {
        // Optional local regression with the original PDF; never copy private books into test data.
        const QString problemFile = qEnvironmentVariable("ARIANNA_TEST_PROBLEM_PDF");
        if (problemFile.isEmpty())
            QSKIP("Set ARIANNA_TEST_PROBLEM_PDF to test the original document.");
        const auto before = digest(problemFile);
        QVERIFY(!before.isEmpty());
        QTemporaryDir dir;
        BookEntry problem;
        problem.filename = problemFile;
        problem.title = QStringLiteral("Alpha problem PDF");
        BookDatabase::self().addEntry(problem);
        const auto after = epub(dir.filePath(QStringLiteral("after.epub")), QStringLiteral("Zeta after PDF"), {chapter("<p>Aham brahmasmi</p>")});
        BookDatabase::self().addEntry(after);
        LibrarySearchModel model;
        model.start(QStringLiteral("Aham brahmasmi"));
        QTRY_VERIFY_WITH_TIMEOUT(!model.running(), 130000);
        QCOMPARE(model.completed(), 2);
        QVERIFY(model.failedCount() <= 1); // A locally patched PoDoFo can search the PDF successfully.
        QVERIFY(model.bookCount() >= 1);
        QVERIFY(model.matchCount() >= 1);
        bool foundLaterBook = false;
        for (int row = 0; row < model.rowCount(); ++row)
            foundLaterBook |= model.data(model.index(row), Qt::UserRole).toMap().value(QStringLiteral("filename")).toString() == after.filename;
        QVERIFY(foundLaterBook);
        if (model.failedCount()) {
            const auto failure = model.data(model.index(model.rowCount() - 1), Qt::UserRole).toMap();
            QCOMPARE(failure.value(QStringLiteral("error")).toString(),
                     i18n("Text extraction stopped unexpectedly for this PDF. The remaining books will still be searched."));
            QCOMPARE(failure.value(QStringLiteral("separator")).toInt(), 2);
        }
        QCOMPARE(digest(problemFile), before);
    }

    void enterStartsCentralSearchAndMenuRestoresResults()
    {
        QTemporaryDir dir;
        auto book =
            epub(dir.filePath(QStringLiteral("ui.epub")), QStringLiteral("Search Book"), {chapter("<p>needle in overview, needle again, third needle</p>")});
        book.thumbnail = dir.filePath(QStringLiteral("cover.png"));
        QImage coverImage(60, 90, QImage::Format_RGB32);
        coverImage.fill(Qt::darkBlue);
        QVERIFY(coverImage.save(book.thumbnail));
        BookDatabase::self().addEntry(book);
        BookDatabase::self().addEntry(epub(dir.filePath(QStringLiteral("other.epub")), QStringLiteral("Other Book"), {chapter("<p>Other text</p>")}));
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        window->resize(1200, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("globalDrawer.width >= globalDrawer.preferredSize - 1")).toBool());
        auto *pane = object->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchPane"));
        auto *model = object->findChild<LibrarySearchModel *>();
        QVERIFY(pane && model);
        auto *field = pane->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchField"));
        auto *searchMenuItem = visualItem(window->contentItem(), QStringLiteral("libraryFullTextSearchMenuItem"));
        QVERIFY(field && searchMenuItem);
        QVERIFY(!pane->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchStart")));
        QTRY_VERIFY(field->isEnabled());
        auto *home = visualItem(window->contentItem(), QStringLiteral("goHomeButton"));
        auto *maintenance = visualItem(window->contentItem(), QStringLiteral("maintenancePageButton"));
        QVERIFY(home && maintenance);
        QTRY_VERIFY(searchMenuItem->isVisible());
        QTRY_VERIFY(searchMenuItem->mapToScene(QPointF()).y() > home->mapToScene(QPointF()).y());
        QVERIFY(pane->mapToScene(QPointF()).y() > searchMenuItem->mapToScene(QPointF()).y());
        QVERIFY(pane->mapToScene(QPointF()).y() < maintenance->mapToScene(QPointF()).y());
        const QString homeWindowTitle = window->title();
        const QString homePageTitle = evaluate(object.get(), QStringLiteral("pageStack.currentItem.title")).toString();
        QVERIFY(!homeWindowTitle.isEmpty() && !homePageTitle.isEmpty());
        const auto click = [window](QQuickItem *item) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };

        // Typing alone must not start the search, even after automatic acceptance would have fired.
        QVERIFY(field->setProperty("text", QStringLiteral("needle")));
        QTest::qWait(500);
        QVERIFY(model->query().isEmpty());
        QVERIFY(!model->running());
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(window->title(), homeWindowTitle);
        QVERIFY(searchMenuItem->isEnabled());
        QVERIFY(!searchMenuItem->property("text").toString().isEmpty());
        click(searchMenuItem);
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("pageStack.currentItem.librarySearchPage === true")).toBool());
        QVERIFY(model->query().isEmpty());
        QCOMPARE(model->rowCount(), 0);
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(model->query(), QStringLiteral("needle"));
        QTRY_VERIFY(!model->running());
        QCOMPARE(model->matchCount(), 3);
        QCOMPARE(model->bookCount(), 1);
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("pageStack.currentItem.librarySearchPage === true")).toBool());
        auto *page = object->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchPage"));
        QVERIFY(page);
        auto *list = page->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchResults"));
        QVERIFY(list);
        QVERIFY(!pane->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchResults")));
        QTRY_VERIFY(list->isVisible());
        QTRY_VERIFY(list->mapToScene(QPointF()).x() >= pane->mapToScene(QPointF(pane->width(), 0)).x());
        const QString searchTitle = i18nc("@title:window, %1 is the number of books with matches", "Library Full-text Search(%1)", 1);
        QTRY_COMPARE(page->property("title").toString(), searchTitle);
        QTRY_COMPARE(window->title(), i18nc("@title:window", "%1 - Arianna", searchTitle));
        QQuickItem *cover = nullptr;
        QTRY_VERIFY((cover = visualItem(window->contentItem(), QStringLiteral("libraryFullTextSearchCover"))));
        QTRY_COMPARE(cover->property("status").toInt(), 1); // Image.Ready
        if (!qEnvironmentVariable("ARIANNA_TEST_SCREENSHOT").isEmpty())
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("ARIANNA_TEST_SCREENSHOT")));

        // The book heading is visible at three matches; clicking it toggles only that book's hits.
        QCOMPARE(model->rowCount(), 1);
        QQuickItem *bookHeading = nullptr;
        QTRY_VERIFY((bookHeading = visualItem(list, QStringLiteral("libraryFullTextSearchBook"))));
        QSignalSpy opened(page, SIGNAL(openResult(QString, QString, QString, int)));
        click(bookHeading);
        QTRY_COMPARE(model->rowCount(), 4);
        QTRY_VERIFY(!bookHeading->property("result").toMap().value(QStringLiteral("collapsed")).toBool());
        QTRY_VERIFY(visualItem(list, QStringLiteral("libraryFullTextSearchHit")));
        QCOMPARE(opened.size(), 0);
        QCOMPARE(model->matchCount(), 3);
        QCOMPARE(page->property("title").toString(), searchTitle);
        click(bookHeading);
        QTRY_COMPARE(model->rowCount(), 1);
        QTRY_VERIFY(bookHeading->property("result").toMap().value(QStringLiteral("collapsed")).toBool());

        // Editing or clearing the draft leaves the last submitted query and its results intact.
        QVERIFY(field->setProperty("text", QString()));
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(model->matchCount(), 3);
        QCOMPARE(model->query(), QStringLiteral("needle"));
        QVERIFY(searchMenuItem->isEnabled());
        click(searchMenuItem);
        QCOMPARE(model->query(), QStringLiteral("needle"));
        QCOMPARE(model->matchCount(), 3);
        QVERIFY(field->setProperty("text", QStringLiteral("missing phrase")));
        QTest::qWait(500);
        QCOMPARE(model->query(), QStringLiteral("needle"));
        QCOMPARE(page->property("title").toString(), searchTitle);
        click(searchMenuItem);
        QCOMPARE(model->query(), QStringLiteral("needle"));
        QCOMPARE(model->matchCount(), 3);
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Enter, Qt::KeypadModifier);
        QTRY_COMPARE(model->query(), QStringLiteral("missing phrase"));
        QTRY_VERIFY(!model->running());
        QCOMPARE(model->bookCount(), 0);
        QCOMPARE(model->rowCount(), 0);
        QTRY_COMPARE(page->property("title").toString(), i18nc("@title:window, %1 is the number of books with matches", "Library Full-text Search(%1)", 0));

        click(home);
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("pageStack.currentItem.libraryPage === true")).toBool());
        QTRY_COMPARE(evaluate(object.get(), QStringLiteral("pageStack.currentItem.title")).toString(), homePageTitle);
        QTRY_COMPARE(window->title(), homeWindowTitle);
        QTest::qWait(300); // Wait for the previous results page to leave the scene.
        QVERIFY(!visualItem(window->contentItem(), QStringLiteral("libraryFullTextSearchResults")));

        // The menu also opens results from the collapsed sidebar, without submitting the draft.
        QVERIFY(field->setProperty("text", QStringLiteral("needle")));
        evaluate(object.get(), QStringLiteral("globalDrawer.collapsed = true"));
        QTRY_VERIFY(!pane->isVisible());
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("globalDrawer.width <= globalDrawer.collapsedSize + 1")).toBool());
        QVERIFY(searchMenuItem->isVisible());
        click(searchMenuItem);
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("pageStack.currentItem.librarySearchPage === true")).toBool());
        QTRY_VERIFY(pane->isVisible());
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("globalDrawer.width >= globalDrawer.preferredSize - 1")).toBool());
        QCOMPARE(model->query(), QStringLiteral("missing phrase"));
        QCOMPARE(model->bookCount(), 0);
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(model->query(), QStringLiteral("needle"));
        QTRY_VERIFY(!model->running());
        QTRY_COMPARE(model->bookCount(), 1);
        page = object->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchPage"));
        QVERIFY(page);
        list = page->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchResults"));
        QVERIFY(list);
        QCOMPARE(model->rowCount(), 1);
        QTRY_VERIFY((bookHeading = visualItem(list, QStringLiteral("libraryFullTextSearchBook"))));
        click(bookHeading);
        QTRY_COMPARE(model->rowCount(), 4);
        QQuickItem *delegate = nullptr;
        const auto findHit = [&] {
            for (auto *child : list->childItems()) {
                for (auto *item : child->childItems()) {
                    const auto result = item->property("result").toMap();
                    if (!result.value(QStringLiteral("heading"), true).toBool())
                        return item;
                }
            }
            return static_cast<QQuickItem *>(nullptr);
        };
        QTRY_VERIFY((delegate = findHit()));
        QSignalSpy clicked(page, SIGNAL(openResult(QString, QString, QString, int)));
        QVERIFY(QMetaObject::invokeMethod(delegate, "clicked"));
        QCOMPARE(clicked.size(), 1);
        QCOMPARE(clicked.first().at(0).toString(), book.filename);
        QCOMPARE(clicked.first().at(2).toString(), QStringLiteral("needle"));
        QCOMPARE(evaluate(object.get(), QStringLiteral("pageStack.layers.currentItem.searchText")).toString(), QStringLiteral("needle"));
        QVERIFY(clicked.first().at(1).toString().startsWith(QStringLiteral("epubcfi(")));
        QCOMPARE(evaluate(object.get(), QStringLiteral("pageStack.layers.currentItem.filename")).toString(), book.filename);
        QCOMPARE(evaluate(object.get(), QStringLiteral("pageStack.layers.currentItem.currentLocation")).toString(), clicked.first().at(1).toString());
    }

    void libraryHitOpensReaderSearch_data()
    {
        QTest::addColumn<bool>("pdf");
        QTest::newRow("epub") << false;
        QTest::newRow("pdf") << true;
    }

    void libraryHitOpensReaderSearch()
    {
        QFETCH(bool, pdf);
        QTemporaryDir dir;
        BookEntry book;
        if (pdf) {
            book.filename = dir.filePath(QStringLiteral("reader.pdf"));
            QPdfWriter writer(book.filename);
            writer.setResolution(72);
            QPainter painter(&writer);
            painter.drawText(80, 150, QStringLiteral("needle first"));
            QVERIFY(writer.newPage());
            painter.drawText(80, 150, QStringLiteral("needle second"));
            painter.drawText(80, 650, QStringLiteral("needle third"));
        } else {
            book = epub(dir.filePath(QStringLiteral("reader.epub")), QStringLiteral("Reader"),
                        {chapter("<p>needle first</p>"),
                         chapter("<p>needle second</p><p style=\"break-before:column\">needle third</p>")});
        }
        book.uniqueIdentifier = QStringLiteral("fulltext-reader-test");
        book.pageMode = QStringLiteral("single");
        book.zoomLevel = 2;
        BookDatabase::self().addEntry(book);
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
        Config::setBookServerPort(probe.serverPort());
        probe.close();
        const QString token = QStringLiteral("fulltext-reader-token");
        BookServer server(token);
        QVERIFY(server.isRunning());
        server.addSessionToken(token);
        BookServerTestInterceptor interceptor(token);

        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), token);
        TestScriptState scriptState;
        scriptState.insert(QStringLiteral("done"), false);
        scriptState.insert(QStringLiteral("result"), QVariant());
        engine.rootContext()->setContextProperty(QStringLiteral("testScript"), &scriptState);
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        window->resize(1200, 900);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(evaluate(object.get(), QStringLiteral("bookListModel.cacheLoaded")).toBool());
        evaluate(object.get(), QStringLiteral("startLibraryFullTextSearch('needle')"));
        auto *model = object->findChild<LibrarySearchModel *>();
        QVERIFY(model);
        QTRY_VERIFY(!model->running());
        QCOMPARE(model->matchCount(), 3);
        model->toggleBook(0);
        QCOMPARE(model->rowCount(), 4);
        auto *page = object->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchPage"));
        QVERIFY(page);
        auto *list = page->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchResults"));
        QVERIFY(list);
        QQuickItem *thirdHit = nullptr;
        const auto findThirdHit = [&]() -> QQuickItem * {
            for (auto *child : list->childItems()) {
                for (auto *item : child->childItems()) {
                    const auto result = item->property("result").toMap();
                    if (!result.value(QStringLiteral("heading"), true).toBool() && result.value(QStringLiteral("number")).toInt() == 3)
                        return item;
                }
            }
            return nullptr;
        };
        QTRY_VERIFY((thirdHit = findThirdHit()));
        // An unsubmitted sidebar draft must not replace the query belonging to these results.
        auto *sidebarField = object->findChild<QQuickItem *>(QStringLiteral("libraryFullTextSearchField"));
        QVERIFY(sidebarField && sidebarField->setProperty("text", QStringLiteral("different draft")));
        QVERIFY(QMetaObject::invokeMethod(thirdHit, "clicked"));
        auto *viewer = evaluate(object.get(), QStringLiteral("pageStack.layers.currentItem")).value<QObject *>();
        QVERIFY(viewer);
        QCOMPARE(viewer->property("searchText").toString(), QStringLiteral("needle"));
        QTRY_COMPARE(evaluate(viewer, QStringLiteral("headerSearchField.text")).toString(), QStringLiteral("needle"));
        if (pdf) {
            QTRY_COMPARE_WITH_TIMEOUT(viewer->property("activeSearchResultCount").toInt(), 3, 15000);
            QTRY_COMPARE(evaluate(viewer, QStringLiteral("view.searchModel.currentResult")).toInt(), 2);
            QCOMPARE(viewer->property("activePage").toInt(), 1);
            auto *searchDialog = viewer->findChild<QObject *>(QStringLiteral("pdfSearchDialog"));
            QVERIFY(searchDialog && !searchDialog->property("visible").toBool());
            auto *pdfView = evaluate(viewer, QStringLiteral("view")).value<QQuickItem *>();
            QVERIFY(pdfView);
            const auto visibleHitY = [&]() {
                for (auto *item : pdfView->findChildren<OkularPdfPageItem *>()) {
                    if (item->pageNumber() == 1) {
                        const auto rect = evaluate(viewer, QStringLiteral("view.searchModel.matches[2].rectangles[0]")).toMap();
                        return item->mapToItem(pdfView, QPointF(0, (rect.value(QStringLiteral("y")).toDouble()
                            + rect.value(QStringLiteral("height")).toDouble() / 2) * item->height())).y();
                    }
                }
                return -10000.0;
            };
            QTRY_VERIFY(qAbs(visibleHitY() - pdfView->height() / 2) < 2);
            evaluate(viewer, QStringLiteral("view.searchBack()"));
            QCOMPARE(evaluate(viewer, QStringLiteral("view.searchModel.currentResult")).toInt(), 1);
        } else {
            auto *search = viewer->findChild<QObject *>(QStringLiteral("epubSearchModel"));
            QVERIFY(search);
            QTRY_COMPARE_WITH_TIMEOUT(search->property("count").toInt(), 3, 20000);
            QTRY_COMPARE(search->property("currentResult").toInt(), 2);
            QVERIFY(!evaluate(search, QStringLiteral("searchDialog.visible")).toBool());
            const auto script = [&](const QString &code) {
                scriptState.insert(QStringLiteral("done"), false);
                const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
                evaluate(search, QStringLiteral("view.runJavaScript(%1[0], value => { testScript.result = value; testScript.done = true; })").arg(encoded));
                QElapsedTimer timer;
                timer.start();
                while (!scriptState.value(QStringLiteral("done")).toBool() && timer.elapsed() < 5000)
                    QTest::qWait(10);
                return scriptState.value(QStringLiteral("result"));
            };
            QTRY_COMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0]?.index")).toInt(), 1);
            const auto cfi = evaluate(search, QStringLiteral("get(currentResult).cfi")).toString();
            const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{cfi}).toJson(QJsonDocument::Compact));
            QTRY_VERIFY(script(QStringLiteral(R"JS((() => {
                const {index, anchor} = reader.view.resolveNavigation(%1[0]);
                const content = reader.view.renderer.getContents().find(item => item.index === index);
                if (!content) return false;
                const range = anchor(content.doc);
                return range.toString() === 'needle' && reader.view.lastLocation?.range?.intersectsNode(range.startContainer);
            })())JS").arg(encoded)).toBool());
            evaluate(search, QStringLiteral("previousResult()"));
            QCOMPARE(search->property("currentResult").toInt(), 1);
        }
    }

    void cfiRangesResolveInWebEngine()
    {
        QTemporaryDir dir;
        const QByteArray xml = chapter("<p>Ein <em>Such</em>wort &amp; mehr.</p><p>nee<!--x-->dle</p><p>groß <![CDATA[& schön]]></p>");
        const auto book = epub(dir.filePath(QStringLiteral("cfi.epub")), QStringLiteral("CFI"), {xml});
        EPubContainer container(nullptr);
        QVERIFY(container.openFile(book.filename));
        QJsonArray inputs;
        for (const auto &query : {QStringLiteral("Suchwort & mehr"), QStringLiteral("needle"), QStringLiteral("groß & schön")}) {
            const auto hits = container.searchText(query);
            QCOMPARE(hits.size(), 1);
            inputs.append(QJsonArray{QString::fromUtf8(xml), hits[0].location, query});
        }
        QVERIFY(QFile::copy(QStringLiteral(ARIANNA_SOURCE_DIR "/src/qml/foliate-js/epubcfi.js"), dir.filePath(QStringLiteral("epubcfi.js"))));
        QFile html(dir.filePath(QStringLiteral("test.html")));
        QVERIFY(html.open(QIODevice::WriteOnly));
        html.write(
            QByteArrayLiteral("<html><script type=\"module\">import * as CFI from './epubcfi.js'; const inputs=")
            + QJsonDocument(inputs).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral(
                "; window.result = inputs.map(([xml,cfi,expected]) => { const doc=new DOMParser().parseFromString(xml,'application/xhtml+xml');"
                "const parts=CFI.parse(cfi); (parts.parent ?? parts).shift(); return CFI.toRange(doc,parts).toString() === expected; });</script></html>"));
        html.close();
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("testUrl"), QUrl::fromLocalFile(html.fileName()));
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import QtWebEngine
            WebEngineView {
                id: browser
                width: 400; height: 300; url: testUrl
                property string result: ""
                Timer { interval: 50; repeat: true; running: true
                    onTriggered: browser.runJavaScript("JSON.stringify(window.result)", value => { if (value) browser.result = value; }) }
            }
        )QML",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> view(component.create());
        QVERIFY2(view != nullptr, qPrintable(component.errorString()));
        QQuickWindow window;
        qobject_cast<QQuickItem *>(view.get())->setParentItem(window.contentItem());
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(view->property("result").toString(), QStringLiteral("[true,true,true]"), 15000);
    }
};

int main(int argc, char **argv)
{
    // A controlled external parser substitute exercises real process failures, not a mock return value.
    if (argc == 2 && QByteArray(argv[1]).startsWith("--pdf-search-test-")) {
        QCoreApplication app(argc, argv);
        const QByteArray mode(argv[1]);
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly))
            return 1;
        input.readAll();
        if (mode.endsWith("-crash")) {
            std::raise(SIGSEGV);
            return 2;
        }
        if (mode.endsWith("-exit"))
            return 2;
        if (mode.endsWith("-hang")) {
            for (;;)
                QThread::msleep(100);
        }
        QFile output;
        if (!output.open(stdout, QIODevice::WriteOnly))
            return 1;
        if (mode.endsWith("-flood")) {
            const QByteArray chunk(8192, 'x');
            for (int i = 0; i < 1024; ++i)
                if (output.write(chunk) < 0 || !output.flush())
                    break;
            return 0;
        }
        if (mode.endsWith("-invalid-hit")) {
            output.write(R"({"version":1,"hits":[{"location":null}],"error":""})");
        } else if (mode.endsWith("-invalid")) {
            output.write("broken JSON");
        } else {
            QFile warnings;
            if (!warnings.open(stderr, QIODevice::WriteOnly))
                return 1;
            for (int i = 0; i < 10000; ++i)
                warnings.write("WARNING: Skipped free object entry with generation number 0\n");
            warnings.flush();
            output.write(R"({"version":1,"hits":[],"error":""})");
        }
        output.flush();
        return 0;
    }
    QTemporaryDir environment;
    if (!environment.isValid())
        return 1;
    const auto path = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", path);
    qputenv("XDG_CONFIG_HOME", path);
    qputenv("XDG_CACHE_HOME", path);
    qputenv("XDG_RUNTIME_DIR", path);
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    LibrarySearchTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "librarysearchtest.moc"
