// SPDX-License-Identifier: GPL-2.0-or-later
#include "librarymaintenance.h"

#include "bookdatabase.h"
#include "bookformats.h"
#include "bookreferencecheck.h"
#include "booktruthstore.h"
#include "config.h"
#include "epubcontainer.h"
#include "maintenancefiles.h"
#include "pdfreferenceindex.h"

#include <KArchiveDirectory>
#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QProcess>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <optional>

namespace
{
bool cancelled()
{
    return QThread::currentThread()->isInterruptionRequested();
}

QString normalized(QString id)
{
    id = id.trimmed();
    if (id.startsWith(QStringLiteral("urn:uuid:"), Qt::CaseInsensitive))
        id.remove(0, 9);
    const QUuid uuid(id);
    return uuid.isNull() ? id : uuid.toString(QUuid::WithoutBraces);
}

QSet<QString> bookIdentities(const BookEntry &book)
{
    QStringList ids{book.uniqueIdentifier, book.identifier, book.filename};
    ids.append(book.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts));
    QSet<QString> result;
    for (const auto &id : ids) {
        const auto key = normalized(id);
        if (!key.isEmpty())
            result.insert(key);
    }
    return result;
}

QVariantMap finding(const QString &status, const QString &code, const QString &message, const QString &file = {}, const QString &bookId = {})
{
    return {{QStringLiteral("status"), status},
            {QStringLiteral("codes"), QStringList{code}},
            {QStringLiteral("messages"), QStringList{message}},
            {QStringLiteral("fileName"), file},
            {QStringLiteral("bookId"), bookId}};
}

QByteArray verifiedDigest(const QString &path)
{
    const QFileInfo before(path);
    QFile file(path);
    if (!before.isFile() || !file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd() && !cancelled()) {
        const auto block = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError)
            return {};
        hash.addData(block);
    }
    const QFileInfo after(path);
    if (cancelled() || !after.isFile() || before.size() != after.size() || before.lastModified() != after.lastModified()
        || before.canonicalFilePath() != after.canonicalFilePath())
        return {};
    return hash.result();
}

struct Snapshot {
    QList<BookEntry> books;
    QVariantList annotations, references, states, history, integrity;
    QString error;
};

