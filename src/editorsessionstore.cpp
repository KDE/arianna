// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "editorsessionstore.h"

#include "bookdatabase.h"
#include "booktruthstore.h"
#include "maintenancefiles.h"

#include "epubcontainer.h"
#include <KLocalizedString>
#include <QCoreApplication>

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
    restoreSessions();
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
    result.insert(QStringLiteral("lastImportedStateHash"), QString::fromLatin1(session.lastImportedStateHash));
    result.insert(QStringLiteral("createdAt"), session.createdAt.toString(Qt::ISODateWithMs));
    result.insert(QStringLiteral("lastImportedAt"), session.lastImportedAt.toString(Qt::ISODateWithMs));
    result.insert(QStringLiteral("editorProcessId"), session.editorProcessId);
    result.insert(QStringLiteral("active"), session.active);
    result.insert(QStringLiteral("reviewCount"), m_reviewHolds.value(uuidToString(session.sessionId)));
    result.insert(QStringLiteral("version"), 1);
    result.insert(QStringLiteral("baseStateId"), uuidToString(session.baseStateId));
    result.insert(QStringLiteral("baseFileHash"), QString::fromLatin1(session.baseFileHash));
    result.insert(QStringLiteral("lastImportStatus"), session.lastImportStatus);
    result.insert(QStringLiteral("ownerPid"), session.ownerPid);
    result.insert(QStringLiteral("ownerToken"), session.ownerToken);
    result.insert(QStringLiteral("editorToken"), session.editorToken);
    return result;
}

bool EditorSessionStore::persist(const EditorSession &session) const
{
    return MaintenanceFiles::writeJson(MaintenanceFiles::manifestPath(session.workingCopyPath), sessionToMap(session));
}

QVariantMap EditorSessionStore::blocked(const QString &message, const QStringList &paths)
{
    auto result = createError(message);
    result.insert(QStringLiteral("blocked"), true);
    result.insert(QStringLiteral("workingCopies"), paths);
    Q_EMIT editorStartBlocked(message, paths);
    return result;
}

