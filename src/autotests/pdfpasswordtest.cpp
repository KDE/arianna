// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookdatabase.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "categoryentriesmodel.h"
#include "config.h"
#include "pdfpasswordstore.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSignalSpy>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtQml/QQmlExtensionPlugin>
#include <memory>

#include <podofo/main/PdfEncrypt.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
const QString password = QStringLiteral("synthetic-reader-password");

QVariant evaluate(QObject *object, const QString &code)
{
    QQmlExpression expression(qmlContext(object), object, code);
    const auto result = expression.evaluate();
    if (expression.hasError()) QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
    return result;
}

QByteArray digest(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
}

// The real KF6::Wallet client talks to this peer on a private bus. Only synthetic
// credentials live here; the user's wallet and its access settings are untouched.
class WalletPeer : public QDBusVirtualObject
{
public:
    QHash<QString, QString> passwords;
    int writes = 0;
    int reads = 0;
    int opens = 0;
    bool open = true;
    bool accessAllowed = true;
    int delay = 20;
    QStringList unexpectedCalls;

    void notify(const QString &member, const QVariantList &arguments)
    {
        auto signal = QDBusMessage::createSignal(QStringLiteral("/modules/kwalletd6"), QStringLiteral("org.kde.KWallet"), member);
        signal.setArguments(arguments);
        QDBusConnection::sessionBus().send(signal);
    }

    void changed() { notify(QStringLiteral("folderUpdated"), {QStringLiteral("kdewallet"), QStringLiteral("arianna")}); }