Snapshot snapshot(const QString &path, bool integrity)
{
    Snapshot result;
    const QString connection = QStringLiteral("maintenance-") + QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=3000"));
        if (!db.open() || !db.transaction()) {
            result.error = db.lastError().text();
        } else {
            auto read = [&](const QString &sql, QVariantList &rows) {
                QSqlQuery query(db);
                if (!query.exec(sql)) {
                    result.error = query.lastError().text();
                    return;
                }
                const auto fields = query.record();
                while (!cancelled() && query.next()) {
                    QVariantMap row;
                    for (int i = 0; i < fields.count(); ++i)
                        row.insert(fields.fieldName(i), query.value(i));
                    rows.append(row);
                }
                if (query.lastError().isValid())
                    result.error = query.lastError().text();
            };
            QVariantList books;
            read(QStringLiteral("SELECT fileName, title, author, formatGroup, uniqueIdentifier, identifier, thumbnail FROM books ORDER BY fileName"), books);
            read(QStringLiteral("SELECT * FROM annotations ORDER BY bookId, annotationId"), result.annotations);
            read(QStringLiteral("SELECT * FROM \"references\" ORDER BY sourceBookId, sourceAnchorId"), result.references);
            read(QStringLiteral("SELECT * FROM book_current_state ORDER BY book_id"), result.states);
            if (db.tables().contains(QStringLiteral("book_state_history")))
                read(QStringLiteral("SELECT * FROM book_state_history"), result.history);
            if (integrity) {
                QVariantList checks, foreignKeys;
                read(QStringLiteral("PRAGMA integrity_check"), checks);
                read(QStringLiteral("PRAGMA foreign_key_check"), foreignKeys);
                for (const auto &check : checks) {
                    const QString message = check.toMap().value(QStringLiteral("integrity_check")).toString();
                    result.integrity.append(finding(message == QStringLiteral("ok") ? QStringLiteral("ok") : QStringLiteral("error"),
                                                    QStringLiteral("sqlite-integrity"),
                                                    message == QStringLiteral("ok") ? i18n("SQLite integrity check passed.") : message));
                }
                for (const auto &check : foreignKeys)
                    result.integrity.append(
                        finding(QStringLiteral("error"),
                                QStringLiteral("sqlite-foreign-key"),
                                i18n("Broken database relation: %1", QString::fromUtf8(QJsonDocument::fromVariant(check).toJson(QJsonDocument::Compact)))));
            }
            for (const auto &value : books) {
                const auto row = value.toMap();
                BookEntry book;
                book.filename = row.value(QStringLiteral("fileName")).toString();
                book.title = row.value(QStringLiteral("title")).toString();
                book.author = row.value(QStringLiteral("author")).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
                book.formatGroup = row.value(QStringLiteral("formatGroup")).toString();
                book.uniqueIdentifier = row.value(QStringLiteral("uniqueIdentifier")).toString();
                book.identifier = row.value(QStringLiteral("identifier")).toString();
                book.thumbnail = row.value(QStringLiteral("thumbnail")).toString();
                result.books.append(book);
            }
            db.rollback(); // End the read snapshot without publishing any changes.
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

QVariantList checkDatabase(const Snapshot &data, const LibraryMaintenance::Progress &progress)
{
    QVariantList rows = data.integrity;
    QHash<QString, QSet<QString>> identities;
    QHash<qint64, QStringList> sizes;
    QHash<QString, QByteArray> fileHashes;
    auto hashForFile = [&](const QString &path) {
        auto it = fileHashes.find(path);
        if (it == fileHashes.end())
            it = fileHashes.insert(path, verifiedDigest(path));
        return it.value();
    };
    for (int i = 0; i < data.books.size() && !cancelled(); ++i) {
        const auto &book = data.books.at(i);
        if (progress)
            progress(i, data.books.size(), book.filename);
        for (const auto &id : bookIdentities(book))
            identities[id].insert(book.filename);
        if (book.uniqueIdentifier.trimmed().isEmpty()) {
            auto row = finding(QStringLiteral("warning"), QStringLiteral("missing-book-id"), i18n("The book has no unique identifier."), book.filename);
            row.insert(QStringLiteral("tableName"), QStringLiteral("books"));
            row.insert(QStringLiteral("recordKeyField"), QStringLiteral("fileName"));
            row.insert(QStringLiteral("recordId"), book.filename);
            row.insert(QStringLiteral("fieldName"), QStringLiteral("uniqueIdentifier"));
            rows.append(row);
        }
        const QFileInfo file(book.filename);
        if (!file.isFile())
            rows.append(
                finding(QStringLiteral("warning"), QStringLiteral("missing-book-file"), i18n("The book file is currently unavailable."), book.filename));
        else
            sizes[file.size()].append(book.filename);
    }
    for (auto it = identities.cbegin(); it != identities.cend() && !cancelled(); ++it) {
        if (it->size() > 1) {
            auto paths = it->values();
            paths.sort();
            auto row = finding(QStringLiteral("warning"),
                               QStringLiteral("shared-book-id"),
                               i18n("Several files share book identifier %1. Check whether they are copies or different editions.", it.key()),
                               paths.join(QLatin1Char('\n')),
                               it.key());
            QVariantMap hashes;
            for (qsizetype i = 0; i < paths.size() && !cancelled(); ++i) {
                if (progress)
                    progress(i, paths.size(), paths.at(i));
                hashes.insert(paths.at(i), QString::fromLatin1(hashForFile(paths.at(i)).toHex()));
            }
            row.insert(QStringLiteral("sharedIdFiles"), paths);
            row.insert(QStringLiteral("tableName"), QStringLiteral("books"));
            row.insert(QStringLiteral("fieldName"), QStringLiteral("uniqueIdentifier / identifier"));
            row.insert(QStringLiteral("matchedBookFiles"), paths);
            row.insert(QStringLiteral("fileHashes"), hashes);
            rows.append(row);
        }
    }
    auto assignment = [&](const QVariantMap &object,
                          const QString &key,
                          const QString &table,
                          const QString &recordKeyField,
                          const QString &recordId,
                          const QString &recordType) {
        const QString id = object.value(key).toString();
        const auto files = identities.value(normalized(id));
        if (files.size() != 1) {
            const bool missing = files.isEmpty();
            auto row = finding(missing ? QStringLiteral("error") : QStringLiteral("warning"),
                               missing ? QStringLiteral("unassigned-record") : QStringLiteral("ambiguous-record"),
                               missing ? i18n("%1 entry %2 in %3.%4 refers to missing book ID %5.",
                                              recordType, recordId, table, recordKeyField, id.isEmpty() ? i18n("(empty)") : id)
                                       : i18n("%1 entry %2 in %3.%4 refers to ambiguous book ID %5.",
                                              recordType, recordId, table, recordKeyField, id),
                               {},
                               id);
            QStringList matchedFiles = files.values();
            matchedFiles.sort();
            row.insert(QStringLiteral("tableName"), table);
            row.insert(QStringLiteral("recordType"), recordType);
            row.insert(QStringLiteral("recordKeyField"), recordKeyField);
            row.insert(QStringLiteral("recordId"), recordId);
            row.insert(QStringLiteral("fieldName"), key);
            row.insert(QStringLiteral("matchedBookFiles"), matchedFiles);
            rows.append(row);
        }
    };
    for (const auto &value : data.annotations) {
        const auto note = value.toMap();
        assignment(note, QStringLiteral("bookId"), QStringLiteral("annotations"), QStringLiteral("bookId + annotationId"),
                   note.value(QStringLiteral("bookId")).toString() + QStringLiteral(" / ") + note.value(QStringLiteral("annotationId")).toString(),
                   i18n("Remark"));
    }
    for (const auto &value : data.references) {
        const auto ref = value.toMap();
        const QString id = ref.value(QStringLiteral("sourceAnchorId")).toString();
        const QString referenceKey = ref.value(QStringLiteral("sourceBookId")).toString() + QStringLiteral(" / ") + id;
        assignment(ref, QStringLiteral("sourceBookId"), QStringLiteral("references"), QStringLiteral("sourceBookId + sourceAnchorId"), referenceKey,
                   i18n("Cross-reference source"));
        assignment(ref, QStringLiteral("targetBookId"), QStringLiteral("references"), QStringLiteral("sourceBookId + sourceAnchorId"), referenceKey,
                   i18n("Cross-reference target"));
        if (ref.value(QStringLiteral("sourceAnchorId")).toString().trimmed().isEmpty()
            || ref.value(QStringLiteral("targetLocation")).toString().trimmed().isEmpty())
            rows.append(
                finding(QStringLiteral("error"), QStringLiteral("incomplete-reference"), i18n("A cross-reference has no source anchor or target location.")));
    }
    for (const auto &value : data.states) {
        const auto state = value.toMap();
        const qsizetype previousRowCount = rows.size();
        assignment(state, QStringLiteral("book_id"), QStringLiteral("book_current_state"), QStringLiteral("book_id"),
                   state.value(QStringLiteral("book_id")).toString(), i18n("Saved book state"));
        if (rows.size() > previousRowCount) {
            auto row = rows.last().toMap();
            row.insert(QStringLiteral("stateId"), state.value(QStringLiteral("state_id")));
            rows.last() = row;
        }
    }
    for (const auto &value : data.history) {
        const auto state = value.toMap();
        const qsizetype previousRowCount = rows.size();
        assignment(state, QStringLiteral("book_id"), QStringLiteral("book_state_history"), QStringLiteral("book_id + state_id"),
               state.value(QStringLiteral("book_id")).toString() + QStringLiteral(" / ") + state.value(QStringLiteral("state_id")).toString(),
               i18n("Saved book state history"));
        if (rows.size() > previousRowCount) {
            auto row = rows.last().toMap();
            row.insert(QStringLiteral("stateId"), state.value(QStringLiteral("state_id")));
            rows.last() = row;
        }
    }

    // Compare hashes of equal-sized files; IDs and titles alone never establish an exact duplicate.
    int done = 0, total = 0;
    for (const auto &paths : sizes)
        if (paths.size() > 1)
            total += paths.size();
    QHash<QByteArray, QStringList> hashes;
    for (const auto &paths : sizes) {
        if (paths.size() < 2)
            continue;
        for (const auto &path : paths) {
            if (cancelled())
                return rows;
            if (progress)
                progress(done++, total, path);
            const auto hash = hashForFile(path);
            if (!hash.isEmpty())
                hashes[hash].append(path);
            else if (!cancelled())
                rows.append(finding(QStringLiteral("warning"),
                                    QStringLiteral("file-unverified"),
                                    i18n("The file could not be read completely or changed during the check."),
                                    path));
        }
    }
    for (auto it = hashes.cbegin(); it != hashes.cend(); ++it) {
        if (it->size() > 1) {
            auto paths = it.value();
            paths.sort();
            auto row = finding(QStringLiteral("warning"),
                               QStringLiteral("identical-files"),
                               i18n("These book files have identical contents."),
                               paths.join(QLatin1Char('\n')));
            row.insert(QStringLiteral("duplicateFiles"), paths);
            row.insert(QStringLiteral("contentHash"), QString::fromLatin1(it.key().toHex()));
            rows.append(row);
        }
    }
    if (!cancelled()) {
        for (const auto &value : BookFormats::candidates(data.books)) {
            const auto plan = value.toMap();
            auto row = finding(QStringLiteral("warning"),
                               QStringLiteral("book-format-candidate"),
                               i18n("EPUB and PDF have the same title and authors. Combine their covers into one library entry."),
                               plan.value(QStringLiteral("files")).toStringList().join(QLatin1Char('\n')));
            row.insert(QStringLiteral("bookTitle"), plan.value(QStringLiteral("title")));
            row.insert(QStringLiteral("details"), plan.value(QStringLiteral("authors")).toStringList().join(QStringLiteral(", ")));
            row.insert(QStringLiteral("formatGroupPlan"), plan);
            rows.append(row);
        }
    }
    return rows;
}

QVariantMap runValidator(const QString &path, QString command, bool pdf)
{
    QStringList args;
    if (command.trimmed().isEmpty() && pdf) {
        command = QStandardPaths::findExecutable(QStringLiteral("qpdf"));
        args.append(QStringLiteral("--check"));
    }
    QString program = command.trimmed();
    if (!QFileInfo::exists(program)) {
        const auto parts = QProcess::splitCommand(program);
        if (!parts.isEmpty()) {
            program = parts.first();
            args += parts.mid(1);
        }
    }
    if (program.isEmpty() || (!QFileInfo::exists(program) && QStandardPaths::findExecutable(program).isEmpty()))
        return finding(QStringLiteral("warning"),
                       QStringLiteral("validator-unavailable"),
                       i18n("No usable %1 check program is configured. Configure it in Settings.", pdf ? QStringLiteral("PDF") : QStringLiteral("EPUB")),
                       path);
    args.append(QFileInfo(path).absoluteFilePath());
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    if (!process.waitForStarted(3000))
        return finding(QStringLiteral("warning"),
                       QStringLiteral("validator-unavailable"),
                       i18n("The check program could not be started: %1", process.errorString()),
                       path);
    QElapsedTimer time;
    time.start();
    QByteArray output;
    while (!process.waitForFinished(100)) {
        output += process.readAll().left(qMax(0, 1024 * 1024 - int(output.size())));
        if (cancelled() || time.elapsed() > 120000) {
            process.kill();
            process.waitForFinished(3000);
            return finding(QStringLiteral("warning"), QStringLiteral("validation-incomplete"), i18n("The book check was interrupted or timed out."), path);
        }
    }
    output += process.readAll().left(qMax(0, 1024 * 1024 - int(output.size())));
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    const bool warnings = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 3 && QFileInfo(program).fileName() == QStringLiteral("qpdf");
    auto row = finding(ok             ? QStringLiteral("ok")
                           : warnings ? QStringLiteral("warning")
                                      : QStringLiteral("error"),
                       QStringLiteral("book-structure"),
                       ok ? i18n("Book structure check passed.") : i18n("The book structure check reported problems."),
                       path);
    row.insert(QStringLiteral("details"), QString::fromUtf8(output));
    return row;
}

QVariantList checkStructure(const Snapshot &data, const QString &epubCommand, const QString &pdfCommand, const LibraryMaintenance::Progress &progress)
{
    QVariantList rows;
    for (int i = 0; i < data.books.size() && !cancelled(); ++i) {
        const auto &book = data.books.at(i);
        if (progress)
            progress(i, data.books.size(), book.filename);
        const QFileInfo file(book.filename);
        if (!file.isFile() || !file.isReadable()) {
            rows.append(
                finding(QStringLiteral("warning"), QStringLiteral("missing-book-file"), i18n("The book file is currently unavailable."), book.filename));
            continue;
        }
        const QString mime = QMimeDatabase().mimeTypeForFile(file).name();
        if (mime != QStringLiteral("application/epub+zip") && mime != QStringLiteral("application/pdf")) {
            rows.append(finding(QStringLiteral("warning"),
                                QStringLiteral("unsupported-format"),
                                i18n("Structure checking is available for EPUB and PDF books."),
                                book.filename));
            continue;
        }
        // Basic checks also work without an external standards validator.
        QStringList issues;
        if (mime == QStringLiteral("application/epub+zip")) {
            EPubContainer epub(nullptr);
            if (!epub.openFile(book.filename)) {
                issues.append(i18n("The EPUB package could not be opened."));
            } else {
                if (epub.items().isEmpty())
                    issues.append(i18n("The EPUB has no reading order."));
                for (const auto &id : epub.items())
                    if (epub.epubItem(id).path.isEmpty())
                        issues.append(i18n("The reading order refers to a missing manifest item: %1", id));
                for (const auto &item : epub.manifestItems()) {
                    if (cancelled())
                        break;
                    const QUrl url(item.path);
                    if (!url.isRelative())
                        continue;
                    const auto *entry = epub.rootDirectory()->entry(item.path);
                    if (!entry || !entry->isFile()) {
                        issues.append(i18n("A manifest resource is missing: %1", item.path));
                        continue;
                    }
                    if (item.mimetype == "application/xhtml+xml" || item.mimetype == "image/svg+xml" || item.mimetype == "application/x-dtbncx+xml") {
                        QDomDocument document;
                        const auto parsed = document.setContent(epub.readData(item.path));
                        if (!parsed) {
                            issues.append(i18n("Invalid XML in %1: %2", item.path, parsed.errorMessage));
                            continue;
                        }
                        QSet<QString> ids;
                        QList<QDomElement> pending{document.documentElement()};
                        while (!pending.isEmpty() && !cancelled()) {
                            const auto element = pending.takeLast();
                            const QString id = element.attribute(QStringLiteral("id"));
                            if (!id.isEmpty() && ids.contains(id))
                                issues.append(i18n("Duplicate element ID in %1: %2", item.path, id));
                            ids.insert(id);
                            for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                                pending.append(child);
                        }
                    }
                }
            }
        } else {
            const auto index = readPdfReferenceIndex(book.filename);
            if (index.needsPassword) {
                rows.append(finding(QStringLiteral("warning"), QStringLiteral("password-required"), index.error, book.filename));
                continue;
            }
            if (!index.error.isEmpty())
                issues.append(index.error);
            else if (index.pageCount <= 0)
                issues.append(i18n("The PDF has no readable pages."));
        }
        if (!issues.isEmpty())
            rows.append(finding(QStringLiteral("error"), QStringLiteral("basic-book-structure"), issues.join(QLatin1Char('\n')), book.filename));
        if (cancelled())
            break;
        auto row = runValidator(book.filename, mime == QStringLiteral("application/pdf") ? pdfCommand : epubCommand, mime == QStringLiteral("application/pdf"));
        row.insert(QStringLiteral("bookTitle"), book.title);
        rows.append(row);
        const QFileInfo after(book.filename);
        if (file.size() != after.size() || file.lastModified() != after.lastModified())
            rows.append(
                finding(QStringLiteral("warning"), QStringLiteral("file-changed"), i18n("The book changed during the check. Check it again."), book.filename));
    }
    return rows;
}
}

LibraryMaintenance::LibraryMaintenance(QObject *parent)
    : QObject(parent)
{
}

LibraryMaintenance::~LibraryMaintenance()
{
    if (m_thread) {
        m_thread->requestInterruption();
        m_thread->wait();
        delete m_thread;
    }
}

void LibraryMaintenance::start(const QString &category)
{
    if (running())
        return;
    m_category = category;
    m_report.clear();
    m_currentFile.clear();
    m_completed = m_total = 0;
    const QString database = BookDatabase::self().databaseFileName();
    const QString epub = Config::self()->epubCheckCommand(), pdf = Config::self()->pdfCheckCommand();
    const auto result = std::make_shared<QVariantMap>();
    m_thread = QThread::create([this, database, category, epub, pdf, result] {
        *result = inspect(database, category, epub, pdf, [this](int done, int total, const QString &file) {
            QMetaObject::invokeMethod(
                this,
                [this, done, total, file] {
                    m_completed = done;
                    m_total = total;
                    m_currentFile = file;
                    Q_EMIT changed();
                },
                Qt::QueuedConnection);
        });
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        m_report = *result;
        m_completed = m_report.value(QStringLiteral("cancelled")).toBool() ? m_completed : m_total;
        m_currentFile.clear();
        m_thread->deleteLater();
        m_thread = nullptr;
        Q_EMIT changed();
    });
    m_thread->start();
    Q_EMIT changed();
}

void LibraryMaintenance::cleanCache(const QStringList &selectedFiles, const bool reviewed)
{
    removeCacheFiles(selectedFiles, reviewed, false);
}

void LibraryMaintenance::discardWorkingCopy(const QString &fileName)
{
    removeCacheFiles({fileName}, true, true);
}

void LibraryMaintenance::removeCacheFiles(const QStringList &selectedFiles, const bool reviewed, const bool discardWorkingCopy)
{
    if (running() || m_category != QStringLiteral("cache") || selectedFiles.isEmpty() || m_report.value(QStringLiteral("cancelled")).toBool())
        return;
    const QString permission = discardWorkingCopy ? QStringLiteral("canDiscard") : QStringLiteral("canDelete");
    QHash<QString, QVariantMap> offered;
    for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
        const auto row = value.toMap();
        if (row.value(permission).toBool() && (reviewed || row.value(QStringLiteral("safeToDelete")).toBool()))
            offered.insert(row.value(QStringLiteral("fileName")).toString(), row.value(QStringLiteral("cleanupPlan")).toMap());
    }
    // Only paths explicitly selected from this check can be removed.
    for (const auto &path : selectedFiles)
        if (!offered.contains(path))
            return;
    const auto database = BookDatabase::self().databaseFileName();
    const auto result = std::make_shared<QVariantMap>();
    m_completed = 0;
    m_total = selectedFiles.size();
    m_thread = QThread::create([this, database, selectedFiles, reviewed, permission, offered, result] {
        auto guard = MaintenanceFiles::lock();
        if (!guard) {
            *result = {{QStringLiteral("cleanupMessage"), i18n("Editor files are currently being checked or changed. Please try again.")}};
            return;
        }
        const auto fresh = inspect(database, QStringLiteral("cache"));
        QHash<QString, QVariantMap> current;
        for (const auto &value : fresh.value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            current.insert(row.value(QStringLiteral("fileName")).toString(), row);
        }
        int removed = 0, done = 0;
        qint64 bytes = 0;
        QVariantList log;
        QSet<QString> visited;
        for (const auto &path : selectedFiles) {
            if (cancelled())
                break;
            if (visited.contains(path))
                continue;
            visited.insert(path);
            const auto row = current.value(path);
            const bool success = row.value(permission).toBool() && (reviewed || row.value(QStringLiteral("safeToDelete")).toBool())
                && offered.value(path) == row.value(QStringLiteral("cleanupPlan")).toMap() && MaintenanceFiles::removeFile(offered.value(path));
            if (success) {
                ++removed;
                bytes += row.value(QStringLiteral("bytes")).toLongLong();
            }
            log.append(QVariantMap{{QStringLiteral("fileName"), path}, {QStringLiteral("removed"), success}});
            ++done;
            QMetaObject::invokeMethod(
                this,
                [this, done, path] {
                    m_completed = done;
                    m_currentFile = path;
                    Q_EMIT changed();
                },
                Qt::QueuedConnection);
        }
        *result = inspect(database, QStringLiteral("cache"));
        result->insert(QStringLiteral("cleanupLog"), log);
        result->insert(QStringLiteral("removedFiles"), removed);
        result->insert(QStringLiteral("removedBytes"), bytes);
        result->insert(
            QStringLiteral("cleanupMessage"),
            i18n("Removed %1 files (%2 MiB). Retained or skipped: %3.", removed, QString::number(bytes / 1048576.0, 'f', 1), selectedFiles.size() - removed));
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        if (result->contains(QStringLiteral("results")))
            m_report = *result;
        else
            m_report.insert(QStringLiteral("cleanupMessage"), result->value(QStringLiteral("cleanupMessage")));
        m_currentFile.clear();
        m_thread->deleteLater();
        m_thread = nullptr;
        Q_EMIT changed();
    });
    m_thread->start();
    Q_EMIT changed();
}

