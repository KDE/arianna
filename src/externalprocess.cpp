// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "externalprocess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

ExternalProcess::ExternalProcess(QObject *parent)
    : QObject(parent)
    , process(new QProcess(this))
    , fileWatcher(new QFileSystemWatcher(this))
    , editedFileChangedTimer(new QTimer(this))
{
    editedFileChangedTimer->setSingleShot(true);
    editedFileChangedTimer->setInterval(500);

    connect(process, &QProcess::started, this, &ExternalProcess::handleStarted);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &ExternalProcess::handleFinished);
    connect(process, &QProcess::errorOccurred, this, &ExternalProcess::handleErrorOccurred);
    connect(process, &QProcess::readyReadStandardOutput, this, &ExternalProcess::handleReadyRead);
    connect(fileWatcher, &QFileSystemWatcher::fileChanged, this, &ExternalProcess::handleEditedFileChanged);
    connect(fileWatcher, &QFileSystemWatcher::directoryChanged, this, &ExternalProcess::handleEditedDirectoryChanged);
    connect(editedFileChangedTimer, &QTimer::timeout, this, [this] {
        const QString file = pendingEditedFile;
        pendingEditedFile.clear();

        if (!file.isEmpty()) {
            Q_EMIT editedFileChanged(file);
            if (!currentEditorSessionId.isEmpty()) {
                Q_EMIT editorSessionFileChanged(currentEditorSessionId, file);
            }
        }
    });
}

void ExternalProcess::start(const QString &program, const QStringList &arguments)
{
    QString editorProgram = program.trimmed();
    QStringList editorArguments = arguments;
    if (editorProgram.isEmpty() || editorArguments.isEmpty() || process->state() != QProcess::NotRunning) {
        return;
    }

    if (!QFileInfo::exists(editorProgram) && editorProgram.contains(QLatin1Char(' '))) {
        const QStringList command = QProcess::splitCommand(editorProgram);
        if (command.isEmpty()) {
            return;
        }

        editorProgram = command.first();
        editorArguments = command.mid(1) + editorArguments;
    }

    watchFile(arguments.first());

    process->start(editorProgram, editorArguments);
}

bool ExternalProcess::startEditorSession(const QString &sessionId, const QString &program, const QStringList &arguments)
{
    if (sessionId.isEmpty() || program.isEmpty() || arguments.isEmpty()) {
        return false;
    }

    if (process->state() != QProcess::NotRunning) {
        if (currentEditorSessionId == sessionId) {
            activateWindowForFile(arguments.first());
            return true;
        }

        qWarning() << "Cannot start editor session while another external process is running:" << currentEditorSessionId;
        return false;
    }

    currentEditorSessionId = sessionId;
    start(program, arguments);
    const bool started = process->state() != QProcess::NotRunning;
    if (!started) {
        currentEditorSessionId.clear();
    }
    return started;
}

bool ExternalProcess::startDetached(const QString &program, const QStringList &arguments)
{
    QString detachedProgram = program.trimmed();
    QStringList detachedArguments = arguments;
    if (detachedProgram.isEmpty()) {
        return false;
    }

    if (!QFileInfo::exists(detachedProgram) && detachedProgram.contains(QLatin1Char(' '))) {
        const QStringList command = QProcess::splitCommand(detachedProgram);
        if (command.isEmpty()) {
            return false;
        }

        detachedProgram = command.first();
        detachedArguments = command.mid(1) + detachedArguments;
    }

    return QProcess::startDetached(detachedProgram, detachedArguments);
}

void ExternalProcess::stop()
{
    if (process->state() == QProcess::NotRunning) {
        return;
    }

    process->terminate();
    if (!process->waitForFinished(3000)) {
        process->kill();
    }
}

bool ExternalProcess::isEditorSessionRunning(const QString &sessionId) const
{
    return !sessionId.isEmpty() && currentEditorSessionId == sessionId && process->state() != QProcess::NotRunning;
}