    QString introspect(const QString &) const override
    {
        return QStringLiteral("<interface name='org.kde.KWallet'/>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        const auto method = message.member();
        const auto args = message.arguments();
        QVariant result;
        if (method == QStringLiteral("isOpen")) {
            result = open;
        } else if (method == QStringLiteral("isEnabled")
            || method == QStringLiteral("hasFolder") || method == QStringLiteral("createFolder")) {
            result = true;
        } else if (method == QStringLiteral("localWallet") || method == QStringLiteral("networkWallet")) {
            result = QStringLiteral("kdewallet");
        } else if (method == QStringLiteral("keyDoesNotExist") || method == QStringLiteral("hasEntry")) {
            const bool found = passwords.contains(args.at(2).toString());
            result = method == QStringLiteral("hasEntry") ? found : !found;
        } else if (method == QStringLiteral("openAsync")) {
            ++opens;
            static int transaction = 0;
            const int id = ++transaction;
            result = id;
            QTimer::singleShot(delay, this, [this, id, connection] {
                auto opened = QDBusMessage::createSignal(QStringLiteral("/modules/kwalletd6"), QStringLiteral("org.kde.KWallet"), QStringLiteral("walletAsyncOpened"));
                if (accessAllowed) open = true;
                opened.setArguments({id, accessAllowed ? id + 100 : -1});
                connection.send(opened);
            });
        } else if (method == QStringLiteral("writePassword")) {
            passwords.insert(args.at(2).toString(), args.at(3).toString());
            ++writes;
            result = 0;
        } else if (method == QStringLiteral("readPassword")) {
            result = passwords.value(args.at(2).toString());
            ++reads;
        } else if (method == QStringLiteral("close")) {
            result = 0;
        } else if (method == QStringLiteral("sync")) {
            return connection.send(message.createReply());
        } else {
            unexpectedCalls.append(method);
            return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"), method));
        }
        return connection.send(message.createReply(result));
    }
};

class ReaderProbe : public QObject
{
    Q_OBJECT
public:
    QString fileName;
    QString bookId;
    QString token;
    QString mode;

private Q_SLOTS:
    void encryptedPdf()
    {
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
        engine.rootContext()->setContextProperty(QStringLiteral("startupDirectReaderMode"), true);
        engine.rootContext()->setContextProperty(QStringLiteral("serverToken"), QStringLiteral("pdf-password-server"));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), token);
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerAddress"), BookServerConfig::address());
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerPort"), BookServerConfig::port());
        engine.rootContext()->setContextProperty(QStringLiteral("testFile"), fileName);
        engine.rootContext()->setContextProperty(QStringLiteral("testPassword"), password);
        const auto entry = BookDatabase::self().loadEntry(fileName);
        QVERIFY(entry);
        QCOMPARE(entry->uniqueIdentifier, bookId);
        engine.rootContext()->setContextProperty(QStringLiteral("testEntry"), QVariant::fromValue(*entry));
        QQmlComponent component(&engine);
        component.loadFromModule(QStringLiteral("org.kde.arianna"), QStringLiteral("Main"));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *viewer = evaluate(window.get(), QStringLiteral("openBookPage(testFile,'','',testEntry,false);"
            "var page=pageStack.layers.currentItem; page && page.item ? page.item : page")).value<QObject *>();
        QVERIFY(viewer);
        if (mode.startsWith(QStringLiteral("legacy-"))) {
            evaluate(viewer, QStringLiteral("passwordHandler.source = url"));
        }
        auto *store = viewer->findChild<PdfPasswordStore *>();
        QVERIFY(store);
        if (!mode.startsWith(QStringLiteral("legacy-"))) {
            QCOMPARE(store->source(), QUrl::fromLocalFile(fileName));
        }
        QSignalSpy saved(store, &PdfPasswordStore::saved);
        QSignalSpy failed(store, &PdfPasswordStore::failed);
        auto *dialog = viewer->findChild<QObject *>(QStringLiteral("pdfPasswordDialog"));
        QVERIFY(dialog);
        QObject *handler = evaluate(viewer, QStringLiteral("passwordHandler")).value<QObject *>();
        QVERIFY(handler);
        QSignalSpy prompts(handler, SIGNAL(passwordRequested(QString)));
        QVERIFY(prompts.isValid());
        if (mode.endsWith(QStringLiteral("save"))) {
            QTRY_VERIFY_WITH_TIMEOUT(dialog->property("visible").toBool(), 5000);
            evaluate(viewer, QStringLiteral("passwordHandler.submit(testPassword, true)"));
            QTRY_VERIFY_WITH_TIMEOUT(viewer->property("documentReady").toBool(), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 5000);
            QCOMPARE(failed.count(), 0);
        } else if (mode.endsWith(QStringLiteral("prompt"))) {
            QTRY_VERIFY_WITH_TIMEOUT(dialog->property("visible").toBool(), 5000);
            QVERIFY(!viewer->property("documentReady").toBool());
            QCOMPARE(saved.count(), 0);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(viewer->property("documentReady").toBool(), 5000);
            QCOMPARE(prompts.count(), 0);
            QVERIFY(!dialog->property("visible").toBool());
            QCOMPARE(failed.count(), 0);
        }
    }
};

class PdfPasswordTest : public QObject
{
    Q_OBJECT
    WalletPeer wallet;
    QTemporaryDir files;
    std::unique_ptr<BookServer> server;

