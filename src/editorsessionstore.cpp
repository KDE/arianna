// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "editorsessionstore.h"

#include "booktruthstore.h"
#include "epubcontainer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStandardPaths>

namespace
{
static constexpr int importDebounceMs = 700;
static constexpr int stabilityCheckMs = 350;

static QString uuidToString(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

static QUuid uuidFromString(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        return {};
    }

    if (!value.startsWith(QLatin1Char('{'))) {
        value = QStringLiteral("{") + value + QStringLiteral("}");
    }

    return QUuid(value);
}

static bool addDeviceDataToHash(QIODevice &device, QCryptographicHash &hash)
{
    QByteArray buffer;
    buffer.resize(64 * 1024);
    while (true) {
        const qint64 bytesRead = device.read(buffer.data(), buffer.size());
        if (bytesRead > 0) {
            hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(bytesRead)));
            continue;
        }

        if (bytesRead < 0) {
            return false;
        }

        return device.atEnd();
    }
}

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!addDeviceDataToHash(file, hash)) {
        return {};
    }

    return hash.result().toHex();
}
}

EditorSessionStore::EditorSessionStore(QObject *parent)
    : QObject(parent)
{
}

QVariantMap EditorSessionStore::createError(const QString &message) const
{
    QVariantMap result;
    result.insert(QStringLiteral("error"), true);
    result.insert(QStringLiteral("message"), message);
    return result;
}

QString EditorSessionStore::normalizedSessionId(const QString &sessionId) const
{
    return uuidToString(uuidFromString(sessionId));
}

QString EditorSessionStore::normalizedBookId(const QString &bookId) const
{
    return bookId.trimmed();
}

QVariantMap EditorSessionStore::sessionToMap(const EditorSession &session) const
{
    QVariantMap result;
    result.insert(QStringLiteral("sessionId"), uuidToString(session.sessionId));
    result.insert(QStringLiteral("bookId"), session.bookId);
    result.insert(QStringLiteral("authoritativeFilePath"), session.authoritativeFilePath);
    result.insert(QStringLiteral("workingCopyPath"), session.workingCopyPath);
    result.insert(QStringLiteral("lastImportedFileHash"), QString::fromLatin1(session.lastImportedFileHash));
    result.insert(QStringLiteral("createdAt"), session.createdAt.toString(Qt::ISODateWithMs));
    result.insert(QStringLiteral("lastImportedAt"), session.lastImportedAt.toString(Qt::ISODateWithMs));
    result.insert(QStringLiteral("editorProcessId"), session.editorProcessId);
    result.insert(QStringLiteral("active"), session.active);
    return result;
}

QVariantMap EditorSessionStore::resultToMap(const QString &sessionId, const BookStateImportResult &result) const
{
    QVariantMap map;
    map.insert(QStringLiteral("sessionId"), sessionId);
    map.insert(QStringLiteral("status"), bookStateImportStatusName(result.status));
    map.insert(QStringLiteral("message"), result.errorMessage);
    map.insert(QStringLiteral("previousStateId"), uuidToString(result.previousStateId));
    map.insert(QStringLiteral("newStateId"), uuidToString(result.newStateId));
    map.insert(QStringLiteral("currentStateId"), uuidToString(result.currentStateId));
    map.insert(QStringLiteral("textContentHash"), QString::fromLatin1(result.textContentHash));
    map.insert(QStringLiteral("documentStateHash"), QString::fromLatin1(result.documentStateHash));
    map.insert(QStringLiteral("epubFileHash"), QString::fromLatin1(result.epubFileHash));
    map.insert(QStringLiteral("importedFileHash"), QString::fromLatin1(result.importedFileHash));
    map.insert(QStringLiteral("anchorValidationIssues"), result.anchorValidationIssues);
    if (const auto it = m_sessions.constFind(sessionId); it != m_sessions.constEnd()) {
        map.insert(QStringLiteral("workingCopyPath"), it->workingCopyPath);
    }
    return map;
}

