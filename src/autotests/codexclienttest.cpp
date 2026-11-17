// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../codexclient.h"

#include <KLocalizedString>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <chrono>
#include <iostream>
#include <thread>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace
{
void send(const QJsonObject &message, bool fragment = false)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (fragment) {
        // Split within a UTF-8 sequence, not just between JSON tokens.
        const qsizetype split = line.indexOf(QByteArray::fromHex("c3")) + 1;
        std::cout.write(line.constData(), split).flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        std::cout.write(line.constData() + split, line.size() - split).flush();
    } else {
        std::cout.write(line.constData(), line.size()).flush();
    }
}

void notify(const QString &method, const QJsonObject &params, bool fragment = false)
{
    send({{QStringLiteral("method"), method}, {QStringLiteral("params"), params}}, fragment);
}

// The test executable acts as a deterministic stdio peer when launched by QProcess.
int fakeServer()
{
    const QString scenario = qEnvironmentVariable("ARIANNA_FAKE_CODEX");
    bool initialized = false;
    bool acknowledged = false;
    int number = 0;
    QString turnId;
    std::cerr << "test peer diagnostic, not JSON\n";
    notify(QStringLiteral("test/processStarted"), {{QStringLiteral("pid"), QCoreApplication::applicationPid()}});
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto m = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString method = m.value(QStringLiteral("method")).toString();
        const auto id = m.value(QStringLiteral("id"));
        const auto params = m.value(QStringLiteral("params")).toObject();
        const auto reply = [&](const QJsonObject &result) {
            send({{QStringLiteral("id"), id}, {QStringLiteral("result"), result}});
        };
        const auto error = [&](const QString &text) {
            send({{QStringLiteral("id"), id}, {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32602}, {QStringLiteral("message"), text}}}});
        };
        const auto completed = [&](const QString &status) {
            notify(QStringLiteral("turn/completed"),
                   {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                    {QStringLiteral("turn"),
                     QJsonObject{{QStringLiteral("id"), turnId}, {QStringLiteral("status"), status}, {QStringLiteral("items"), QJsonArray{}}}}});
        };
        if (method == QLatin1String("initialize")) {
            initialized = true;
            if (scenario == QLatin1String("bad-json")) {
                std::cout << "broken JSON\n" << std::flush;
                continue;
            }
            reply({{QStringLiteral("userAgent"), QStringLiteral("test/0.162.0")}});
        } else if (method == QLatin1String("initialized")) {
            acknowledged = initialized;
        } else if (method == QLatin1String("thread/start")) {
            if (!acknowledged || params.value(QStringLiteral("sandbox")).toString() != QLatin1String("read-only")
                || params.value(QStringLiteral("approvalPolicy")).toString() != QLatin1String("on-request")
                || params.value(QStringLiteral("approvalsReviewer")).toString() != QLatin1String("user")
                || !params.value(QStringLiteral("ephemeral")).toBool()) {
                error(QStringLiteral("unsafe handshake"));
                continue;
            }
            reply({{QStringLiteral("thread"),
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("thread-test")}, {QStringLiteral("cliVersion"), QStringLiteral("0.162.0-test")}}},
                   {QStringLiteral("sandbox"),
                    QJsonObject{{QStringLiteral("type"), scenario == QLatin1String("unsafe") ? QStringLiteral("workspaceWrite") : QStringLiteral("readOnly")}}},
                   {QStringLiteral("approvalPolicy"), QStringLiteral("on-request")},
                   {QStringLiteral("approvalsReviewer"), QStringLiteral("user")}});
        } else if (method == QLatin1String("turn/start")) {
            notify(QStringLiteral("test/submittedInput"), params);
            if (params.value(QStringLiteral("sandboxPolicy")).toObject().value(QStringLiteral("type")).toString() != QLatin1String("readOnly")
                || params.value(QStringLiteral("sandboxPolicy")).toObject().value(QStringLiteral("networkAccess")).toBool()
                || params.value(QStringLiteral("approvalPolicy")).toString() != QLatin1String("on-request")) {
                error(QStringLiteral("unsafe turn"));
                continue;
            }
            if (scenario == QLatin1String("rpc-error")) {
                error(QStringLiteral("test RPC error"));
                continue;
            }
            if (scenario == QLatin1String("crash"))
                return 42;
            if (number > 0) {
                notify(QStringLiteral("item/agentMessage/delta"),
                       {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                        {QStringLiteral("turnId"), turnId},
                        {QStringLiteral("itemId"), QStringLiteral("previous")},
                        {QStringLiteral("delta"), QStringLiteral("stale")}});
            }
            turnId = QStringLiteral("turn-%1").arg(++number);
            const QJsonObject turn{{QStringLiteral("id"), turnId},
                                   {QStringLiteral("status"), QStringLiteral("inProgress")},
                                   {QStringLiteral("items"), QJsonArray{}}};
            notify(QStringLiteral("turn/started"), {{QStringLiteral("threadId"), QStringLiteral("thread-test")}, {QStringLiteral("turn"), turn}});
            reply({{QStringLiteral("turn"), turn}});
            if (scenario == QLatin1String("cancel"))
                continue;
            if (scenario == QLatin1String("approval")) {
                for (const auto &approval : {QStringLiteral("item/commandExecution/requestApproval"),
                                             QStringLiteral("item/fileChange/requestApproval"),
                                             QStringLiteral("item/permissions/requestApproval"),
                                             QStringLiteral("unknown/request")}) {
                    send({{QStringLiteral("id"), approval}, {QStringLiteral("method"), approval}, {QStringLiteral("params"), QJsonObject{}}});
                }
                continue;
            }
            if (scenario == QLatin1String("turn-error")) {
                notify(QStringLiteral("error"),
                       {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                        {QStringLiteral("turnId"), turnId},
                        {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("stream failed")}}},
                        {QStringLiteral("willRetry"), false}});
                notify(QStringLiteral("turn/completed"),
                       {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                        {QStringLiteral("turn"),
                         QJsonObject{{QStringLiteral("id"), turnId},
                                     {QStringLiteral("status"), QStringLiteral("failed")},
                                     {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("stream failed")}}}}}});
                continue;
            }
            notify(QStringLiteral("item/agentMessage/delta"),
                   {{QStringLiteral("threadId"), QStringLiteral("other")},
                    {QStringLiteral("turnId"), turnId},
                    {QStringLiteral("itemId"), QStringLiteral("wrong")},
                    {QStringLiteral("delta"), QStringLiteral("ignore")}});
            for (const auto &delta : {QStringLiteral("Grüße "), QStringLiteral("世界")}) {
                notify(QStringLiteral("item/agentMessage/delta"),
                       {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                        {QStringLiteral("turnId"), turnId},
                        {QStringLiteral("itemId"), QStringLiteral("message")},
                        {QStringLiteral("delta"), delta}},
                       delta.startsWith(QLatin1String("Gr")));
            }
            notify(QStringLiteral("item/completed"),
                   {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                    {QStringLiteral("turnId"), turnId},
                    {QStringLiteral("item"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("agentMessage")},
                                 {QStringLiteral("id"), QStringLiteral("message")},
                                 {QStringLiteral("text"), QStringLiteral("Grüße 世界")}}}});
            completed(QStringLiteral("completed"));
            notify(QStringLiteral("item/agentMessage/delta"),
                   {{QStringLiteral("threadId"), QStringLiteral("thread-test")},
                    {QStringLiteral("turnId"), turnId},
                    {QStringLiteral("itemId"), QStringLiteral("message")},
                    {QStringLiteral("delta"), QStringLiteral("late")}});
        } else if (method == QLatin1String("turn/interrupt")) {
            if (params.value(QStringLiteral("turnId")).toString() != turnId) {
                error(QStringLiteral("wrong turn"));
                continue;
            }
            reply({});
            completed(QStringLiteral("interrupted"));
        } else if (method.isEmpty() && scenario == QLatin1String("approval")) {
            const auto result = m.value(QStringLiteral("result")).toObject();
            if (id.toString().contains(QLatin1String("permissions"))) {
                if (!result.value(QStringLiteral("permissions")).toObject().isEmpty())
                    return 90;
            } else if (id.toString() == QLatin1String("unknown/request")) {
                if (m.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt() != -32601)
                    return 91;
                completed(QStringLiteral("completed"));
            } else if (result.value(QStringLiteral("decision")).toString() != QLatin1String("decline")) {
                return 92;
            }
        }
    }
    if (scenario == QLatin1String("stubborn"))
        std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}