void EditorSessionStore::restoreSessions()
{
    for (const auto &path : MaintenanceFiles::sessionCopies()) {
        const auto data = MaintenanceFiles::readJson(MaintenanceFiles::manifestPath(path));
        EditorSession session;
        session.sessionId = uuidFromString(data.value(QStringLiteral("sessionId")).toString());
        const auto key = uuidToString(session.sessionId);
        if (session.sessionId.isNull() || key != QFileInfo(path).dir().dirName() || data.value(QStringLiteral("workingCopyPath")).toString() != path
            || m_sessions.contains(key))
            continue;
        session.bookId = data.value(QStringLiteral("bookId")).toString();
        session.authoritativeFilePath = data.value(QStringLiteral("authoritativeFilePath")).toString();
        session.workingCopyPath = path;
        session.lastImportedFileHash = data.value(QStringLiteral("lastImportedFileHash")).toByteArray();
        session.lastImportedStateHash = data.value(QStringLiteral("lastImportedStateHash")).toByteArray();
        session.createdAt = QDateTime::fromString(data.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
        session.lastImportedAt = QDateTime::fromString(data.value(QStringLiteral("lastImportedAt")).toString(), Qt::ISODateWithMs);
        session.baseStateId = uuidFromString(data.value(QStringLiteral("baseStateId")).toString());
        session.baseFileHash = data.value(QStringLiteral("baseFileHash")).toByteArray();
        session.lastImportStatus = data.value(QStringLiteral("lastImportStatus")).toString();
        session.ownerPid = data.value(QStringLiteral("ownerPid"), -1).toLongLong();
        session.ownerToken = data.value(QStringLiteral("ownerToken")).toString();
        session.editorProcessId = data.value(QStringLiteral("editorProcessId"), -1).toLongLong();
        session.editorToken = data.value(QStringLiteral("editorToken")).toString();
        // A restored session must be explicitly recovered before this instance takes ownership.
        session.active = false;
        m_sessions.insert(key, session);
    }
}

void EditorSessionStore::retainWorkingCopy(const QString &sessionId)
{
    const auto key = normalizedSessionId(sessionId);
    const auto it = m_sessions.constFind(key);
    if (it == m_sessions.constEnd() || it->ownerPid != QCoreApplication::applicationPid() || !QFileInfo::exists(it->workingCopyPath))
        return;
    ++m_reviewHolds[key];
    persist(*it);
}

void EditorSessionStore::releaseWorkingCopy(const QString &sessionId)
{
    const auto key = normalizedSessionId(sessionId);
    const auto it = m_sessions.constFind(key);
    if (it == m_sessions.constEnd() || m_reviewHolds.value(key) <= 0)
        return;
    if (--m_reviewHolds[key] == 0)
        m_reviewHolds.remove(key);
    if (QFileInfo::exists(it->workingCopyPath))
        persist(*it);
    QTimer::singleShot(0, this, [this, key] {
        discardImportedCopy(key);
    });
}

QVariantMap EditorSessionStore::recoverWorkingCopy(const QString &bookId, const QString &path)
{
    auto guard = MaintenanceFiles::lock();
    if (!guard)
        return blocked(i18n("Editor files are currently being checked or changed. Please try again."));
    const auto book = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    const auto file = QFileInfo(path).absoluteFilePath();
    if (!book || !MaintenanceFiles::conflictingCopies(*book).contains(file))
        return createError(i18n("This working copy cannot be assigned to the selected book."));
    if (MaintenanceFiles::workingCopyInUse(file))
        return blocked(i18n("This working copy is still in use. Close the editor before recovering it."), {file});
    // Legacy copies need an unambiguous identifier, not just a matching filename, for import.
    EPubContainer epub(nullptr);
    if (!epub.openFile(file))
        return createError(i18n("The working copy is not a readable EPUB."));
    QStringList ids = epub.metadata(QStringLiteral("identifier"));
    ids += epub.metadata(QStringLiteral("unique-identifier"));
    bool matches = false;
    for (const auto &id : ids)
        matches |= MaintenanceFiles::normalizedId(id) == MaintenanceFiles::normalizedId(book->uniqueIdentifier);
    if (!matches)
        return createError(i18n("The working copy's identifier does not match this book."));
    for (const auto &other : BookDatabase::self().loadEntries()) {
        if (other.filename != book->filename
            && MaintenanceFiles::normalizedId(other.uniqueIdentifier) == MaintenanceFiles::normalizedId(book->uniqueIdentifier))
            return createError(i18n("The working copy's book identifier is ambiguous."));
    }
    EditorSession session;
    session.sessionId = QUuid(QFileInfo(file).dir().dirName());
    session.bookId = book->uniqueIdentifier;
    session.authoritativeFilePath = book->filename;
    session.workingCopyPath = file;
    session.createdAt = QFileInfo(file).birthTime();
    session.ownerPid = QCoreApplication::applicationPid();
    session.ownerToken = MaintenanceFiles::processToken(session.ownerPid);
    session.lastImportStatus = QStringLiteral("Recovered");
    // Do not trust a legacy manifest's imported hash: recovery must run the normal validation.
    if (!persist(session))
        return createError(i18n("Unable to save the editor session."));
    m_sessions.insert(uuidToString(session.sessionId), session);
    return sessionToMap(session);
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

    auto guard = MaintenanceFiles::lock();
    if (!guard)
        return blocked(i18n("Editor files are currently being checked or changed. Please try again."));
    const auto book = BookDatabase::self().loadEntryByUniqueIdentifier(normalizedBook);
    if (!book)
        return createError(i18n("The book is no longer in the library."));
    const auto copies = MaintenanceFiles::conflictingCopies(*book);
    if (!copies.isEmpty())
        return blocked(i18n("An editor working copy already exists for this book. Close any running editor and review the copies under Home → Maintenance → "
                            "Cache and Safety Backups before starting a new session."),
                       copies);

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
    if (initialHash.isEmpty() || initialHash != snapshot.epubFileHash) {
        QFile::remove(workingCopyPath);
        QDir().rmdir(sessionDirectoryPath);
        return createError(QStringLiteral("Unable to hash editor working copy: %1").arg(workingCopyPath));
    }

    EditorSession session;
    session.sessionId = sessionUuid;
    session.bookId = normalizedBook;
    session.authoritativeFilePath = sourcePath;
    session.workingCopyPath = workingCopyPath;
    session.lastImportedFileHash = snapshot.epubFileHash == snapshot.state->epubFileHash ? initialHash : QByteArray();
    session.lastImportedStateHash = snapshot.state->epubFileHash;
    session.createdAt = QDateTime::currentDateTimeUtc();
    session.lastImportedAt = session.createdAt;
    session.active = true;
    session.baseStateId = snapshot.state->stateId;
    session.baseFileHash = snapshot.epubFileHash;
    session.ownerPid = QCoreApplication::applicationPid();
    session.ownerToken = MaintenanceFiles::processToken(session.ownerPid);
    session.lastImportStatus = QStringLiteral("Unchanged");
    if (!persist(session)) {
        QFile::remove(workingCopyPath);
        QDir().rmdir(sessionDirectoryPath);
        return createError(i18n("Unable to save the editor session."));
    }

    m_sessions.insert(sessionId, session);
    m_activeSessionByBookId.insert(normalizedBook, sessionId);
    return sessionToMap(session);
}

QVariantMap EditorSessionStore::createOrReuseEditorSession(const QString &bookId, const QString &authoritativeFilePath)
{
    auto active = activeSessionForBook(bookId);
    if (!active.contains(QStringLiteral("error"))
        && active.value(QStringLiteral("ownerPid")).toLongLong() == QCoreApplication::applicationPid()
        && active.value(QStringLiteral("ownerToken")).toString() == MaintenanceFiles::processToken(QCoreApplication::applicationPid())
        && QFileInfo(active.value(QStringLiteral("authoritativeFilePath")).toString()).absoluteFilePath()
            == QFileInfo(authoritativeFilePath).absoluteFilePath()) {
        active.insert(QStringLiteral("reused"), true);
        return active;
    }
    auto session = createEditorSession(bookId, authoritativeFilePath);
    if (!session.contains(QStringLiteral("error")))
        session.insert(QStringLiteral("reused"), false);
    return session;
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

QVariantMap EditorSessionStore::resumeEditorSession(const QString &sessionId)
{
    auto guard = MaintenanceFiles::lock();
    if (!guard)
        return blocked(i18n("Editor files are currently being checked or changed. Please try again."));
    const QString key = normalizedSessionId(sessionId);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end() || !QFileInfo::exists(it->workingCopyPath))
        return createError(QStringLiteral("Editor working copy is missing"));
    const bool ownReview =
        m_reviewHolds.value(key) > 0 && it->ownerPid == QCoreApplication::applicationPid() && it->ownerToken == MaintenanceFiles::processToken(it->ownerPid);
    if (it->active || MaintenanceFiles::workingCopyInUse(it->workingCopyPath, !ownReview))
        return blocked(i18n("This working copy is still in use. Close the editor before recovering it."), {it->workingCopyPath});
    const auto book = BookDatabase::self().loadEntryByUniqueIdentifier(it->bookId);
    if (!book)
        return createError(i18n("The book is no longer in the library."));
    auto copies = MaintenanceFiles::conflictingCopies(*book);
    copies.removeAll(it->workingCopyPath);
    if (!copies.isEmpty())
        return blocked(i18n("Other working copies exist for this book. Review them in Maintenance first."), copies);
    it->active = true;
    it->ownerPid = QCoreApplication::applicationPid();
    it->ownerToken = MaintenanceFiles::processToken(it->ownerPid);
    if (!persist(*it)) {
        it->active = false;
        return createError(i18n("Unable to save the editor session."));
    }
    m_activeSessionByBookId.insert(normalizedBookId(it->bookId), key);
    QVariantMap session = sessionToMap(*it);
    session.insert(QStringLiteral("reused"), true);
    return session;
}

void EditorSessionStore::noteEditorProcessStarted(const QString &sessionId, const qint64 editorProcessId)
{
    const QString key = normalizedSessionId(sessionId);
    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        return;
    }

    it->editorProcessId = editorProcessId;
    it->editorToken = MaintenanceFiles::processToken(editorProcessId);
    persist(*it);
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

    auto guard = MaintenanceFiles::lock();
    if (!guard)
        return createError(i18n("Editor files are currently being checked or changed. Please try again."));
    if (it->ownerPid != QCoreApplication::applicationPid() || it->ownerToken != MaintenanceFiles::processToken(it->ownerPid))
        return createError(i18n("Recover this working copy in Maintenance before importing it."));
    BookStateImport importer;
    const BookStateImportResult result = importer.importEditorWorkingCopy(*it, allowInvalidAnchors);
    it->lastImportStatus = bookStateImportStatusName(result.status);
    if (result.status == BookStateImportStatus::Imported && !result.epubFileHash.isEmpty())
        it->lastImportedStateHash = result.epubFileHash;
    persist(*it);
    const QVariantMap map = resultToMap(key, result);
    guard.reset();
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
    it->lastImportStatus = map.value(QStringLiteral("status")).toString();
    persist(*it);
    const QString bookKey = normalizedBookId(it->bookId);
    if (m_activeSessionByBookId.value(bookKey) == key) {
        m_activeSessionByBookId.remove(bookKey);
    }
    Q_EMIT editorSessionFinished(key, map.value(QStringLiteral("status")).toString(), map.value(QStringLiteral("message")).toString(), it->workingCopyPath);
    QTimer::singleShot(0, this, [this, key] {
        discardImportedCopy(key);
    });
    return map;
}

