// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "externalprocess.h"

#include "maintenancefiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QUuid>

namespace
{
bool isSigilCommand(const QString &program)
{
    const QString trimmed = program.trimmed();
    const QStringList command = QFileInfo::exists(trimmed) ? QStringList{trimmed} : QProcess::splitCommand(trimmed);
    return !command.isEmpty() && QFileInfo(command.first()).completeBaseName().compare(QLatin1String("sigil"), Qt::CaseInsensitive) == 0;
}

bool sigilTextArguments(QStringList &arguments, QString &text)
{
    // Keep the generic editor arguments intact; normalize only Sigil's confirmed CLI option.
    bool hasText = false;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if ((argument == QLatin1String("--select-text") || argument == QLatin1String("--selected-text")) && i + 1 < arguments.size()) {
            text = arguments.takeAt(i + 1);
        } else if (argument.startsWith(QLatin1String("--select-text=")) || argument.startsWith(QLatin1String("--selected-text="))) {
            text = argument.mid(argument.indexOf(QLatin1Char('=')) + 1);
        } else {
            continue;
        }
        arguments[i] = QStringLiteral("--select-text=") + text;
        hasText = true;
    }
    return hasText;
}

QString documentArgument(const QStringList &arguments, const QString &prefix)
{
    for (const auto &argument : arguments)
        if (argument.startsWith(prefix)) return argument.mid(prefix.size());
    return {};
}
}

ExternalProcess::ExternalProcess(QObject *parent)
    : ExternalProcess(QDBusConnection::sessionBus(), 3000, parent)
{
}

