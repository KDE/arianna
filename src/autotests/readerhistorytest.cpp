// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookdatabase.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "readerhistory.h"
#include "readercontrol.h"
#include "navigation.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>
#include <memory>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

static BookEntry createBook(const QString &path)
{
    BookEntry entry;
    entry.filename = path;
    entry.title = QFileInfo(path).completeBaseName();
    entry.uniqueIdentifier = QStringLiteral("uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.identifier = entry.uniqueIdentifier;
    KZip zip(path);
    if (!zip.open(QIODevice::WriteOnly)) return BookEntry{};
    if (!zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip")
        || !zip.writeFile(QStringLiteral("META-INF/container.xml"),
            "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles>"
            "<rootfile full-path='package.opf' media-type='application/oebps-package+xml'/></rootfiles></container>")
        || !zip.writeFile(QStringLiteral("package.opf"),
            QStringLiteral("<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>"
                "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>%1</dc:identifier>"
                "<dc:title>%2</dc:title><dc:language>en</dc:language></metadata>"
                "<manifest><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest>"
                "<spine><itemref idref='chapter'/></spine></package>").arg(entry.identifier, entry.title).toUtf8())
        || !zip.writeFile(QStringLiteral("chapter.xhtml"),
            "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Reader history</title></head>"
            "<body><p>Reader history navigation test.</p></body></html>") || !zip.close()) return BookEntry{};
    BookDatabase::self().addEntry(entry);
    return entry;
}

class ReaderHistoryTest : public QObject
{
    Q_OBJECT
    QVariant evaluate(QObject *object, const QString &code)
    {
        QQmlExpression expression(qmlContext(object), object, code);
        const auto value = expression.evaluate();
        if (expression.hasError()) QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
        return value;
    }

private Q_SLOTS:
    void selectByCloseTimeRestoreByOpeningTime()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto &db = BookDatabase::self();
        const auto a = createBook(directory.filePath(QStringLiteral("A.epub")));
        const auto b = createBook(directory.filePath(QStringLiteral("B.epub")));
        const auto c = createBook(directory.filePath(QStringLiteral("C.epub")));
        const auto missing = createBook(directory.filePath(QStringLiteral("missing.epub")));
        const auto neverClosed = createBook(directory.filePath(QStringLiteral("never.epub")));
        QVERIFY(!a.filename.isEmpty());
        const auto time = QDateTime::fromString(QStringLiteral("2026-01-01T12:00:00.000Z"), Qt::ISODateWithMs);
        db.updateEntry(a.filename, QStringLiteral("currentLocation"), QStringLiteral("epubcfi(/6/2!/4/2/1:7)"));
        QVERIFY(db.recordReaderClosed(a.filename, time.addSecs(1), time.addSecs(6)));
        QVERIFY(db.recordReaderClosed(b.filename, time.addSecs(2), time.addSecs(4)));
        QVERIFY(db.recordReaderClosed(c.filename, time.addSecs(3), time.addSecs(5)));
        QVERIFY(db.recordReaderClosed(missing.filename, time.addSecs(7), time.addSecs(9)));
        QVERIFY(QFile::remove(missing.filename));
        QVERIFY(!db.recordReaderClosed(QStringLiteral("/not/imported.epub"), time, time));
        QVERIFY(!db.loadEntry(QStringLiteral("/not/imported.epub")));
        QVERIFY(!db.recordReaderClosed(a.filename, {}, time));
        QVERIFY(ReaderHistory::lastClosedBooks(0).isEmpty());
        QVERIFY(ReaderHistory::lastClosedBooks(-1).isEmpty());
        const auto selected = ReaderHistory::lastClosedBooks(2);
        QCOMPARE(selected.size(), 2);
        QCOMPARE(selected.at(0).filename, a.filename);
        QCOMPARE(selected.at(1).filename, c.filename);
        QCOMPARE(selected.first().currentLocation, QStringLiteral("epubcfi(/6/2!/4/2/1:7)"));
        QCOMPARE(selected.first().lastClosedTime, time.addSecs(6));
        QCOMPARE(ReaderHistory::lastClosedBooks(99).size(), 3);
        QVERIFY(!db.loadEntry(neverClosed.filename)->lastClosedTime.isValid());
        // A later open must not rewrite the order of the previously closed session.
        db.updateEntry(a.filename, QStringLiteral("lastOpenedTime"), time.addSecs(100));
        QCOMPARE(ReaderHistory::lastClosedBooks(2).first().filename, a.filename);
        QVERIFY(db.recordReaderClosed(a.filename, time.addSecs(10), time.addSecs(11)));
        const auto reopened = ReaderHistory::lastClosedBooks(2);
        QCOMPARE(reopened.first().filename, c.filename);
        QCOMPARE(reopened.last().filename, a.filename);
        auto metadata = *db.loadEntry(a.filename);
        metadata.title = QStringLiteral("New title");
        db.updateEntry(metadata);
        QCOMPARE(db.loadEntry(a.filename)->lastClosedTime, time.addSecs(11));
        db.updateEntry(a); // Deliberately stale metadata has no recorded history.
        QCOMPARE(db.loadEntry(a.filename)->lastClosedTime, time.addSecs(11));
    }

    void lifetimeAndDuplicateRegistration()
    {
        QTemporaryDir directory;
        const auto a = createBook(directory.filePath(QStringLiteral("lifetimeA.epub")));
        const auto b = createBook(directory.filePath(QStringLiteral("lifetimeB.epub")));
        auto &db = BookDatabase::self();
        auto first = std::make_unique<QObject>();
        auto second = std::make_unique<QObject>();
        {
            ReaderHistory history;
            history.trackReader(first.get(), a.filename);
            history.trackReader(second.get(), b.filename);
            history.trackReader(first.get(), a.filename);
            QVERIFY(!db.loadEntry(a.filename)->lastClosedTime.isValid());
            second.reset(); // Close in the opposite order to opening.
            first.reset();
            QVERIFY(db.loadEntry(b.filename)->lastClosedTime < db.loadEntry(a.filename)->lastClosedTime);
            QVERIFY(db.loadEntry(a.filename)->lastClosedOpenedTime < db.loadEntry(b.filename)->lastClosedOpenedTime);
            const auto selected = ReaderHistory::lastClosedBooks(2);
            QCOMPARE(selected.first().filename, a.filename);
            QCOMPARE(selected.last().filename, b.filename);
        }
        // Destroying the history owner also records readers still alive exactly once.
        QObject reader;
        {
            ReaderHistory history;
            history.trackReader(&reader, b.filename);
        }
        QVERIFY(db.loadEntry(b.filename)->lastClosedTime.isValid());
    }

    void normalApplicationExit()
    {
        QTemporaryDir directory;
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--quit-probe"), directory.path()});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(20000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
    }

    void refreshRequestsAreIdempotent()
    {
        int calls = 0;
        ReaderControl control([&calls] { ++calls; return 3; });
        QCOMPARE(control.RefreshReaders(QStringLiteral("invalid")), -1);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(control.RefreshReaders(id), 3);
        QCOMPARE(control.RefreshReaders(id), 3);
        QCOMPARE(calls, 1);
        QCOMPARE(control.RefreshReaders(QUuid::createUuid().toString()), 3);
        QCOMPARE(calls, 2);
    }

    void serverControlAndSessionRenewal()
    {
        QTemporaryDir directory;
        const auto book = createBook(directory.filePath(QStringLiteral("transient.epub")));
        const auto other = createBook(directory.filePath(QStringLiteral("other.epub")));
        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
        Config::setBookServerPort(port.serverPort());
        port.close();
        BookServer server(QStringLiteral("control-server"));
        QVERIFY(server.isRunning());
        QNetworkAccessManager network;
        const auto request = [&](const QByteArray &method, const QString &path, const QString &token,
                                 const QJsonObject &data = {}, bool session = false) {
            QNetworkRequest request(QUrl(BookServerConfig::baseUrl() + path));
            request.setRawHeader(session ? "X-Arianna-Session-Token" : "X-Arianna-Server-Token", token.toUtf8());
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            std::unique_ptr<QNetworkReply> reply(network.sendCustomRequest(request, method, QJsonDocument(data).toJson()));
            QSignalSpy finished(reply.get(), &QNetworkReply::finished);
            if (!reply->isFinished()) finished.wait(5000);
            return qMakePair(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                             QJsonDocument::fromJson(reply->readAll()).object());
        };
        QCOMPARE(request("DELETE", QStringLiteral("/server-control"), QStringLiteral("wrong")).first, 401);
        QVERIFY(server.isRunning());
        QCOMPARE(request("GET", QStringLiteral("/server-control"), QStringLiteral("control-server")).first, 200);
        const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject renewal{{QStringLiteral("sessionToken"), token}};
        QCOMPARE(request("POST", QStringLiteral("/session"), QStringLiteral("control-server"), renewal).first, 200);
        QCOMPARE(request("POST", QStringLiteral("/session"), QStringLiteral("control-server"), renewal).second.value(QStringLiteral("registeredReaders")).toInt(), 1);
        QCOMPARE(server.registeredReaderCount(), 1);
        QCOMPARE(request("POST", QStringLiteral("/session"), QStringLiteral("control-server"),
            {{QStringLiteral("sessionToken"), QStringLiteral("invalid")}}).first, 400);
        const auto identifier = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject registration{{QStringLiteral("fileName"), book.filename}, {QStringLiteral("identifier"), identifier}};
        QCOMPARE(request("POST", QStringLiteral("/read-only-books"), token, registration, true).first, 200);
        QCOMPARE(request("POST", QStringLiteral("/read-only-books"), token, registration, true).second.value(QStringLiteral("identifier")).toString(), identifier);
        QCOMPARE(request("POST", QStringLiteral("/read-only-books"), token,
            {{QStringLiteral("fileName"), other.filename}, {QStringLiteral("identifier"), identifier}}, true).first, 409);
        QCOMPARE(request("DELETE", QStringLiteral("/server-control"), QStringLiteral("control-server")).first, 200);
        QTRY_VERIFY(!server.isRunning());
        network.clearConnectionCache();
        BookServer restarted(QStringLiteral("control-server"));
        QVERIFY(restarted.isRunning());
        QCOMPARE(request("POST", QStringLiteral("/session"), QStringLiteral("control-server"), renewal).first, 200);
        QCOMPARE(request("POST", QStringLiteral("/read-only-books"), token, registration, true).first, 200);
        QCOMPARE(restarted.registeredReaderCount(), 1);
        QCOMPARE(request("GET", QStringLiteral("/") + identifier + QStringLiteral("/book.epub"), token, {}, true).first, 200);
    }

    void actualReaderPagesDoNotCloseWhenCovered()
    {
        QTemporaryDir directory;
        const auto a = createBook(directory.filePath(QStringLiteral("readerA.epub")));
        const auto b = createBook(directory.filePath(QStringLiteral("readerB.epub")));
        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
        Config::setBookServerPort(port.serverPort());
        port.close();
        BookServer server(QStringLiteral("history-server"));
        QVERIFY(server.isRunning());
        server.registerReaderSession(QStringLiteral("history-reader"));
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
        engine.rootContext()->setContextProperty(QStringLiteral("startupDirectReaderMode"), true);
        engine.rootContext()->setContextProperty(QStringLiteral("serverToken"), QStringLiteral("history-server"));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("history-reader"));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerAddress"), BookServerConfig::address());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerPort"), BookServerConfig::port());
        QQmlComponent component(&engine);
        component.loadFromModule(QStringLiteral("org.kde.arianna"), QStringLiteral("Main"));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *navigation = engine.singletonInstance<Navigation *>("org.kde.arianna", "Navigation");
        QVERIFY(navigation);
        navigation->setWebEngineSessionToken(QStringLiteral("history-reader"));
        const auto open = [&](const BookEntry &entry, bool readOnly = false, bool noImport = false) {
            engine.rootContext()->setContextProperty(QStringLiteral("testEntry"), QVariant::fromValue(entry));
            const auto file = QString::fromUtf8(QJsonDocument(QJsonArray{entry.filename}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
            return evaluate(window.get(), QStringLiteral("openBookPage(%1,'','',testEntry,%2,'',null,%3);"
                "var page=pageStack.layers.currentItem; page && page.item ? page.item : page")
                .arg(file, readOnly ? QStringLiteral("true") : QStringLiteral("false"), noImport ? QStringLiteral("true") : QStringLiteral("false"))).value<QObject *>();
        };
        auto *first = open(a);
        QVERIFY(first);
        QSignalSpy firstReady(first, SIGNAL(bookReady(QVariant)));
        QVERIFY(firstReady.isValid());
        QTRY_VERIFY_WITH_TIMEOUT(firstReady.count() > 0, 5000);
        auto *second = open(b);
        QVERIFY(second);
        QSignalSpy secondReady(second, SIGNAL(bookReady(QVariant)));
        QVERIFY(secondReady.isValid());
        QTRY_VERIFY_WITH_TIMEOUT(secondReady.count() > 0, 5000);
        QVERIFY(first->property("readerSuspended").toBool());
        auto &db = BookDatabase::self();
        QVERIFY(!db.loadEntry(a.filename)->lastClosedTime.isValid());
        QVERIFY(!db.loadEntry(b.filename)->lastClosedTime.isValid());
        QCOMPARE(evaluate(window.get(), QStringLiteral("refreshReaderSessions()")).toInt(), 2);
        QVERIFY(first->property("readerSuspended").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(secondReady.count() > 1, 5000);
        QVERIFY(!db.loadEntry(a.filename)->lastClosedTime.isValid());
        QVERIFY(!db.loadEntry(b.filename)->lastClosedTime.isValid());
        evaluate(window.get(), QStringLiteral("pageStack.layers.pop()"));
        QTRY_VERIFY(db.loadEntry(b.filename)->lastClosedTime.isValid());
        QTRY_VERIFY_WITH_TIMEOUT(firstReady.count() > 1, 5000);
        QVERIFY(!db.loadEntry(a.filename)->lastClosedTime.isValid());
        window.reset();
        QVERIFY(db.loadEntry(a.filename)->lastClosedTime.isValid());
        QCOMPARE(ReaderHistory::lastClosedBooks(2).first().filename, a.filename);
        QVERIFY(server.unregisterReaderSession(QStringLiteral("history-reader")));
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    const bool probe = argc == 3 && QByteArray(argv[1]) == "--quit-probe";
    const QByteArray home = probe ? QByteArray(argv[2]) : QFile::encodeName(environment.path());
    for (const char *name : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) qputenv(name, home);
    qputenv("SPEECHD_ADDRESS", "unix_socket:/nonexistent/reader-history-test");
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("arianna-reader-history-test"));
    KLocalizedString::setApplicationDomain("arianna");
    if (probe) {
        const auto book = createBook(QString::fromLocal8Bit(argv[2]) + QStringLiteral("/quit.epub"));
        auto history = std::make_unique<ReaderHistory>();
        QObject reader;
        history->trackReader(&reader, book.filename);
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        app.exec();
        const auto before = BookDatabase::self().loadEntry(book.filename);
        if (!before || !before->lastClosedTime.isValid()) return 1;
        history.reset();
        const auto after = BookDatabase::self().loadEntry(book.filename);
        return after && after->lastClosedTime == before->lastClosedTime ? 0 : 2;
    }
    ReaderHistoryTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "readerhistorytest.moc"