void LibraryMaintenance::cancel()
{
    if (m_thread)
        m_thread->requestInterruption();
}

bool LibraryMaintenance::saveReport(const QUrl &destination) const
{
    if (running() || m_report.isEmpty() || !destination.isLocalFile())
        return false;
    QSaveFile file(destination.toLocalFile());
    const auto json = QJsonDocument::fromVariant(m_report).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
}

QVariantMap LibraryMaintenance::findRemarkText(const QVariantMap &remark) const
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    const auto reportRows = m_report.value(QStringLiteral("results")).toList();
    bool offered = false;
    for (const auto &value : reportRows) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("annotationBookId")) == remark.value(QStringLiteral("annotationBookId"))
            && row.value(QStringLiteral("annotationId")) == remark.value(QStringLiteral("annotationId"))
            && row.value(QStringLiteral("anchorId")) == remark.value(QStringLiteral("anchorId"))
            && row.value(QStringLiteral("text")) == remark.value(QStringLiteral("text"))
            && row.value(QStringLiteral("fileName")) == remark.value(QStringLiteral("fileName"))
            && row.value(QStringLiteral("cfiRange")) == remark.value(QStringLiteral("cfiRange"))) {
            const auto codes = row.value(QStringLiteral("codes")).toStringList();
            offered = codes.contains(QStringLiteral("annotation-anchor-unresolved"))
                || codes.contains(QStringLiteral("annotation-position-missing"))
                || codes.contains(QStringLiteral("annotation-cfi-unverified"));
            break;
        }
    }
    if (running() || m_category != QStringLiteral("annotations") || m_report.value(QStringLiteral("cancelled")).toBool() || !offered)
        return failure(i18n("Run the remark check again before setting an anchor."));
    const QString bookId = remark.value(QStringLiteral("bookId")).toString();
    const QString annotationBookId = remark.value(QStringLiteral("annotationBookId")).toString();
    const QString annotationId = remark.value(QStringLiteral("annotationId")).toString();
    const QString fileName = remark.value(QStringLiteral("fileName")).toString();
    const QString text = remark.value(QStringLiteral("text")).toString();
    if (bookId.isEmpty() || annotationBookId.isEmpty() || annotationId.isEmpty() || text.simplified().isEmpty()
        || QFileInfo(fileName).suffix().compare(QStringLiteral("epub"), Qt::CaseInsensitive) != 0)
        return failure(i18n("The selected remark passage is incomplete."));
    bool loaded = false;
    const auto books = BookDatabase::self().loadEntries(&loaded);
    int bookMatches = 0;
    for (const auto &book : books) {
        if (bookIdentities(book).contains(normalized(bookId))
            && QFileInfo(book.filename).canonicalFilePath() == QFileInfo(fileName).canonicalFilePath())
            ++bookMatches;
    }
    if (!loaded || bookMatches != 1)
        return failure(i18n("The remark's book has changed. Run the check again."));
    const auto annotations = BookDatabase::self().loadAnnotations(annotationBookId, &loaded);
    int noteMatches = 0;
    for (const auto &value : annotations) {
        const auto note = value.toMap();
        noteMatches += note.value(QStringLiteral("annotationId")) == remark.value(QStringLiteral("annotationId"))
            && note.value(QStringLiteral("anchorId")) == remark.value(QStringLiteral("anchorId"))
            && note.value(QStringLiteral("text")) == remark.value(QStringLiteral("text"))
            && note.value(QStringLiteral("cfiRange")) == remark.value(QStringLiteral("cfiRange"));
    }
    if (!loaded || noteMatches != 1)
        return failure(i18n("The remark has changed. Run the check again."));
    const QByteArray before = verifiedDigest(fileName);
    EPubContainer container(nullptr);
    if (before.isEmpty() || !container.openFile(fileName))
        return failure(i18n("The EPUB could not be read for the text search."));
    auto matches = container.findAnnotationText(text);
    if (before != verifiedDigest(fileName))
        return failure(i18n("The EPUB changed during the text search. Search again."));
    if (matches.isEmpty())
        return failure(i18n("The saved remark text was not found in the EPUB."));
    for (auto &value : matches) {
        auto match = value.toMap();
        match.insert(QStringLiteral("fileHash"), QString::fromLatin1(before.toHex()));
        value = match;
    }
    return {{QStringLiteral("success"), true}, {QStringLiteral("matches"), matches}};
}