    BookEntry createPdf(const QString &name)
    {
        BookEntry entry;
        entry.filename = files.filePath(name);
        entry.title = name;
        entry.uniqueIdentifier = QStringLiteral("arianna-pdf-") + QString::fromLatin1(QCryptographicHash::hash(entry.filename.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
        entry.identifier = entry.uniqueIdentifier;
        entry.passwordProtected = true;
        PoDoFo::PdfMemDocument pdf;
        pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        pdf.SetEncrypted(password.toStdString(), "synthetic-owner-password", PoDoFo::PdfPermissions::None,
                         PoDoFo::PdfEncryptionAlgorithm::AESV2, PoDoFo::PdfKeyLength::L128);
        pdf.Save(entry.filename.toStdString(), PoDoFo::PdfSaveOptions::NoMetadataUpdate);
        BookDatabase::self().addEntry(entry);
        return entry;
    }

    void runReader(const BookEntry &entry, const QString &mode, const QString &token)
    {
        server->registerReaderSession(token);
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--reader"), entry.filename, token,
            QString::number(BookServerConfig::port()), entry.uniqueIdentifier, mode});
        QVERIFY(reader.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(reader.state() == QProcess::NotRunning, 15000);
        const QByteArray output = reader.readAllStandardOutput() + reader.readAllStandardError();
        QVERIFY2(reader.exitStatus() == QProcess::NormalExit && reader.exitCode() == 0, output.constData());
        QVERIFY(server->unregisterReaderSession(token));
    }

    void savePassword(const BookEntry &entry, const QString &value = password)
    {
        PdfPasswordStore store;
        store.setFileName(entry.filename);
        store.setBookId(entry.uniqueIdentifier);
        QSignalSpy saved(&store, &PdfPasswordStore::saved);
        QSignalSpy failed(&store, &PdfPasswordStore::failed);
        store.save(value);
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
    }

    std::unique_ptr<QObject> cover(QQmlEngine &engine, const BookEntry &entry, bool thumbnail = false)
    {
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testFile"), entry.filename);
        engine.rootContext()->setContextProperty(QStringLiteral("testBookId"), entry.uniqueIdentifier);
        engine.rootContext()->setContextProperty(QStringLiteral("testThumbnail"), thumbnail);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 300; height: 430; visible: true
                GridBrowserDelegate {
                    objectName: "coverDelegate"
                    anchors.fill: parent
                    imageUrl: testThumbnail ? Qt.resolvedUrl("qrc:/qt/qml/org/kde/arianna/qml/icons/pdf_icon.png") : ""
                    iconName: "application-pdf"
                    mainText: "Protected PDF"; secondaryText: ""; currentProgress: 0
                    showPasswordBadge: true
                    passwordFileName: testFile
                    passwordBookId: testBookId
                }
            }
        )", QUrl());
        if (!component.isReady()) {
            QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
            return {};
        }
        auto window = std::unique_ptr<QObject>(component.create());
        if (!window) QTest::qFail(qPrintable(component.errorString()), __FILE__, __LINE__);
        return window;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(files.isValid());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QVERIFY(bus.registerVirtualObject(QStringLiteral("/modules/kwalletd6"), &wallet));
        QVERIFY(bus.registerService(QStringLiteral("org.kde.kwalletd6")));
        QTcpServer port;
        QVERIFY(port.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerPort(port.serverPort());
        port.close();
        server = std::make_unique<BookServer>(QStringLiteral("pdf-password-server"));
        QVERIFY(server->isRunning());
        // Keep the test server alive between individual reader processes.
        server->registerReaderSession(QStringLiteral("test-owner"));
    }

    void init()
    {
        wallet.passwords.clear();
        wallet.writes = 0;
        wallet.reads = 0;
        wallet.opens = 0;
        wallet.open = true;
        wallet.accessAllowed = true;
        wallet.delay = 20;
        wallet.unexpectedCalls.clear();
        wallet.changed();
    }

    void restartWithDifferentTokenAndRenamedCopy()
    {
        const auto original = createPdf(QStringLiteral("Geschütztes PDF #1.pdf"));
        const auto before = digest(original.filename);
        runReader(original, QStringLiteral("save"), QStringLiteral("session-one"));
        QCOMPARE(wallet.writes, 1);
        QCOMPARE(wallet.passwords.size(), 1);
        runReader(original, QStringLiteral("load"), QStringLiteral("session-two"));
        QCOMPARE(wallet.reads, 1);
        QCOMPARE(wallet.writes, 1);
        auto copy = original;
        copy.filename = files.filePath(QStringLiteral("renamed copy.pdf"));
        copy.uniqueIdentifier = QStringLiteral("arianna-pdf-renamed");
        copy.identifier = copy.uniqueIdentifier;
        QVERIFY(QFile::copy(original.filename, copy.filename));
        BookDatabase::self().addEntry(copy);
        runReader(copy, QStringLiteral("load"), QStringLiteral("session-three"));
        QCOMPARE(wallet.reads, 2);
        QCOMPARE(wallet.passwords.size(), 1);
        QCOMPARE(digest(original.filename), before);
        QCOMPARE(digest(copy.filename), before);
        QVERIFY(wallet.unexpectedCalls.isEmpty());
    }

