// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookdatabase.h"
#include "categoryentriesmodel.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "externalprocess.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>
#include <memory>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

class NoLibraryImportTest : public QObject
{
    Q_OBJECT
    QVariant evaluate(QObject *object, const QString &code)
    {
        QQmlExpression expression(qmlContext(object), object, code);
        auto result = expression.evaluate();
        if (expression.hasError()) QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
        return result;
    }
private Q_SLOTS:
    void externalEditingAndReadOnlyStaySeparate()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto file = directory.filePath(QStringLiteral("outside library.epub"));
        KZip zip(file);
        QVERIFY(zip.open(QIODevice::WriteOnly));
        QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
        QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
            "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles>"
            "<rootfile full-path='package.opf'/></rootfiles></container>"));
        QVERIFY(zip.writeFile(QStringLiteral("package.opf"),
            "<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'>"
            "<metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:identifier id='id'>outside</dc:identifier>"
            "<dc:title>Outside</dc:title><dc:language>en</dc:language></metadata>"
            "<manifest><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest>"
            "<spine><itemref idref='chapter'/></spine></package>"));
        QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"),
            "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Outside</title></head><body><p>Selected text.</p></body></html>"));
        QVERIFY(zip.close());

        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
        Config::setBookServerPort(port.serverPort());
        port.close();
        BookServer server(QStringLiteral("no-import-server"));
        QVERIFY(server.isRunning());
        server.registerReaderSession(QStringLiteral("no-import-reader"));
        QNetworkAccessManager network;
        QNetworkRequest registration{QUrl(BookServerConfig::baseUrl() + QStringLiteral("/read-only-books"))};
        registration.setRawHeader("X-Arianna-Session-Token", "no-import-reader");
        registration.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        std::unique_ptr<QNetworkReply> reply(network.post(registration,
            QJsonDocument(QJsonObject{{QStringLiteral("fileName"), file}}).toJson()));
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        const auto identifier = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("identifier")).toString();
        QVERIFY(!identifier.isEmpty());

        const auto record = directory.filePath(QStringLiteral("editor-arguments.json"));
        const auto command = directory.filePath(QStringLiteral("editor with spaces"));
        QFile editor(command);
        QVERIFY(editor.open(QIODevice::WriteOnly));
        editor.write("#!/usr/bin/python3\nimport json,sys,time,zipfile\nfrom pathlib import Path\n");
        editor.write((QStringLiteral("Path(%1).write_text(json.dumps(sys.argv[1:]))\n")
            .arg(QString::fromUtf8(QJsonDocument(QJsonArray{record}).toJson(QJsonDocument::Compact)).mid(1).chopped(1))).toUtf8());
        editor.write("with zipfile.ZipFile(sys.argv[1], 'a') as z: z.comment=b'explicit external editor save'\ntime.sleep(2)\n");
        editor.close();
        QVERIFY(editor.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        Config::setEditorCommand(command);

        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
        engine.rootContext()->setContextProperty(QStringLiteral("startupDirectReaderMode"), true);
        engine.rootContext()->setContextProperty(QStringLiteral("serverToken"), QStringLiteral("no-import-server"));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("no-import-reader"));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerAddress"), BookServerConfig::address());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerPort"), BookServerConfig::port());
        QQmlComponent component(&engine);
        component.loadFromModule(QStringLiteral("org.kde.arianna"), QStringLiteral("Main"));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        const auto encodedFile = QString::fromUtf8(QJsonDocument(QJsonArray{file}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
        const auto entry = QStringLiteral("{filename:%1,uniqueIdentifier:'%2',title:'Outside'}").arg(encodedFile,identifier);
        const auto open = [&](bool readOnly, bool external) {
            return evaluate(window.get(), QStringLiteral("openBookPage(%1,'','',%2,%3,'',null,%4); var layer=pageStack.layers.currentItem; layer && layer.item ? layer.item : layer")
                .arg(encodedFile,entry,readOnly?QStringLiteral("true"):QStringLiteral("false"),external?QStringLiteral("true"):QStringLiteral("false"))).value<QObject *>();
        };
        QObject *viewer = open(false,true);
        QVERIFY(viewer);
        QVERIFY(viewer->property("noLibraryImport").toBool());
        QVERIFY(!viewer->property("readOnly").toBool());
        QVERIFY(!viewer->property("libraryManaged").toBool());
        QVERIFY(evaluate(viewer,QStringLiteral("canEditBook()")).toBool());
        QVERIFY(!BookDatabase::self().loadEntry(file));
        auto *external = engine.singletonInstance<ExternalProcess *>("org.kde.arianna","ExternalProcess");
        QVERIFY(external);
        QSignalSpy reload(external,&ExternalProcess::editedFileChanged);
        QSignalSpy imports(external,&ExternalProcess::editorSessionFileChanged);
        QVERIFY(evaluate(viewer,QStringLiteral("editBook('Selected text.')")).toBool());
        QTRY_VERIFY(QFile::exists(record));
        QFile args(record);QVERIFY(args.open(QIODevice::ReadOnly));
        const auto actual = QJsonDocument::fromJson(args.readAll()).array();
        QCOMPARE(actual.first().toString(),file);
        QCOMPARE(actual.at(1).toString(),QStringLiteral("--select-text"));
        QCOMPARE(actual.at(2).toString(),QStringLiteral("Selected text."));
        QTRY_VERIFY_WITH_TIMEOUT(reload.count()>0,5000);
        QCOMPARE(imports.count(),0);
        QVERIFY(!BookDatabase::self().loadEntry(file));
        // Metadata/position signals and manual refresh must not import the file.
        evaluate(viewer,QStringLiteral("locationsLoaded('temporary');zoomLevelSaved(1.5);pageModeSaved('single');refreshLibraryEntryFromFile()"));
        QVERIFY(!BookDatabase::self().loadEntry(file));
        QTest::qWait(2100);
        evaluate(window.get(),QStringLiteral("pageStack.layers.pop()"));
        viewer=open(true,false);
        QVERIFY(viewer);
        QVERIFY(!evaluate(viewer,QStringLiteral("canEditBook()")).toBool());
        QVERIFY(!evaluate(viewer,QStringLiteral("editBook()")).toBool());
        evaluate(window.get(),QStringLiteral("pageStack.layers.pop()"));
        viewer=open(true,true);
        QVERIFY(viewer);
        QVERIFY(!evaluate(viewer,QStringLiteral("canEditBook()")).toBool());
        QVERIFY(!evaluate(viewer,QStringLiteral("editBook()")).toBool());
        QVERIFY(!BookDatabase::self().loadEntry(file));
        QVERIFY(server.unregisterReaderSession(QStringLiteral("no-import-reader")));
    }
};

int main(int argc,char **argv)
{
    QTemporaryDir environment;
    for(const char *name:{"XDG_DATA_HOME","XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_RUNTIME_DIR"})
        qputenv(name,QFile::encodeName(environment.path()));
    qputenv("SPEECHD_ADDRESS","unix_socket:/nonexistent/no-import-test");
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc,argv);
    KLocalizedString::setApplicationDomain("arianna");
    app.setApplicationName(QStringLiteral("arianna-no-library-test"));
    NoLibraryImportTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "nolibraryimporttest.moc"
