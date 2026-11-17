// SPDX-License-Identifier: GPL-2.0-or-later

#include "sigilnavigation.h"
#include "externalprocess.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

namespace
{
const QString service = QStringLiteral("org.sigil_ebook.Sigil");

QVariantMap defaultCapabilities()
{
    return {{QStringLiteral("protocolVersion"), QVariant::fromValue(uint(1))},
            {QStringLiteral("methods"), QStringList{QStringLiteral("GetCapabilities"), QStringLiteral("NavigateToText")}},
            {QStringLiteral("ready"), true},
            {QStringLiteral("navigateToText"), true},
            {QStringLiteral("openBook"), false}};
}
const QString text = QStringLiteral("धर्मक्षेत्रे कुरुक्षेत्रे\nśrī kṛṣṇaḥ \"quoted\" 'text' = value; $(touch SHOULD_NOT_EXIST) `id`");

class FakeSigil : public QDBusVirtualObject
{
public:
    QVariantMap capabilities = defaultCapabilities();
    QString capabilityError;
    QString tracePath;
    int capabilityQueries = 0;
    bool rejectMissingBook = false;
    QString errorName;
    QString openBook;
    bool found = true;
    bool silent = false;
    bool malformed = false;
    int delayMs = 0;
    QString documentError;
    QList<QDBusMessage> calls;

    QString introspect(const QString &) const override
    {
        return QStringLiteral(
            "<interface name=\"org.sigil_ebook.Editor1\"><method name=\"GetCapabilities\">"
            "<arg type=\"a{sv}\" direction=\"out\"/></method><method name=\"NavigateToText\">"
            "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
            "<arg type=\"b\" direction=\"out\"/></method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (!tracePath.isEmpty()) {
            QFile trace(tracePath);
            if (trace.open(QIODevice::WriteOnly | QIODevice::Append)) {
                trace.write(QJsonDocument(QJsonArray{message.member(), QJsonArray::fromVariantList(message.arguments())}).toJson(QJsonDocument::Compact));
                trace.write("\n");
            }
        }
        if (message.member() == QLatin1String("GetCapabilities")) {
            ++capabilityQueries;
            connection.send(capabilityError.isEmpty() ? message.createReply(QVariant::fromValue(capabilities))
                                                      : message.createErrorReply(capabilityError, QStringLiteral("Capabilities unavailable")));
            return true;
        }
        if (message.member() == QLatin1String("OpenBookAtLocation") || message.member() == QLatin1String("PrepareAnchorEdit")) {
            calls.append(message);
            const auto args = message.arguments();
            const bool anchor = message.member() == QLatin1String("PrepareAnchorEdit");
            const QString id = args.at(anchor ? 4 : 2).toString();
            QVariantMap result{{QStringLiteral("requestId"), id}, {QStringLiteral("bookPath"), args.at(0)},
                {QStringLiteral("documentFile"), args.at(1)}, {QStringLiteral("status"), QStringLiteral("Received")}};
            if (anchor) {
                result.insert(QStringLiteral("anchorId"), args.at(2));
                result.insert(QStringLiteral("verseReference"), args.at(3));
                result.insert(QStringLiteral("anchorEditPrepared"), !malformed);
            }
            auto requests = capabilities.value(QStringLiteral("documentRequests")).toMap();
            requests.insert(id, result);
            capabilities.insert(QStringLiteral("documentRequests"), requests);
            if (!silent) connection.send(message.createReply(QVariant::fromValue(result)));
            QTimer::singleShot(delayMs, this, [this, id, result]() mutable {
                result.insert(QStringLiteral("status"), documentError.isEmpty() ? QStringLiteral("DocumentOpened") : QStringLiteral("Failed"));
                result.insert(QStringLiteral("errorCode"), documentError);
                auto requests = capabilities.value(QStringLiteral("documentRequests")).toMap();
                requests.insert(id, result);
                capabilities.insert(QStringLiteral("documentRequests"), requests);
            });
            return true;
        }
        if (message.member() != QLatin1String("NavigateToText") && message.member() != QLatin1String("OpenBookAndNavigate")) {
            return false;
        }
        calls.append(message);
        if (message.member() == QLatin1String("OpenBookAndNavigate") && capabilities.value(QStringLiteral("openBook")).toBool())
            openBook = message.arguments().first().toString();
        QDBusMessage reply;
        if (!errorName.isEmpty()) {
            reply = message.createErrorReply(errorName, QStringLiteral("Test rejection"));
        } else if ((!openBook.isEmpty() || rejectMissingBook) && openBook != message.arguments().first().toString()) {
            reply = message.createErrorReply(QStringLiteral("org.sigil_ebook.Editor1.Error.BookNotOpen"), QStringLiteral("Different working copy is open"));
        } else if (malformed) {
            reply = message.createReply(QStringLiteral("not a bool"));
        } else {
            reply = message.createReply(found);
        }
        if (!silent) {
            QTimer::singleShot(delayMs, this, [connection, reply] {
                connection.send(reply);
            });
        }
        return true;
    }
};

// A separate daemon reads a real activation file and starts another process.
// The desktop bus and installed activation files are never involved.
class ActivationBus
{
public:
    QTemporaryDir directory;
    QProcess daemon;
    const QString connectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDBusConnection connection{connectionName};
    QString address;