QVariantMap LibraryMaintenance::anchorRemarkAtText(const QVariantMap &remark, const QVariantMap &match)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    const auto fresh = findRemarkText(remark);
    if (!fresh.value(QStringLiteral("success")).toBool())
        return fresh;
    bool chosen = false;
    for (const auto &value : fresh.value(QStringLiteral("matches")).toList())
        chosen |= value.toMap() == match;
    if (!chosen)
        return failure(i18n("The text matches have changed. Search again before setting the anchor."));
    const QString fileName = remark.value(QStringLiteral("fileName")).toString();
    const QString bookId = remark.value(QStringLiteral("bookId")).toString();
    const QString annotationBookId = remark.value(QStringLiteral("annotationBookId")).toString();
    const QString annotationId = remark.value(QStringLiteral("annotationId")).toString();
    const QString oldId = remark.value(QStringLiteral("anchorId")).toString();
    const QString text = remark.value(QStringLiteral("text")).toString();
    const QString cfi = remark.value(QStringLiteral("cfiRange")).toString();
    bool loaded = false;
    const auto books = BookDatabase::self().loadEntries(&loaded);
    std::optional<BookEntry> entry;
    for (const auto &book : books) {
        if (bookIdentities(book).contains(normalized(bookId))
            && QFileInfo(book.filename).canonicalFilePath() == QFileInfo(fileName).canonicalFilePath()) {
            if (entry)
                return failure(i18n("The remark's book has changed. Run the check again."));
            entry = book;
        }
    }
    if (!loaded || !entry)
        return failure(i18n("The remark's book has changed. Run the check again."));
    const QString blocked = MaintenanceFiles::anchorEditBlockReason(*entry);
    if (!blocked.isEmpty())
        return failure(blocked);
    const QUuid newUuid = QUuid::createUuidV7();
    const QString newId = newUuid.toString(QUuid::WithoutBraces);
    if (!BookDatabase::self().replaceAnnotationAnchor(annotationBookId, annotationId, oldId, text, cfi, newId))
        return failure(i18n("The remark has changed. Run the check again."));
    const auto commit = BookTruthStore().createAnchorAtText(entry->uniqueIdentifier, match.value(QStringLiteral("documentPath")).toString(),
                                                            match.value(QStringLiteral("occurrence")).toInt(), text,
                                                            match.value(QStringLiteral("fileHash")).toString().toLatin1(), newUuid);
    if (!commit.success) {
        const bool reverted = BookDatabase::self().replaceAnnotationAnchor(annotationBookId, annotationId, newId, text, cfi, oldId);
        return failure(reverted ? commit.errorMessage : i18n("The EPUB anchor could not be created, and the remark could not be restored automatically: %1", commit.errorMessage));
    }
    start(QStringLiteral("annotations"));
    return {{QStringLiteral("success"), true}, {QStringLiteral("anchorId"), newId},
            {QStringLiteral("message"), i18n("Remark anchor set in %1.", match.value(QStringLiteral("documentPath")).toString())}};
}

