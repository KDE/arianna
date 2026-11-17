// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QXmlStreamReader>
#include <memory>

// Speaks SSIP to the real libspeechd client without producing audio. The server
// has its own event loop so client destruction can synchronously close a session.
class FakeSpeechDispatcher : public QThread
{
public:
    struct IndexMark {
        QString name;
        qsizetype start;
    };
    struct Message {
        int id;
        int client;
        QString text;
        QString language;
        QString voice;
        QString module;
        QString dataMode;
        QString rawText;
        QList<IndexMark> marks;
    };
    explicit FakeSpeechDispatcher(const QString &path, bool multipleModules = false, const QString &module = QStringLiteral("test"),
                                  bool sanskrit = false, bool bengali = false)
        : m_path(path)
        , m_multipleModules(multipleModules)
        , m_module(module)
        , m_sanskrit(sanskrit)
        , m_bengali(bengali)
    {
    }
    ~FakeSpeechDispatcher() override
    {
        quit();
        wait();
    }
    bool startServer()
    {
        start();
        m_ready.acquire();
        return m_listening;
    }
    QList<Message> messages()
    {
        QMutexLocker lock(&m_mutex);
        return m_messages;
    }
    QStringList commands()
    {
        QMutexLocker lock(&m_mutex);
        return m_commands;
    }
    void finish(int message)
    {
        QMetaObject::invokeMethod(
            m_server,
            [this, message] {
                for (const auto &client : std::as_const(m_clients))
                    if (client->message == message)
                        event(*client, 702, message);
            },
            Qt::BlockingQueuedConnection);
    }
    void sendOldEnd(int message, int clientId)
    {
        sendOldEvent(702, message, clientId);
    }
    void sendOldBegin(int message, int clientId)
    {
        sendOldEvent(701, message, clientId);
    }
    void sendIndexMark(int message, int clientId, const QString &name)
    {
        QMetaObject::invokeMethod(m_server, [this, message, clientId, name] {
            for (const auto &client : std::as_const(m_clients)) {
                if (client->id == clientId && client->socket && client->socket->state() == QLocalSocket::ConnectedState) {
                    client->socket->write("700-" + QByteArray::number(message) + "\r\n700-" + QByteArray::number(clientId)
                                          + "\r\n700-" + name.toUtf8() + "\r\n700 INDEX MARK\r\n");
                }
            }
        }, Qt::BlockingQueuedConnection);
    }
    void sendOldEvent(int code, int message, int clientId)
    {
        QMetaObject::invokeMethod(
            m_server,
            [this, code, message, clientId] {
                for (const auto &client : std::as_const(m_clients))
                    if (client->id == clientId)
                        event(*client, code, message);
            },
            Qt::BlockingQueuedConnection);
    }
    void disconnectClients()
    {
        QMetaObject::invokeMethod(
            m_server,
            [this] {
                for (const auto &client : std::as_const(m_clients))
                    if (client->socket)
                        client->socket->abort();
            },
            Qt::BlockingQueuedConnection);
    }
    void rejectSpeech(bool reject)
    {
        QMetaObject::invokeMethod(
            m_server,
            [this, reject] {
                m_rejectSpeech = reject;
            },
            Qt::BlockingQueuedConnection);
    }
    void rejectSsml(bool reject)
    {
        QMetaObject::invokeMethod(m_server, [this, reject] { m_rejectSsml = reject; }, Qt::BlockingQueuedConnection);
    }

protected:
    void run() override
    {
        QLocalServer server;
        m_server = &server;
        m_listening = server.listen(m_path);
        connect(&server, &QLocalServer::newConnection, &server, [this, &server] {
            while (auto *socket = server.nextPendingConnection()) {
                auto client = std::make_shared<Client>();
                client->socket = socket;
                client->id = ++m_clientId;
                client->module = m_module;
                m_clients.append(client);
                connect(socket, &QLocalSocket::readyRead, socket, [this, client] {
                    client->buffer += client->socket->readAll();
                    qsizetype end;
                    while ((end = client->buffer.indexOf("\r\n")) >= 0) {
                        const QByteArray line = client->buffer.left(end);
                        client->buffer.remove(0, end + 2);
                        receive(*client, line);
                    }
                });
            }
        });
        m_ready.release();
        exec();
        m_clients.clear();
        m_server = nullptr;
    }

private:
    struct Client {
        QPointer<QLocalSocket> socket;
        int id = 0;
        int message = -1;
        bool data = false;
        QByteArray buffer;
        QByteArray text;
        QString language = QStringLiteral("de");
        QString voice;
        QString dataMode = QStringLiteral("TEXT");
        QString module = QStringLiteral("test");
    };
    void event(Client &client, int code, int message)
    {
        if (client.socket && client.socket->state() == QLocalSocket::ConnectedState)
            client.socket->write(QByteArray::number(code) + '-' + QByteArray::number(message) + "\r\n" + QByteArray::number(code) + '-'
                                 + QByteArray::number(client.id) + "\r\n" + QByteArray::number(code) + " EVENT\r\n");
    }
    void receive(Client &client, const QByteArray &line)
    {
        auto *socket = client.socket.data();
        if (client.data) {
            if (line == ".") {
                client.data = false;
                client.message = ++m_messageId;
                if (client.text.endsWith("\r\n"))
                    client.text.chop(2);
                const QString rawText = QString::fromUtf8(client.text).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
                QString text = rawText;
                QList<IndexMark> marks;
                if (client.dataMode == QLatin1String("SSML")) {
                    text.clear();
                    QXmlStreamReader xml(rawText);
                    while (!xml.atEnd()) {
                        xml.readNext();
                        if (xml.isCharacters()) text += xml.text();
                        if (xml.isStartElement() && xml.name() == QLatin1String("mark"))
                            marks.append({xml.attributes().value(QStringLiteral("name")).toString(), text.size()});
                    }
                }
                {
                    QMutexLocker lock(&m_mutex);
                    m_messages.append({client.message,
                                       client.id,
                                       text,
                                       client.language,
                                       client.voice,
                                       client.module,
                                       client.dataMode,
                                       rawText,
                                       marks});
                }
                socket->write("200-" + QByteArray::number(client.message) + "\r\n200 QUEUED\r\n");
                event(client, 701, client.message);
            } else {
                client.text += (line.startsWith("..") ? line.mid(1) : line) + "\r\n";
            }
            return;
        }
        const QByteArray command = line.toUpper();
        {
            QMutexLocker lock(&m_mutex);
            m_commands.append(QString::fromUtf8(command));
        }
        if (command == "HISTORY GET CLIENT_ID") {
            socket->write("200-" + QByteArray::number(client.id) + "\r\n200 CLIENT ID\r\n");
        } else if (command == "LIST OUTPUT_MODULES") {
            if (m_multipleModules) socket->write("200-generic\r\n");
            if (m_sanskrit || m_bengali) socket->write("200-vits\r\n");
            socket->write("200-" + m_module.toUtf8() + "\r\n200 OK\r\n");
        } else if (command == "GET OUTPUT_MODULE") {
            socket->write("200-" + client.module.toUtf8() + "\r\n200 OK\r\n");
        } else if (command == "LIST SYNTHESIS_VOICES") {
            if ((m_sanskrit || m_bengali) && client.module == QLatin1String("vits")) {
                if (m_sanskrit)
                    socket->write("200-sa_male1\tsa\tmale1\r\n200-sa_male2\tsa\tmale2\r\n");
                if (m_bengali)
                    socket->write("200-bn_female1\tbn\tfemale1\r\n200-bn_male1\tbn\tmale1\r\n");
                socket->write("200 OK\r\n");
            } else {
                if (m_sanskrit && client.module == QLatin1String("generic"))
                    socket->write("200-sa-generic\tsa\tmale1\r\n");
                if (m_bengali)
                    socket->write("200-bn-other\tbn\tmale1\r\n");
                socket->write("200-de-test\tde\tmale1\r\n200-en-test\ten\tfemale1\r\n200 OK\r\n");
            }
        } else if (command == "GET LANGUAGE") {
            socket->write("200-de\r\n200 OK\r\n");
        } else if (command == "SPEAK") {
            if (m_rejectSpeech) {
                socket->write("300 SPEECH REJECTED\r\n");
            } else {
                client.text.clear();
                client.data = true;
                socket->write("200 SEND DATA\r\n");
            }
        } else {
            if (command == "SET SELF SSML_MODE ON" && m_rejectSsml) {
                socket->write("300 SSML REJECTED\r\n");
                return;
            }
            if (command.startsWith("SET SELF LANGUAGE "))
                client.language = QString::fromUtf8(line.mid(18)).remove(QLatin1Char('"'));
            if (command.startsWith("SET SELF SYNTHESIS_VOICE "))
                client.voice = QString::fromUtf8(line.mid(25)).remove(QLatin1Char('"'));
            if (command.startsWith("SET SELF SSML_MODE "))
                client.dataMode = command.endsWith("ON") ? QStringLiteral("SSML") : QStringLiteral("TEXT");
            if (command.startsWith("SET SELF OUTPUT_MODULE "))
                client.module = QString::fromUtf8(line.mid(23)).remove(QLatin1Char('"'));
            socket->write("200 OK\r\n");
            if (command == "CANCEL SELF")
                event(client, 703, client.message);
            if (command == "PAUSE SELF")
                event(client, 704, client.message);
            if (command == "RESUME SELF")
                event(client, 705, client.message);
        }
    }
    QString m_path;
    bool m_multipleModules;
    QString m_module;
    bool m_sanskrit;
    bool m_bengali;
    QLocalServer *m_server = nullptr;
    QSemaphore m_ready;
    bool m_listening = false;
    bool m_rejectSpeech = false;
    bool m_rejectSsml = false;
    int m_clientId = 0;
    int m_messageId = 0;
    QList<std::shared_ptr<Client>> m_clients;
    QMutex m_mutex;
    QList<Message> m_messages;
    QStringList m_commands;
};