    bool start(const QString &mode, const QString &bookPath)
    {
        if (!directory.isValid())
            return false;
        QFile activation(directory.filePath(service + QStringLiteral(".service")));
        if (!activation.open(QIODevice::WriteOnly))
            return false;
        const QString executable =
            mode == QLatin1String("activation-failed") ? directory.filePath(QStringLiteral("missing-program")) : QCoreApplication::applicationFilePath();
        activation.write(QStringLiteral("[D-BUS Service]\nName=%1\nExec=\"%2\" --activation-server\n").arg(service, executable).toUtf8());
        activation.close();
        QFile config(directory.filePath(QStringLiteral("bus.conf")));
        if (!config.open(QIODevice::WriteOnly))
            return false;
        config.write(QStringLiteral("<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>"
                                    "<servicedir>%1</servicedir><policy context=\"default\"><allow own=\"*\"/>"
                                    "<allow send_destination=\"*\"/><allow receive_sender=\"*\"/></policy></busconfig>")
                         .arg(directory.path().toHtmlEscaped())
                         .toUtf8());
        config.close();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("ARIANNA_ACTIVATION_MODE"), mode);
        environment.insert(QStringLiteral("ARIANNA_ACTIVATION_BOOK"), bookPath);
        environment.insert(QStringLiteral("ARIANNA_ACTIVATION_TRACE"), directory.filePath(QStringLiteral("calls.jsonl")));
        daemon.setProcessEnvironment(environment);
        daemon.start(QStringLiteral("dbus-daemon"),
                     {QStringLiteral("--nofork"),
                      QStringLiteral("--nopidfile"),
                      QStringLiteral("--print-address=1"),
                      QStringLiteral("--config-file=") + config.fileName()});
        if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000))
            return false;
        address = QString::fromUtf8(daemon.readLine()).trimmed();
        connection = QDBusConnection::connectToBus(address, connectionName);
        return connection.isConnected();
    }

    QList<QJsonArray> calls() const
    {
        QFile file(directory.filePath(QStringLiteral("calls.jsonl")));
        QList<QJsonArray> result;
        if (file.open(QIODevice::ReadOnly)) {
            while (!file.atEnd())
                result.append(QJsonDocument::fromJson(file.readLine()).array());
        }
        return result;
    }

    ~ActivationBus()
    {
        QDBusConnection::disconnectFromBus(connectionName);
        daemon.terminate();
        daemon.waitForFinished(3000);
    }
};
}