QVariantMap LibraryMaintenance::combineFormats(const QVariantMap &plan)
{
    bool offered = false;
    if (!running() && m_category == QStringLiteral("database")) {
        for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
            const auto candidate = value.toMap().value(QStringLiteral("formatGroupPlan")).toMap();
            if (!candidate.isEmpty() && candidate.value(QStringLiteral("key")).toString() == plan.value(QStringLiteral("key")).toString()
                && candidate.value(QStringLiteral("files")).toStringList() == plan.value(QStringLiteral("files")).toStringList()) {
                offered = true;
                break;
            }
        }
    }
    const bool success = offered && BookDatabase::self().combineBookFormats(plan);
    if (success)
        start(QStringLiteral("database"));
    return {{QStringLiteral("success"), success},
            {QStringLiteral("message"),
             success ? i18n("Formats combined. The library now shows one cover for this book.")
                     : i18n("Unable to combine these formats. Run the database check again; the book data may have changed.")}};
}

QVariantMap LibraryMaintenance::assignNewBookId(const QString &fileName, const QString &expectedIdentifier)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || m_category != QStringLiteral("database") || m_report.value(QStringLiteral("cancelled")).toBool())
        return failure(i18n("Run the database consistency check again before changing a book ID."));

    bool offered = false;
    for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
        const auto row = value.toMap();
        const auto codes = row.value(QStringLiteral("codes")).toStringList();
        const bool missing = expectedIdentifier.isEmpty() && codes.contains(QStringLiteral("missing-book-id"))
            && row.value(QStringLiteral("fileName")).toString() == fileName;
        const bool ambiguous = !expectedIdentifier.isEmpty() && codes.contains(QStringLiteral("shared-book-id"))
            && normalized(row.value(QStringLiteral("bookId")).toString()) == normalized(expectedIdentifier)
            && row.value(QStringLiteral("sharedIdFiles")).toStringList().contains(fileName);
        offered |= missing || ambiguous;
    }
    if (!offered)
        return failure(i18n("This book ID repair is no longer present in the report. Run the check again."));

    auto result = BookDatabase::self().assignNewUniqueIdentifier(fileName, expectedIdentifier);
    if (result.value(QStringLiteral("success")).toBool()) {
        result.insert(QStringLiteral("message"),
                      i18n("Assigned new UUIDv7 %1 to %2.", result.value(QStringLiteral("newIdentifier")).toString(), fileName));
    }
    return result;
}

