// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"

#include <KZip>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>

class BookServerCacheTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    const QByteArray chapter = "<html xmlns='http://www.w3.org/1999/xhtml'><body><p>नित्यो śrī</p></body></html>";
    const QByteArray media = "0123456789-media";

    QString fixture(int index)
    {
        const auto path = directory.filePath(QStringLiteral("cache-%1.epub").arg(index));
        if (QFile::exists(path))
            return path;
        KZip zip(path);
        if (!zip.open(QIODevice::WriteOnly) || !zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip")
            || !zip.writeFile(QStringLiteral("META-INF/container.xml"),
                              "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles>"
                              "<rootfile full-path='OEBPS/package.opf'/></rootfiles></container>")
            || !zip.writeFile(QStringLiteral("OEBPS/package.opf"),
                              "<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>"
                              "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>cache</dc:identifier>"
                              "<dc:title>Cache</dc:title></metadata><manifest>"
                              "<item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/>"
                              "<item id='media' href='media.mp3' media-type='audio/mpeg'/></manifest>"
                              "<spine><itemref idref='chapter'/></spine></package>")
            || !zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), chapter) || !zip.writeFile(QStringLiteral("OEBPS/media.mp3"), media) || !zip.close())
            return {};
        return path;
    }

    std::shared_ptr<EPubContainer> load(BookServer &server, int index)
    {
        const auto id = QStringLiteral("cache-%1").arg(index);
        server.m_readOnlyFilesByIdentifier.insert(id, fixture(index));
        return server.containerForIdentifier(id);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME", QFile::encodeName(directory.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(directory.path()));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(directory.path()));
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(directory.path()));
        QCoreApplication::setApplicationName(QStringLiteral("arianna-bookservercachetest"));
    }

    void init()
    {
        Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerPort(port.serverPort());
    }

    void evictsLeastRecentlyUsedArchive()
    {
        BookServer server(QStringLiteral("cache-test"));
        QVERIFY(server.isRunning());
        QList<std::weak_ptr<EPubContainer>> observed;
        for (int i = 0; i < BookServer::MaxCachedContainers; ++i) {
            const auto container = load(server, i);
            QVERIFY(container);
            observed.append(container);
        }
        // A hit must refresh recency rather than evicting the first-opened book.
        QVERIFY(load(server, 0));
        QVERIFY(load(server, BookServer::MaxCachedContainers));
        QVERIFY(!observed[0].expired());
        QVERIFY(observed[1].expired());
        for (qsizetype i = 2; i < observed.size(); ++i)
            QVERIFY(!observed[i].expired());
        QCOMPARE(server.m_containerCache.size(), BookServer::MaxCachedContainers);
        QVERIFY(load(server, BookServer::MaxCachedContainers + 1));
        QVERIFY(observed[2].expired());
        QCOMPARE(server.m_containerCache.size(), BookServer::MaxCachedContainers);
    }

    void targetLookupWithoutStoredStructureKeepsArchiveAndSkipsAnalysis()
    {
        BookServer server(QStringLiteral("cache-test"));
        QVERIFY(server.isRunning());
        const auto container = load(server, 0);
        QVERIFY(container);
        QVERIFY(!container->property("workStructureSnapshot").isValid());
        const auto before = container->getFileHash();
        QNetworkAccessManager network;
        const auto port = server.server.servers().first()->serverPort();
        QNetworkRequest request{QUrl(QStringLiteral("http://127.0.0.1:%1/cache-0/targetLocations").arg(port))};
        request.setRawHeader("X-Arianna-Session-Token", "cache-test");
        const std::unique_ptr<QNetworkReply> reply(network.get(request));
        QSignalSpy finished(reply.get(), &QNetworkReply::finished);
        QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        const auto targets = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("targetLocations")).toArray();
        QCOMPARE(targets.size(), 1);
        QCOMPARE(targets.first().toObject().value(QStringLiteral("hrefLocation")).toString(), QStringLiteral("OEBPS/chapter.xhtml"));
        // No implicit structure scan/reopen and no source mutation for an ordinary lookup.
        QCOMPARE(server.containerForIdentifier(QStringLiteral("cache-0")).get(), container.get());
        QVERIFY(!container->property("workStructureSnapshot").isValid());
        QNetworkRequest stateRequest{QUrl(QStringLiteral("http://127.0.0.1:%1/cache-0/workStructure").arg(port))};
        stateRequest.setRawHeader("X-Arianna-Session-Token", "cache-test");
        const std::unique_ptr<QNetworkReply> stateReply(network.get(stateRequest));
        QTRY_VERIFY_WITH_TIMEOUT(stateReply->isFinished(), 5000);
        QCOMPARE(stateReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(QJsonDocument::fromJson(stateReply->readAll()).object().value(QStringLiteral("validity")).toString(), QStringLiteral("absent"));
        QNetworkRequest resolveRequest{QUrl(QStringLiteral("http://127.0.0.1:%1/cache-0/workStructure/resolve").arg(port))};
        resolveRequest.setRawHeader("X-Arianna-Session-Token", "cache-test");
        resolveRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const std::unique_ptr<QNetworkReply> resolveReply(network.post(resolveRequest, QByteArrayLiteral("{\"text\":\"1-2\",\"role\":\"navigation\"}")));
        QTRY_VERIFY_WITH_TIMEOUT(resolveReply->isFinished(), 5000);
        QCOMPARE(resolveReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(!QJsonDocument::fromJson(resolveReply->readAll()).object().value(QStringLiteral("resolved")).toBool());
        QCOMPARE(server.containerForIdentifier(QStringLiteral("cache-0")).get(), container.get());
        QVERIFY(!container->property("workStructureSnapshot").isValid());
        QCOMPARE(container->getFileHash(), before);
    }

    void inFlightArchiveLivesUntilItsConsumerFinishes()
    {
        BookServer server(QStringLiteral("cache-test"));
        QVERIFY(server.isRunning());
        auto inFlight = load(server, 0);
        QVERIFY(inFlight);
        QPointer<EPubContainer> original(inFlight.get());
        const auto hash = inFlight->getFileHash();
        for (int i = 1; i <= BookServer::MaxCachedContainers; ++i)
            QVERIFY(load(server, i));
        QVERIFY(!server.m_containerCache.contains(QStringLiteral("cache-0")));
        QVERIFY(original);
        QCOMPARE(inFlight->readData(QStringLiteral("OEBPS/chapter.xhtml")), chapter);
        inFlight.reset();
        QVERIFY(original.isNull());
        auto reopened = server.containerForIdentifier(QStringLiteral("cache-0"));
        QVERIFY(reopened);
        QCOMPARE(reopened->getFileHash(), hash);
        QCOMPARE(reopened->readData(QStringLiteral("OEBPS/chapter.xhtml")), chapter);
    }

    void mediaUrlsAndSessionIsolationSurviveEviction()
    {
        BookServer server(QStringLiteral("cache-test"));
        QVERIFY(server.isRunning());
        Config::setBookServerPort(server.server.servers().first()->serverPort());
        server.registerReaderSession(QStringLiteral("reader-one"));
        server.registerReaderSession(QStringLiteral("reader-two"));
        server.registerReaderSessionForIdentifier(QStringLiteral("reader-one"), QStringLiteral("cache-0"));
        QVERIFY(load(server, 0));
        std::weak_ptr<EPubContainer> original = server.containerForIdentifier(QStringLiteral("cache-0"), QStringLiteral("edition-one"));
        QVERIFY(!original.expired());
        server.m_servedFilesByIdentifier[QStringLiteral("cache-0")] = {QStringLiteral("OEBPS/media.mp3")};
        const auto urls = server.makeResourceMap({QStringLiteral("OEBPS/media.mp3")}, QStringLiteral("cache-0"), QStringLiteral("reader-one"));
        const auto url = urls.value(QStringLiteral("OEBPS/media.mp3"));
        QVERIFY(!url.isEmpty());
        for (int i = 1; i <= BookServer::MaxCachedContainers; ++i)
            QVERIFY(load(server, i));
        QVERIFY(original.expired());
        QNetworkAccessManager manager;
        const auto fetch = [&](const QByteArray &token, int status, const QByteArray &expected, const QByteArray &range = {}) {
            QNetworkRequest request{QUrl(url)};
            request.setRawHeader("X-Arianna-Session-Token", token);
            if (!range.isEmpty())
                request.setRawHeader("Range", range);
            const std::unique_ptr<QNetworkReply> reply(manager.get(request));
            QSignalSpy finished(reply.get(), &QNetworkReply::finished);
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 5000);
            QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), status);
            if (status < 400)
                QCOMPARE(reply->readAll(), expected);
        };
        fetch("reader-one", 200, media);
        QVERIFY(server.containerForIdentifier(QStringLiteral("cache-0"), QStringLiteral("edition-one")));
        fetch("reader-one", 200, media);
        fetch("reader-one", 206, "123", "bytes=1-3");
        fetch("reader-two", 403, {});
        // Reopening retained the delivery version; an actual new version must
        // still invalidate the previous edition's media mappings.
        QVERIFY(server.containerForIdentifier(QStringLiteral("cache-0"), QStringLiteral("edition-two")));
        fetch("reader-one", 404, {});
        QVERIFY(server.unregisterReaderSession(QStringLiteral("reader-one")));
        fetch("cache-test", 404, {});
    }
};

QTEST_GUILESS_MAIN(BookServerCacheTest)
#include "bookservercachetest.moc"