// The installed Codex binary talks to this local Responses SSE fixture.
class ResponsesFixture : public QTcpServer
{
public:
    bool holdOpen = false;
    int requests = 0;
    QByteArray lastBody;
    ResponsesFixture()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                auto answered = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, answered] {
                    *buffer += socket->readAll();
                    if (*answered)
                        return;
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    int length = 0;
                    for (const auto &header : buffer->left(end).split('\n'))
                        if (header.toLower().startsWith("content-length:"))
                            length = header.mid(15).trimmed().toInt();
                    if (buffer->size() < end + 4 + length)
                        return;
                    *answered = true;
                    lastBody = buffer->mid(end + 4, length);
                    ++requests;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                    const auto event = [socket](const QJsonObject &object) {
                        socket->write("data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n");
                    };
                    const QString responseId = QStringLiteral("resp_%1").arg(requests);
                    const QJsonObject part{{QStringLiteral("type"), QStringLiteral("output_text")},
                                           {QStringLiteral("text"), QStringLiteral("Grüße aus Codex")},
                                           {QStringLiteral("annotations"), QJsonArray{}}};
                    const QJsonObject item{{QStringLiteral("id"), QStringLiteral("msg_%1").arg(requests)},
                                           {QStringLiteral("type"), QStringLiteral("message")},
                                           {QStringLiteral("role"), QStringLiteral("assistant")},
                                           {QStringLiteral("status"), QStringLiteral("completed")},
                                           {QStringLiteral("content"), QJsonArray{part}}};
                    event({{QStringLiteral("type"), QStringLiteral("response.created")},
                           {QStringLiteral("response"), QJsonObject{{QStringLiteral("id"), responseId}}}});
                    event({{QStringLiteral("type"), QStringLiteral("response.output_item.added")},
                           {QStringLiteral("output_index"), 0},
                           {QStringLiteral("item"), item}});
                    event({{QStringLiteral("type"), QStringLiteral("response.output_text.delta")},
                           {QStringLiteral("item_id"), item.value(QStringLiteral("id"))},
                           {QStringLiteral("output_index"), 0},
                           {QStringLiteral("content_index"), 0},
                           {QStringLiteral("delta"), QStringLiteral("Grüße ")}});
                    event({{QStringLiteral("type"), QStringLiteral("response.output_text.delta")},
                           {QStringLiteral("item_id"), item.value(QStringLiteral("id"))},
                           {QStringLiteral("output_index"), 0},
                           {QStringLiteral("content_index"), 0},
                           {QStringLiteral("delta"), QStringLiteral("aus Codex")}});
                    if (holdOpen)
                        return;
                    event({{QStringLiteral("type"), QStringLiteral("response.output_item.done")},
                           {QStringLiteral("output_index"), 0},
                           {QStringLiteral("item"), item}});
                    const QJsonObject usage{{QStringLiteral("input_tokens"), 10}, {QStringLiteral("output_tokens"), 4}, {QStringLiteral("total_tokens"), 14}};
                    const QJsonObject response{{QStringLiteral("id"), responseId},
                                               {QStringLiteral("status"), QStringLiteral("completed")},
                                               {QStringLiteral("output"), QJsonArray{item}},
                                               {QStringLiteral("usage"), usage}};
                    event({{QStringLiteral("type"), QStringLiteral("response.completed")}, {QStringLiteral("response"), response}});
                    socket->disconnectFromHost();
                });
            }
        });
    }
};
}

class CodexClientTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        qputenv("ARIANNA_FAKE_CODEX", "stream");
    }
    void cleanup()
    {
        qunsetenv("ARIANNA_FAKE_CODEX");
    }

    void structuredResponse()
    {
        CodexClient client;
        QSignalSpy submitted(&client, &CodexClient::notificationReceived);
        QSignalSpy finished(&client, &CodexClient::structuredAnswer);
        client.connectToServer(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(client.state(), CodexClient::Ready);
        const QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")}};
        QVERIFY(client.startStructuredTurn(QJsonObject{{QStringLiteral("snapshotId"), QStringLiteral("test")}}, schema, QStringLiteral("Analyze supplied evidence.")));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("completed"));
        QCOMPARE(finished.first().at(3).toString(), QStringLiteral("Grüße 世界"));
        bool seen = false;
        for (const auto &notification : submitted) {
            if (notification.at(0).toString() == QLatin1String("test/submittedInput")) {
                const auto params = notification.at(1).value<QJsonObject>();
                QCOMPARE(params.value(QStringLiteral("outputSchema")).toObject(), schema);
                QCOMPARE(params.value(QStringLiteral("input")).toArray().size(), 2);
                seen = true;
            }
        }
        QVERIFY(seen);
        client.shutdown();
    }

    void streamAndReuse()
    {
        CodexClient client;
        QSignalSpy deltas(&client, &CodexClient::textDelta);
        QSignalSpy finished(&client, &CodexClient::turnFinished);
        QSignalSpy diagnostics(&client, &CodexClient::diagnosticReceived);
        client.connectToServer(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(client.state(), CodexClient::Ready);
        QCOMPARE(client.version(), QStringLiteral("0.162.0-test"));
        QVERIFY(client.connected());
        QVERIFY(!diagnostics.isEmpty());
        for (int n = 1; n <= 2; ++n) {
            client.startTurn(QStringLiteral("Discuss this passage."));
            QTRY_COMPARE(finished.size(), n);
            QCOMPARE(client.output(), QStringLiteral("Grüße 世界"));
            QCOMPARE(finished.last().at(2).toString(), QStringLiteral("completed"));
            QCOMPARE(client.state(), CodexClient::Ready);
        }
        QCOMPARE(deltas.size(), 4);
        QVERIFY(client.errorString().isEmpty());
        client.disconnectFromServer();
        QTRY_COMPARE(client.state(), CodexClient::Disconnected);
    }

    void immediateCancellation()
    {
        qputenv("ARIANNA_FAKE_CODEX", "cancel");
        CodexClient client;
        QSignalSpy finished(&client, &CodexClient::turnFinished);
        client.connectToServer(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(client.state(), CodexClient::Ready);
        client.startTurn(QStringLiteral("Wait."));
        client.cancel(); // Before either response or turn/started arrives.
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("interrupted"));
        QCOMPARE(client.state(), CodexClient::Ready);
        client.startTurn(QStringLiteral("Again."));
        QTRY_COMPARE(client.state(), CodexClient::Running);
        client.cancel();
        QTRY_COMPARE(finished.size(), 2);
    }

    void denyServerRequests()
    {
        qputenv("ARIANNA_FAKE_CODEX", "approval");
        CodexClient client;
        QSignalSpy denied(&client, &CodexClient::approvalDenied);
        QSignalSpy finished(&client, &CodexClient::turnFinished);
        client.connectToServer(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(client.state(), CodexClient::Ready);
        client.startTurn(QStringLiteral("No extra permissions."));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(denied.size(), 4);
        QCOMPARE(client.state(), CodexClient::Ready);
        QVERIFY(!client.errorString().isEmpty());
    }

    void failures_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::addColumn<bool>("startTurn");
        QTest::addColumn<bool>("fatal");
        QTest::newRow("malformed stdout") << QByteArray("bad-json") << false << true;
        QTest::newRow("unsafe sandbox") << QByteArray("unsafe") << false << true;
        QTest::newRow("RPC error") << QByteArray("rpc-error") << true << false;
        QTest::newRow("stream error") << QByteArray("turn-error") << true << false;
        QTest::newRow("process exit") << QByteArray("crash") << true << true;
    }
    void failures()
    {
        QFETCH(QByteArray, scenario);
        QFETCH(bool, startTurn);
        QFETCH(bool, fatal);
        qputenv("ARIANNA_FAKE_CODEX", scenario);
        CodexClient client;
        client.connectToServer(QCoreApplication::applicationFilePath());
        if (startTurn) {
            QTRY_COMPARE(client.state(), CodexClient::Ready);
            client.startTurn(QStringLiteral("Test failure."));
        }
        QTRY_VERIFY(!client.errorString().isEmpty());
        QCOMPARE(client.state(), fatal ? CodexClient::Failed : CodexClient::Ready);
        QCOMPARE(client.connected(), !fatal);
    }

    void missingExecutableAndRetry()
    {
        CodexClient client;
        client.connectToServer(QStringLiteral("/missing/arianna-codex"));
        QCOMPARE(client.state(), CodexClient::Failed);
        QVERIFY(!client.errorString().isEmpty());
        client.connectToServer(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(client.state(), CodexClient::Ready);
        QVERIFY(client.errorString().isEmpty());
    }

    void controlledShutdown()
    {
        qputenv("ARIANNA_FAKE_CODEX", "stubborn");
        CodexClient client;
        QSignalSpy notifications(&client, &CodexClient::notificationReceived);
        client.connectToServer(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(client.state(), CodexClient::Ready);
        const auto pid = notifications.first().at(1).value<QJsonObject>().value(QStringLiteral("pid")).toInteger();
        QVERIFY(pid > 0);
        // Exercise the actual application shutdown connection and TERM fallback.
        QVERIFY(QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit", Qt::DirectConnection));
        QCOMPARE(client.state(), CodexClient::Disconnected);
#ifdef Q_OS_UNIX
        QVERIFY(::kill(pid_t(pid), 0) != 0);
#endif
    }

    void installedBinary()
    {
        qunsetenv("ARIANNA_FAKE_CODEX");
        const QString overridePath = qEnvironmentVariable("ARIANNA_CODEX_BINARY");
        const QString binary = QStandardPaths::findExecutable(overridePath.isEmpty() ? QStringLiteral("codex") : overridePath);
        if (binary.isEmpty() && overridePath.isEmpty())
            QSKIP("Codex not installed; set ARIANNA_CODEX_BINARY to require a specific binary.");
        QVERIFY2(!binary.isEmpty(), "ARIANNA_CODEX_BINARY does not resolve to an executable");
        ResponsesFixture provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QFile config(home.filePath(QStringLiteral("config.toml")));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QStringLiteral("model = \"arianna-test-model\"\nmodel_provider = \"arianna_test\"\n"
                                    "[model_providers.arianna_test]\nname = \"Arianna test fixture\"\nbase_url = \"http://127.0.0.1:%1/v1\"\n"
                                    "wire_api = \"responses\"\nrequires_openai_auth = false\nsupports_websockets = false\n")
                         .arg(provider.serverPort())
                         .toUtf8());
        config.close();
        const bool hadHome = qEnvironmentVariableIsSet("CODEX_HOME");
        const QByteArray oldHome = qgetenv("CODEX_HOME");
        qputenv("CODEX_HOME", home.path().toUtf8());
        CodexClient client;
        QSignalSpy finished(&client, &CodexClient::turnFinished);
        QSignalSpy deltas(&client, &CodexClient::textDelta);
        QSignalSpy replies(&client, &CodexClient::requestCompleted);
        QString diagnostics;
        connect(&client, &CodexClient::diagnosticReceived, this, [&](const QString &text) {
            diagnostics += text;
        });
        client.connectToServer(binary);
        if (hadHome)
            qputenv("CODEX_HOME", oldHome);
        else
            qunsetenv("CODEX_HOME");
        QTRY_VERIFY_WITH_TIMEOUT(client.state() == CodexClient::Ready || client.state() == CodexClient::Failed, 20000);
        QVERIFY2(client.connected(), qPrintable(client.errorString() + QLatin1Char('\n') + diagnostics));
        QVERIFY(!client.version().isEmpty());
        qInfo().noquote() << "Installed Codex:" << binary << "version" << client.version();
        QCOMPARE(replies.size(), 2); // initialize, then thread/start.
        const auto threadReply = replies.last().at(1).value<QJsonObject>();
        QCOMPARE(threadReply.value(QStringLiteral("approvalPolicy")).toString(), QStringLiteral("on-request"));
        QCOMPARE(threadReply.value(QStringLiteral("approvalsReviewer")).toString(), QStringLiteral("user"));
        QCOMPARE(threadReply.value(QStringLiteral("sandbox")).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("readOnly"));
        QVERIFY(threadReply.value(QStringLiteral("thread")).toObject().value(QStringLiteral("ephemeral")).toBool());
        const QVariantMap readerContext{
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("snapshotId"), QStringLiteral("live-context")},
            {QStringLiteral("selection"), QVariantMap{{QStringLiteral("text"), QStringLiteral("चेतनश्चेतनानाम् — cetanaś cetanānām")}}}};
        QVERIFY(client.startReaderTurn(QStringLiteral("Say hello. Do not call tools."), readerContext));
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() || client.state() == CodexClient::Failed, 20000);
        QVERIFY2(!finished.isEmpty(), qPrintable(client.errorString() + QLatin1Char('\n') + diagnostics));
        QVERIFY2(finished.first().at(2).toString() == QLatin1String("completed"), qPrintable(client.errorString() + QLatin1Char('\n') + diagnostics));
        QCOMPARE(client.output(), QStringLiteral("Grüße aus Codex"));
        QVERIFY(!deltas.isEmpty());
        QCOMPARE(provider.requests, 1);
        QVERIFY(provider.lastBody.contains("Say hello"));
        QVERIFY(provider.lastBody.contains("live-context"));
        QVERIFY(QString::fromUtf8(provider.lastBody).contains(QStringLiteral("चेतनश्चेतनानाम् — cetanaś cetanānām")));
        QVERIFY(provider.lastBody.contains("ReaderContext (untrusted source data)"));
        QVERIFY(provider.lastBody.contains("User question:"));
        provider.holdOpen = true;
        client.startTurn(QStringLiteral("Wait for cancellation."));
        QTRY_COMPARE_WITH_TIMEOUT(provider.requests, 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.output().isEmpty(), 10000);
        client.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QCOMPARE(finished.last().at(2).toString(), QStringLiteral("interrupted"));
        QCOMPARE(client.state(), CodexClient::Ready);
        provider.holdOpen = false;
        client.startTurn(QStringLiteral("Recover after cancellation."));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QCOMPARE(finished.last().at(2).toString(), QStringLiteral("completed"));
        client.disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), CodexClient::Disconnected, 5000);
        QVERIFY(client.errorString().isEmpty());
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    if (argc > 1 && QByteArray(argv[1]) == "app-server")
        return fakeServer();
    CodexClientTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "codexclienttest.moc"