    void oldSessionUrlKeyDoesNotMatchAfterRestart()
    {
        const auto original = createPdf(QStringLiteral("legacy.pdf"));
        runReader(original, QStringLiteral("legacy-save"), QStringLiteral("old-token-one"));
        QCOMPARE(wallet.writes, 1);
        runReader(original, QStringLiteral("legacy-prompt"), QStringLiteral("old-token-two"));
        QCOMPARE(wallet.reads, 0);
    }

    void wrongSavedPasswordStillRequiresUserInput()
    {
        const auto original = createPdf(QStringLiteral("wrong-password.pdf"));
        const auto before = digest(original.filename);
        runReader(original, QStringLiteral("save"), QStringLiteral("wrong-one"));
        QCOMPARE(wallet.passwords.size(), 1);
        wallet.passwords.begin().value() = QStringLiteral("incorrect-synthetic-password");
        runReader(original, QStringLiteral("prompt"), QStringLiteral("wrong-two"));
        QCOMPARE(wallet.reads, 1);
        QCOMPARE(wallet.writes, 1);
        QCOMPARE(digest(original.filename), before);
    }

    void coverValidatesPasswordAndTracksWalletChanges()
    {
        const auto entry = createPdf(QStringLiteral("cover-valid.pdf"));
        const auto before = digest(entry.filename);
        savePassword(entry);
        QQmlEngine engine;
        auto window = cover(engine, entry);
        QVERIFY(window);
        auto *store = window->findChild<PdfPasswordStore *>();
        auto *image = window->findChild<QObject *>(QStringLiteral("lockedPdfCoverIcon"));
        QVERIFY(store && image);
        QSignalSpy loaded(store, &PdfPasswordStore::loaded);
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid(), 5000);
        QTRY_VERIFY(image->property("source").toUrl().path().endsWith(QStringLiteral("unlocked-pdf-cover.svg")));
        QTRY_COMPARE(image->property("status").toInt(), 1); // Image.Ready
        QVERIFY(image->property("visible").toBool());
        QCOMPARE(loaded.count(), 0); // Cover QML never receives the credential.
        QCOMPARE(wallet.reads, 1);

        // Recreating a shelf tile reuses only the validation result.
        QQmlEngine secondEngine;
        auto second = cover(secondEngine, entry, true);
        QVERIFY(second);
        auto *secondStore = second->findChild<PdfPasswordStore *>();
        auto *badge = second->findChild<QObject *>(QStringLiteral("passwordBadge"));
        auto *icon = second->findChild<QObject *>(QStringLiteral("passwordBadgeIcon"));
        QVERIFY(secondStore && badge && icon);
        QTRY_VERIFY_WITH_TIMEOUT(secondStore->passwordValid(), 5000);
        QTRY_VERIFY(badge->property("visible").toBool());
        QCOMPARE(icon->property("source").toString(), QStringLiteral("object-unlocked-symbolic"));
        QCOMPARE(wallet.reads, 1);

        wallet.passwords.begin().value() = QStringLiteral("incorrect-password");
        wallet.changed();
        QTRY_VERIFY(!store->passwordValid() && !secondStore->passwordValid());
        QTRY_VERIFY_WITH_TIMEOUT(wallet.reads >= 2 && !store->busy() && !secondStore->busy(), 5000);
        QVERIFY(!store->passwordValid() && !secondStore->passwordValid());
        QCOMPARE(icon->property("source").toString(), QStringLiteral("object-locked-symbolic"));
        QVERIFY(image->property("source").toUrl().path().endsWith(QStringLiteral("/locked-pdf-cover.svg")));