void EditorSessionStore::discardImportedCopy(const QString &sessionId)
{
    auto guard = MaintenanceFiles::lock();
    const auto it = m_sessions.constFind(sessionId);
    if (!guard || it == m_sessions.constEnd() || it->active || m_reviewHolds.value(sessionId) > 0 || it->lastImportedFileHash.isEmpty())
        return;
    if (BookDatabase::self().loadEntry(it->workingCopyPath))
        return;
    const auto state = BookDatabase::self().currentBookState(it->bookId);
    if (!state || MaintenanceFiles::digest(it->authoritativeFilePath) != state->epubFileHash
        || MaintenanceFiles::digest(it->workingCopyPath) != it->lastImportedFileHash)
        return;
    const QFileInfo file(it->workingCopyPath);
    const auto metadata = MaintenanceFiles::manifestPath(it->workingCopyPath);
    MaintenanceFiles::removeFile({{QStringLiteral("fileName"), it->workingCopyPath},
                                  {QStringLiteral("hash"), QString::fromLatin1(it->lastImportedFileHash)},
                                  {QStringLiteral("size"), file.size()},
                                  {QStringLiteral("modified"), file.lastModified().toMSecsSinceEpoch()},
                                  {QStringLiteral("source"), it->authoritativeFilePath},
                                  {QStringLiteral("sourceHash"), QString::fromLatin1(state->epubFileHash)},
                                  {QStringLiteral("metadataPath"), metadata},
                                  {QStringLiteral("metadataHash"), QString::fromLatin1(MaintenanceFiles::digest(metadata))}});
}

#include "moc_editorsessionstore.cpp"