QVariantMap LibraryMaintenance::deleteOrphanedBookStateHistory(const QString &bookId, const QString &stateId)
{
    return deleteOrphanedBookStateEntry(QStringLiteral("book_state_history"), bookId, stateId);
}

QVariantMap LibraryMaintenance::deleteOrphanedBookCurrentState(const QString &bookId, const QString &stateId)
{
    return deleteOrphanedBookStateEntry(QStringLiteral("book_current_state"), bookId, stateId);
}

QVariantMap LibraryMaintenance::deleteOrphanedBookStateEntry(const QString &tableName, const QString &bookId, const QString &stateId)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || m_category != QStringLiteral("database") || m_report.value(QStringLiteral("cancelled")).toBool()
        || (tableName != QStringLiteral("book_state_history") && tableName != QStringLiteral("book_current_state")))
        return failure(i18n("Run the database consistency check again before deleting saved-state history."));

    bool offered = false;
    for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("tableName")).toString() == tableName
            && row.value(QStringLiteral("bookId")).toString() == bookId && row.value(QStringLiteral("stateId")).toString() == stateId
            && row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("unassigned-record"))) {
            offered = true;
            break;
        }
    }
    if (!offered)
        return failure(i18n("This saved-state entry is no longer listed as orphaned. Run the check again."));

    const auto result = tableName == QStringLiteral("book_current_state")
        ? BookDatabase::self().deleteOrphanedBookCurrentState(bookId, stateId)
        : BookDatabase::self().deleteOrphanedBookStateHistory(bookId, stateId);
    if (result.value(QStringLiteral("success")).toBool())
        start(QStringLiteral("database"));
    return result;
}