        savePassword(entry);
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid() && secondStore->passwordValid(), 5000);
        wallet.passwords.clear();
        wallet.notify(QStringLiteral("folderListUpdated"), {QStringLiteral("kdewallet")});
        QTRY_VERIFY(!store->passwordValid() && !secondStore->passwordValid());
        QTRY_VERIFY_WITH_TIMEOUT(!store->busy() && !secondStore->busy(), 5000);
        QCOMPARE(digest(entry.filename), before);
        QVERIFY(wallet.unexpectedCalls.isEmpty());
    }

    void coverWithoutPasswordOrWithDeniedAccessStaysClosed()
    {
        const auto entry = createPdf(QStringLiteral("cover-denied.pdf"));
        QQmlEngine engine;
        auto window = cover(engine, entry);
        QVERIFY(window);
        auto *store = window->findChild<PdfPasswordStore *>();
        QVERIFY(store);
        QSignalSpy checked(store, &PdfPasswordStore::checked);
        QTRY_VERIFY_WITH_TIMEOUT(checked.count() >= 1, 5000);
        QVERIFY(!store->passwordValid());
        QCOMPARE(wallet.opens, 0);

        savePassword(entry);
        wallet.accessAllowed = false;
        wallet.open = true; // An unlocked wallet can still deny this application.
        checked.clear();
        store->check(true);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 5000);
        QVERIFY(!store->passwordValid());
        QVERIFY(wallet.opens >= 2);
        const int opens = wallet.opens;
        checked.clear();
        store->check(false);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 5000);
        QCOMPARE(wallet.opens, opens); // No permission-dialog loop on refocus.
    }

    void bookDetailsUsesTheSameValidatedStatus()
    {
        const auto entry = createPdf(QStringLiteral("details.pdf"));
        savePassword(entry);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testEntry"), QVariant::fromValue(entry));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 700; height: 800; visible: true
                pageStack.initialPage: BookDetailsPage {
                    metadata: testEntry
                    readOnly: true
                }
            }
        )", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *store = window->findChild<PdfPasswordStore *>();
        auto *badge = window->findChild<QObject *>(QStringLiteral("passwordBadge"));
        auto *icon = window->findChild<QObject *>(QStringLiteral("passwordBadgeIcon"));
        QVERIFY(store && badge && icon);
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid(), 5000);
        QVERIFY(badge->property("visible").toBool());
        QCOMPARE(icon->property("source").toString(), QStringLiteral("object-unlocked-symbolic"));
        wallet.passwords.clear();
        wallet.changed();
        QTRY_VERIFY(!store->passwordValid());
        QCOMPARE(icon->property("source").toString(), QStringLiteral("object-locked-symbolic"));
    }

    void coverCanUnlockButDoesNotReopenClosedWallet()
    {
        const auto entry = createPdf(QStringLiteral("cover-unlock.pdf"));
        savePassword(entry);
        wallet.open = false;
        QQmlEngine engine;
        auto window = cover(engine, entry);
        QVERIFY(window);
        auto *store = window->findChild<PdfPasswordStore *>();
        QVERIFY(store);
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid(), 5000);
        QVERIFY(wallet.open);
        const int opens = wallet.opens;
        wallet.open = false;
        wallet.notify(QStringLiteral("walletClosed"), {QStringLiteral("kdewallet")});
        QTRY_VERIFY(!store->passwordValid());
        QTest::qWait(250);
        QCOMPARE(wallet.opens, opens);
        wallet.open = true;
        wallet.notify(QStringLiteral("walletOpened"), {QStringLiteral("kdewallet")});
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid(), 5000);
    }

    void changedOrMissingPdfInvalidatesCover()
    {
        auto entry = createPdf(QStringLiteral("cover-change.pdf"));
        entry.uniqueIdentifier = QStringLiteral("authoritative-pdf-identifier");
        entry.identifier = entry.uniqueIdentifier;
        BookDatabase::self().updateEntry(entry);
        savePassword(entry);
        QQmlEngine engine;
        auto window = cover(engine, entry);
        QVERIFY(window);
        auto *store = window->findChild<PdfPasswordStore *>();
        QVERIFY(store);
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid(), 5000);
        // Same path/identifier, different original content and password.
        QVERIFY(QFile::remove(entry.filename));
        PoDoFo::PdfMemDocument other;
        other.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
        other.SetEncrypted("other-password", "other-owner", PoDoFo::PdfPermissions::None,
                           PoDoFo::PdfEncryptionAlgorithm::AESV2, PoDoFo::PdfKeyLength::L128);
        other.Save(entry.filename.toStdString());
        QTRY_VERIFY(!store->passwordValid());
        QSignalSpy checked(store, &PdfPasswordStore::checked);
        QTRY_VERIFY_WITH_TIMEOUT(checked.count() >= 1, 5000);
        QVERIFY(!store->passwordValid());
        savePassword(entry, QStringLiteral("other-password"));
        QTRY_VERIFY_WITH_TIMEOUT(store->passwordValid(), 5000);
        const int previousChecks = checked.count();
        QVERIFY(QFile::remove(entry.filename));
        QTRY_VERIFY(!store->passwordValid());
        QTRY_VERIFY_WITH_TIMEOUT(checked.count() > previousChecks && !store->busy(), 5000);
    }

    void hiddenOrReusedTileIgnoresPendingReply()
    {
        const auto entry = createPdf(QStringLiteral("cover-reused.pdf"));
        savePassword(entry);
        wallet.delay = 400;
        QQmlEngine engine;
        auto window = cover(engine, entry);
        QVERIFY(window);
        auto *store = window->findChild<PdfPasswordStore *>();
        auto *tile = window->findChild<QObject *>(QStringLiteral("coverDelegate"));
        QVERIFY(store && tile);
        QTRY_VERIFY_WITH_TIMEOUT(wallet.opens == 2, 5000);
        tile->setProperty("visible", false);
        QTest::qWait(500);
        QVERIFY(!store->passwordValid());
        QVERIFY(!store->busy());
        QCOMPARE(wallet.reads, 0);
        tile->setProperty("passwordFileName", files.filePath(QStringLiteral("missing.pdf")));
        tile->setProperty("visible", true);
        QSignalSpy checked(store, &PdfPasswordStore::checked);
        QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 5000);
        QVERIFY(!store->passwordValid());
    }
};

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("ARIANNA_PRIVATE_WALLET_TEST")) return 2;
    const bool reader = argc == 7 && QByteArray(argv[1]) == "--reader";
    QTemporaryDir environment;
    if (!reader) {
        for (const char *name : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
            qputenv(name, QFile::encodeName(environment.path()));
        QFile config(environment.filePath(QStringLiteral("kwalletrc")));
        if (!config.open(QIODevice::WriteOnly)) return 3;
        config.write("[Wallet]\nEnabled=true\nDefault Wallet=kdewallet\nUse One Wallet=true\n");
    }
    qputenv("SPEECHD_ADDRESS", "unix_socket:/nonexistent/pdf-password-test");
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("arianna-pdf-password-test"));
    KLocalizedString::setApplicationDomain("arianna");
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    if (reader) {
        Config::setBookServerPort(QByteArray(argv[4]).toInt());
        ReaderProbe probe;
        probe.fileName = QString::fromLocal8Bit(argv[2]);
        probe.token = QString::fromLocal8Bit(argv[3]);
        probe.bookId = QString::fromLocal8Bit(argv[5]);
        probe.mode = QString::fromLocal8Bit(argv[6]);
        const int result = QTest::qExec(&probe, QStringList{QString::fromLocal8Bit(argv[0])});
        // Drain the real wallet worker through its ordinary application-exit path.
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
        app.exec();
        return result;
    }
    PdfPasswordTest test;
    const int result = QTest::qExec(&test, argc, argv);
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    app.exec();
    return result;
}

#include "pdfpasswordtest.moc"