QVariantMap EditorSessionStore::createEditorSession(const QString &bookId, const QString &authoritativeFilePath)
{
    const QString normalizedBook = normalizedBookId(bookId);
    if (normalizedBook.isEmpty()) {
        return createError(QStringLiteral("Cannot open editor without a book identifier"));
    }

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(normalizedBook);
    if (!snapshot.success || !snapshot.state) {
        return createError(snapshot.errorMessage);
    }

    const QString sourcePath = QFileInfo(snapshot.filename.isEmpty() ? authoritativeFilePath : snapshot.filename).absoluteFilePath();
    if (!QFileInfo::exists(sourcePath)) {
        return createError(QStringLiteral("Authoritative EPUB does not exist: %1").arg(sourcePath));
    }

    const QUuid sessionUuid = QUuid::createUuidV7();
    const QString sessionId = uuidToString(sessionUuid);
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        return createError(QStringLiteral("No writable cache location available for editor session"));
    }

    const QString sessionDirectoryPath = QDir(cacheRoot).filePath(QStringLiteral("editor-sessions/") + sessionId);
    QDir sessionDirectory(sessionDirectoryPath);
    if (!sessionDirectory.exists() && !sessionDirectory.mkpath(QStringLiteral("."))) {
        return createError(QStringLiteral("Unable to create editor session directory: %1").arg(sessionDirectoryPath));
    }

    const QString workingCopyPath =
        sessionDirectory.filePath(QFileInfo(sourcePath).fileName().isEmpty() ? QStringLiteral("book.epub") : QFileInfo(sourcePath).fileName());
    QFile::remove(workingCopyPath);
    if (!QFile::copy(sourcePath, workingCopyPath)) {
        return createError(QStringLiteral("Unable to create editor working copy: %1").arg(workingCopyPath));
    }

    const QByteArray initialHash = fileHash(workingCopyPath);
    if (initialHash.isEmpty()) {
        return createError(QStringLiteral("Unable to hash editor working copy: %1").arg(workingCopyPath));
    }

    EditorSession session;
    session.sessionId = sessionUuid;
    session.bookId = normalizedBook;
    session.authoritativeFilePath = sourcePath;
    session.workingCopyPath = workingCopyPath;
    session.lastImportedFileHash = initialHash;
    session.createdAt = QDateTime::currentDateTimeUtc();
    session.lastImportedAt = session.createdAt;
    session.active = true;

    m_sessions.insert(sessionId, session);
    m_activeSessionByBookId.insert(normalizedBook, sessionId);
    return sessionToMap(session);
}

QVariantMap EditorSessionStore::createOrReuseEditorSession(const QString &bookId, const QString &authoritativeFilePath)
{
    const QString normalizedBook = normalizedBookId(bookId);
    if (normalizedBook.isEmpty()) {
        return createError(QStringLiteral("Cannot open editor without a book identifier"));
    }

    auto activeIt = m_activeSessionByBookId.find(normalizedBook);
    if (activeIt != m_activeSessionByBookId.end()) {
        auto sessionIt = m_sessions.find(activeIt.value());
        if (sessionIt != m_sessions.end() && sessionIt->active && QFileInfo::exists(sessionIt->workingCopyPath)) {
            QVariantMap reusedSession = sessionToMap(*sessionIt);
            reusedSession.insert(QStringLiteral("reused"), true);
            return reusedSession;
        }

        m_activeSessionByBookId.erase(activeIt);
    }

    QVariantMap createdSession = createEditorSession(normalizedBook, authoritativeFilePath);
    if (!createdSession.contains(QStringLiteral("error"))) {
        createdSession.insert(QStringLiteral("reused"), false);
    }
    return createdSession;
}

QVariantMap EditorSessionStore::activeSessionForBook(const QString &bookId) const
{
    const QString normalizedBook = normalizedBookId(bookId);
    if (normalizedBook.isEmpty()) {
        return createError(QStringLiteral("Cannot find an editor session without a book identifier"));
    }

    const auto activeIt = m_activeSessionByBookId.constFind(normalizedBook);
    if (activeIt == m_activeSessionByBookId.constEnd()) {
        return createError(QStringLiteral("No active editor session for book: %1").arg(normalizedBook));
    }

    const auto sessionIt = m_sessions.constFind(activeIt.value());
    if (sessionIt == m_sessions.constEnd() || !sessionIt->active || !QFileInfo::exists(sessionIt->workingCopyPath)) {
        return createError(QStringLiteral("No active editor session for book: %1").arg(normalizedBook));
    }

    return sessionToMap(*sessionIt);
}