QVariantMap LibraryMaintenance::deleteAllOrphanedBookStateHistory()
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || m_category != QStringLiteral("database") || m_report.value(QStringLiteral("cancelled")).toBool())
        return failure(i18n("Run the database consistency check again before deleting saved-state history."));

    QVariantList entries;
    for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("tableName")).toString() == QStringLiteral("book_state_history")
            && row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("unassigned-record"))) {
            entries.append(QVariantMap{{QStringLiteral("bookId"), row.value(QStringLiteral("bookId"))},
                                       {QStringLiteral("stateId"), row.value(QStringLiteral("stateId"))}});
        }
    }
    if (entries.isEmpty())
        return failure(i18n("There are no orphaned saved-state history entries in the current report."));

    auto result = BookDatabase::self().deleteOrphanedBookStateHistoryEntries(entries);
    if (result.value(QStringLiteral("success")).toBool()) {
        const int deletedCount = result.value(QStringLiteral("deletedCount")).toInt();
        result.insert(QStringLiteral("message"), deletedCount > 0
                          ? i18n("Deleted %1 orphaned saved-state history entries.", deletedCount)
                          : i18n("No saved-state history entries were deleted; the database changed since the report."));
        start(QStringLiteral("database"));
    }
    return result;
}

QVariantMap LibraryMaintenance::deleteAllOrphanedBookStates()
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || m_category != QStringLiteral("database") || m_report.value(QStringLiteral("cancelled")).toBool())
        return failure(i18n("Run the database consistency check again before deleting saved-state entries."));

    QVariantList entries;
    for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
        const auto row = value.toMap();
        const QString tableName = row.value(QStringLiteral("tableName")).toString();
        if ((tableName == QStringLiteral("book_state_history") || tableName == QStringLiteral("book_current_state"))
            && row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("unassigned-record"))) {
            entries.append(QVariantMap{{QStringLiteral("tableName"), tableName},
                                       {QStringLiteral("bookId"), row.value(QStringLiteral("bookId"))},
                                       {QStringLiteral("stateId"), row.value(QStringLiteral("stateId"))}});
        }
    }
    if (entries.isEmpty())
        return failure(i18n("There are no orphaned saved-state entries in the current report."));

    auto result = BookDatabase::self().deleteOrphanedBookStateEntries(entries);
    if (result.value(QStringLiteral("success")).toBool()) {
        const int deletedCount = result.value(QStringLiteral("deletedCount")).toInt();
        result.insert(QStringLiteral("message"), deletedCount > 0
                          ? i18n("Deleted %1 orphaned saved-state entries.", deletedCount)
                          : i18n("No saved-state entries were deleted; the database changed since the report."));
        start(QStringLiteral("database"));
    }
    return result;
}

QVariantMap LibraryMaintenance::deleteDuplicate(const QString &fileName)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    QStringList duplicates;
    QByteArray expectedHash;
    if (!running() && m_category == QStringLiteral("database") && !m_report.value(QStringLiteral("cancelled")).toBool()) {
        for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            const auto paths = row.value(QStringLiteral("duplicateFiles")).toStringList();
            if (paths.contains(fileName)) {
                duplicates = paths;
                expectedHash = QByteArray::fromHex(row.value(QStringLiteral("contentHash")).toString().toLatin1());
                break;
            }
        }
    }
    if (expectedHash.isEmpty())
        return failure(i18n("Only identical book files from the current database check can be deleted."));

    bool loaded = false;
    const auto books = BookDatabase::self().loadEntries(&loaded);
    BookEntry selected;
    bool retainedCopy = false;
    const auto selectedPath = QFileInfo(fileName).canonicalFilePath();
    for (const auto &book : books) {
        if (book.filename == fileName) {
            selected = book;
        } else if (!retainedCopy && duplicates.contains(book.filename) && QFileInfo(book.filename).canonicalFilePath() != selectedPath) {
            retainedCopy = verifiedDigest(book.filename) == expectedHash;
        }
    }
    // Recheck both sides: an old report must never authorize deleting the last copy or a changed book.
    if (!loaded || selected.filename.isEmpty() || !retainedCopy || verifiedDigest(fileName) != expectedHash)
        return failure(i18n("The files have changed or no identical copy remains. Run the database check again."));

    return removeBook(selected, true);
}

