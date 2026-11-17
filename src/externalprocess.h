// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "sigilnavigation.h"

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <qqmlintegration.h>

class ExternalProcess : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit ExternalProcess(QObject *parent = nullptr);
    ExternalProcess(const QDBusConnection &connection, int navigationTimeoutMs, QObject *parent = nullptr);

    Q_INVOKABLE void start(const QString &program, const QStringList &arguments);
    Q_INVOKABLE bool startExternalEditor(const QString &program, const QStringList &arguments);
    Q_INVOKABLE bool startEditorSession(const QString &sessionId, const QString &program, const QStringList &arguments);
    Q_INVOKABLE bool startEditorDocumentSession(const QString &sessionId, const QString &program, const QString &bookPath, const QString &documentFile,
                                               const QString &anchorId = {}, const QString &verseReference = {});
    Q_INVOKABLE bool isSigilEditor(const QString &program) const;
    Q_INVOKABLE bool startDetached(const QString &program, const QStringList &arguments = {});
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clearWatchedFile(bool keepActiveEditorSession = false);
    Q_INVOKABLE bool isEditorSessionRunning(const QString &sessionId) const;
    Q_INVOKABLE bool activateWindowForFile(const QString &filePath) const;
    Q_INVOKABLE QByteArray readAllStandardOutput();
    Q_INVOKABLE QByteArray readAllStandardError();

Q_SIGNALS:
    void processStarted();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void processErrorOccurred(QProcess::ProcessError error);
    void editedFileChanged(const QString &file);
    void editorSessionProcessStarted(const QString &sessionId, qint64 processId);
    void editorSessionFileChanged(const QString &sessionId, const QString &file);
    void editorSessionFinished(const QString &sessionId, int exitCode, QProcess::ExitStatus exitStatus);
    void editorSessionNavigationFinished(const QString &sessionId, const QString &status, const QString &message);
    void readyRead();

private Q_SLOTS:
    void handleStarted();
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleErrorOccurred(QProcess::ProcessError error);
    void handleEditedFileChanged(const QString &file);
    void handleEditedDirectoryChanged(const QString &path);
    void handleReadyRead();

private:
    void clearWatchedFileState(bool clearEditorSession);
    void watchFile(const QString &filePath);
    void scheduleEditedFileChanged(const QString &filePath);
    bool updateWatchedFileState();
    bool launchEditorSession(const QString &sessionId, const QString &program, const QStringList &arguments);
    void handleSigilNavigation(SigilNavigation::Outcome outcome, const QString &owner, qint64 processId, const QString &errorName, const QString &message);
    void drainEditorRequests(const QString &sessionId, const QString &program);

    QProcess *process = nullptr;
    QFileSystemWatcher *fileWatcher = nullptr;
    QTimer *editedFileChangedTimer = nullptr;
    QString watchedFilePath;
    QString pendingEditedFile;
    QString currentEditorSessionId;
    QDateTime watchedFileLastModified;
    qint64 watchedFileSize = -1;
    SigilNavigation *sigilNavigation = nullptr;
    QTimer *sigilSessionTimer = nullptr;
    bool sigilNavigationPending = false;
    bool sigilStartupPending = false;
    QString pendingEditorProgram;
    QStringList pendingEditorArguments;
    QQueue<QStringList> queuedEditorArguments;
    qint64 sigilProcessId = -1;
    QString sigilProcessToken;
    qint64 launchedEditorProcessId = -1;
    QDBusConnection editorConnection;
    int editorTimeoutMs;
    QHash<QString, ExternalProcess *> documentSessions;
    QHash<QString, ExternalProcess *> externalEditors;
};
