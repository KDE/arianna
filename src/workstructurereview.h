// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "codexclient.h"
#include <QNetworkAccessManager>
#include <QObject>
#include <QVariantMap>
#include <qqmlintegration.h>

class WorkStructureReview : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY changed)
    Q_PROPERTY(QVariantMap draft READ draft NOTIFY changed)
    Q_PROPERTY(QVariantList terms READ terms CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(int draftVersion READ draftVersion NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString aiRequest READ aiRequest NOTIFY changed)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY changed)
public:
    explicit WorkStructureReview(QObject *parent = nullptr);
    QVariantMap snapshot() const
    {
        return m_snapshot.toVariantMap();
    }
    QVariantMap draft() const
    {
        return m_draft.toVariantMap();
    }
    QVariantList terms() const;
    bool busy() const
    {
        return m_busy;
    }
    bool canCancel() const
    {
        return m_busy && !m_writing;
    }
    int draftVersion() const
    {
        return m_draftVersion;
    }
    bool readOnly() const
    {
        return m_readOnly;
    }
    QString message() const
    {
        return m_message;
    }
    QString aiRequest() const;
    Q_INVOKABLE void analyze(const QString &bookId, const QString &baseUrl, const QString &token);
    Q_INVOKABLE void askModel();
    Q_INVOKABLE void commit(const QVariantMap &draft);
    Q_INVOKABLE void cancel();
Q_SIGNALS:
    void changed();
    void saved(const QString &bookId);

private:
    QJsonObject modelContext() const;
    void sendModelTurn();
    void setMessage(const QString &text);
    QNetworkAccessManager m_network;
    // Same transport/authentication as Reader, with an independently owned
    // ephemeral thread/process: closing either view cannot interrupt the other.
    CodexClient m_codex;
    QJsonObject m_snapshot, m_draft, m_submittedModelSnapshot;
    QString m_bookId, m_baseUrl, m_token, m_message;
    quint64 m_generation = 0;
    int m_draftVersion = 0;
    bool m_writing = false;
    quint64 m_modelGeneration = 0;
    bool m_modelActive = false;
    bool m_busy = false, m_readOnly = false, m_modelPending = false;
};
