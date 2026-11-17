// SPDX-License-Identifier: GPL-2.0-or-later
#include "maintenancefiles.h"
#include "epubcontainer.h"

#include <KLocalizedString>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>

namespace
{
bool cancelled()
{
    return QThread::currentThread()->isInterruptionRequested();
}
QString cleanPath(const QString &path)
{
    return QFileInfo(path).absoluteFilePath();
}
bool plainFile(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink() && info.canonicalFilePath() == info.absoluteFilePath();
}
QString backupOriginal(const QString &path)
{
    const QStringList stemMarkers{QStringLiteral(".before_anchoring_"),
                                  QStringLiteral(".before.ananchored_"),
                                  QStringLiteral(".before.reference-title-update_"),
                                  QStringLiteral(".before.image-not-inverse_")};
    const QFileInfo info(path);
    if (path.endsWith(QStringLiteral(".arianna.json")))
        return {};
    for (const auto &marker : stemMarkers) {
        const auto pos = info.fileName().indexOf(marker);
        if (pos >= 0 && info.suffix().compare(QStringLiteral("epub"), Qt::CaseInsensitive) == 0)
            return info.dir().filePath(info.fileName().left(pos) + QLatin1Char('.') + info.suffix());
    }
    const QStringList suffixMarkers{QStringLiteral(".arianna-anchor-title-"),
                                    QStringLiteral(".arianna-reference-deletion-"),
                                    QStringLiteral(".arianna-book-deletion-"),
                                    QStringLiteral(".arianna-anchor-removal-"),
                                    QStringLiteral(".arianna-toc-backup"),
                                    QStringLiteral(".arianna-anchor-backup"),
                                    QStringLiteral(".arianna-link-backup"),
                                    QStringLiteral(".bak-")};
    for (const auto &marker : suffixMarkers) {
        const auto pos = info.fileName().indexOf(marker);
        if (pos >= 0) {
            const auto original = info.dir().filePath(info.fileName().left(pos));
            const auto suffix = QFileInfo(original).suffix().toLower();
            if (suffix == QStringLiteral("epub") || suffix == QStringLiteral("pdf"))
                return original;
        }
    }
    return {};
}
QStringList bookIds(const BookEntry &book)
{
    QStringList ids{book.uniqueIdentifier, book.identifier, book.filename};
    ids += book.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (auto &id : ids)
        id = MaintenanceFiles::normalizedId(id);
    ids.removeAll(QString());
    ids.removeDuplicates();
    return ids;
}
QStringList epubIds(const QString &path)
{
    EPubContainer epub(nullptr);
    if (!epub.openFile(path))
        return {};
    auto ids = epub.metadata(QStringLiteral("identifier"));
    ids += epub.metadata(QStringLiteral("unique-identifier"));
    for (auto &id : ids)
        id = MaintenanceFiles::normalizedId(id);
    ids.removeAll(QString());
    return ids;
}
QStringList processFiles()
{
    QStringList result;
#ifdef Q_OS_LINUX
    const QDir proc(QStringLiteral("/proc"));
    for (const auto &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        entry.toLongLong(&numeric);
        if (!numeric)
            continue;
        QFile file(proc.filePath(entry + QStringLiteral("/cmdline")));
        if (file.open(QIODevice::ReadOnly)) {
            for (const auto &arg : file.read(1024 * 1024).split('\0'))
                if (arg.contains("editor-sessions/") || arg.contains(".epub") || arg.contains(".pdf") || arg.contains(".arianna-"))
                    result.append(QString::fromLocal8Bit(arg));
        }
        // Editors may hand the file to an existing process without keeping it in argv.
        const QDir descriptors(proc.filePath(entry + QStringLiteral("/fd")));
        for (const auto &fd : descriptors.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot)) {
            const auto target = fd.symLinkTarget();
            if (target.contains(QStringLiteral("editor-sessions/")) || target.contains(QStringLiteral(".arianna-"))
                || target.endsWith(QStringLiteral(".epub"), Qt::CaseInsensitive) || target.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
                result.append(target);
        }
    }
#endif
    return result;
}
bool inUse(const QString &path, const QStringList &arguments)
{
    return std::any_of(arguments.cbegin(), arguments.cend(), [&](const QString &arg) {
        return arg.contains(path);
    });
}
}