ExternalProcess::ExternalProcess(const QDBusConnection &connection, int navigationTimeoutMs, QObject *parent)
    : QObject(parent)
    , process(new QProcess(this))
    , fileWatcher(new QFileSystemWatcher(this))
    , editedFileChangedTimer(new QTimer(this))
    , sigilNavigation(new SigilNavigation(connection, navigationTimeoutMs, this))
    , sigilSessionTimer(new QTimer(this))
    , editorConnection(connection)
    , editorTimeoutMs(navigationTimeoutMs)
{
    editedFileChangedTimer->setSingleShot(true);
    editedFileChangedTimer->setInterval(500);
    sigilSessionTimer->setInterval(1000);
    connect(sigilNavigation, &SigilNavigation::finished, this, &ExternalProcess::handleSigilNavigation);
    connect(sigilSessionTimer, &QTimer::timeout, this, [this] {
        // A lost D-Bus name alone does not prove that the editor closed the working copy.
        if (sigilNavigationPending || MaintenanceFiles::processAlive(sigilProcessId, sigilProcessToken)) {
            return;
        }
        sigilSessionTimer->stop();
        sigilProcessId = -1;
        sigilProcessToken.clear();
        if (process->state() == QProcess::NotRunning) {
            const QString sessionId = currentEditorSessionId;
            clearWatchedFile();
            Q_EMIT editorSessionFinished(sessionId, 0, QProcess::NormalExit);
        }
    });

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
    if (editorProgram.isEmpty() || editorArguments.isEmpty() || process->state() != QProcess::NotRunning || sigilNavigationPending || sigilProcessId > 0) {
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

    qInfo().noquote() << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << "Editor start Arianna PID" << QCoreApplication::applicationPid()
                      << "path=ExternalProcess::start/QProcess"
                      << "session" << currentEditorSessionId << "service=org.sigil_ebook.Sigil" << "program" << editorProgram << "arguments" << editorArguments;
    process->start(editorProgram, editorArguments);
}

bool ExternalProcess::startExternalEditor(const QString &program, const QStringList &arguments)
{
    if (program.trimmed().isEmpty() || arguments.isEmpty() || !QFileInfo(arguments.first()).isFile()) return false;
    const QString file = QFileInfo(arguments.first()).absoluteFilePath();
    auto *editor = externalEditors.value(file);
    if (!editor) {
        editor = new ExternalProcess(editorConnection, editorTimeoutMs, this);
        externalEditors.insert(file, editor);
        // Only the file-reload notification escapes. Library session/import
        // signals stay private to this unmanaged editor.
        connect(editor, &ExternalProcess::editedFileChanged, this, &ExternalProcess::editedFileChanged);
    }
    return editor->startEditorSession(QStringLiteral("external:") + file, program, arguments);
}

bool ExternalProcess::startEditorSession(const QString &sessionId, const QString &program, const QStringList &arguments)
{
    if (sessionId.isEmpty() || program.isEmpty() || arguments.isEmpty()) {
        return false;
    }

    QStringList editorArguments = arguments;
    QString selectedText;
    const bool sigilTextRequest = isSigilCommand(program) && sigilTextArguments(editorArguments, selectedText);
    const QString documentFile = isSigilCommand(program) ? documentArgument(editorArguments, QStringLiteral("--open-document=")) : QString();
    if (sigilNavigationPending) {
        const bool sameCopy =
            currentEditorSessionId == sessionId && watchedFilePath == QFileInfo(arguments.first()).absoluteFilePath() && program == pendingEditorProgram;
        if (sameCopy && program == pendingEditorProgram && editorArguments != pendingEditorArguments && !queuedEditorArguments.contains(editorArguments)) {
            queuedEditorArguments.enqueue(editorArguments);
            qInfo() << "Sigil request queued until readiness/navigation completes:" << sessionId;
        } else {
            qInfo() << "Sigil navigation already pending; duplicate request suppressed:" << sessionId << sameCopy;
        }
        return sameCopy;
    }

    if (process->state() != QProcess::NotRunning || sigilProcessId > 0) {
        if (currentEditorSessionId != sessionId || watchedFilePath != QFileInfo(arguments.first()).absoluteFilePath()) {
            qWarning() << "Cannot start editor session while another external process is running:" << currentEditorSessionId;
            return false;
        }
    }

    if (sigilTextRequest || !documentFile.isEmpty()) {
        currentEditorSessionId = sessionId;
        watchFile(arguments.first());
        pendingEditorProgram = program;
        pendingEditorArguments = editorArguments;
        sigilNavigationPending = true;
        qInfo() << "Checking Sigil D-Bus navigation for working copy:" << watchedFilePath;
        if (!documentFile.isEmpty())
            sigilNavigation->openDocument(watchedFilePath, documentFile, documentArgument(editorArguments, QStringLiteral("--request-id=")), false,
                documentArgument(editorArguments, QStringLiteral("--prepare-anchor=")), documentArgument(editorArguments, QStringLiteral("--verse-reference=")));
        else
            sigilNavigation->navigate(watchedFilePath, selectedText);
        return true; // The eventual result is reported asynchronously.
    }

    return launchEditorSession(sessionId, program, editorArguments);
}

bool ExternalProcess::isSigilEditor(const QString &program) const
{
    return isSigilCommand(program);
}

bool ExternalProcess::startEditorDocumentSession(const QString &sessionId, const QString &program, const QString &bookPath, const QString &documentFile,
                                                const QString &anchorId, const QString &verseReference)
{
    if (sessionId.isEmpty() || program.isEmpty() || bookPath.isEmpty() || documentFile.isEmpty()) return false;
    if (!isSigilCommand(program)) return startEditorSession(sessionId, program, {bookPath});
    if (auto *session = documentSessions.value(sessionId))
        return session->startEditorDocumentSession(sessionId, program, bookPath, documentFile, anchorId, verseReference);
    // A starting editor is authoritative. Never race it with a second launch
    // for another book. Existing sessions keep their own import watcher.
    if (sigilNavigationPending) {
        return currentEditorSessionId == sessionId && watchedFilePath == QFileInfo(bookPath).absoluteFilePath()
            && pendingEditorProgram == program && documentArgument(pendingEditorArguments, QStringLiteral("--open-document=")) == documentFile
            && documentArgument(pendingEditorArguments, QStringLiteral("--prepare-anchor=")) == anchorId
            && documentArgument(pendingEditorArguments, QStringLiteral("--verse-reference=")) == verseReference;
    }
    for (auto *session : std::as_const(documentSessions))
        if (session->sigilNavigationPending) return false;
    if (!currentEditorSessionId.isEmpty() && currentEditorSessionId != sessionId && isEditorSessionRunning(currentEditorSessionId)) {
        auto *session = new ExternalProcess(editorConnection, editorTimeoutMs, this);
        documentSessions.insert(sessionId, session);
        connect(session, &ExternalProcess::editorSessionProcessStarted, this, &ExternalProcess::editorSessionProcessStarted);
        connect(session, &ExternalProcess::editorSessionFileChanged, this, &ExternalProcess::editorSessionFileChanged);
        connect(session, &ExternalProcess::editorSessionNavigationFinished, this, &ExternalProcess::editorSessionNavigationFinished);
        connect(session, &ExternalProcess::editorSessionFinished, this, [this, session, sessionId](const QString &id, int code, QProcess::ExitStatus status) {
            documentSessions.remove(sessionId);
            Q_EMIT editorSessionFinished(id, code, status);
            session->deleteLater();
        });
        return session->startEditorDocumentSession(sessionId, program, bookPath, documentFile, anchorId, verseReference);
    }
    QStringList arguments{bookPath, QStringLiteral("--open-document=") + documentFile,
        QStringLiteral("--request-id=") + QUuid::createUuid().toString(QUuid::WithoutBraces)};
    if (!anchorId.isEmpty()) {
        arguments << QStringLiteral("--prepare-anchor=") + anchorId << QStringLiteral("--verse-reference=") + verseReference;
    }
    return startEditorSession(sessionId, program, arguments);
}

bool ExternalProcess::launchEditorSession(const QString &sessionId, const QString &program, const QStringList &arguments)
{
    if (process->state() != QProcess::NotRunning || sigilProcessId > 0) {
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

void ExternalProcess::handleSigilNavigation(SigilNavigation::Outcome outcome,
                                            const QString &owner,
                                            qint64 processId,
                                            const QString &errorName,
                                            const QString &message)
{
    const bool wasStarting = sigilStartupPending;
    sigilStartupPending = false;
    sigilNavigationPending = false;
    const QString sessionId = currentEditorSessionId;
    const QString program = pendingEditorProgram;
    const QStringList arguments = pendingEditorArguments;
    pendingEditorProgram.clear();
    pendingEditorArguments.clear();

    QString status;
    QString detail;
    switch (outcome) {
    case SigilNavigation::AnchorEditPrepared:
        status = QStringLiteral("anchor-prepared");
        detail = QStringLiteral("Sigil opened the exact XHTML file and prepared the AI anchor instruction without submitting it.");
        break;
    case SigilNavigation::DocumentOpened:
        status = QStringLiteral("document-opened");
        detail = QStringLiteral("Sigil opened the requested EPUB and exact XHTML document.");
        break;
    case SigilNavigation::Found:
        status = QStringLiteral("found");
        detail = QStringLiteral("Sigil found the selected text in the working copy.");
        break;
    case SigilNavigation::NotFound:
        status = QStringLiteral("not-found");
        detail = QStringLiteral("Sigil protocol 1 replied found=false: the selected text was not found. No CLI retry.");
        break;
    case SigilNavigation::BookNotOpen:
        status = QStringLiteral("book-not-open");
        detail = QStringLiteral("Sigil explicitly rejected navigation to this working copy.");
        break;
    case SigilNavigation::ServiceUnavailable:
        status = QStringLiteral("service-unavailable");
        detail = QStringLiteral("No registered or starting Sigil; launch once with the working copy and text.");
        break;
    case SigilNavigation::Rejected:
        status = QStringLiteral("rejected");
        detail = QStringLiteral("Sigil D-Bus navigation rejected; no additional editor launch.");
        break;
    case SigilNavigation::QueryError:
        status = QStringLiteral("ipc-error");
        detail = QStringLiteral("D-Bus query failed; no additional editor launch.");
        break;
    case SigilNavigation::IpcError:
        status = QStringLiteral("ipc-error");
        detail = QStringLiteral("Sigil IPC failed after dispatch; execution is uncertain. No CLI retry to avoid opening the copy twice.");
        break;
    case SigilNavigation::ReadinessTimeout:
        status = QStringLiteral("not-ready");
        detail = QStringLiteral("Sigil editor readiness timed out; no additional editor launch.");
        break;
    case SigilNavigation::Busy:
        status = QStringLiteral("busy");
        detail = QStringLiteral("The working copy is open in Sigil, but the editor is busy. No CLI retry.");
        break;
    case SigilNavigation::AmbiguousBook:
        status = QStringLiteral("ambiguous-book");
        detail = QStringLiteral("Sigil has several windows for this working copy. No CLI retry.");
        break;
    case SigilNavigation::Ready:
        status = QStringLiteral("started");
        detail = QStringLiteral("Sigil is ready. Book and text were already delivered once by CLI; no D-Bus navigation retry.");
        break;
    case SigilNavigation::OpenFailed:
        status = QStringLiteral("open-failed");
        detail = QStringLiteral("Sigil could not open the EPUB; no additional editor launch.");
        break;
    }
    if (!errorName.isEmpty()) {
        detail += QStringLiteral(" %1: %2").arg(errorName, message);
    } else if (!message.isEmpty()) {
        detail += QLatin1Char(' ') + message;
    }
    qInfo().noquote() << "Sigil navigation:" << sessionId << watchedFilePath << owner << status << detail;

    // Only absence permits the first launch. In particular BookNotOpen from an
    // older service and any ambiguous IPC/startup result must never start again.
    if (wasStarting || outcome != SigilNavigation::ServiceUnavailable || !owner.isEmpty() || processId > 0) {
        const bool newOwner = processId > 0 && processId != sigilProcessId;
        if (processId > 0) {
            sigilProcessId = processId;
            sigilProcessToken = MaintenanceFiles::processToken(processId);
        }
        if (sigilProcessId > 0) {
            sigilSessionTimer->start();
            if (newOwner && processId != launchedEditorProcessId)
                Q_EMIT editorSessionProcessStarted(sessionId, processId);
        }
        Q_EMIT editorSessionNavigationFinished(sessionId, status, detail);
        if (sigilProcessId <= 0 && process->state() == QProcess::NotRunning) {
            clearWatchedFile();
            Q_EMIT editorSessionFinished(sessionId, -1, QProcess::CrashExit);
        }
        drainEditorRequests(sessionId, program);
        return;
    }

    if (process->state() != QProcess::NotRunning || sigilProcessId > 0) {
        // An existing session remains authoritative even when its navigation interface disappears.
        qInfo() << "Sigil CLI fallback suppressed for existing editor session:" << sessionId;
        activateWindowForFile(arguments.first());
        Q_EMIT editorSessionNavigationFinished(sessionId, QStringLiteral("existing-session"), QStringLiteral("Existing editor retained; no second launch."));
        drainEditorRequests(sessionId, program);
        return;
    }
    qInfo() << "Starting configured Sigil exactly once for:" << arguments.first();
    if (!launchEditorSession(sessionId, program, arguments)) {
        clearWatchedFile();
        Q_EMIT editorSessionFinished(sessionId, -1, QProcess::CrashExit);
    } else {
        sigilStartupPending = true;
        sigilNavigationPending = true;
        pendingEditorProgram = program;
        pendingEditorArguments = arguments;
        const auto documentFile = documentArgument(arguments, QStringLiteral("--open-document="));
        if (documentFile.isEmpty()) sigilNavigation->awaitStartup(watchedFilePath);
        else sigilNavigation->openDocument(watchedFilePath, documentFile, documentArgument(arguments, QStringLiteral("--request-id=")), true,
            documentArgument(arguments, QStringLiteral("--prepare-anchor=")), documentArgument(arguments, QStringLiteral("--verse-reference=")));
    }
}

void ExternalProcess::drainEditorRequests(const QString &sessionId, const QString &program)
{
    if (queuedEditorArguments.isEmpty())
        return;
    QTimer::singleShot(0, this, [this, sessionId, program] {
        if (currentEditorSessionId != sessionId || sigilNavigationPending || queuedEditorArguments.isEmpty())
            return;
        const auto arguments = queuedEditorArguments.dequeue();
        startEditorSession(sessionId, program, arguments);
    });
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
    if (const auto *session = documentSessions.value(sessionId)) return session->isEditorSessionRunning(sessionId);
    return !sessionId.isEmpty() && currentEditorSessionId == sessionId
        && (process->state() != QProcess::NotRunning || sigilNavigationPending || sigilProcessId > 0);
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

void ExternalProcess::clearWatchedFile(bool keepActiveEditorSession)
{
    if (keepActiveEditorSession && isEditorSessionRunning(currentEditorSessionId)) {
        return;
    }
    clearWatchedFileState(true);
}

void ExternalProcess::clearWatchedFileState(const bool clearEditorSession)
{
    editedFileChangedTimer->stop();
    pendingEditedFile.clear();
    watchedFilePath.clear();
    if (clearEditorSession) {
        sigilNavigation->cancel();
        sigilNavigationPending = false;
        sigilStartupPending = false;
        pendingEditorProgram.clear();
        pendingEditorArguments.clear();
        queuedEditorArguments.clear();
        sigilSessionTimer->stop();
        sigilProcessId = -1;
        sigilProcessToken.clear();
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
    launchedEditorProcessId = process->processId();
    qInfo().noquote() << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << "Editor started Arianna PID" << QCoreApplication::applicationPid()
                      << "editor PID" << launchedEditorProcessId << "path=ExternalProcess::handleStarted" << "program" << process->program() << "arguments"
                      << process->arguments();
    Q_EMIT processStarted();
    if (!currentEditorSessionId.isEmpty()) {
        Q_EMIT editorSessionProcessStarted(currentEditorSessionId, process->processId());
    }
}

void ExternalProcess::handleFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString finishedEditorSessionId = currentEditorSessionId;
    Q_EMIT processFinished(exitCode, exitStatus);
    if (sigilStartupPending && (exitCode != 0 || exitStatus != QProcess::NormalExit)) {
        sigilNavigation->cancel();
        sigilNavigationPending = false;
        sigilStartupPending = false;
        Q_EMIT editorSessionNavigationFinished(finishedEditorSessionId,
                                               QStringLiteral("start-failed"),
                                               QStringLiteral("Sigil exited during startup; no retry."));
    }
    if (sigilNavigationPending || (sigilProcessId > 0 && sigilProcessId != launchedEditorProcessId)) {
        // The D-Bus owner may be a different process than the CLI launcher.
        launchedEditorProcessId = -1;
        return;
    }
    launchedEditorProcessId = -1;
    if (!finishedEditorSessionId.isEmpty()) {
        Q_EMIT editorSessionFinished(finishedEditorSessionId, exitCode, exitStatus);
    }
    clearWatchedFile();
}

void ExternalProcess::handleErrorOccurred(QProcess::ProcessError error)
{
    const QString failedEditorSessionId = currentEditorSessionId;
    Q_EMIT processErrorOccurred(error);
    qWarning() << "External editor process error:" << failedEditorSessionId << process->errorString();
    if (error == QProcess::FailedToStart && sigilStartupPending) {
        sigilNavigation->cancel();
        sigilNavigationPending = false;
        sigilStartupPending = false;
        Q_EMIT editorSessionNavigationFinished(failedEditorSessionId, QStringLiteral("start-failed"), process->errorString());
    }
    if (error == QProcess::FailedToStart && !failedEditorSessionId.isEmpty() && !sigilNavigationPending && sigilProcessId <= 0) {
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