QVariantMap EditorSessionStore::sessionInfo(const QString &sessionId) const
{
    const QString key = normalizedSessionId(sessionId);
    const auto it = m_sessions.constFind(key);
    if (it == m_sessions.constEnd()) {
        return createError(QStringLiteral("Unknown editor session: %1").arg(sessionId));
    }

    return sessionToMap(*it);
}

void EditorSessionStore::noteEditorProcessStarted(const QString &sessionId, const qint64 editorProcessId)
{
    const QString key = normalizedSessionId(sessionId);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        return;
    }

    it->editorProcessId = editorProcessId;
}

void EditorSessionStore::scheduleImport(const QString &sessionId)
{
    const QString key = normalizedSessionId(sessionId);
    const auto sessionIt = m_sessions.constFind(key);
    if (sessionIt == m_sessions.constEnd() || !sessionIt->active) {
        return;
    }

    QTimer *timer = m_debounceTimers.value(key);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, key] {
            startStabilityCheck(key);
        });
        m_debounceTimers.insert(key, timer);
    }

    timer->start(importDebounceMs);
}

void EditorSessionStore::startStabilityCheck(const QString &sessionId)
{
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd() || !it->active) {
        return;
    }

    const QFileInfo info(it->workingCopyPath);
    if (!info.exists() || !info.isFile()) {
        importNow(sessionId);
        return;
    }

    QTimer::singleShot(stabilityCheckMs, this, [this, sessionId, size = info.size(), modified = info.lastModified()] {
        completeStabilityCheck(sessionId, size, modified);
    });
}

void EditorSessionStore::completeStabilityCheck(const QString &sessionId, const qint64 previousSize, const QDateTime &previousModified)
{
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd() || !it->active) {
        return;
    }

    const QFileInfo info(it->workingCopyPath);
    if (!info.exists() || !info.isFile()) {
        importNow(sessionId);
        return;
    }

    if (info.size() != previousSize || info.lastModified() != previousModified) {
        scheduleImport(sessionId);
        return;
    }

    importNow(sessionId);
}

QVariantMap EditorSessionStore::importNow(const QString &sessionId, const bool allowInvalidAnchors)
{
    const QString key = normalizedSessionId(sessionId);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        return createError(QStringLiteral("Unknown editor session: %1").arg(sessionId));
    }

    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(*it, allowInvalidAnchors);
    const QVariantMap map = resultToMap(key, result);
    Q_EMIT editorSessionImportFinished(key,
                                       bookStateImportStatusName(result.status),
                                       result.errorMessage,
                                       it->workingCopyPath,
                                       uuidToString(result.newStateId),
                                       result.anchorValidationIssues);
    return map;
}

QVariantMap EditorSessionStore::finishSession(const QString &sessionId)
{
    const QString key = normalizedSessionId(sessionId);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        return createError(QStringLiteral("Unknown editor session: %1").arg(sessionId));
    }

    if (!it->active) {
        BookStateImportResult unchanged;
        unchanged.status = BookStateImportStatus::Unchanged;
        unchanged.importedFileHash = fileHash(it->workingCopyPath);
        return resultToMap(key, unchanged);
    }

    const QByteArray currentHash = fileHash(it->workingCopyPath);
    QVariantMap map;
    if (!currentHash.isEmpty() && currentHash != it->lastImportedFileHash) {
        map = importNow(key);
    } else {
        BookStateImportResult unchanged;
        unchanged.status = BookStateImportStatus::Unchanged;
        unchanged.importedFileHash = currentHash;
        map = resultToMap(key, unchanged);
        Q_EMIT editorSessionImportFinished(key,
                                           bookStateImportStatusName(unchanged.status),
                                           unchanged.errorMessage,
                                           it->workingCopyPath,
                                           QString(),
                                           unchanged.anchorValidationIssues);
    }

    it->active = false;
    const QString bookKey = normalizedBookId(it->bookId);
    if (m_activeSessionByBookId.value(bookKey) == key) {
        m_activeSessionByBookId.remove(bookKey);
    }
    Q_EMIT editorSessionFinished(key, map.value(QStringLiteral("status")).toString(), map.value(QStringLiteral("message")).toString(), it->workingCopyPath);
    return map;
}

#include "moc_editorsessionstore.cpp"