QString MaintenanceFiles::sessionsRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("editor-sessions"));
}
QString MaintenanceFiles::normalizedId(QString id)
{
    id = id.trimmed();
    if (id.startsWith(QStringLiteral("urn:uuid:"), Qt::CaseInsensitive))
        id.remove(0, 9);
    const QUuid uuid(id);
    return uuid.isNull() ? id : uuid.toString(QUuid::WithoutBraces);
}
QByteArray MaintenanceFiles::digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto data = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError || cancelled())
            return {};
        hash.addData(data);
    }
    return hash.result().toHex();
}
QVariantMap MaintenanceFiles::readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return {};
    return QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
}
bool MaintenanceFiles::writeJson(const QString &path, const QVariantMap &data)
{
    QSaveFile file(path);
    const auto json = QJsonDocument::fromVariant(data).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
}
QString MaintenanceFiles::processToken(qint64 pid)
{
    if (pid <= 0)
        return {};
#ifdef Q_OS_LINUX
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid)), boot(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!stat.open(QIODevice::ReadOnly) || !boot.open(QIODevice::ReadOnly))
        return {};
    const auto text = stat.readAll();
    const auto fields = text.mid(text.lastIndexOf(')') + 2).split(' ');
    if (fields.size() <= 19 || fields.first() == "Z")
        return {};
    return QString::fromLatin1(boot.readAll().trimmed() + ':' + fields.at(19));
#else
    // Unknown liveness is protected rather than treated as a dead editor.
    return QString::number(pid);
#endif
}
bool MaintenanceFiles::workingCopyInUse(const QString &path, const bool includeReviews)
{
    auto manifest = readJson(manifestPath(path));
    if (!includeReviews)
        manifest.remove(QStringLiteral("reviewCount"));
    return sessionActive(manifest) || inUse(path, processFiles());
}
bool MaintenanceFiles::processAlive(qint64 pid, const QString &token)
{
    const auto current = processToken(pid);
    return !current.isEmpty() && (token.isEmpty() || token == current);
}
bool MaintenanceFiles::sessionActive(const QVariantMap &session)
{
    return ((session.value(QStringLiteral("active")).toBool() || session.value(QStringLiteral("reviewCount")).toInt() > 0)
            && processAlive(session.value(QStringLiteral("ownerPid")).toLongLong(), session.value(QStringLiteral("ownerToken")).toString()))
        || processAlive(session.value(QStringLiteral("editorProcessId")).toLongLong(), session.value(QStringLiteral("editorToken")).toString());
}
QString MaintenanceFiles::manifestPath(const QString &workingCopy)
{
    return QFileInfo(workingCopy).dir().filePath(QStringLiteral("session.json"));
}
QStringList MaintenanceFiles::sessionCopies()
{
    QStringList result;
    const QDir root(sessionsRoot());
    for (const auto &dir : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (QUuid(dir.fileName()).isNull() || dir.canonicalFilePath() != dir.absoluteFilePath())
            continue;
        for (const auto &file : QDir(dir.filePath()).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
            if (file.suffix().compare(QStringLiteral("epub"), Qt::CaseInsensitive) == 0 && backupOriginal(file.filePath()).isEmpty())
                result.append(file.absoluteFilePath());
        }
    }
    return result;
}
QStringList MaintenanceFiles::conflictingCopies(const BookEntry &book)
{
    QStringList result;
    const auto ids = bookIds(book);
    for (const auto &path : sessionCopies()) {
        const auto manifest = readJson(manifestPath(path));
        bool matches = ids.contains(normalizedId(manifest.value(QStringLiteral("bookId")).toString()))
            || (!manifest.value(QStringLiteral("authoritativeFilePath")).toString().isEmpty()
                && cleanPath(manifest.value(QStringLiteral("authoritativeFilePath")).toString()) == cleanPath(book.filename));
        QStringList candidateIds;
        if (!matches && manifest.isEmpty()) {
            candidateIds = epubIds(path);
            matches = std::any_of(candidateIds.cbegin(), candidateIds.cend(), [&](const auto &id) {
                return ids.contains(id);
            });
        }
        // Legacy or damaged copies still reserve their source filename.
        if (matches || (manifest.isEmpty() && candidateIds.isEmpty() && QFileInfo(path).fileName() == QFileInfo(book.filename).fileName()))
            result.append(path);
    }
    return result;
}
QString MaintenanceFiles::anchorEditBlockReason(const BookEntry &book)
{
    if (QFileInfo(book.filename).suffix().compare(QStringLiteral("epub"), Qt::CaseInsensitive) != 0)
        return {};
    const auto copies = conflictingCopies(book);
    if (copies.isEmpty())
        return {};
    return i18n("Anchor changes for %1 are blocked because an editor working copy exists. Finish and import the editor changes, or close the editor and discard the working copy under Home → Maintenance → Cache and Safety Backups, then try again.", book.title)
        + QStringLiteral("\n\n") + copies.join(QLatin1Char('\n'));
}

