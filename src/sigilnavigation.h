// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QObject>
#include <QVariantMap>
#include <functional>

class QDBusMessage;

class SigilNavigation : public QObject
{
    Q_OBJECT

public:
    enum Outcome {
        Found,
        NotFound,
        ServiceUnavailable,
        BookNotOpen,
        Rejected,
        QueryError,
        IpcError,
        ReadinessTimeout,
        Busy,
        AmbiguousBook,
        Ready,
        OpenFailed,
        DocumentOpened,
        AnchorEditPrepared,
    };
    Q_ENUM(Outcome)

    explicit SigilNavigation(QObject *parent = nullptr);
    SigilNavigation(const QDBusConnection &connection, int timeoutMs, QObject *parent = nullptr);
    void navigate(const QString &bookPath, const QString &text);
    // The CLI already received the book and text. Poll readiness without
    // delivering that request a second time over D-Bus.
    void awaitStartup(const QString &bookPath);
    void openDocument(const QString &bookPath, const QString &documentFile, const QString &requestId, bool deliveredByCli = false,
                      const QString &anchorId = {}, const QString &verseReference = {});
    void cancel();

Q_SIGNALS:
    void finished(SigilNavigation::Outcome outcome, const QString &owner, qint64 processId, const QString &errorName, const QString &message);

private:
    void call(const QDBusMessage &message, std::function<void(const QDBusMessage &)> callback, int timeoutMs = -1);
    void discover();
    void resolveOwner();
    void queryCapabilities();
    void waitForReady(std::function<void()> retry, Outcome timeoutOutcome = ReadinessTimeout, const QString &errorName = {}, const QString &message = {});
    void sendNavigation();
    void documentResult(const QVariantMap &result);
    void complete(Outcome outcome, const QString &errorName = {}, const QString &message = {});

    QDBusConnection m_connection;
    int m_timeoutMs;
    int m_readinessTimeoutMs;
    quint64 m_request = 0;
    QString m_bookPath;
    QString m_text;
    QString m_documentFile;
    QString m_documentRequestId;
    QString m_anchorId;
    QString m_verseReference;
    QString m_owner;
    qint64 m_processId = -1;
    bool m_waitingForStartup = false;
    bool m_deliveredByCli = false;
    bool m_canOpenBook = false;
    QElapsedTimer m_readinessTimer;
};
