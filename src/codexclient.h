// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>
#include <qqmlintegration.h>

// One lazy app-server connection per QML engine, independent of document services.
class CodexClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString version READ version NOTIFY sessionChanged)
    Q_PROPERTY(QString threadId READ threadId NOTIFY sessionChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    enum State {
        Disconnected,
        Connecting,
        Ready,
        StartingTurn,
        Running,
        Cancelling,
        Stopping,
        Failed
    };
    Q_ENUM(State)

    explicit CodexClient(QObject *parent = nullptr);
    ~CodexClient() override;

    State state() const
    {
        return m_state;
    }
    bool connected() const;
    bool busy() const;
    QString version() const
    {
        return m_version;
    }
    QString threadId() const
    {
        return m_threadId;
    }
    QString output() const
    {
        return m_output;
    }
    QString errorString() const
    {
        return m_error;
    }

    Q_INVOKABLE void connectToServer(const QString &executable);
    Q_INVOKABLE void startTurn(const QString &text);
    Q_INVOKABLE bool startReaderTurn(const QString &question, const QVariantMap &context);
    bool startStructuredTurn(const QJsonObject &context, const QJsonObject &schema, const QString &task);
    void setTaskInstructions(const QString &instructions) { m_taskInstructions = instructions; }
    QJsonObject modelInfo() const { return m_modelInfo; }
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void disconnectFromServer();
    // Bounded blocking cleanup only at application teardown (or destruction).
    void shutdown();

Q_SIGNALS:
    void stateChanged();
    void sessionChanged();
    void outputChanged();
    void errorStringChanged();
    void errorOccurred(const QString &message);
    void textDelta(const QString &threadId, const QString &turnId, const QString &itemId, const QString &delta);
    void structuredAnswer(const QString &threadId, const QString &turnId, const QString &status, const QString &text);
    void turnFinished(const QString &threadId, const QString &turnId, const QString &status);
    void notificationReceived(const QString &method, const QJsonObject &params);
    void requestCompleted(const QString &method, const QJsonObject &result);
    void approvalDenied(const QString &method);
    void diagnosticReceived(const QString &message);

private:
    struct PendingRequest {
        QString method;
        qint64 deadline;
    };
    void setState(State state);
    void setError(const QString &message);
    void fail(const QString &message);
    bool writeMessage(const QJsonObject &message);
    void request(const QString &method, const QJsonObject &params);
    bool startInput(const QJsonArray &input, const QJsonObject &schema = {});
    void readOutput();
    void handleMessage(const QJsonObject &message);
    void handleResult(const QString &method, const QJsonObject &result);
    void handleNotification(const QString &method, const QJsonObject &params);
    void handleServerRequest(const QJsonValue &id, const QString &method);
    void interruptTurn();
    void finishTurn(const QJsonObject &turn);
    void updateItem(const QString &id, const QString &text, bool append);
    void beginStop();
    void clearSession();

    QProcess m_process;
    QTemporaryDir m_workspace;
    QTimer m_requestTimer;
    QTimer m_stopTimer;
    QTimer m_cancelTimer;
    QByteArray m_stdout;
    QHash<qint64, PendingRequest> m_pending;
    QHash<QString, QString> m_items;
    QStringList m_itemOrder;
    State m_state = Disconnected;
    QString m_version;
    QString m_threadId;
    QString m_turnId;
    QString m_output;
    QString m_error;
    qint64 m_nextId = 0;
    bool m_turnPending = false;
    bool m_cancelRequested = false;
    bool m_interruptSent = false;
    bool m_expectedExit = false;
    bool m_structuredTurn = false;
    QString m_finalText;
    QString m_taskInstructions;
    QJsonObject m_modelInfo;
};