bool ExternalProcess::activateWindowForFile(const QString &filePath) const
{
    const QString fileName = QFileInfo(filePath).fileName();
    if (fileName.isEmpty()) {
        return false;
    }

    const QString helper =
        QStandardPaths::findExecutable(QStringLiteral("kwin_wmgmt_helper"),
                                       {QDir::home().filePath(QStringLiteral("bin")), QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin")});
    if (helper.isEmpty()) {
        return false;
    }

    QProcess activator;
    activator.setProgram(helper);
    activator.setArguments({fileName});
    activator.setProcessChannelMode(QProcess::MergedChannels);
    activator.start();
    if (!activator.waitForFinished(1500)) {
        activator.kill();
        activator.waitForFinished();
        return false;
    }

    return activator.exitStatus() == QProcess::NormalExit && activator.exitCode() == 0;
}

void ExternalProcess::clearWatchedFile()
{
    clearWatchedFileState(true);
}

void ExternalProcess::clearWatchedFileState(const bool clearEditorSession)
{
    editedFileChangedTimer->stop();
    pendingEditedFile.clear();
    watchedFilePath.clear();
    if (clearEditorSession) {
        currentEditorSessionId.clear();
    }
    watchedFileLastModified = {};
    watchedFileSize = -1;

    if (!fileWatcher->files().isEmpty()) {
        fileWatcher->removePaths(fileWatcher->files());
    }

    if (!fileWatcher->directories().isEmpty()) {
        fileWatcher->removePaths(fileWatcher->directories());
    }
}

QByteArray ExternalProcess::readAllStandardOutput()
{
    return process->readAllStandardOutput();
}

QByteArray ExternalProcess::readAllStandardError()
{
    return process->readAllStandardError();
}

void ExternalProcess::handleStarted()
{
    Q_EMIT processStarted();
    if (!currentEditorSessionId.isEmpty()) {
        Q_EMIT editorSessionProcessStarted(currentEditorSessionId, process->processId());
    }
}

void ExternalProcess::handleFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString finishedEditorSessionId = currentEditorSessionId;
    Q_EMIT processFinished(exitCode, exitStatus);
    if (!finishedEditorSessionId.isEmpty()) {
        Q_EMIT editorSessionFinished(finishedEditorSessionId, exitCode, exitStatus);
    }
    clearWatchedFile();
}

void ExternalProcess::handleErrorOccurred(QProcess::ProcessError error)
{
    const QString failedEditorSessionId = currentEditorSessionId;
    Q_EMIT processErrorOccurred(error);
    if (error == QProcess::FailedToStart && !failedEditorSessionId.isEmpty()) {
        Q_EMIT editorSessionFinished(failedEditorSessionId, -1, QProcess::CrashExit);
        clearWatchedFile();
    }
}

void ExternalProcess::handleEditedFileChanged(const QString &file)
{
    qDebug() << "Edited file changed:" << file;
    if (QFile::exists(file) && !fileWatcher->files().contains(file)) {
        fileWatcher->addPath(file);
    }

    if (QFileInfo(file).absoluteFilePath() != watchedFilePath) {
        return;
    }

    updateWatchedFileState();
    scheduleEditedFileChanged(file);
}

void ExternalProcess::handleEditedDirectoryChanged(const QString &path)
{
    Q_UNUSED(path)

    if (watchedFilePath.isEmpty() || !QFile::exists(watchedFilePath)) {
        return;
    }

    if (!fileWatcher->files().contains(watchedFilePath)) {
        fileWatcher->addPath(watchedFilePath);
        qDebug() << "Re-added file to watcher:" << watchedFilePath;
    }

    if (updateWatchedFileState()) {
        scheduleEditedFileChanged(watchedFilePath);
    }
}

void ExternalProcess::handleReadyRead()
{
    Q_EMIT readyRead();
}

void ExternalProcess::watchFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    const QString absoluteFilePath = fileInfo.absoluteFilePath();

    if (absoluteFilePath.isEmpty()) {
        return;
    }

    if (absoluteFilePath != watchedFilePath) {
        clearWatchedFileState(false);
    }

    watchedFilePath = absoluteFilePath;
    updateWatchedFileState();

    if (QFile::exists(watchedFilePath) && !fileWatcher->files().contains(watchedFilePath)) {
        fileWatcher->addPath(watchedFilePath);
    }

    const QString directory = QFileInfo(watchedFilePath).absolutePath();
    if (!directory.isEmpty() && !fileWatcher->directories().contains(directory)) {
        fileWatcher->addPath(directory);
    }
}

void ExternalProcess::scheduleEditedFileChanged(const QString &filePath)
{
    pendingEditedFile = QFileInfo(filePath).absoluteFilePath();
    editedFileChangedTimer->start();
}

bool ExternalProcess::updateWatchedFileState()
{
    const QFileInfo fileInfo(watchedFilePath);

    if (!fileInfo.exists()) {
        const bool changed = watchedFileSize != -1 || watchedFileLastModified.isValid();
        watchedFileSize = -1;
        watchedFileLastModified = {};
        return changed;
    }

    const QDateTime lastModified = fileInfo.lastModified();
    const qint64 size = fileInfo.size();
    const bool changed = watchedFileLastModified.isValid() && (watchedFileLastModified != lastModified || watchedFileSize != size);

    watchedFileLastModified = lastModified;
    watchedFileSize = size;

    return changed;
}

#include "moc_externalprocess.cpp"