QVariantMap LibraryMaintenance::deleteSharedBookIdFile(const QString &fileName, const QString &bookId, bool deleteFile)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    const QString id = normalized(bookId);
    QStringList paths;
    QVariantMap hashes;
    if (!running() && m_category == QStringLiteral("database") && !m_report.value(QStringLiteral("cancelled")).toBool() && !id.isEmpty()) {
        for (const auto &value : m_report.value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            const auto files = row.value(QStringLiteral("sharedIdFiles")).toStringList();
            if (row.value(QStringLiteral("bookId")).toString() == id && files.contains(fileName)) {
                paths = files;
                hashes = row.value(QStringLiteral("fileHashes")).toMap();
                break;
            }
        }
    }
    if (paths.isEmpty())
        return failure(i18n("Only files sharing a book ID from the current database check can be deleted."));

    bool loaded = false;
    const auto books = BookDatabase::self().loadEntries(&loaded);
    BookEntry selected;
    bool retainedFile = false;
    const auto selectedPath = QFileInfo(fileName).canonicalFilePath();
    for (const auto &book : books) {
        if (!bookIdentities(book).contains(id))
            continue;
        if (book.filename == fileName) {
            selected = book;
        } else if (!retainedFile && paths.contains(book.filename) && QFileInfo(book.filename).canonicalFilePath() != selectedPath) {
            const auto hash = QByteArray::fromHex(hashes.value(book.filename).toString().toLatin1());
            retainedFile = !hash.isEmpty() && verifiedDigest(book.filename) == hash;
        }
    }
    // The contents can differ, but both files and their shared identity must still match the report.
    const auto selectedHash = QByteArray::fromHex(hashes.value(fileName).toString().toLatin1());
    if (!loaded || selected.filename.isEmpty() || !retainedFile || selectedHash.isEmpty() || verifiedDigest(fileName) != selectedHash)
        return failure(i18n("The files or book IDs have changed, or no other file with this book ID remains. Run the database check again."));
    return removeBook(selected, deleteFile);
}

QVariantMap LibraryMaintenance::removeBook(const BookEntry &book, bool deleteFile)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    const auto &fileName = book.filename;
    // Keep the file recoverable until removal of the entry and its references succeeds.
    QString staged;
    if (deleteFile) {
        staged = fileName + QStringLiteral(".arianna-delete-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(fileName, staged))
            return failure(i18n("Unable to delete the book file: %1", fileName));
    }
    const auto result = BookReferenceCheck::removeBook(book);
    if (!result.value(QStringLiteral("success")).toBool()) {
        if (deleteFile && !QFile::rename(staged, fileName))
            return failure(result.value(QStringLiteral("message")).toString() + QLatin1Char('\n')
                           + i18n("The book file could not be restored. It is still available at: %1", staged));
        return result;
    }
    const bool removed = !deleteFile || QFile::remove(staged);
    start(QStringLiteral("database"));
    return {{QStringLiteral("success"), removed},
            {QStringLiteral("message"),
             !deleteFile ? i18n("Book removed from the library: %1", fileName)
                 : removed ? i18n("Book file deleted: %1", fileName)
                           : i18n("The library entry was removed, but the file could not be deleted: %1", staged)}};
}

QVariantMap LibraryMaintenance::inspect(const QString &databaseFile,
                                        const QString &category,
                                        const QString &epubCommand,
                                        const QString &pdfCommand,
                                        const Progress &progress)
{
    const auto data = snapshot(databaseFile, category == QStringLiteral("database"));
    QVariantList rows;
    if (!data.error.isEmpty())
        rows.append(finding(QStringLiteral("error"), QStringLiteral("database-unreadable"), i18n("Unable to read the library database: %1", data.error)));
    else if (category == QStringLiteral("cache"))
        rows = MaintenanceFiles::inspect(data.books, data.states, data.history, progress);
    else if (category == QStringLiteral("database"))
        rows = checkDatabase(data, progress);
    else if (category == QStringLiteral("structure"))
        rows = checkStructure(data, epubCommand, pdfCommand, progress);
    else if (category == QStringLiteral("annotations"))
        rows = BookReferenceCheck::inspectLibraryAnnotations(data.books, data.annotations, progress);
    else if (category == QStringLiteral("references"))
        rows = BookReferenceCheck::inspectStoredReferences(data.books, data.references, progress);
    else if (category == QStringLiteral("references-all"))
        rows = BookReferenceCheck::inspectLibraryReferences(data.books, data.references, data.annotations, progress);
    else
        rows.append(finding(QStringLiteral("error"), QStringLiteral("unknown-check"), i18n("Unknown maintenance check.")));
    int errors = 0, warnings = 0, passed = 0;
    for (const auto &value : rows) {
        const auto status = value.toMap().value(QStringLiteral("status")).toString();
        errors += status == QStringLiteral("error");
        warnings += status == QStringLiteral("warning");
        passed += status == QStringLiteral("ok");
    }
    QSet<QString> displayGroups;
    int bookCount = 0;
    for (const auto &book : data.books) {
        const auto key = BookFormats::displayKey(book);
        if (key.isEmpty() || !displayGroups.contains(key))
            ++bookCount;
        if (!key.isEmpty())
            displayGroups.insert(key);
    }
    return {{QStringLiteral("category"), category},
            {QStringLiteral("results"), rows},
            {QStringLiteral("errors"), errors},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("passed"), passed},
            {QStringLiteral("cancelled"), cancelled()},
            {QStringLiteral("success"), !cancelled() && errors == 0 && warnings == 0},
            {QStringLiteral("books"), bookCount},
            {QStringLiteral("files"), data.books.size()},
            {QStringLiteral("annotations"), data.annotations.size()},
            {QStringLiteral("references"), data.references.size()},
            {QStringLiteral("checkedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
}
#include "moc_librarymaintenance.cpp"
