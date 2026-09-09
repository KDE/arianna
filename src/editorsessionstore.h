// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "bookstateimport.h"
#include "editorsession.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <qqmlintegration.h>

class EditorSessionStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit EditorSessionStore(QObject *parent = nullptr);

    Q_INVOKABLE QVariantMap createEditorSession(const QString &bookId, const QString &authoritativeFilePath);
    Q_INVOKABLE QVariantMap createOrReuseEditorSession(const QString &bookId, const QString &authoritativeFilePath);
    Q_INVOKABLE QVariantMap activeSessionForBook(const QString &bookId) const;
    Q_INVOKABLE QVariantMap sessionInfo(const QString &sessionId) const;
    Q_INVOKABLE void noteEditorProcessStarted(const QString &sessionId, qint64 editorProcessId);
    Q_INVOKABLE void scheduleImport(const QString &sessionId);
    Q_INVOKABLE QVariantMap importNow(const QString &sessionId, bool allowInvalidAnchors = false);
    Q_INVOKABLE QVariantMap finishSession(const QString &sessionId);

Q_SIGNALS:
    void editorSessionImportFinished(const QString &sessionId,
                                     const QString &status,
                                     const QString &message,
                                     const QString &workingCopyPath,
                                     const QString &newStateId,
                                     const QVariantList &anchorValidationIssues);
    void editorSessionFinished(const QString &sessionId, const QString &status, const QString &message, const QString &workingCopyPath);

private:
    QVariantMap createError(const QString &message) const;
    QVariantMap sessionToMap(const EditorSession &session) const;
    QVariantMap resultToMap(const QString &sessionId, const BookStateImportResult &result) const;
    QString normalizedBookId(const QString &bookId) const;
    QString normalizedSessionId(const QString &sessionId) const;
    void startStabilityCheck(const QString &sessionId);
    void completeStabilityCheck(const QString &sessionId, qint64 previousSize, const QDateTime &previousModified);

    QHash<QString, EditorSession> m_sessions;
    QHash<QString, QString> m_activeSessionByBookId;
    QHash<QString, QTimer *> m_debounceTimers;
};