class SigilNavigationTest : public QObject
{
    Q_OBJECT

private:
    QDBusConnection m_server = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("arianna-fake-sigil"));
    QTemporaryDir m_directory;
    QString m_program;
    QString m_copy;
    QString m_log;
    FakeSigil m_sigil;

    bool registerSigil(bool object = true)
    {
        return m_server.registerService(service) && (!object || m_server.registerVirtualObject(QStringLiteral("/Editor"), &m_sigil));
    }

    QJsonArray cliArguments() const
    {
        QFile file(m_log);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QJsonDocument::fromJson(file.readLine()).array();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(m_server.isConnected(), "Run this test under dbus-run-session; it must not use the desktop session bus.");
        QVERIFY(m_directory.isValid());
        m_program = m_directory.filePath(QStringLiteral("sigil"));
        m_copy = m_directory.filePath(QStringLiteral("Arbeitskopie पुस्तक.epub"));
        m_log = m_directory.filePath(QStringLiteral("arguments.jsonl"));
        QFile copy(m_copy);
        QVERIFY(copy.open(QIODevice::WriteOnly));
        copy.write("working copy");
        copy.close();
        QFile program(m_program);
        QVERIFY(program.open(QIODevice::WriteOnly));
        // A real process captures argv, including embedded newlines and shell metacharacters.
        program.write(
            "#!/usr/bin/python3\nimport json, os, sys, time\n"
            "with open(os.environ['ARIANNA_TEST_SIGIL_ARGS'], 'a', encoding='utf-8') as f:\n"
            "    f.write(json.dumps(sys.argv[1:], ensure_ascii=False) + '\\n')\n"
            "if os.environ.get('ARIANNA_TEST_CLI_MODE'):\n"
            "    os.execv(os.environ['ARIANNA_TEST_EXECUTABLE'], [os.environ['ARIANNA_TEST_EXECUTABLE'], '--cli-server'])\n"
            "time.sleep(60)\n");
        program.close();
        QVERIFY(program.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("ARIANNA_TEST_SIGIL_ARGS", m_log.toUtf8());
        qputenv("ARIANNA_TEST_EXECUTABLE", QCoreApplication::applicationFilePath().toUtf8());
    }

    void init()
    {
        m_sigil.capabilities = defaultCapabilities();
        m_sigil.capabilityError.clear();
        m_sigil.capabilityQueries = 0;
        m_sigil.calls.clear();
        m_sigil.errorName.clear();
        m_sigil.openBook.clear();
        m_sigil.found = true;
        m_sigil.silent = false;
        m_sigil.malformed = false;
        m_sigil.delayMs = 0;
        m_sigil.documentError.clear();
        QFile::remove(m_log);
        QFile::remove(m_log + QStringLiteral(".ipc"));
        qunsetenv("ARIANNA_TEST_CLI_MODE");
    }

    void cleanup()
    {
        m_server.unregisterObject(QStringLiteral("/Editor"));
        m_server.unregisterService(service);
    }

    void runningCorrectCopyReceivesUnicodeAndWatchesSaves()
    {
        m_sigil.openBook = m_copy;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy started(&editor, &ExternalProcess::editorSessionProcessStarted);
        QSignalSpy saved(&editor, &ExternalProcess::editorSessionFileChanged);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("found"));
        QCOMPARE(m_sigil.calls.size(), 1);
        QCOMPARE(m_sigil.calls.first().path(), QStringLiteral("/Editor"));
        QCOMPARE(m_sigil.calls.first().interface(), QStringLiteral("org.sigil_ebook.Editor1"));
        QCOMPARE(m_sigil.calls.first().signature(), QStringLiteral("ss"));
        QCOMPARE(m_sigil.calls.first().arguments(), QVariantList({m_copy, text}));
        QCOMPARE(started.size(), 1);
        QCOMPARE(started.first().at(1).toLongLong(), QCoreApplication::applicationPid());
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
        QVERIFY(!QFile::exists(m_log));

        // Leaving the reader must keep the editor's save monitor and duplicate protection.
        editor.clearWatchedFile(true);
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));

        QFile copy(m_copy);
        QVERIFY(copy.open(QIODevice::Append));
        copy.write(" saved");
        copy.close();
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(saved.first().at(1).toString(), m_copy);

        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), QStringLiteral("next text")}));
        QTRY_COMPARE(result.size(), 2);
        QCOMPARE(m_sigil.calls.last().arguments().last().toString(), QStringLiteral("next text"));
        QVERIFY(!QFile::exists(m_log));
    }

    void exactDocumentRequest_data()
    {
        QTest::addColumn<QString>("error");
        QTest::addColumn<QString>("expected");
        QTest::newRow("exact-unicode-path") << QString() << QStringLiteral("document-opened");
        QTest::newRow("missing-document") << QStringLiteral("MissingDocument") << QStringLiteral("open-failed");
        QTest::newRow("busy") << QStringLiteral("Busy") << QStringLiteral("busy");
    }

    void exactDocumentRequest()
    {
        QFETCH(QString, error);
        QFETCH(QString, expected);
        m_sigil.capabilities.insert(QStringLiteral("openBookAtLocation"), true);
        m_sigil.capabilities.insert(QStringLiteral("methods"), QStringList{QStringLiteral("NavigateToText"), QStringLiteral("OpenBookAtLocation")});
        m_sigil.documentError = error;
        m_sigil.delayMs = 150;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        const QString document = QStringLiteral("OPS/भाग/śrī & file.xhtml");
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("session"), m_program, m_copy, document));
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("session"), m_program, m_copy, document));
        QVERIFY(!editor.startEditorDocumentSession(QStringLiteral("other"), m_program, m_copy, document));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), expected);
        QCOMPARE(m_sigil.calls.size(), 1);
        QCOMPARE(m_sigil.calls.first().arguments().at(0).toString(), m_copy);
        QCOMPARE(m_sigil.calls.first().arguments().at(1).toString(), document);
        QVERIFY(!QUuid(m_sigil.calls.first().arguments().at(2).toString()).isNull());
        QVERIFY(!QFile::exists(m_log));
    }

    void documentRequestsKeepBothWorkingCopyWatchers()
    {
        m_sigil.capabilities.insert(QStringLiteral("openBookAtLocation"), true);
        m_sigil.capabilities.insert(QStringLiteral("methods"), QStringList{QStringLiteral("NavigateToText"), QStringLiteral("OpenBookAtLocation")});
        QVERIFY(registerSigil());
        const QString other = m_directory.filePath(QStringLiteral("second.epub"));
        QFile::remove(other);
        QVERIFY(QFile::copy(m_copy, other));
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy saved(&editor, &ExternalProcess::editorSessionFileChanged);
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("first"), m_program, m_copy, QStringLiteral("OPS/first.xhtml")));
        QTRY_COMPARE(result.size(), 1);
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("second"), m_program, other, QStringLiteral("OPS/second.xhtml")));
        QTRY_COMPARE(result.size(), 2);
        QCOMPARE(result.last().at(1).toString(), QStringLiteral("document-opened"));
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("first")));
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("second")));
        for (const auto &path : {m_copy, other}) {
            QFile file(path); QVERIFY(file.open(QIODevice::Append)); file.write("change");
        }
        QTRY_COMPARE(saved.size(), 2);
        QCOMPARE(m_sigil.calls.size(), 2);
        QVERIFY(!QFile::exists(m_log));
    }

    void oldSigilRejectsDocumentRequestWithoutLaunching()
    {
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("session"), m_program, m_copy, QStringLiteral("OPS/chapter.xhtml")));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("rejected"));
        QVERIFY(m_sigil.calls.isEmpty());
        QVERIFY(!QFile::exists(m_log));
    }

    void anchorInstructionRequest_data()
    {
        QTest::addColumn<QString>("error");
        QTest::addColumn<bool>("malformed");
        QTest::addColumn<QString>("expected");
        QTest::newRow("prepared") << QString() << false << QStringLiteral("anchor-prepared");
        QTest::newRow("edited-instruction") << QStringLiteral("InstructionConflict") << false << QStringLiteral("rejected");
        QTest::newRow("missing-confirmation") << QString() << true << QStringLiteral("ipc-error");
    }

    void anchorInstructionRequest()
    {
        QFETCH(QString, error);
        QFETCH(bool, malformed);
        QFETCH(QString, expected);
        m_sigil.capabilities.insert(QStringLiteral("openBookAtLocation"), true);
        m_sigil.capabilities.insert(QStringLiteral("prepareAnchorEdit"), true);
        m_sigil.capabilities.insert(QStringLiteral("methods"), QStringList{QStringLiteral("NavigateToText"), QStringLiteral("OpenBookAtLocation"), QStringLiteral("PrepareAnchorEdit")});
        m_sigil.documentError = error;
        m_sigil.malformed = malformed;
        m_sigil.delayMs = 150;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        const QString document = QStringLiteral("OPS/chapter_९.xhtml");
        const QString anchor = QStringLiteral("vers_१८-२०_\"&");
        const QString verse = QStringLiteral("18–20 śrī कृष्ण");
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("session"), m_program, m_copy, document, anchor, verse));
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("session"), m_program, m_copy, document, anchor, verse));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), expected);
        QCOMPARE(m_sigil.calls.size(), 1);
        const auto call = m_sigil.calls.first();
        QCOMPARE(call.member(), QStringLiteral("PrepareAnchorEdit"));
        QCOMPARE(call.arguments().mid(0,4), QVariantList({m_copy, document, anchor, verse}));
        QVERIFY(!QUuid(call.arguments().at(4).toString()).isNull());
        QVERIFY(!QFile::exists(m_log));
    }

    void oldDocumentServiceRejectsAnchorPrefillWithoutLaunching()
    {
        m_sigil.capabilities.insert(QStringLiteral("openBookAtLocation"), true);
        m_sigil.capabilities.insert(QStringLiteral("methods"), QStringList{QStringLiteral("NavigateToText"), QStringLiteral("OpenBookAtLocation")});
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorDocumentSession(QStringLiteral("session"), m_program, m_copy, QStringLiteral("OPS/chapter.xhtml"), QStringLiteral("vers_18")));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("rejected"));
        QVERIFY(m_sigil.calls.isEmpty());
        QVERIFY(!QFile::exists(m_log));
    }

    void pendingCallsDoNotLaunchTwiceAndKeepEventLoopResponsive()
    {
        m_sigil.delayMs = 150;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] {
            ++ticks;
        });
        heartbeat.start(10);
        const QStringList arguments{m_copy, QStringLiteral("--select-text"), text};
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
        QVERIFY(!editor.startEditorSession(QStringLiteral("other-session"), m_program, arguments));
        QTRY_COMPARE(result.size(), 1);
        QVERIFY(ticks >= 5);
        QCOMPARE(m_sigil.calls.size(), 1);
        QVERIFY(!QFile::exists(m_log));
    }

    void falseReplyDoesNotTriggerFallback()
    {
        m_sigil.found = false;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("not-found"));
        QVERIFY(result.first().at(2).toString().contains(QStringLiteral("not found")));
        QVERIFY(!QFile::exists(m_log));
    }

    void differentConsecutiveSelectionsAreDeliveredOnce()
    {
        m_sigil.delayMs = 100;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        const QString next = QStringLiteral("next śloka");
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), next}));
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), next}));
        QTRY_COMPARE(result.size(), 2);
        QCOMPARE(m_sigil.calls.size(), 2);
        QCOMPARE(m_sigil.calls.at(0).arguments().last().toString(), text);
        QCOMPARE(m_sigil.calls.at(1).arguments().last().toString(), next);
        QVERIFY(!QFile::exists(m_log));
    }

    void capableOwnerOpensBookInSameProcess_data()
    {
        QTest::addColumn<bool>("failOpening");
        QTest::newRow("open-and-navigate") << false;
        QTest::newRow("open-failed-no-retry") << true;
    }

    void capableOwnerOpensBookInSameProcess()
    {
        QFETCH(bool, failOpening);
        m_sigil.capabilities[QStringLiteral("openBook")] = true;
        m_sigil.capabilities[QStringLiteral("methods")] =
            QStringList{QStringLiteral("GetCapabilities"), QStringLiteral("NavigateToText"), QStringLiteral("OpenBookAndNavigate")};
        m_sigil.openBook = QStringLiteral("another.epub");
        if (failOpening)
            m_sigil.errorName = QStringLiteral("org.sigil_ebook.Editor1.Error.OpenFailed");
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), failOpening ? QStringLiteral("open-failed") : QStringLiteral("found"));
        QCOMPARE(m_sigil.calls.size(), 1);
        QCOMPARE(m_sigil.calls.first().member(), QStringLiteral("OpenBookAndNavigate"));
        QCOMPARE(m_sigil.calls.first().arguments(), QVariantList({m_copy, text}));
        QVERIFY(!QFile::exists(m_log));
    }

    void existingErrorsNeverLaunch_data()
    {
        QTest::addColumn<QString>("error");
        QTest::addColumn<QString>("status");
        QTest::newRow("wrong-book") << QStringLiteral("different-book") << QStringLiteral("book-not-open");
        QTest::newRow("method-missing") << QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod") << QStringLiteral("rejected");
        QTest::newRow("object-missing") << QStringLiteral("no-object") << QStringLiteral("not-ready");
        QTest::newRow("access-denied") << QStringLiteral("org.freedesktop.DBus.Error.AccessDenied") << QStringLiteral("rejected");
    }

    void existingErrorsNeverLaunch()
    {
        QFETCH(QString, error);
        QFETCH(QString, status);
        if (error == QLatin1String("different-book")) {
            m_sigil.openBook = m_directory.filePath(QStringLiteral("other.epub"));
        } else {
            m_sigil.errorName = error;
        }
        if (error != QLatin1String("absent")) {
            QVERIFY(registerSigil(error != QLatin1String("no-object")));
        }
        ExternalProcess editor(QDBusConnection::sessionBus(), 100);
        const auto stop = qScopeGuard([&editor] {
            editor.stop();
        });
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy started(&editor, &ExternalProcess::editorSessionProcessStarted);
        const QString command = QStringLiteral("\"") + m_program + QStringLiteral("\" --configured-option");
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), command, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), status);
        QTRY_COMPARE(started.size(), 1);
        QVERIFY(!QFile::exists(m_log));
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
        QVERIFY(!QFile::exists(QStringLiteral("SHOULD_NOT_EXIST")));
        // Repeated errors must keep the known process, without a CLI start.
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), command, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 2);
        QCOMPARE(result.last().at(1).toString(), status);
        QVERIFY(!QFile::exists(m_log));
    }

    void incompatibleCapabilities_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"version", "version-type", "disabled", "missing-method", "missing-ready", "no-capabilities"}) {
            QTest::newRow(mode) << QString::fromLatin1(mode);
        }
    }

    void incompatibleCapabilities()
    {
        QFETCH(QString, mode);
        if (mode == QLatin1String("version"))
            m_sigil.capabilities[QStringLiteral("protocolVersion")] = QVariant::fromValue(uint(2));
        if (mode == QLatin1String("version-type"))
            m_sigil.capabilities[QStringLiteral("protocolVersion")] = QStringLiteral("1");
        if (mode == QLatin1String("disabled"))
            m_sigil.capabilities[QStringLiteral("navigateToText")] = false;
        if (mode == QLatin1String("missing-method"))
            m_sigil.capabilities[QStringLiteral("methods")] = QStringList{};
        if (mode == QLatin1String("missing-ready"))
            m_sigil.capabilities.remove(QStringLiteral("ready"));
        if (mode == QLatin1String("no-capabilities"))
            m_sigil.capabilityError = QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod");
        QVERIFY(registerSigil());
        ExternalProcess editor;
        const auto stop = qScopeGuard([&editor] {
            editor.stop();
        });
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("rejected"));
        QVERIFY(!QFile::exists(m_log));
        QCOMPARE(m_sigil.calls.size(), 0);
        QCOMPARE(m_sigil.capabilityQueries, 1);
    }

    void capabilitiesAreAwaitedWithoutBlocking()
    {
        m_sigil.capabilities[QStringLiteral("ready")] = false;
        QVERIFY(registerSigil());
        QTimer::singleShot(250, this, [this] {
            m_sigil.capabilities[QStringLiteral("ready")] = true;
        });
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] {
            ++ticks;
        });
        heartbeat.start(10);
        ExternalProcess editor(QDBusConnection::sessionBus(), 100);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("found"));
        QVERIFY(ticks >= 10);
        QVERIFY(m_sigil.capabilityQueries >= 2);
        QCOMPARE(m_sigil.calls.size(), 1);
        QVERIFY(!QFile::exists(m_log));
    }

    void readinessTimeoutDoesNotReopenPossibleWorkingCopy()
    {
        m_sigil.capabilities[QStringLiteral("ready")] = false;
        m_sigil.openBook = m_copy;
        QVERIFY(registerSigil());
        ExternalProcess editor(QDBusConnection::sessionBus(), 50);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("not-ready"));
        QCOMPARE(m_sigil.calls.size(), 0);
        QVERIFY(!QFile::exists(m_log));
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
    }

    void navigationHasMoreTimeThanMetadataQueries()
    {
        m_sigil.delayMs = 250;
        QVERIFY(registerSigil());
        ExternalProcess editor(QDBusConnection::sessionBus(), 100);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("found"));
        QVERIFY(!QFile::exists(m_log));
    }

    void readinessAndNavigationStayWithResolvedOwner()
    {
        m_sigil.capabilities[QStringLiteral("ready")] = false;
        QVERIFY(registerSigil());
        const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto replacement = QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
        FakeSigil other;
        other.openBook = m_directory.filePath(QStringLiteral("other.epub"));
        const auto disconnect = qScopeGuard([&replacement, name] {
            replacement.unregisterObject(QStringLiteral("/Editor"));
            replacement.unregisterService(service);
            QDBusConnection::disconnectFromBus(name);
        });
        ExternalProcess editor(QDBusConnection::sessionBus(), 150);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_VERIFY(m_sigil.capabilityQueries > 0);
        QVERIFY(m_server.unregisterService(service));
        QVERIFY(replacement.registerVirtualObject(QStringLiteral("/Editor"), &other));
        QVERIFY(replacement.registerService(service));
        m_sigil.capabilities[QStringLiteral("ready")] = true;
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("found"));
        QCOMPARE(m_sigil.calls.size(), 1);
        QCOMPARE(other.calls.size(), 0);
        QCOMPARE(other.capabilityQueries, 0);
        QVERIFY(!QFile::exists(m_log));
    }

    void cancellationStopsReadinessPolling()
    {
        m_sigil.capabilities[QStringLiteral("ready")] = false;
        QVERIFY(registerSigil());
        ExternalProcess editor(QDBusConnection::sessionBus(), 100);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_VERIFY(m_sigil.capabilityQueries > 0);
        editor.clearWatchedFile();
        const int queries = m_sigil.capabilityQueries;
        QTest::qWait(350);
        QCOMPARE(result.size(), 0);
        QCOMPARE(m_sigil.capabilityQueries, queries);
        QCOMPARE(m_sigil.calls.size(), 0);
    }

    void existingBookRejectionsDoNotOpenAnotherWindow_data()
    {
        QTest::addColumn<QString>("error");
        QTest::addColumn<QString>("status");
        QTest::newRow("busy") << QStringLiteral("org.sigil_ebook.Editor1.Error.Busy") << QStringLiteral("busy");
        QTest::newRow("ambiguous") << QStringLiteral("org.sigil_ebook.Editor1.Error.AmbiguousBook") << QStringLiteral("ambiguous-book");
    }

    void existingBookRejectionsDoNotOpenAnotherWindow()
    {
        QFETCH(QString, error);
        QFETCH(QString, status);
        m_sigil.errorName = error;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), status);
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
        QVERIFY(!QFile::exists(m_log));
    }

    void registeredOwnerIsPreferredOverActivation()
    {
        ActivationBus bus;
        QVERIFY(bus.start(QStringLiteral("ready"), m_copy));
        const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto server = QDBusConnection::connectToBus(bus.address, name);
        const auto disconnect = qScopeGuard([name] {
            QDBusConnection::disconnectFromBus(name);
        });
        FakeSigil sigil;
        sigil.openBook = m_copy;
        QVERIFY(server.registerService(service));
        QVERIFY(server.registerVirtualObject(QStringLiteral("/Editor"), &sigil));
        QVERIFY(bus.connection.interface()->activatableServiceNames().value().contains(service));
        ExternalProcess editor(bus.connection, 300);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("found"));
        QCOMPARE(bus.connection.interface()->serviceOwner(service).value(), server.baseService());
        QCOMPARE(sigil.capabilityQueries, 1);
        QCOMPARE(sigil.calls.size(), 1);
        QVERIFY(bus.calls().isEmpty());
        QVERIFY(!QFile::exists(m_log));
    }

    void coldStartNeverActivates_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("status");
        QTest::newRow("ready") << QStringLiteral("ready") << QStringLiteral("started");
        QTest::newRow("delayed-registration") << QStringLiteral("late-registration") << QStringLiteral("started");
        QTest::newRow("delayed-readiness") << QStringLiteral("delayed-readiness") << QStringLiteral("started");
        QTest::newRow("cli-selection-pending") << QStringLiteral("pending-selection") << QStringLiteral("started");
        QTest::newRow("object-appears-later") << QStringLiteral("late-object") << QStringLiteral("started");
        QTest::newRow("incompatible-activated-service") << QStringLiteral("incompatible") << QStringLiteral("rejected");
        QTest::newRow("interface-missing") << QStringLiteral("missing-object") << QStringLiteral("not-ready");
        QTest::newRow("registration-missing") << QStringLiteral("missing-registration") << QStringLiteral("not-ready");
        QTest::newRow("failed-opening") << QStringLiteral("open-failed") << QStringLiteral("start-failed");
    }

    void manuallyStartingSigilIsAwaitedWithoutLaunching()
    {
        const QString directory = m_directory.filePath(QStringLiteral("starting"));
        QVERIFY(QDir().mkpath(directory));
        const QString executable = directory + QStringLiteral("/sigil");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
        qputenv("ARIANNA_TEST_CLI_MODE", "late-registration");
        QProcess server;
        server.start(executable, {QStringLiteral("--cli-server")});
        const auto stop = qScopeGuard([&server] {
            server.kill();
            server.waitForFinished();
        });
        QVERIFY(server.waitForStarted());
        QVERIFY(!QDBusConnection::sessionBus().interface()->isServiceRegistered(service).value());
        ExternalProcess editor(QDBusConnection::sessionBus(), 300);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy started(&editor, &ExternalProcess::editorSessionProcessStarted);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("found"));
        QCOMPARE(started.size(), 1);
        QCOMPARE(started.first().at(1).toLongLong(), server.processId());
        QVERIFY(!QFile::exists(m_log));
    }

    void coldStartNeverActivates()
    {
        QFETCH(QString, mode);
        QFETCH(QString, status);
        ActivationBus bus;
        QVERIFY(bus.start(mode, m_copy));
        QVERIFY(!bus.connection.interface()->isServiceRegistered(service).value());
        QVERIFY(bus.connection.interface()->activatableServiceNames().value().contains(service));
        // QProcess must inherit the same private bus used by this client.
        const auto originalAddress = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        qputenv("DBUS_SESSION_BUS_ADDRESS", bus.address.toUtf8());
        const auto restoreAddress = qScopeGuard([originalAddress] {
            qputenv("DBUS_SESSION_BUS_ADDRESS", originalAddress);
        });
        qputenv("ARIANNA_TEST_CLI_MODE", mode.toUtf8());
        ExternalProcess editor(bus.connection, 300);
        const auto stop = qScopeGuard([&editor] {
            editor.stop();
        });
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] {
            ++ticks;
        });
        heartbeat.start(10);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy started(&editor, &ExternalProcess::editorSessionProcessStarted);
        const QStringList arguments{m_copy, QStringLiteral("--select-text"), text};
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.last().at(1).toString(), status);
        QTRY_COMPARE(cliArguments().size(), 2);
        QCOMPARE(cliArguments(), QJsonArray({m_copy, QString(QStringLiteral("--select-text=") + text)}));
        QFile log(m_log);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count('\n'), 1);
        QCOMPARE(started.size(), 1);
        // Only the CLI child may publish the service. The activator's trace
        // would contain a call if Arianna had started an empty second editor.
        QVERIFY(bus.calls().isEmpty());
        QFile ipc(m_log + QStringLiteral(".ipc"));
        if (ipc.open(QIODevice::ReadOnly)) {
            const auto calls = ipc.readAll();
            QVERIFY(!calls.contains("NavigateToText"));
            QVERIFY(!calls.contains("OpenBookAndNavigate"));
        } else {
            QVERIFY(mode == QLatin1String("open-failed") || mode == QLatin1String("missing-registration") || mode == QLatin1String("missing-object"));
        }
        if (status == QLatin1String("started")) {
            QCOMPARE(started.size(), 1);
            const auto owner = bus.connection.interface()->serviceOwner(service).value();
            QCOMPARE(started.first().at(1).toLongLong(), qint64(bus.connection.interface()->servicePid(owner).value()));
            if (mode == QLatin1String("delayed-readiness") || mode == QLatin1String("late-object"))
                QVERIFY(ticks >= 10);
            // A later navigation must reuse the same activation process.
            QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
            QTRY_COMPARE(result.size(), 2);
            QCOMPARE(result.last().at(1).toString(), QStringLiteral("found"));
            QCOMPARE(bus.connection.interface()->serviceOwner(service).value(), owner);
            QCOMPARE(started.size(), 1);
        }
    }

    void ambiguousIpcFailure_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("timeout") << QStringLiteral("timeout");
        QTest::newRow("malformed-reply") << QStringLiteral("malformed");
        QTest::newRow("unspecified-server-failure") << QStringLiteral("failed");
        QTest::newRow("owner-disappeared-after-dispatch") << QStringLiteral("owner-gone");
    }

    void ambiguousIpcFailure()
    {
        QFETCH(QString, mode);
        m_sigil.silent = mode == QLatin1String("timeout");
        m_sigil.malformed = mode == QLatin1String("malformed");
        if (mode == QLatin1String("failed")) {
            m_sigil.errorName = QStringLiteral("org.freedesktop.DBus.Error.Failed");
        }
        if (mode == QLatin1String("owner-gone")) {
            m_sigil.errorName = QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown");
        }
        QVERIFY(registerSigil());
        ExternalProcess editor(QDBusConnection::sessionBus(), 100);
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text=") + text}));
        QTRY_VERIFY(!result.isEmpty());
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("ipc-error"));
        QVERIFY(!QFile::exists(m_log));
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
    }

    void unavailableSessionBusFallsBack()
    {
        ExternalProcess editor(QDBusConnection(QStringLiteral("disconnected-test-bus")), 100);
        const auto stop = qScopeGuard([&editor] {
            editor.stop();
        });
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result.first().at(1).toString(), QStringLiteral("service-unavailable"));
        QTRY_COMPARE(cliArguments().size(), 2);
    }

    void otherConfiguredEditorRetainsOriginalArguments()
    {
        QVERIFY(registerSigil());
        const QString other = m_directory.filePath(QStringLiteral("ebook-edit"));
        QVERIFY(QFile::link(m_program, other));
        ExternalProcess editor(QDBusConnection::sessionBus(), 100);
        const auto stop = qScopeGuard([&editor] {
            editor.stop();
        });
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), other, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(cliArguments().size(), 3);
        QCOMPARE(cliArguments(), QJsonArray({m_copy, QStringLiteral("--select-text"), text}));
        QCOMPARE(m_sigil.calls.size(), 0);
    }

    void cancellationIgnoresLateReply()
    {
        m_sigil.delayMs = 150;
        QVERIFY(registerSigil());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(m_sigil.calls.size(), 1);
        editor.clearWatchedFile();
        QTest::qWait(250);
        QCOMPARE(result.size(), 0);
        QVERIFY(!editor.isEditorSessionRunning(QStringLiteral("session")));
        QVERIFY(!QFile::exists(m_log));
    }

    void externalSigilProcessLifetimeFinishesSession()
    {
        QProcess server;
        server.setProcessChannelMode(QProcess::MergedChannels);
        server.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--serve-sigil")});
        const auto stop = qScopeGuard([&server] {
            server.kill();
            server.waitForFinished();
        });
        QVERIFY(server.waitForStarted());
        QByteArray output;
        QTRY_VERIFY(([&] {
            output += server.readAllStandardOutput();
            return output.contains("READY");
        })());
        ExternalProcess editor;
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy started(&editor, &ExternalProcess::editorSessionProcessStarted);
        QSignalSpy finished(&editor, &ExternalProcess::editorSessionFinished);
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(started.first().at(1).toLongLong(), server.processId());
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
        server.terminate();
        QVERIFY(server.waitForFinished());
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("session"));
        QVERIFY(!editor.isEditorSessionRunning(QStringLiteral("session")));
        QVERIFY(!QFile::exists(m_log));
    }

    void launcherExitRetainsSessionOwnedByAnotherProcess()
    {
        ExternalProcess editor;
        const auto stop = qScopeGuard([&editor] {
            editor.stop();
        });
        QSignalSpy result(&editor, &ExternalProcess::editorSessionNavigationFinished);
        QSignalSpy finished(&editor, &ExternalProcess::editorSessionFinished);
        const QStringList arguments{m_copy, QStringLiteral("--select-text"), text};
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
        QTRY_COMPARE(cliArguments().size(), 2);
        QVERIFY(registerSigil());
        QTRY_COMPARE(result.size(), 1); // CLI readiness, without another navigation.
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), m_program, arguments));
        QTRY_COMPARE(result.size(), 2);
        QCOMPARE(result.last().at(1).toString(), QStringLiteral("found"));
        editor.stop();
        QVERIFY(finished.isEmpty());
        QVERIFY(editor.isEditorSessionRunning(QStringLiteral("session")));
    }

    void failedCliFallbackReleasesPendingSession()
    {
        ExternalProcess editor;
        QSignalSpy finished(&editor, &ExternalProcess::editorSessionFinished);
        const QString missing = m_directory.filePath(QStringLiteral("missing/sigil"));
        QVERIFY(editor.startEditorSession(QStringLiteral("session"), missing, {m_copy, QStringLiteral("--select-text"), text}));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("session"));
        QCOMPARE(finished.first().at(1).toInt(), -1);
        QVERIFY(!editor.isEditorSessionRunning(QStringLiteral("session")));
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--cli-server"))) {
        FakeSigil sigil;
        sigil.tracePath = qEnvironmentVariable("ARIANNA_TEST_SIGIL_ARGS") + QStringLiteral(".ipc");
        const auto mode = qEnvironmentVariable("ARIANNA_TEST_CLI_MODE");
        if (mode == QLatin1String("open-failed"))
            return 1;
        if (mode == QLatin1String("incompatible"))
            sigil.capabilities[QStringLiteral("protocolVersion")] = QVariant::fromValue(uint(2));
        if (mode == QLatin1String("delayed-readiness")) {
            sigil.capabilities[QStringLiteral("ready")] = false;
            QTimer::singleShot(250, &application, [&sigil] {
                sigil.capabilities[QStringLiteral("ready")] = true;
            });
        }
        if (mode == QLatin1String("pending-selection")) {
            sigil.capabilities[QStringLiteral("navigationPending")] = true;
            QTimer::singleShot(250, &application, [&sigil] {
                sigil.capabilities[QStringLiteral("navigationPending")] = false;
            });
        }
        auto bus = QDBusConnection::sessionBus();
        auto registerObject = [bus, &sigil]() mutable {
            bus.registerVirtualObject(QStringLiteral("/Editor"), &sigil);
        };
        if (mode != QLatin1String("missing-object")) {
            if (mode == QLatin1String("late-object"))
                QTimer::singleShot(250, &application, registerObject);
            else
                registerObject();
        }
        if (mode != QLatin1String("missing-registration")) {
            if (mode == QLatin1String("late-registration"))
                QTimer::singleShot(250, &application, [bus]() mutable {
                    bus.registerService(service);
                });
            else if (!bus.registerService(service))
                return 1;
        }
        return application.exec();
    }
    if (application.arguments().contains(QStringLiteral("--editor-probe")) || application.arguments().contains(QStringLiteral("--document-probe"))
        || application.arguments().contains(QStringLiteral("--anchor-probe"))) {
        const auto arguments = application.arguments();
        const bool anchorProbe = arguments.at(1) == QLatin1String("--anchor-probe");
        if (arguments.size() != (anchorProbe ? 8 : 6))
            return 2;
        ExternalProcess editor;
        QObject::connect(&editor, &ExternalProcess::editorSessionProcessStarted, &application, [](const QString &, qint64 pid) {
            qInfo() << "PROBE EDITOR PID" << pid;
        });
        QObject::connect(&editor,
                         &ExternalProcess::editorSessionNavigationFinished,
                         &application,
                         [&application, expected = arguments.last()](const QString &, const QString &status, const QString &message) {
                             qInfo() << "PROBE RESULT" << status << message;
                             // Let the asynchronous CLI start complete before exiting the probe.
                             const int holdMs = qEnvironmentVariableIntValue("ARIANNA_PROBE_HOLD_MS");
                             QTimer::singleShot(holdMs > 0 ? holdMs : 500, &application, [&application, status, expected] {
                                 application.exit(status == expected ? 0 : 1);
                             });
                         });
        if (anchorProbe)
            editor.startEditorDocumentSession(QStringLiteral("live-probe"), arguments.at(2), arguments.at(3), arguments.at(4), arguments.at(5), arguments.at(6));
        else if (arguments.at(1) == QLatin1String("--document-probe"))
            editor.startEditorDocumentSession(QStringLiteral("live-probe"), arguments.at(2), arguments.at(3), arguments.at(4));
        else
            editor.startEditorSession(QStringLiteral("live-probe"), arguments.at(2), {arguments.at(3), QStringLiteral("--select-text=") + arguments.at(4)});
        return application.exec();
    }
    if (application.arguments().contains(QStringLiteral("--activation-server"))) {
        FakeSigil sigil;
        const auto mode = qEnvironmentVariable("ARIANNA_ACTIVATION_MODE");
        sigil.tracePath = qEnvironmentVariable("ARIANNA_ACTIVATION_TRACE");
        sigil.openBook = mode == QLatin1String("empty-book") ? QString() : qEnvironmentVariable("ARIANNA_ACTIVATION_BOOK");
        sigil.rejectMissingBook = true;
        if (mode == QLatin1String("incompatible"))
            sigil.capabilities[QStringLiteral("protocolVersion")] = QVariant::fromValue(uint(2));
        if (mode == QLatin1String("delayed-readiness")) {
            sigil.capabilities[QStringLiteral("ready")] = false;
            QTimer::singleShot(250, &application, [&sigil] {
                sigil.capabilities[QStringLiteral("ready")] = true;
            });
        }
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService(service))
            return 1;
        if (mode == QLatin1String("late-object")) {
            QTimer::singleShot(250, &application, [bus, &sigil]() mutable {
                bus.registerVirtualObject(QStringLiteral("/Editor"), &sigil);
            });
        } else if (mode != QLatin1String("missing-object") && !bus.registerVirtualObject(QStringLiteral("/Editor"), &sigil)) {
            return 1;
        }
        bus.connect({},
                    QStringLiteral("/org/freedesktop/DBus/Local"),
                    QStringLiteral("org.freedesktop.DBus.Local"),
                    QStringLiteral("Disconnected"),
                    &application,
                    SLOT(quit()));
        QTimer::singleShot(5000, &application, &QCoreApplication::quit);
        return application.exec();
    }
    if (application.arguments().contains(QStringLiteral("--probe"))) {
        const auto arguments = application.arguments();
        if (arguments.size() != 5)
            return 2;
        SigilNavigation navigation;
        QObject::connect(
            &navigation,
            &SigilNavigation::finished,
            &application,
            [&application,
             expected = arguments.at(4)](SigilNavigation::Outcome outcome, const QString &owner, qint64 pid, const QString &errorName, const QString &message) {
                const auto name = QString::fromLatin1(QMetaEnum::fromType<SigilNavigation::Outcome>().valueToKey(outcome));
                qInfo() << name << owner << pid << errorName << message;
                application.exit(name == expected ? 0 : 1);
            });
        navigation.navigate(arguments.at(2), arguments.at(3));
        return application.exec();
    }
    if (application.arguments().contains(QStringLiteral("--serve-sigil"))) {
        FakeSigil sigil;
        auto bus = QDBusConnection::sessionBus();
        if (!bus.registerService(service) || !bus.registerVirtualObject(QStringLiteral("/Editor"), &sigil)) {
            return 1;
        }
        qInfo("READY");
        return application.exec();
    }
    SigilNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "sigilnavigationtest.moc"