std::unique_ptr<QLockFile> MaintenanceFiles::lock()
{
    QDir root(sessionsRoot());
    if (!root.mkpath(QStringLiteral(".")))
        return {};
    auto result = std::make_unique<QLockFile>(root.filePath(QStringLiteral(".maintenance.lock")));
    result->setStaleLockTime(0);
    if (!result->tryLock())
        return {};
    return result;
}
bool MaintenanceFiles::registerBackup(const QString &original, const QString &backup)
{
    const auto hash = digest(backup);
    if (hash.isEmpty())
        return false;
    return writeJson(backup + QStringLiteral(".arianna.json"),
                     {{QStringLiteral("version"), 1},
                      {QStringLiteral("kind"), QStringLiteral("backup")},
                      {QStringLiteral("original"), cleanPath(original)},
                      {QStringLiteral("hash"), QString::fromLatin1(hash)},
                      {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                      {QStringLiteral("ownerPid"), QCoreApplication::applicationPid()},
                      {QStringLiteral("ownerToken"), processToken(QCoreApplication::applicationPid())},
                      {QStringLiteral("completed"), false}});
}
void MaintenanceFiles::completeBackup(const QString &backup)
{
    const auto path = backup + QStringLiteral(".arianna.json");
    auto data = readJson(path);
    if (data.isEmpty())
        return;
    const auto after = digest(data.value(QStringLiteral("original")).toString());
    if (after.isEmpty())
        return;
    data.insert(QStringLiteral("afterHash"), QString::fromLatin1(after));
    data.insert(QStringLiteral("completed"), true);
    writeJson(path, data);
}

QVariantList MaintenanceFiles::inspect(const QList<BookEntry> &books,
                                       const QVariantList &states,
                                       const QVariantList &history,
                                       const std::function<void(int, int, const QString &)> &progress)
{
    QHash<QString, QList<int>> identities;
    QHash<QString, int> paths;
    QSet<QString> directories;
    for (int i = 0; i < books.size(); ++i) {
        paths.insert(cleanPath(books.at(i).filename), i);
        directories.insert(QFileInfo(books.at(i).filename).absolutePath());
        for (const auto &id : bookIds(books.at(i)))
            identities[id].append(i);
    }
    QHash<QString, QVariantMap> current;
    QHash<QString, QSet<QString>> importedHashes;
    for (const auto &value : states) {
        const auto state = value.toMap();
        const auto id = normalizedId(state.value(QStringLiteral("book_id")).toString());
        current[id] = state;
        importedHashes[id].insert(state.value(QStringLiteral("epub_file_hash")).toString());
    }
    for (const auto &value : history) {
        const auto state = value.toMap();
        importedHashes[normalizedId(state.value(QStringLiteral("book_id")).toString())].insert(state.value(QStringLiteral("epub_file_hash")).toString());
    }
    const auto copies = sessionCopies();
    QSet<QString> cacheDirectories;
    for (const auto &dir : QDir(sessionsRoot()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (!QUuid(dir.fileName()).isNull() && dir.canonicalFilePath() == dir.absoluteFilePath()) {
            directories.insert(dir.absoluteFilePath());
            cacheDirectories.insert(dir.absoluteFilePath());
        }
    }
    QStringList files = copies;
    for (const auto &copy : copies)
        directories.insert(QFileInfo(copy).absolutePath());
    QHash<QString, QString> originals, newestBackup;
    for (const auto &directory : directories) {
        if (cancelled())
            return {};
        for (const auto &file : QDir(directory).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
            const auto original = backupOriginal(file.absoluteFilePath());
            if (original.isEmpty() || paths.contains(file.absoluteFilePath()))
                continue;
            // Only siblings of a registered book or of an identified cache copy are in scope.
            if (!paths.contains(original) && !copies.contains(original) && !cacheDirectories.contains(directory))
                continue;
            files.append(file.absoluteFilePath());
            originals.insert(file.absoluteFilePath(), original);
            if (!newestBackup.contains(original) || QFileInfo(newestBackup[original]).lastModified() < file.lastModified())
                newestBackup[original] = file.absoluteFilePath();
        }
    }
    files.sort();
    const auto arguments = processFiles();
    QHash<QString, QByteArray> sourceHashes;
    QVariantList rows;
    for (int n = 0; n < files.size() && !cancelled(); ++n) {
        const auto path = files.at(n);
        if (progress)
            progress(n, files.size(), path);
        const QFileInfo before(path);
        const bool backup = originals.contains(path);
        const auto metadataPath = backup ? path + QStringLiteral(".arianna.json") : manifestPath(path);
        const auto metadata = readJson(metadataPath);
        const auto metadataHash = QFileInfo::exists(metadataPath) ? digest(metadataPath) : QByteArray();
        const auto manifest = backup ? readJson(manifestPath(originals.value(path))) : metadata;
        // A completed backup is a separate immutable file. Merely viewing the
        // original EPUB must not disable the reviewed deletion of its backup.
        bool protectedFile = (QFileInfo::exists(metadataPath) && metadata.isEmpty()) || !plainFile(path) || paths.contains(path) || sessionActive(manifest)
            || inUse(path, arguments);
        if (backup && !metadata.value(QStringLiteral("completed")).toBool()
            && processAlive(metadata.value(QStringLiteral("ownerPid")).toLongLong(), metadata.value(QStringLiteral("ownerToken")).toString()))
            protectedFile = true;
        QSet<int> owners;
        if (backup && paths.contains(originals.value(path)))
            owners.insert(paths.value(originals.value(path)));
        else {
            const auto original = manifest.value(QStringLiteral("authoritativeFilePath")).toString();
            if (!original.isEmpty() && paths.contains(cleanPath(original)))
                owners.insert(paths.value(cleanPath(original)));
            for (const auto &id : {normalizedId(manifest.value(QStringLiteral("bookId")).toString())})
                for (int i : identities.value(id))
                    owners.insert(i);
            if (owners.isEmpty())
                for (const auto &id : epubIds(path))
                    for (int i : identities.value(id))
                        owners.insert(i);
        }
        const auto hash = protectedFile ? QByteArray() : digest(path);
        const QFileInfo after(path);
        protectedFile |= hash.isEmpty() || before.size() != after.size() || before.lastModified() != after.lastModified();
        // Explicitly discarding an idle editor copy must remain possible even
        // when its original has diverged from the registered state. Automatic
        // cleanup still requires a verified current version below.
        const bool canDiscard = !backup && !protectedFile;
        bool safe = false;
        QString reason = protectedFile ? i18n("In use, unreadable, or changed during the check. This file is protected.")
                                       : i18n("The copy cannot be assigned to exactly one library book. Review it before deleting.");
        if (protectedFile && sessionActive(manifest)) {
            const bool reviewing = manifest.value(QStringLiteral("reviewCount")).toInt() > 0
                && processAlive(manifest.value(QStringLiteral("ownerPid")).toLongLong(), manifest.value(QStringLiteral("ownerToken")).toString());
            reason = reviewing ? i18n("This working copy is being checked or its import report is open. Close the report and wait for the check to finish, then check the cache again.")
                               : i18n("This working copy is still open in an editor session. Close the editor, then check the cache again.");
        }
        QString bookId, title, source;
        QByteArray sourceHash;
        if (owners.size() == 1) {
            const auto &book = books.at(*owners.cbegin());
            bookId = book.uniqueIdentifier;
            title = book.title;
            source = cleanPath(book.filename);
            const auto state = current.value(normalizedId(bookId));
            if (!sourceHashes.contains(source))
                sourceHashes[source] = digest(source);
            sourceHash = sourceHashes.value(source);
            // PDF has no EPUB truth-store row; its journal proves a completed replacement.
            const bool pdfBackup = backup && QFileInfo(source).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
            const bool verified =
                plainFile(source) && !sourceHash.isEmpty() && (pdfBackup || sourceHash == state.value(QStringLiteral("epub_file_hash")).toByteArray());
            if (!protectedFile) {
                if (!verified) {
                    reason = i18n("The original is missing or differs from its registered state. Keep this recovery copy.");
                    protectedFile = true;
                } else if (backup) {
                    const bool committed = metadata.value(QStringLiteral("completed")).toBool() && metadata.value(QStringLiteral("hash")).toByteArray() == hash
                        && cleanPath(metadata.value(QStringLiteral("original")).toString()) == originals.value(path)
                        && (pdfBackup ? metadata.value(QStringLiteral("afterHash")).toByteArray() == sourceHash
                                      : importedHashes.value(normalizedId(bookId)).contains(metadata.value(QStringLiteral("afterHash")).toString()));
                    safe = pdfBackup ? committed
                                     : (committed || metadata.isEmpty()) && importedHashes.value(normalizedId(bookId)).contains(QString::fromLatin1(hash));
                    if (newestBackup.value(originals.value(path)) == path) {
                        safe = false;
                        reason = i18n("The most recent safety backup of this book is retained.");
                    } else {
                        reason = safe ? i18n("This backup is a known book version superseded by a verified current version.")
                                      : i18n("The backup's successful replacement cannot be verified. Review it before deleting.");
                    }
                } else {
                    safe = importedHashes.value(normalizedId(bookId)).contains(QString::fromLatin1(hash))
                        || (metadata.value(QStringLiteral("lastImportedFileHash")).toByteArray() == hash
                            && importedHashes.value(normalizedId(bookId)).contains(metadata.value(QStringLiteral("lastImportedStateHash")).toString()));
                    reason = safe ? i18n("This working copy matches the current book or a previously imported version.")
                                  : i18n("This working copy contains changes without an import record. Its age alone does not make it disposable.");
                }
            }
        }
        QVariantMap plan{{QStringLiteral("fileName"), path},
                         {QStringLiteral("hash"), QString::fromLatin1(hash)},
                         {QStringLiteral("size"), before.size()},
                         {QStringLiteral("modified"), before.lastModified().toMSecsSinceEpoch()},
                         {QStringLiteral("metadataPath"), metadataPath},
                         {QStringLiteral("metadataHash"), QString::fromLatin1(metadataHash)},
                         {QStringLiteral("source"), source},
                         {QStringLiteral("sourceHash"), QString::fromLatin1(sourceHash)},
                         {QStringLiteral("kind"), backup ? QStringLiteral("backup") : QStringLiteral("session")}};
                const auto createdAt = backup ? metadata.value(QStringLiteral("createdAt")).toString() : QString();
        rows.append(QVariantMap{{QStringLiteral("status"), safe ? QStringLiteral("ok") : QStringLiteral("warning")},
                                {QStringLiteral("codes"), QStringList{safe ? QStringLiteral("cache-disposable") : QStringLiteral("cache-retained")}},
                                {QStringLiteral("messages"), QStringList{reason}},
                                {QStringLiteral("fileName"), path},
                                {QStringLiteral("bookTitle"), title},
                                {QStringLiteral("originalTitle"), title},
                                {QStringLiteral("bookId"), bookId},
                                {QStringLiteral("thumbnail"), owners.size() == 1 ? books.at(*owners.cbegin()).thumbnail : QString()},
                                {QStringLiteral("bytes"), before.size()},
                                {QStringLiteral("modifiedAt"), before.lastModified().toString(Qt::ISODate)},
                                {QStringLiteral("createdAt"), createdAt.isEmpty() ? before.lastModified().toString(Qt::ISODate) : createdAt},
                                {QStringLiteral("safeToDelete"), safe && !protectedFile},
                                {QStringLiteral("canDelete"), !protectedFile},
                                {QStringLiteral("canDiscard"), canDiscard},
                                {QStringLiteral("cleanupPlan"), plan}});
    }
    return rows;
}

bool MaintenanceFiles::removeFile(const QVariantMap &plan)
{
    const auto path = plan.value(QStringLiteral("fileName")).toString();
    const QFileInfo info(path);
    if (!plainFile(path) || info.size() != plan.value(QStringLiteral("size")).toLongLong()
        || info.lastModified().toMSecsSinceEpoch() != plan.value(QStringLiteral("modified")).toLongLong()
        || digest(path) != plan.value(QStringLiteral("hash")).toByteArray())
        return false;
    const auto source = plan.value(QStringLiteral("source")).toString();
    if (!source.isEmpty() && digest(source) != plan.value(QStringLiteral("sourceHash")).toByteArray())
        return false;
    const auto metadataPath = plan.value(QStringLiteral("metadataPath")).toString();
    const auto metadataHash = QFileInfo::exists(metadataPath) ? digest(metadataPath) : QByteArray();
    if (metadataHash != plan.value(QStringLiteral("metadataHash")).toByteArray())
        return false;
    if (sessionActive(readJson(manifestPath(path))) || inUse(path, processFiles()))
        return false;
    if (!QFile::remove(path))
        return false;
    if (metadataPath == path + QStringLiteral(".arianna.json")) {
        if (!metadataHash.isEmpty())
            QFile::remove(metadataPath);
    }
    // Preserve the manifest while safety backups or editor-created side files remain.
    const auto directory = info.dir();
    if (directory.absolutePath().startsWith(sessionsRoot() + QLatin1Char('/'))
        && directory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden) == QStringList{QStringLiteral("session.json")}) {
        const auto manifest = manifestPath(path);
        const auto session = readJson(manifest);
        if (!QFileInfo::exists(session.value(QStringLiteral("workingCopyPath")).toString()) && !sessionActive(session))
            QFile::remove(manifest);
    }
    if (info.absolutePath().startsWith(sessionsRoot() + QLatin1Char('/')))
        QDir().rmdir(info.absolutePath());
    return true;
}
