// SPDX-FileCopyrightText: 2017 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "bookdatabase.h"
#include "bookformats.h"
#include "bookreferencecheck.h"

#include "categoryentriesmodel.h"

#include <KLocalizedString>
#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QMetaType>
#include <QMimeDatabase>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <QDir>

#include <arianna_debug.h>

#include <qdebug.h>
#include <algorithm>
#include <utility>

class BookDatabase::Private
{
public:
    Private()
    {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));

        QDir location{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)};
        if (!location.exists())
            location.mkpath(QStringLiteral("."));

        dbfile = location.absoluteFilePath(QStringLiteral("library.sqlite"));
        db.setDatabaseName(dbfile);

        databaseChangedTimer.setInterval(250);
        databaseChangedTimer.setSingleShot(true);
        ensureWatching();
    }

    QSqlDatabase db;
    QString dbfile;
    QStringList fieldNames;
    QFileSystemWatcher watcher;
    QTimer databaseChangedTimer;

    void ensureWatching()
    {
        const QFileInfo databaseInfo(dbfile);
        const QString directory = databaseInfo.absolutePath();
        if (!watcher.directories().contains(directory)) {
            watcher.addPath(directory);
        }

        if (databaseInfo.exists()) {
            const QString databasePath = databaseInfo.absoluteFilePath();
            if (!watcher.files().contains(databasePath)) {
                watcher.addPath(databasePath);
            }
        }
    }

    void ensureColumn(const QString &name, const QString &definition)
    {
        if (fieldNames.contains(name)) {
            return;
        }

        QSqlQuery alterQuery;
        if (!alterQuery.exec(QStringLiteral("ALTER TABLE books ADD COLUMN %1").arg(definition))) {
            qCDebug(ARIANNA_LOG) << "Unable to add database column" << name << alterQuery.lastError();
            return;
        }

        fieldNames.append(name);
    }

    void migrateLegacyGenresToSubjects()
    {
        if (!fieldNames.contains(QStringLiteral("subjects")) || !fieldNames.contains(QStringLiteral("genres"))) {
            return;
        }

        QSqlQuery migrateQuery;
        if (!migrateQuery.exec(
                QStringLiteral("UPDATE books SET subjects = genres WHERE (subjects IS NULL OR subjects = '') AND genres IS NOT NULL AND genres <> ''"))) {
            qCDebug(ARIANNA_LOG) << "Unable to migrate legacy genres to subjects" << migrateQuery.lastError();
        }
    }

    bool ensureTableColumn(const QString &table, const QString &name, const QString &definition)
    {
        QSqlQuery columns(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table));
        while (columns.next()) {
            if (columns.value(1).toString().compare(name, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }

        QSqlQuery alterQuery;
        if (!alterQuery.exec(QStringLiteral("ALTER TABLE \"%1\" ADD COLUMN %2").arg(table, definition))) {
            qCDebug(ARIANNA_LOG) << "Unable to add database column" << table << name << alterQuery.lastError();
            return false;
        }

        return true;
    }

    bool prepareDb()
    {
        if (!db.open()) {
            qCDebug(ARIANNA_LOG) << QStringLiteral("Failed to open the book database file") << dbfile << db.lastError();
            return false;
        }

        const QStringList tables = db.tables();
        if (tables.contains(QStringLiteral("books"), Qt::CaseInsensitive)) {
            if (fieldNames.isEmpty()) {
                QSqlQuery qu(QStringLiteral("SELECT * FROM books"));
                for (int i = 0; i < qu.record().count(); i++) {
                    fieldNames.append(qu.record().fieldName(i));
                }
                qCDebug(ARIANNA_LOG) << Q_FUNC_INFO << ": opening database with following fieldNames:" << fieldNames;
            }
            ensureColumn(QStringLiteral("zoomLevel"), QStringLiteral("zoomLevel real default 1.0"));
            ensureColumn(QStringLiteral("pageMode"), QStringLiteral("pageMode varchar"));
            ensureColumn(QStringLiteral("pageWidthDistance"), QStringLiteral("pageWidthDistance integer default -1"));
            ensureColumn(QStringLiteral("bookVersion"), QStringLiteral("bookVersion varchar"));
            ensureColumn(QStringLiteral("uniqueIdentifier"), QStringLiteral("uniqueIdentifier varchar"));
            ensureColumn(QStringLiteral("formatGroup"), QStringLiteral("formatGroup varchar"));
            ensureColumn(QStringLiteral("subjects"), QStringLiteral("subjects varchar"));
            ensureColumn(QStringLiteral("pdfNotInvertedRegions"), QStringLiteral("pdfNotInvertedRegions text"));
            ensureColumn(QStringLiteral("lastClosedTime"), QStringLiteral("lastClosedTime datetime"));
            ensureColumn(QStringLiteral("lastClosedOpenedTime"), QStringLiteral("lastClosedOpenedTime datetime"));
            ensureColumn(QStringLiteral("passwordProtected"), QStringLiteral("passwordProtected int default 0"));
            migrateLegacyGenresToSubjects();
            if (!ensureAnnotationsTable()) {
                closeDb();
                return false;
            }

            if (!ensureReferencesTable()) {
                closeDb();
                return false;
            }
            if (!ensureBookStateTable()) {
                closeDb();
                return false;
            }
            ensureWatching();
            return true;
        }

        QSqlQuery q;
        QStringList entryNames;
        // clang-format off
        entryNames << QStringLiteral("fileName varchar primary key")
                   << QStringLiteral("fileTitle varchar")
                   << QStringLiteral("title varchar")
                   << QStringLiteral("subjects varchar")
                   << QStringLiteral("genres varchar")
                   << QStringLiteral("keywords varchar")
                   << QStringLiteral("characters varchar")
                   << QStringLiteral("description varchar")
                   << QStringLiteral("series varchar")
                   << QStringLiteral("seriesNumbers varchar")
                   << QStringLiteral("seriesVolumes varchar")
                   << QStringLiteral("author varchar")
                   << QStringLiteral("publisher varchar")
                   << QStringLiteral("created datetime")
                   << QStringLiteral("lastOpenedTime datetime")
                   << QStringLiteral("lastClosedTime datetime")
                   << QStringLiteral("lastClosedOpenedTime datetime")
                   << QStringLiteral("thumbnail varchar")
                   << QStringLiteral("passwordProtected int default 0")
                   << QStringLiteral("comment varchar")
                   << QStringLiteral("tags varchar")
                   << QStringLiteral("rating varchar")
                   << QStringLiteral("locations text")
                   << QStringLiteral("pdfNotInvertedRegions text")
                   << QStringLiteral("currentLocation varchar")
                   << QStringLiteral("currentProgress int")
                   << QStringLiteral("zoomLevel real default 1.0")
                   << QStringLiteral("pageMode varchar")
                   << QStringLiteral("pageWidthDistance integer default -1")
                   << QStringLiteral("bookVersion varchar")
                   << QStringLiteral("rights varchar")
                   << QStringLiteral("source varchar")
                   << QStringLiteral("identifier varchar")
                   << QStringLiteral("uniqueIdentifier varchar")
                   << QStringLiteral("formatGroup varchar")
                   << QStringLiteral("language varchar");
        // clang-format on

        if (!q.exec(QStringLiteral("create table books(") + entryNames.join(QStringLiteral(", ")) + QLatin1Char(')'))) {
            qCDebug(ARIANNA_LOG) << "Database could not create the table books" << q.lastError();
            return false;
        }
        for (int i = 0; i < entryNames.size(); i++) {
            QString fieldName = entryNames.at(i).split(QLatin1Char(' ')).first();
            fieldNames.append(fieldName);
        }
        qCDebug(ARIANNA_LOG) << Q_FUNC_INFO << ": making database with following fieldNames:" << fieldNames;

        if (!ensureAnnotationsTable()) {
            return false;
        }

        if (!ensureReferencesTable()) {
            return false;
        }

        if (!ensureBookStateTable()) {
            return false;
        }

        ensureWatching();
        return true;
    }

    bool ensureReferencesTable()
    {
        QSqlQuery q;
        if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS 'references' ("
                                   "sourceBookId varchar not null, "
                                   "sourceAnchorId varchar not null, "
                                   "sourceAnchorTitle text, "
                                   "targetBookId varchar not null, "
                                   "targetLocation text not null, "
                                   "targetPreviewHtml text, "
                                   "created datetime, "
                                   "modified datetime, "
                                   "primary key(sourceBookId, sourceAnchorId))"))) {
            qCDebug(ARIANNA_LOG) << "Database could not create the table references" << q.lastError();
            return false;
        }

        QSqlQuery columns(QStringLiteral("PRAGMA table_info(\"references\")"));
        QStringList referenceColumns;
        QStringList referencePrimaryKeyColumns;
        while (columns.next()) {
            const QString columnName = columns.value(1).toString();
            referenceColumns.append(columnName);

            const int primaryKeyIndex = columns.value(5).toInt();
            if (primaryKeyIndex > 0) {
                while (referencePrimaryKeyColumns.size() < primaryKeyIndex) {
                    referencePrimaryKeyColumns.append(QString());
                }
                referencePrimaryKeyColumns[primaryKeyIndex - 1] = columnName;
            }
        }
        columns.finish();

        const QStringList expectedReferenceColumns = {
            QStringLiteral("sourceBookId"),
            QStringLiteral("sourceAnchorId"),
            QStringLiteral("sourceAnchorTitle"),
            QStringLiteral("targetBookId"),
            QStringLiteral("targetLocation"),
            QStringLiteral("targetPreviewHtml"),
            QStringLiteral("created"),
            QStringLiteral("modified"),
        };
        const QStringList expectedReferencePrimaryKeyColumns = {
            QStringLiteral("sourceBookId"),
            QStringLiteral("sourceAnchorId"),
        };

        if (referenceColumns != expectedReferenceColumns || referencePrimaryKeyColumns != expectedReferencePrimaryKeyColumns) {
            if (!db.transaction()) {
                qCDebug(ARIANNA_LOG) << "Unable to start references schema migration" << db.lastError();
                return false;
            }

            auto execMigrationQuery = [](const QString &sql) {
                QSqlQuery query;
                if (!query.exec(sql)) {
                    qCDebug(ARIANNA_LOG) << "References schema migration failed" << query.lastError() << sql;
                    return false;
                }
                return true;
            };

            const bool hasSourceBookId = referenceColumns.contains(QStringLiteral("sourceBookId"), Qt::CaseInsensitive);
            const bool hasSourceAnchorId = referenceColumns.contains(QStringLiteral("sourceAnchorId"), Qt::CaseInsensitive);
            const bool hasSourceAnchorTitle = referenceColumns.contains(QStringLiteral("sourceAnchorTitle"), Qt::CaseInsensitive);
            const bool hasTargetBookId = referenceColumns.contains(QStringLiteral("targetBookId"), Qt::CaseInsensitive);
            const bool hasTargetLocation = referenceColumns.contains(QStringLiteral("targetLocation"), Qt::CaseInsensitive);
            const bool hasTargetPreviewHtml = referenceColumns.contains(QStringLiteral("targetPreviewHtml"), Qt::CaseInsensitive);
            const bool hasCreated = referenceColumns.contains(QStringLiteral("created"), Qt::CaseInsensitive);
            const bool hasModified = referenceColumns.contains(QStringLiteral("modified"), Qt::CaseInsensitive);

            const bool canMigrateReferences = hasSourceBookId && hasSourceAnchorId && hasTargetBookId && hasTargetLocation;
            const QString targetLocationExpression = QStringLiteral("NULLIF(TRIM(targetLocation), '')");
            const QString referenceOrderExpression = hasCreated ? QStringLiteral("created ASC, rowid ASC") : QStringLiteral("rowid ASC");

            bool migrated = execMigrationQuery(QStringLiteral("DROP TABLE IF EXISTS \"references_new\""))
                && execMigrationQuery(QStringLiteral("CREATE TABLE \"references_new\" ("
                                                     "sourceBookId varchar not null, "
                                                     "sourceAnchorId varchar not null, "
                                                     "sourceAnchorTitle text, "
                                                     "targetBookId varchar not null, "
                                                     "targetLocation text not null, "
                                                     "targetPreviewHtml text, "
                                                     "created datetime, "
                                                     "modified datetime, "
                                                     "primary key(sourceBookId, sourceAnchorId))"));

            if (migrated && canMigrateReferences) {
                migrated = execMigrationQuery(QStringLiteral("INSERT OR IGNORE INTO \"references_new\"("
                                                             "sourceBookId, sourceAnchorId, sourceAnchorTitle, "
                                                             "targetBookId, targetLocation, targetPreviewHtml, "
                                                             "created, modified) "
                                                             "SELECT sourceBookId, sourceAnchorId, %1, "
                                                             "targetBookId, %2, %3, "
                                                             "%4, %5 "
                                                             "FROM \"references\" "
                                                             "WHERE TRIM(sourceBookId) <> '' "
                                                             "AND TRIM(sourceAnchorId) <> '' "
                                                             "AND TRIM(targetBookId) <> '' "
                                                             "AND %2 IS NOT NULL "
                                                             "ORDER BY %6")
                                                  .arg(hasSourceAnchorTitle ? QStringLiteral("sourceAnchorTitle") : QStringLiteral("NULL"),
                                                       targetLocationExpression,
                                                       hasTargetPreviewHtml ? QStringLiteral("targetPreviewHtml") : QStringLiteral("NULL"),
                                                       hasCreated ? QStringLiteral("created") : QStringLiteral("NULL"),
                                                       hasModified ? QStringLiteral("modified") : QStringLiteral("NULL"),
                                                       referenceOrderExpression));
            }

            migrated = migrated && execMigrationQuery(QStringLiteral("DROP TABLE \"references\""))
                && execMigrationQuery(QStringLiteral("ALTER TABLE \"references_new\" RENAME TO \"references\""));

            if (!migrated) {
                db.rollback();
                return false;
            }

            if (!db.commit()) {
                qCDebug(ARIANNA_LOG) << "Unable to commit references schema migration" << db.lastError();
                db.rollback();
                return false;
            }
        }

        return true;
    }

    bool ensureBookStateTable()
    {
        QSqlQuery q;
        if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS book_current_state ("
                                   "book_id TEXT PRIMARY KEY, "
                                   "state_id TEXT NOT NULL, "
                                   "text_content_hash TEXT NOT NULL, "
                                   "document_state_hash TEXT NOT NULL, "
                                   "epub_file_hash TEXT NOT NULL, "
                                   "modified datetime)"))) {
            qCDebug(ARIANNA_LOG) << "Database could not create the table book_current_state" << q.lastError();
            return false;
        }
        if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS book_state_history (book_id TEXT NOT NULL, state_id TEXT NOT NULL, "
                                   "epub_file_hash TEXT NOT NULL, modified datetime, PRIMARY KEY(book_id, state_id))")))
            return false;
        return q.exec(QStringLiteral("INSERT OR IGNORE INTO book_state_history SELECT book_id,state_id,epub_file_hash,modified FROM book_current_state"));
    }

    bool ensureAnnotationsTable()
    {
        QSqlQuery q;
        if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS annotations("
                                   "bookId varchar not null, "
                                   "annotationId varchar not null, "
                                   "cfiRange varchar, "
                                   "anchorId varchar, "
                                   "color varchar, "
                                   "text text, "
                                   "note text, "
                                   "format varchar, "
                                   "location text, "
                                   "created datetime, "
                                   "modified datetime, "
                                   "noteHtml text, noteSource text, noteMetadata text, "
                                   "primary key(bookId, annotationId))"))) {
            qCDebug(ARIANNA_LOG) << "Database could not create the table annotations" << q.lastError();
            return false;
        }

        QSqlQuery columns(QStringLiteral("PRAGMA table_info(\"annotations\")"));
        QStringList annotationColumns;
        while (columns.next()) {
            annotationColumns.append(columns.value(1).toString());
        }
        columns.finish();

        const QStringList expectedAnnotationColumns = {
            QStringLiteral("bookId"),
            QStringLiteral("annotationId"),
            QStringLiteral("cfiRange"),
            QStringLiteral("anchorId"),
            QStringLiteral("color"),
            QStringLiteral("text"),
            QStringLiteral("note"),
            QStringLiteral("format"),
            QStringLiteral("location"),
            QStringLiteral("created"),
            QStringLiteral("modified"),
            QStringLiteral("noteHtml"),
            QStringLiteral("noteSource"),
            QStringLiteral("noteMetadata"),
        };

        if (annotationColumns == expectedAnnotationColumns) {
            return true;
        }

        if (!db.transaction()) {
            qCDebug(ARIANNA_LOG) << "Unable to start annotations schema migration" << db.lastError();
            return false;
        }

        auto execMigrationQuery = [](const QString &sql) {
            QSqlQuery query;
            if (!query.exec(sql)) {
                qCDebug(ARIANNA_LOG) << "Annotations schema migration failed" << query.lastError() << sql;
                return false;
            }
            return true;
        };
        auto hasColumn = [&annotationColumns](const QString &name) {
            return annotationColumns.contains(name, Qt::CaseInsensitive);
        };
        auto columnOrNull = [&hasColumn](const QString &name) {
            return hasColumn(name) ? QStringLiteral("\"%1\"").arg(name) : QStringLiteral("NULL");
        };

        const QString annotationIdExpression = hasColumn(QStringLiteral("annotationId"))
            ? QStringLiteral("CASE WHEN annotationId IS NOT NULL AND annotationId != '' THEN annotationId ELSE 'legacy-' || rowid END")
            : QStringLiteral("'legacy-' || rowid");
        QString cfiRangeExpression;
        if (hasColumn(QStringLiteral("cfiRange")) && hasColumn(QStringLiteral("value"))) {
            cfiRangeExpression = QStringLiteral("CASE WHEN cfiRange IS NOT NULL AND cfiRange != '' THEN cfiRange ELSE value END");
        } else if (hasColumn(QStringLiteral("cfiRange"))) {
            cfiRangeExpression = QStringLiteral("cfiRange");
        } else if (hasColumn(QStringLiteral("value"))) {
            cfiRangeExpression = QStringLiteral("value");
        } else {
            cfiRangeExpression = QStringLiteral("NULL");
        }

        const bool migrated = execMigrationQuery(QStringLiteral("DROP TABLE IF EXISTS annotations_new"))
            && execMigrationQuery(QStringLiteral("CREATE TABLE annotations_new("
                                                 "bookId varchar not null, "
                                                 "annotationId varchar not null, "
                                                 "cfiRange varchar, "
                                                 "anchorId varchar, "
                                                 "color varchar, "
                                                 "text text, "
                                                 "note text, "
                                                 "format varchar, "
                                                 "location text, "
                                                 "created datetime, "
                                                 "modified datetime, "
                                                 "noteHtml text, noteSource text, noteMetadata text, "
                                                 "primary key(bookId, annotationId))"))
            && execMigrationQuery(QStringLiteral("INSERT OR REPLACE INTO annotations_new("
                                                 "bookId, annotationId, cfiRange, anchorId, "
                                                 "color, text, note, format, location, created, modified, noteHtml, noteSource, noteMetadata) "
                                                 "SELECT bookId, %1, %2, %3, %4, %5, %6, %7, %8, %9, %10, %11, %12, %13 "
                                                 "FROM annotations "
                                                 "WHERE bookId IS NOT NULL AND bookId != ''")
                                      .arg(annotationIdExpression,
                                           cfiRangeExpression,
                                           columnOrNull(QStringLiteral("anchorId")),
                                           columnOrNull(QStringLiteral("color")),
                                           columnOrNull(QStringLiteral("text")),
                                           columnOrNull(QStringLiteral("note")),
                                           columnOrNull(QStringLiteral("format")),
                                           columnOrNull(QStringLiteral("location")),
                                           columnOrNull(QStringLiteral("created")),
                                           columnOrNull(QStringLiteral("modified")),
                                           columnOrNull(QStringLiteral("noteHtml")),
                                           columnOrNull(QStringLiteral("noteSource")),
                                           columnOrNull(QStringLiteral("noteMetadata"))))
            && execMigrationQuery(QStringLiteral("DROP TABLE annotations"))
            && execMigrationQuery(QStringLiteral("ALTER TABLE annotations_new RENAME TO annotations"));

        if (!migrated) {
            db.rollback();
            return false;
        }

        if (!db.commit()) {
            qCDebug(ARIANNA_LOG) << "Unable to commit annotations schema migration" << db.lastError();
            db.rollback();
            return false;
        }

        return true;
    }

    void closeDb()
    {
        db.close();
    }

    BookEntry fromSqlQuery(const QSqlQuery &query)
    {
        BookEntry entry;
        entry.filename = query.value(fieldNames.indexOf(QStringLiteral("fileName"))).toString();
        entry.formatGroup = query.value(fieldNames.indexOf(QStringLiteral("formatGroup"))).toString();
        entry.filetitle = query.value(fieldNames.indexOf(QStringLiteral("fileTitle"))).toString();
        entry.title = query.value(fieldNames.indexOf(QStringLiteral("title"))).toString();
        entry.series = query.value(fieldNames.indexOf(QStringLiteral("series"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.author = query.value(fieldNames.indexOf(QStringLiteral("author"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.publisher = query.value(fieldNames.indexOf(QStringLiteral("publisher"))).toString();
        entry.created = query.value(fieldNames.indexOf(QStringLiteral("created"))).toDateTime();
        entry.lastOpenedTime = query.value(fieldNames.indexOf(QStringLiteral("lastOpenedTime"))).toDateTime();
        entry.lastClosedTime = query.value(fieldNames.indexOf(QStringLiteral("lastClosedTime"))).toDateTime();
        entry.lastClosedOpenedTime = query.value(fieldNames.indexOf(QStringLiteral("lastClosedOpenedTime"))).toDateTime();
        entry.currentLocation = query.value(fieldNames.indexOf(QStringLiteral("currentLocation"))).toString();
        entry.currentProgress = query.value(fieldNames.indexOf(QStringLiteral("currentProgress"))).toInt();
        entry.zoomLevel = query.value(fieldNames.indexOf(QStringLiteral("zoomLevel"))).toDouble();
        if (entry.zoomLevel <= 0.0) {
            entry.zoomLevel = 1.0;
        }
        const QVariant pageWidthDistance = query.value(fieldNames.indexOf(QStringLiteral("pageWidthDistance")));
        entry.pageWidthDistance = pageWidthDistance.isNull() ? -1 : qMax(-1, pageWidthDistance.toInt());
        entry.bookVersion = query.value(fieldNames.indexOf(QStringLiteral("bookVersion"))).toString();
        const int pageModeIndex = fieldNames.indexOf(QStringLiteral("pageMode"));
        if (pageModeIndex >= 0) {
            entry.pageMode = query.value(pageModeIndex).toString();
            if (entry.pageMode != QStringLiteral("single") && entry.pageMode != QStringLiteral("two")) {
                entry.pageMode.clear();
            }
        }
        entry.thumbnail = query.value(fieldNames.indexOf(QStringLiteral("thumbnail"))).toString();
        const int passwordProtectedIndex = fieldNames.indexOf(QStringLiteral("passwordProtected"));
        entry.passwordProtected = passwordProtectedIndex >= 0 && query.value(passwordProtectedIndex).toBool();
        entry.description = query.value(fieldNames.indexOf(QStringLiteral("description"))).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        entry.comment = query.value(fieldNames.indexOf(QStringLiteral("comment"))).toString();
        entry.tags = query.value(fieldNames.indexOf(QStringLiteral("tags"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.rating = query.value(fieldNames.indexOf(QStringLiteral("rating"))).toInt();
        entry.seriesNumbers = query.value(fieldNames.indexOf(QStringLiteral("seriesNumbers"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.seriesVolumes = query.value(fieldNames.indexOf(QStringLiteral("seriesVolumes"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        const int subjectsIndex = fieldNames.indexOf(QStringLiteral("subjects"));
        const int legacyGenresIndex = fieldNames.indexOf(QStringLiteral("genres"));
        const QString subjectsValue =
            subjectsIndex >= 0 ? query.value(subjectsIndex).toString() : (legacyGenresIndex >= 0 ? query.value(legacyGenresIndex).toString() : QString());
        entry.genres = subjectsValue.split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.keywords = query.value(fieldNames.indexOf(QStringLiteral("keywords"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.characters = query.value(fieldNames.indexOf(QStringLiteral("characters"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.locations = query.value(fieldNames.indexOf(QStringLiteral("locations"))).toString();
        entry.pdfNotInvertedRegions = query.value(fieldNames.indexOf(QStringLiteral("pdfNotInvertedRegions"))).toString();
        entry.language = query.value(fieldNames.indexOf(QStringLiteral("language"))).toString();
        entry.identifier = query.value(fieldNames.indexOf(QStringLiteral("identifier"))).toString();
        entry.uniqueIdentifier = query.value(fieldNames.indexOf(QStringLiteral("uniqueIdentifier"))).toString();
        entry.rights = query.value(fieldNames.indexOf(QStringLiteral("rights"))).toString();
        entry.source = query.value(fieldNames.indexOf(QStringLiteral("source"))).toString();
        return entry;
    }

    bool insertEntry(const BookEntry &entry)
    {
        QStringList valueNames;
        for (const auto &field : fieldNames)
            valueNames.append(QLatin1Char(':') + field);
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO books (") + fieldNames.join(QStringLiteral(", ")) + QStringLiteral(") VALUES (")
                      + valueNames.join(QStringLiteral(", ")) + QLatin1Char(')'));
        bindEntry(query, entry);
        if (!query.exec()) {
            qCWarning(ARIANNA_LOG) << "Unable to insert book" << entry.filename << query.lastError();
            return false;
        }
        return true;
    }

    void bindEntry(QSqlQuery &query, const BookEntry &entry, bool includeReaderHistory = true)
    {
        query.bindValue(QStringLiteral(":fileName"), entry.filename);
        query.bindValue(QStringLiteral(":formatGroup"), entry.formatGroup);
        query.bindValue(QStringLiteral(":fileTitle"), entry.filetitle);
        query.bindValue(QStringLiteral(":title"), entry.title);
        query.bindValue(QStringLiteral(":series"), entry.series.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":author"), entry.author.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":publisher"), entry.publisher);
        query.bindValue(QStringLiteral(":created"), entry.created);
        query.bindValue(QStringLiteral(":lastOpenedTime"), entry.lastOpenedTime);
        if (includeReaderHistory) {
            query.bindValue(QStringLiteral(":lastClosedTime"), entry.lastClosedTime);
            query.bindValue(QStringLiteral(":lastClosedOpenedTime"), entry.lastClosedOpenedTime);
        }
        query.bindValue(QStringLiteral(":currentLocation"), entry.currentLocation);
        query.bindValue(QStringLiteral(":currentProgress"), entry.currentProgress);
        query.bindValue(QStringLiteral(":zoomLevel"), entry.zoomLevel);
        query.bindValue(QStringLiteral(":pageMode"), entry.pageMode);
        query.bindValue(QStringLiteral(":pageWidthDistance"), entry.pageWidthDistance);
        query.bindValue(QStringLiteral(":bookVersion"), entry.bookVersion);
        query.bindValue(QStringLiteral(":thumbnail"), entry.thumbnail);
        query.bindValue(QStringLiteral(":passwordProtected"), entry.passwordProtected);
        query.bindValue(QStringLiteral(":description"), entry.description.join(QLatin1Char('\n')));
        query.bindValue(QStringLiteral(":comment"), entry.comment);
        query.bindValue(QStringLiteral(":tags"), entry.tags.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":rating"), entry.rating);
        query.bindValue(QStringLiteral(":seriesNumbers"), entry.seriesNumbers.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":seriesVolumes"), entry.seriesVolumes.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":subjects"), entry.genres.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":genres"), entry.genres.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":keywords"), entry.keywords.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":characters"), entry.characters.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":locations"), entry.locations);
        query.bindValue(QStringLiteral(":pdfNotInvertedRegions"), entry.pdfNotInvertedRegions);
        query.bindValue(QStringLiteral(":rights"), entry.rights);
        query.bindValue(QStringLiteral(":source"), entry.source);
        query.bindValue(QStringLiteral(":identifier"), entry.identifier);
        query.bindValue(QStringLiteral(":uniqueIdentifier"), entry.uniqueIdentifier);
        query.bindValue(QStringLiteral(":language"), entry.language);
    }
};

BookDatabase::BookDatabase(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    connect(&d->watcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        d->ensureWatching();
        d->databaseChangedTimer.start();
    });
    connect(&d->watcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        d->ensureWatching();
        d->databaseChangedTimer.start();
    });
    connect(&d->databaseChangedTimer, &QTimer::timeout, this, [this]() {
        d->ensureWatching();
        Q_EMIT databaseChanged();
    });
}

BookDatabase::~BookDatabase() = default;

QString BookDatabase::databaseFileName() const
{
    return d->dbfile;
}

bool BookDatabase::combineBookFormats(const QVariantMap &plan)
{
    if (!d->prepareDb())
        return false;
    if (!d->db.transaction()) {
        d->closeDb();
        return false;
    }
    const bool saved = [&] {
        QSqlQuery query(d->db);
        if (!query.exec(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(" FROM books ORDER BY fileName")))
            return false;
        QList<BookEntry> books;
        while (query.next())
            books.append(d->fromSqlQuery(query));
        if (query.lastError().isValid())
            return false;
        query.finish();
        const auto candidates = BookFormats::candidates(books);
        const bool current = std::any_of(candidates.cbegin(), candidates.cend(), [&](const QVariant &value) {
            const auto candidate = value.toMap();
            return candidate.value(QStringLiteral("key")).toString() == plan.value(QStringLiteral("key")).toString()
                && candidate.value(QStringLiteral("files")).toStringList() == plan.value(QStringLiteral("files")).toStringList();
        });
        if (!current)
            return false;
        const QString group = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!query.prepare(QStringLiteral("UPDATE books SET formatGroup=:group WHERE fileName=:file")))
            return false;
        for (const auto &file : plan.value(QStringLiteral("files")).toStringList()) {
            query.bindValue(QStringLiteral(":group"), group);
            query.bindValue(QStringLiteral(":file"), file);
            if (!query.exec() || query.numRowsAffected() != 1)
                return false;
            query.finish();
        }
        return d->db.commit();
    }();
    if (!saved)
        d->db.rollback();
    d->closeDb();
    if (saved)
        Q_EMIT databaseChanged();
    return saved;
}

static QString uuidToDatabaseString(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

static QUuid uuidFromDatabaseString(QString value)
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

static BookState stateFromSqlQuery(const QSqlQuery &query)
{
    BookState state;
    state.bookId = query.value(0).toString();
    state.stateId = uuidFromDatabaseString(query.value(1).toString());
    state.textContentHash = query.value(2).toString().toLatin1();
    state.documentStateHash = query.value(3).toString().toLatin1();
    state.epubFileHash = query.value(4).toString().toLatin1();
    return state;
}

static QString stateSelectFields()
{
    return QStringLiteral("book_id, state_id, text_content_hash, document_state_hash, epub_file_hash");
}

QList<BookEntry> BookDatabase::loadEntries(bool *success)
{
    if (success) {
        *success = false;
    }
    if (!d->prepareDb()) {
        return {};
    }

    QList<BookEntry> entries;
    QSqlQuery allEntries(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(" FROM books"));

    while (allEntries.next()) {
        entries.append(d->fromSqlQuery(allEntries));
    }

    if (success) {
        *success = allEntries.isActive() && !allEntries.lastError().isValid();
    }

    d->closeDb();
    return entries;
}

std::optional<BookEntry> BookDatabase::loadEntry(const QString &fileName)
{
    if (!d->prepareDb()) {
        return std::nullopt;
    }

    std::optional<BookEntry> result;
    QSqlQuery entry;
    entry.prepare(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(" FROM books WHERE fileName = :fileName LIMIT 1"));
    entry.bindValue(QStringLiteral(":fileName"), fileName);
    if (entry.exec() && entry.first()) {
        result = d->fromSqlQuery(entry);
    }

    entry.finish();
    d->closeDb();
    return result;
}

std::optional<BookEntry> BookDatabase::loadEntryByIdentifier(const QString &identifier)
{
    return loadEntryByUniqueIdentifier(identifier);
}

std::optional<BookEntry> BookDatabase::loadEntryByUniqueIdentifier(const QString &uniqueIdentifier)
{
    if (!d->prepareDb()) {
        return std::nullopt;
    }

    std::optional<BookEntry> result;
    QSqlQuery entry;
    entry.prepare(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", "))
                  + QStringLiteral(" FROM books WHERE uniqueIdentifier = :identifier OR identifier = :identifier LIMIT 1"));
    entry.bindValue(QStringLiteral(":identifier"), uniqueIdentifier);
    if (entry.exec() && entry.first()) {
        result = d->fromSqlQuery(entry);
    }

    entry.finish();
    d->closeDb();
    return result;
}

void BookDatabase::addEntry(const BookEntry &entry)
{
    if (!d->prepareDb()) {
        return;
    }
    QSqlQuery existingEntry;
    existingEntry.prepare(QStringLiteral("SELECT 1 FROM books WHERE fileName = :fileName LIMIT 1"));
    existingEntry.bindValue(QStringLiteral(":fileName"), entry.filename);
    if (existingEntry.exec() && existingEntry.first()) {
        existingEntry.finish();
        d->closeDb();
        return;
    }

    qCDebug(ARIANNA_LOG) << "Adding newly discovered book to the database" << entry.filename;

    d->insertEntry(entry);
    d->closeDb();
}

bool BookDatabase::recordReaderClosed(const QString &fileName, const QDateTime &opened, const QDateTime &closed)
{
    if (fileName.isEmpty() || !opened.isValid() || !closed.isValid() || !d->prepareDb()) {
        return false;
    }
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral("UPDATE books SET lastClosedTime=:closed, lastClosedOpenedTime=:opened WHERE fileName=:fileName"));
    query.bindValue(QStringLiteral(":closed"), closed.toUTC());
    query.bindValue(QStringLiteral(":opened"), opened.toUTC());
    query.bindValue(QStringLiteral(":fileName"), fileName);
    const bool success = query.exec();
    const bool updated = success && query.numRowsAffected() == 1;
    if (!success) {
        qCWarning(ARIANNA_LOG) << "Unable to record reader close time" << fileName << query.lastError();
    }
    d->closeDb();
    return updated;
}

void BookDatabase::updateEntry(const BookEntry &entry)
{
    if (!d->prepareDb()) {
        return;
    }

    QStringList assignments;
    for (const QString &fieldName : std::as_const(d->fieldNames)) {
        // Reader lifecycle owns these fields; a stale metadata/model snapshot
        // must not erase a close recorded by another reader/process.
        if (fieldName == QStringLiteral("fileName") || fieldName == QStringLiteral("lastClosedTime") || fieldName == QStringLiteral("lastClosedOpenedTime")) {
            continue;
        }
        assignments.append(QStringLiteral("%1=:%1").arg(fieldName));
    }

    QSqlQuery updateEntry;
    updateEntry.prepare(QStringLiteral("UPDATE books SET ") + assignments.join(QStringLiteral(", ")) + QStringLiteral(" WHERE fileName=:fileName"));
    d->bindEntry(updateEntry, entry, false);
    if (!updateEntry.exec()) {
        qCDebug(ARIANNA_LOG) << updateEntry.lastError();
        qCDebug(ARIANNA_LOG) << "Query failed, string:" << updateEntry.lastQuery();
    }

    d->closeDb();
}

static QString normalizedBookId(QString id)
{
    id = id.trimmed();
    if (id.startsWith(QStringLiteral("urn:uuid:"), Qt::CaseInsensitive)) {
        id = id.mid(9);
    }
    const QUuid uuid(id);
    return uuid.isNull() ? id : uuid.toString(QUuid::WithoutBraces);
}

static QSet<QString> bookDataIdentifiers(const QString &fileName, const QString &uniqueIdentifier, const QString &identifier)
{
    // Personal PDF notes can use the filename before a book is imported.
    QStringList identifiers{fileName, uniqueIdentifier, identifier};
    identifiers.append(identifier.split(QLatin1Char(','), Qt::SkipEmptyParts));
    QSet<QString> result;
    for (const QString &value : std::as_const(identifiers)) {
        const QString id = normalizedBookId(value);
        if (!id.isEmpty()) {
            result.insert(id);
        }
    }
    return result;
}

static bool identifiersCanBeReplaced(const QSqlDatabase &db, const QStringList &bookIds, const QString &fileName)
{
    QSet<QString> ids;
    for (const auto &id : bookIds)
        if (!normalizedBookId(id).isEmpty())
            ids.insert(normalizedBookId(id));
    if (ids.isEmpty())
        return false;
    if (QMimeDatabase().mimeTypeForFile(fileName).name() == QStringLiteral("application/epub+zip")) {
        QVariantList notes, references;
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT bookId, anchorId, cfiRange, format FROM annotations")))
            return false;
        while (query.next()) {
            if (ids.contains(normalizedBookId(query.value(0).toString())))
                notes.append(QVariantMap{{QStringLiteral("bookId"), query.value(0)},
                                         {QStringLiteral("anchorId"), query.value(1)},
                                         {QStringLiteral("cfiRange"), query.value(2)},
                                         {QStringLiteral("format"), query.value(3)}});
        }
        if (query.lastError().isValid())
            return false;
        query.finish();
        if (!query.exec(QStringLiteral("SELECT sourceBookId, sourceAnchorId, targetBookId, targetLocation FROM \"references\"")))
            return false;
        while (query.next()) {
            if (ids.contains(normalizedBookId(query.value(0).toString())) || ids.contains(normalizedBookId(query.value(2).toString())))
                references.append(QVariantMap{{QStringLiteral("sourceBookId"), query.value(0)},
                                              {QStringLiteral("sourceAnchorId"), query.value(1)},
                                              {QStringLiteral("targetBookId"), query.value(2)},
                                              {QStringLiteral("targetLocation"), query.value(3)}});
        }
        if (query.lastError().isValid())
            return false;
        query.finish();
        return BookReferenceCheck::canReplaceEpubIdentifiers(fileName, bookIds, notes, references);
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT bookId FROM annotations UNION SELECT sourceBookId FROM \"references\" "
                                   "UNION SELECT targetBookId FROM \"references\"")))
        return false;
    while (query.next())
        if (ids.contains(normalizedBookId(query.value(0).toString())))
            return false;
    return !query.lastError().isValid();
}

bool BookDatabase::canChangeBookIdentifiers(const QStringList &bookIds, const QString &fileName)
{
    if (!d->prepareDb())
        return false;
    const bool allowed = identifiersCanBeReplaced(d->db, bookIds, fileName);
    d->closeDb();
    return allowed;
}

QVariantMap BookDatabase::assignNewUniqueIdentifier(const QString &fileName, const QString &expectedIdentifier)
{
    const auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (fileName.isEmpty() || !d->prepareDb())
        return failure(i18n("Unable to open the library database."));
    if (!d->db.transaction()) {
        d->closeDb();
        return failure(i18n("Unable to start a database transaction."));
    }

    const auto result = [&]() -> QVariantMap {
        QSqlQuery selected(d->db);
        selected.prepare(QStringLiteral("SELECT uniqueIdentifier, identifier FROM books WHERE fileName=:fileName"));
        selected.bindValue(QStringLiteral(":fileName"), fileName);
        if (!selected.exec() || !selected.next())
            return failure(i18n("The book entry changed or is no longer available. Run the check again."));
        const QString uniqueIdentifier = selected.value(0).toString();
        const QString identifier = selected.value(1).toString();
        selected.finish();
        const QString oldId = normalizedBookId(expectedIdentifier);
        QHash<QString, QSet<QString>> identities;
        QSqlQuery books(d->db);
        if (!books.exec(QStringLiteral("SELECT fileName, uniqueIdentifier, identifier FROM books")))
            return failure(i18n("Unable to verify the current book identifiers."));
        while (books.next()) {
            const QString path = books.value(0).toString();
            for (const auto &id : bookDataIdentifiers(path, books.value(1).toString(), books.value(2).toString()))
                identities[id].insert(path);
        }
        if (books.lastError().isValid())
            return failure(i18n("Unable to verify the current book identifiers."));
        books.finish();

        if (oldId.isEmpty()) {
            if (!uniqueIdentifier.trimmed().isEmpty())
                return failure(i18n("The book already has a unique identifier. Run the check again."));
        } else if (!bookDataIdentifiers(fileName, uniqueIdentifier, identifier).contains(oldId)
                   || identities.value(oldId).size() < 2 || normalizedBookId(fileName) == oldId) {
            return failure(i18n("The identifier is no longer ambiguous for this book. Run the check again."));
        }

        QString newId;
        do {
            newId = QUuid::createUuidV7().toString(QUuid::WithoutBraces);
        } while (identities.contains(newId));

        QStringList identifierParts;
        for (const auto &part : identifier.split(QLatin1Char(','), Qt::KeepEmptyParts)) {
            if (oldId.isEmpty() || normalizedBookId(part) != oldId)
                identifierParts.append(part);
        }
        QSqlQuery update(d->db);
        update.prepare(QStringLiteral("UPDATE books SET uniqueIdentifier=:newId, identifier=:identifier WHERE fileName=:fileName"));
        update.bindValue(QStringLiteral(":newId"), newId);
        update.bindValue(QStringLiteral(":identifier"), identifierParts.join(QLatin1Char(',')));
        update.bindValue(QStringLiteral(":fileName"), fileName);
        if (!update.exec() || update.numRowsAffected() != 1)
            return failure(i18n("Unable to update the book identifier."));
        return {{QStringLiteral("success"), true}, {QStringLiteral("newIdentifier"), newId}};
    }();

    const bool success = result.value(QStringLiteral("success")).toBool() && d->db.commit();
    if (!success)
        d->db.rollback();
    d->closeDb();
    if (!success)
        return failure(i18n("Unable to save the new book identifier."));
    Q_EMIT databaseChanged();
    return result;
}

QVariantMap BookDatabase::deleteOrphanedBookStateHistory(const QString &bookId, const QString &stateId)
{
    const auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (bookId.trimmed().isEmpty() || stateId.trimmed().isEmpty() || !d->prepareDb())
        return failure(i18n("The saved book state has changed. Run the check again."));
    if (!d->db.transaction()) {
        d->closeDb();
        return failure(i18n("Unable to start a database transaction."));
    }

    bool idFound = false;
    QSqlQuery books(d->db);
    if (!books.exec(QStringLiteral("SELECT fileName, uniqueIdentifier, identifier FROM books"))) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("Unable to verify whether the book ID is still missing."));
    }
    while (books.next()) {
        if (bookDataIdentifiers(books.value(0).toString(), books.value(1).toString(), books.value(2).toString()).contains(normalizedBookId(bookId))) {
            idFound = true;
            break;
        }
    }
    if (books.lastError().isValid()) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("Unable to verify whether the book ID is still missing."));
    }
    books.finish();
    if (idFound) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("The book ID now matches a library book. Run the check again."));
    }

    QSqlQuery remove(d->db);
    remove.prepare(QStringLiteral("DELETE FROM book_state_history WHERE book_id=:bookId AND state_id=:stateId"));
    remove.bindValue(QStringLiteral(":bookId"), bookId);
    remove.bindValue(QStringLiteral(":stateId"), stateId);
    const bool deleted = remove.exec() && remove.numRowsAffected() == 1;
    if (!deleted || !d->db.commit()) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("The saved book state changed or could not be deleted. Run the check again."));
    }
    d->closeDb();
    Q_EMIT databaseChanged();
    return {{QStringLiteral("success"), true}, {QStringLiteral("message"), i18n("Orphaned saved-state history entry deleted.")}};
}

QVariantMap BookDatabase::deleteOrphanedBookCurrentState(const QString &bookId, const QString &stateId)
{
    auto result = deleteOrphanedBookStateEntries({QVariantMap{{QStringLiteral("tableName"), QStringLiteral("book_current_state")},
                                                              {QStringLiteral("bookId"), bookId},
                                                              {QStringLiteral("stateId"), stateId}}});
    if (result.value(QStringLiteral("success")).toBool() && result.value(QStringLiteral("deletedCount")).toInt() == 1)
        result.insert(QStringLiteral("message"), i18n("Orphaned current book state deleted."));
    else if (result.value(QStringLiteral("success")).toBool())
        return {{QStringLiteral("success"), false}, {QStringLiteral("message"), i18n("The current book state changed. Run the check again.")}};
    return result;
}

QVariantMap BookDatabase::deleteOrphanedBookStateHistoryEntries(const QVariantList &entries)
{
    QVariantList historyEntries;
    for (const auto &value : entries) {
        auto entry = value.toMap();
        entry.insert(QStringLiteral("tableName"), QStringLiteral("book_state_history"));
        historyEntries.append(entry);
    }
    return deleteOrphanedBookStateEntries(historyEntries);
}

QVariantMap BookDatabase::deleteOrphanedBookStateEntries(const QVariantList &entries)
{
    const auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (entries.isEmpty() || !d->prepareDb())
        return failure(i18n("There are no orphaned saved-state history entries to delete."));
    if (!d->db.transaction()) {
        d->closeDb();
        return failure(i18n("Unable to start a database transaction."));
    }

    QSet<QString> knownIds;
    QSqlQuery books(d->db);
    if (!books.exec(QStringLiteral("SELECT fileName, uniqueIdentifier, identifier FROM books"))) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("Unable to verify whether the book IDs are still missing."));
    }
    while (books.next()) {
        knownIds.unite(bookDataIdentifiers(books.value(0).toString(), books.value(1).toString(), books.value(2).toString()));
    }
    if (books.lastError().isValid()) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("Unable to verify whether the book IDs are still missing."));
    }
    books.finish();

    QSet<QString> seen;
    int deletedCount = 0;
    for (const auto &value : entries) {
        const auto entry = value.toMap();
        const QString tableName = entry.value(QStringLiteral("tableName")).toString();
        const QString bookId = entry.value(QStringLiteral("bookId")).toString();
        const QString stateId = entry.value(QStringLiteral("stateId")).toString();
        if ((tableName != QStringLiteral("book_state_history") && tableName != QStringLiteral("book_current_state"))
            || bookId.trimmed().isEmpty() || stateId.trimmed().isEmpty() || knownIds.contains(normalizedBookId(bookId))) {
            continue;
        }
        const QString key = tableName + QLatin1Char(':') + QString::number(bookId.size()) + QLatin1Char(':') + bookId + stateId;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        QSqlQuery remove(d->db);
        remove.prepare(tableName == QStringLiteral("book_current_state")
                          ? QStringLiteral("DELETE FROM book_current_state WHERE book_id=:bookId AND state_id=:stateId")
                          : QStringLiteral("DELETE FROM book_state_history WHERE book_id=:bookId AND state_id=:stateId"));
        remove.bindValue(QStringLiteral(":bookId"), bookId);
        remove.bindValue(QStringLiteral(":stateId"), stateId);
        if (!remove.exec()) {
            d->db.rollback();
            d->closeDb();
            return failure(i18n("A saved-state history entry could not be deleted."));
        }
        if (remove.numRowsAffected() == 1)
            ++deletedCount;
        else if (remove.numRowsAffected() > 1) {
            d->db.rollback();
            d->closeDb();
            return failure(i18n("A saved-state history entry changed unexpectedly. Run the check again."));
        }
        remove.finish();
    }
    if (!d->db.commit()) {
        d->db.rollback();
        d->closeDb();
        return failure(i18n("Unable to save the saved-state history cleanup."));
    }
    d->closeDb();
    if (deletedCount > 0)
        Q_EMIT databaseChanged();
    return {{QStringLiteral("success"), true},
            {QStringLiteral("deletedCount"), deletedCount}};
}

QVariantMap BookDatabase::importEntry(const BookEntry &entry, const QStringList &replacedBookIds)
{
    const QVariantMap failure{{QStringLiteral("success"), false},
                              {QStringLiteral("status"), QStringLiteral("failed")},
                              {QStringLiteral("fileName"), entry.filename},
                              {QStringLiteral("message"), i18n("Unable to import %1 into the library database.", entry.filename)}};
    if (entry.filename.isEmpty() || !d->prepareDb())
        return failure;

    const auto result = [&]() -> QVariantMap {
        QSqlQuery transaction(d->db);
        if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE")))
            return failure;
        transaction.finish();
        if (!replacedBookIds.isEmpty() && !identifiersCanBeReplaced(d->db, replacedBookIds, entry.filename))
            return {{QStringLiteral("success"), false},
                    {QStringLiteral("status"), QStringLiteral("identifier-in-use")},
                    {QStringLiteral("canChangeIdentifier"), false},
                    {QStringLiteral("message"),
                     i18n("The book identifier cannot be changed because stored remarks or cross-references are anchored in this file or their positions could "
                          "not be verified.")}};

        QStringList ids = bookDataIdentifiers(entry.filename, entry.uniqueIdentifier, entry.identifier).values();
        ids.sort();
        const auto primaryId = normalizedBookId(entry.uniqueIdentifier);
        if (!primaryId.isEmpty()) {
            ids.removeAll(primaryId);
            ids.prepend(primaryId);
        }
        QSqlQuery books(d->db);
        books.setForwardOnly(true);
        // Reopening a registered file must remain possible even in a legacy library with duplicate IDs.
        books.prepare(QStringLiteral("SELECT fileName, uniqueIdentifier, identifier FROM books ORDER BY fileName=:fileName DESC, fileName"));
        books.bindValue(QStringLiteral(":fileName"), entry.filename);
        if (!books.exec())
            return failure;
        QMap<QString, QString> conflicts;
        while (books.next()) {
            const QString path = books.value(0).toString();
            if (path == entry.filename)
                return {{QStringLiteral("success"), true},
                        {QStringLiteral("status"), QStringLiteral("already-known")},
                        {QStringLiteral("fileName"), entry.filename}};
            const auto existingIds = bookDataIdentifiers(path, books.value(1).toString(), books.value(2).toString());
            for (const auto &id : ids) {
                if (existingIds.contains(id) && !conflicts.contains(id))
                    conflicts.insert(id, path);
            }
        }
        if (books.lastError().isValid())
            return failure;
        books.finish();
        if (!conflicts.isEmpty()) {
            const auto conflictIds = conflicts.keys();
            const auto id = conflicts.contains(primaryId) ? primaryId : conflictIds.first();
            const auto path = conflicts.value(id);
            return {{QStringLiteral("success"), false},
                    {QStringLiteral("status"), QStringLiteral("duplicate-book-id")},
                    {QStringLiteral("fileName"), entry.filename},
                    {QStringLiteral("bookId"), id},
                    {QStringLiteral("conflictingBookIds"), conflictIds},
                    {QStringLiteral("canChangeIdentifier"), identifiersCanBeReplaced(d->db, conflictIds, entry.filename)},
                    {QStringLiteral("existingFile"), path},
                    {QStringLiteral("message"), i18n("Import rejected: %1\nBook ID %2 is already used by:\n%3", entry.filename, id, path)}};
        }
        if (!d->insertEntry(entry) || !d->db.commit())
            return failure;
        return {{QStringLiteral("success"), true}, {QStringLiteral("status"), QStringLiteral("imported")}, {QStringLiteral("fileName"), entry.filename}};
    }();

    const bool imported = result.value(QStringLiteral("status")) == QStringLiteral("imported");
    if (!imported)
        d->db.rollback();
    d->closeDb();
    if (imported)
        Q_EMIT databaseChanged();
    return result;
}

QSet<QString> BookDatabase::identifiersForRemoval(const QString &fileName, const QList<BookEntry> &entries)
{
    QSet<QString> removedIds;
    QSet<QString> retainedIds;
    for (const auto &entry : entries) {
        const auto ids = bookDataIdentifiers(entry.filename, entry.uniqueIdentifier, entry.identifier);
        if (entry.filename == fileName) {
            removedIds = ids;
        } else {
            retainedIds.unite(ids);
        }
    }
    return removedIds.subtract(retainedIds);
}

static bool writeBookState(const BookState &state);

static QVariantList readAllReferences(bool *success)
{
    QVariantList references;
    QSqlQuery query;
    const bool ok = query.exec(QStringLiteral("SELECT sourceBookId, sourceAnchorId, sourceAnchorTitle, targetBookId, targetLocation "
                                             "FROM \"references\" ORDER BY sourceBookId, sourceAnchorId"));
    if (ok) {
        while (query.next()) {
            QVariantMap reference;
            for (int i = 0; i < query.record().count(); ++i) {
                reference.insert(query.record().fieldName(i), query.value(i));
            }
            references.append(reference);
        }
    }
    if (success) {
        *success = ok && !query.lastError().isValid();
    }
    return references;
}

bool BookDatabase::removeEntry(const BookEntry &entry, const QList<BookState> &states, const QVariantList *expectedReferences)
{
    if (entry.filename.isEmpty() || !d->prepareDb()) {
        return false;
    }
    if (!d->db.transaction()) {
        qCWarning(ARIANNA_LOG) << "Unable to start book removal" << d->db.lastError();
        d->closeDb();
        return false;
    }

    const bool removed = [&]() {
        if (expectedReferences) {
            bool loaded = false;
            const auto references = readAllReferences(&loaded);
            if (!loaded || references != *expectedReferences) {
                qCWarning(ARIANNA_LOG) << "References changed while preparing book removal";
                return false;
            }
        }
        // Resolve identities from the stored entry: callers can have incomplete
        // or stale metadata. Retain shared IDs while another copy remains.
        QSqlQuery books(d->db);
        if (!books.exec(QStringLiteral("SELECT fileName, uniqueIdentifier, identifier FROM books"))) {
            qCWarning(ARIANNA_LOG) << "Unable to read book identities for removal" << books.lastError();
            return false;
        }
        QList<BookEntry> entries;
        bool found = false;
        while (books.next()) {
            BookEntry stored;
            stored.filename = books.value(0).toString();
            stored.uniqueIdentifier = books.value(1).toString();
            stored.identifier = books.value(2).toString();
            found |= stored.filename == entry.filename;
            entries.append(stored);
        }
        if (books.lastError().isValid() || !found) {
            qCWarning(ARIANNA_LOG) << "Unable to find the stored book for removal" << entry.filename << books.lastError();
            return false;
        }
        books.finish();
        const auto removedIds = identifiersForRemoval(entry.filename, entries);

        // Read the original keys so that aliases and all UUID spellings can be
        // matched without rewriting IDs in unrelated annotations/references.
        QSqlQuery identifiers(d->db);
        if (!identifiers.exec(QStringLiteral("SELECT bookId FROM annotations "
                                             "UNION SELECT sourceBookId FROM \"references\" "
                                             "UNION SELECT targetBookId FROM \"references\" "
                                             "UNION SELECT book_id FROM book_current_state "
                                             "UNION SELECT book_id FROM book_state_history"))) {
            qCWarning(ARIANNA_LOG) << "Unable to read related book data" << identifiers.lastError();
            return false;
        }
        QStringList storedIds;
        while (identifiers.next()) {
            const QString id = identifiers.value(0).toString();
            if (removedIds.contains(normalizedBookId(id))) {
                storedIds.append(id);
            }
        }
        if (identifiers.lastError().isValid()) {
            qCWarning(ARIANNA_LOG) << "Unable to read related book data" << identifiers.lastError();
            return false;
        }
        identifiers.finish();

        const QStringList deletions{
            QStringLiteral("DELETE FROM annotations WHERE bookId=:bookId"),
            QStringLiteral("DELETE FROM \"references\" WHERE sourceBookId=:bookId OR targetBookId=:bookId"),
            QStringLiteral("DELETE FROM book_current_state WHERE book_id=:bookId"),
            QStringLiteral("DELETE FROM book_state_history WHERE book_id=:bookId"),
        };
        for (const QString &sql : deletions) {
            QSqlQuery query(d->db);
            if (!query.prepare(sql)) {
                qCWarning(ARIANNA_LOG) << "Unable to prepare related book data removal" << query.lastError();
                return false;
            }
            for (const QString &id : std::as_const(storedIds)) {
                query.bindValue(QStringLiteral(":bookId"), id);
                if (!query.exec()) {
                    qCWarning(ARIANNA_LOG) << "Unable to remove related book data" << query.lastError();
                    return false;
                }
                query.finish();
            }
        }

        for (const auto &state : states) {
            if (!writeBookState(state)) {
                return false;
            }
        }

        QSqlQuery removeBook(d->db);
        removeBook.prepare(QStringLiteral("DELETE FROM books WHERE fileName=:fileName"));
        removeBook.bindValue(QStringLiteral(":fileName"), entry.filename);
        if (!removeBook.exec() || removeBook.numRowsAffected() != 1) {
            qCWarning(ARIANNA_LOG) << "Unable to remove book" << entry.filename << removeBook.lastError();
            return false;
        }
        removeBook.finish();
        if (!d->db.commit()) {
            qCWarning(ARIANNA_LOG) << "Unable to commit book removal" << d->db.lastError();
            return false;
        }
        return true;
    }();

    if (!removed) {
        d->db.rollback();
    }
    d->closeDb();
    if (removed) {
        Q_EMIT databaseChanged();
    }
    return removed;
}

void BookDatabase::updateEntry(const QString &fileName, const QString &property, const QVariant &value)
{
    if (!d->prepareDb()) {
        return;
    }
    // qCDebug(QTQUICK_LOG) << "Updating book in the database" << fileName << property << value;

    if (!d->fieldNames.contains(property)) {
        d->closeDb();
        return;
    }

    const QStringList stringListValues{
        QStringLiteral("series"),
        QStringLiteral("author"),
        QStringLiteral("characters"),
        QStringLiteral("subjects"),
        QStringLiteral("genres"),
        QStringLiteral("keywords"),
        QStringLiteral("tags"),
    };

    QString val;
    bool useSerializedValue = false;
    if (stringListValues.contains(property)) {
        if (value.metaType() == QMetaType::fromType<QStringList>()) {
            val = value.toStringList().join(QLatin1Char(','));
            useSerializedValue = true;
        }
    } else if (property == QStringLiteral("description")) {
        if (value.metaType() == QMetaType::fromType<QStringList>()) {
            val = value.toStringList().join(QLatin1Char('\n'));
            useSerializedValue = true;
        }
    }

    QSqlQuery updateEntry;
    if (property == QStringLiteral("subjects") && d->fieldNames.contains(QStringLiteral("genres"))) {
        updateEntry.prepare(QStringLiteral("UPDATE books SET subjects=:value, genres=:value WHERE fileName=:filename"));
    } else if (property == QStringLiteral("genres") && d->fieldNames.contains(QStringLiteral("subjects"))) {
        updateEntry.prepare(QStringLiteral("UPDATE books SET genres=:value, subjects=:value WHERE fileName=:filename"));
    } else {
        updateEntry.prepare(QStringLiteral("UPDATE books SET %1=:value WHERE fileName=:filename ").arg(property));
    }
    updateEntry.bindValue(QStringLiteral(":value"), useSerializedValue ? QVariant(val) : value);
    updateEntry.bindValue(QStringLiteral(":filename"), fileName);
    if (!updateEntry.exec()) {
        qCDebug(ARIANNA_LOG) << updateEntry.lastError();
        qCDebug(ARIANNA_LOG) << "Query failed, string:" << updateEntry.lastQuery();
        qCDebug(ARIANNA_LOG) << updateEntry.boundValue(QStringLiteral(":value"));
        qCDebug(ARIANNA_LOG) << updateEntry.boundValue(QStringLiteral(":filename"));
        qCDebug(ARIANNA_LOG) << d->db.lastError();
    }

    d->closeDb();
}

std::optional<BookState> BookDatabase::currentBookState(const QString &bookId)
{
    if (bookId.isEmpty() || !d->prepareDb()) {
        return std::nullopt;
    }

    std::optional<BookState> result;
    QSqlQuery query;
    query.prepare(QStringLiteral("SELECT ") + stateSelectFields() + QStringLiteral(" FROM book_current_state WHERE book_id=:bookId LIMIT 1"));
    query.bindValue(QStringLiteral(":bookId"), bookId);
    if (query.exec() && query.first()) {
        result = stateFromSqlQuery(query);
    } else if (query.lastError().isValid()) {
        qCDebug(ARIANNA_LOG) << query.lastError();
    }

    query.finish();
    d->closeDb();
    return result;
}

static bool writeBookState(const BookState &state)
{
    if (state.stateId.isNull() || state.bookId.isEmpty() || state.textContentHash.isEmpty() || state.documentStateHash.isEmpty()
        || state.epubFileHash.isEmpty()) {
        return false;
    }

    QSqlQuery query;
    query.prepare(
        QStringLiteral("INSERT INTO book_current_state("
                       "book_id, state_id, text_content_hash, document_state_hash, epub_file_hash, modified) "
                       "VALUES(:bookId, :stateId, :textContentHash, :documentStateHash, :epubFileHash, :modified) "
                       "ON CONFLICT(book_id) DO UPDATE SET "
                       "state_id=excluded.state_id, "
                       "text_content_hash=excluded.text_content_hash, "
                       "document_state_hash=excluded.document_state_hash, "
                       "epub_file_hash=excluded.epub_file_hash, "
                       "modified=excluded.modified"));
    query.bindValue(QStringLiteral(":bookId"), state.bookId);
    query.bindValue(QStringLiteral(":stateId"), uuidToDatabaseString(state.stateId));
    query.bindValue(QStringLiteral(":textContentHash"), QString::fromLatin1(state.textContentHash));
    query.bindValue(QStringLiteral(":documentStateHash"), QString::fromLatin1(state.documentStateHash));
    query.bindValue(QStringLiteral(":epubFileHash"), QString::fromLatin1(state.epubFileHash));
    query.bindValue(QStringLiteral(":modified"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    const bool saved = query.exec();
    if (!saved) {
        qCDebug(ARIANNA_LOG) << query.lastError();
    }

    query.finish();
    if (!saved) return false;
    QSqlQuery history;
    history.prepare(QStringLiteral("INSERT OR IGNORE INTO book_state_history SELECT book_id,state_id,epub_file_hash,modified "
                                   "FROM book_current_state WHERE book_id=:bookId"));
    history.bindValue(QStringLiteral(":bookId"), state.bookId);
    return history.exec();
}

bool BookDatabase::saveBookState(const BookState &state)
{
    if (!d->prepareDb()) {
        return false;
    }
    const bool saved = d->db.transaction() && writeBookState(state) && d->db.commit();
    if (!saved) d->db.rollback();
    d->closeDb();
    return saved;
}

QVariantList BookDatabase::loadAnnotations(const QString &bookId, bool *success)
{
    if (success)
        *success = false;
    if (!d->prepareDb()) {
        return {};
    }

    QVariantList annotations;
    QSqlQuery annotationQuery;
    annotationQuery.prepare(
        QStringLiteral("SELECT cfiRange, color, text, note, format, location, created, modified, annotationId, anchorId, noteHtml, noteSource, noteMetadata "
                       "FROM annotations WHERE bookId=:bookId "
                       "ORDER BY created ASC, rowid ASC"));
    annotationQuery.bindValue(QStringLiteral(":bookId"), bookId);
    if (annotationQuery.exec()) {
        while (annotationQuery.next()) {
            QVariantMap annotation;
            const QString cfiRange = annotationQuery.value(0).toString();
            annotation.insert(QStringLiteral("cfiRange"), cfiRange);
            annotation.insert(QStringLiteral("value"), cfiRange);
            annotation.insert(QStringLiteral("color"), annotationQuery.value(1).toString());
            annotation.insert(QStringLiteral("text"), annotationQuery.value(2).toString());
            annotation.insert(QStringLiteral("note"), annotationQuery.value(3).toString());
            annotation.insert(QStringLiteral("format"), annotationQuery.value(4).toString());
            const QString location = annotationQuery.value(5).toString();
            const QJsonDocument locationDocument = QJsonDocument::fromJson(location.toUtf8());
            annotation.insert(QStringLiteral("location"), locationDocument.isNull() ? QVariant(location) : locationDocument.toVariant());
            annotation.insert(QStringLiteral("created"), annotationQuery.value(6).toString());
            annotation.insert(QStringLiteral("modified"), annotationQuery.value(7).toString());
            annotation.insert(QStringLiteral("annotationId"), annotationQuery.value(8).toString());
            annotation.insert(QStringLiteral("anchorId"), annotationQuery.value(9).toString());
            annotation.insert(QStringLiteral("noteHtml"), annotationQuery.value(10).toString());
            annotation.insert(QStringLiteral("noteSource"), annotationQuery.value(11).toString());
            annotation.insert(QStringLiteral("noteMetadata"), QJsonDocument::fromJson(annotationQuery.value(12).toByteArray()).toVariant());
            annotation.insert(QStringLiteral("invalid"), false);
            annotations.append(annotation);
        }
    } else {
        qCDebug(ARIANNA_LOG) << annotationQuery.lastError();
    }

    if (success)
        *success = !annotationQuery.lastError().isValid();
    annotationQuery.finish();
    d->closeDb();
    return annotations;
}

bool BookDatabase::saveAnnotation(const QString &bookId, const QVariantMap &annotation)
{
    const QString cfiRange = annotation.value(QStringLiteral("cfiRange")).toString().isEmpty() ? annotation.value(QStringLiteral("value")).toString()
                                                                                               : annotation.value(QStringLiteral("cfiRange")).toString();
    QString annotationId = annotation.value(QStringLiteral("annotationId")).toString().trimmed();
    const QString anchorId = annotation.value(QStringLiteral("anchorId")).toString().trimmed();
    const QVariant locationValue = annotation.value(QStringLiteral("location"));
    const QString location = locationValue.metaType().id() == QMetaType::QString
        ? locationValue.toString()
        : QString::fromUtf8(QJsonDocument::fromVariant(locationValue).toJson(QJsonDocument::Compact));
    if (annotationId.isEmpty()) {
        annotationId = anchorId.isEmpty() ? QUuid::createUuidV7().toString(QUuid::WithoutBraces) : anchorId;
    }
    if (bookId.isEmpty() || annotationId.isEmpty() || (cfiRange.isEmpty() && anchorId.isEmpty() && location.isEmpty())) {
        return false;
    }

    if (!d->prepareDb()) {
        return false;
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString created = annotation.value(QStringLiteral("created")).toString().isEmpty() ? now : annotation.value(QStringLiteral("created")).toString();
    const QString modified = annotation.value(QStringLiteral("modified")).toString();

    QSqlQuery annotationQuery;
    annotationQuery.prepare(
        QStringLiteral("INSERT INTO annotations(bookId, annotationId, cfiRange, anchorId, color, text, note, format, location, created, modified, noteHtml, "
                       "noteSource, noteMetadata) "
                       "VALUES(:bookId, :annotationId, :cfiRange, :anchorId, :color, :text, :note, :format, :location, :created, :modified, :noteHtml, "
                       ":noteSource, :noteMetadata) "
                       "ON CONFLICT(bookId, annotationId) DO UPDATE SET "
                       "cfiRange=CASE WHEN excluded.cfiRange IS NOT NULL AND excluded.cfiRange != '' "
                       "THEN excluded.cfiRange ELSE annotations.cfiRange END, "
                       "anchorId=CASE WHEN excluded.anchorId IS NOT NULL AND excluded.anchorId != '' "
                       "THEN excluded.anchorId ELSE annotations.anchorId END, "
                       "color=excluded.color, "
                       "text=excluded.text, "
                       "note=excluded.note, "
                       "format=excluded.format, "
                       "location=excluded.location, "
                       "modified=excluded.modified, noteHtml=excluded.noteHtml, noteSource=excluded.noteSource, noteMetadata=excluded.noteMetadata"));
    annotationQuery.bindValue(QStringLiteral(":bookId"), bookId);
    annotationQuery.bindValue(QStringLiteral(":annotationId"), annotationId);
    annotationQuery.bindValue(QStringLiteral(":cfiRange"), cfiRange);
    annotationQuery.bindValue(QStringLiteral(":anchorId"), anchorId);
    annotationQuery.bindValue(QStringLiteral(":color"), annotation.value(QStringLiteral("color"), QStringLiteral("yellow")).toString());
    annotationQuery.bindValue(QStringLiteral(":text"), annotation.value(QStringLiteral("text")).toString());
    annotationQuery.bindValue(QStringLiteral(":note"), annotation.value(QStringLiteral("note")).toString());
    annotationQuery.bindValue(QStringLiteral(":format"), annotation.value(QStringLiteral("format")).toString());
    annotationQuery.bindValue(QStringLiteral(":location"), location);
    annotationQuery.bindValue(QStringLiteral(":created"), created);
    annotationQuery.bindValue(QStringLiteral(":modified"), modified);

    annotationQuery.bindValue(QStringLiteral(":noteHtml"), annotation.value(QStringLiteral("noteHtml")).toString());
    annotationQuery.bindValue(QStringLiteral(":noteSource"), annotation.value(QStringLiteral("noteSource")).toString());
    annotationQuery.bindValue(QStringLiteral(":noteMetadata"),
                              QString::fromUtf8(QJsonDocument::fromVariant(annotation.value(QStringLiteral("noteMetadata"))).toJson(QJsonDocument::Compact)));
    const bool saved = annotationQuery.exec();
    if (!saved) {
        qCDebug(ARIANNA_LOG) << annotationQuery.lastError();
        qCDebug(ARIANNA_LOG) << "Query failed, string:" << annotationQuery.lastQuery();
    }

    d->closeDb();
    return saved;
}

bool BookDatabase::replaceAnnotationAnchor(const QString &bookId, const QString &annotationId, const QString &expectedAnchorId,
                                          const QString &expectedText, const QString &expectedCfiRange, const QString &newAnchorId)
{
    if (bookId.isEmpty() || annotationId.isEmpty() || !d->prepareDb())
        return false;
    QSqlQuery query;
    query.prepare(QStringLiteral("UPDATE annotations SET anchorId=:newAnchorId "
                                 "WHERE bookId=:bookId AND annotationId=:annotationId "
                                 "AND COALESCE(anchorId, '')=COALESCE(:oldAnchorId, '') AND COALESCE(text, '')=:text "
                                 "AND COALESCE(cfiRange, '')=COALESCE(:cfiRange, '')"));
    query.bindValue(QStringLiteral(":newAnchorId"), newAnchorId);
    query.bindValue(QStringLiteral(":bookId"), bookId);
    query.bindValue(QStringLiteral(":annotationId"), annotationId);
    query.bindValue(QStringLiteral(":oldAnchorId"), expectedAnchorId);
    query.bindValue(QStringLiteral(":text"), expectedText);
    query.bindValue(QStringLiteral(":cfiRange"), expectedCfiRange);
    const bool changed = query.exec() && query.numRowsAffected() == 1;
    if (!changed)
        qCDebug(ARIANNA_LOG) << "Unable to replace remark anchor:" << query.lastError();
    query.finish();
    d->closeDb();
    return changed;
}

void BookDatabase::removeAnnotation(const QString &bookId, const QString &annotationKey)
{
    if (bookId.isEmpty() || annotationKey.isEmpty()) {
        return;
    }

    if (!d->prepareDb()) {
        return;
    }

    QSqlQuery annotationQuery;
    annotationQuery.prepare(QStringLiteral("DELETE FROM annotations WHERE bookId=:bookId AND (annotationId=:annotationKey OR cfiRange=:annotationKey)"));
    annotationQuery.bindValue(QStringLiteral(":bookId"), bookId);
    annotationQuery.bindValue(QStringLiteral(":annotationKey"), annotationKey);
    if (!annotationQuery.exec()) {
        qCDebug(ARIANNA_LOG) << annotationQuery.lastError();
    }

    d->closeDb();
}

QVariantList BookDatabase::loadReferences(const QString &sourceBookId)
{
    if (!d->prepareDb()) {
        return {};
    }

    QVariantList references;
    QSqlQuery referenceQuery;
    referenceQuery.prepare(
        QStringLiteral("SELECT sourceBookId, sourceAnchorId, "
                       "sourceAnchorTitle, targetBookId, targetLocation, targetPreviewHtml, created, modified "
                       "FROM \"references\" "
                       "WHERE sourceBookId=:sourceBookId "
                       "ORDER BY created ASC, rowid ASC"));
    referenceQuery.bindValue(QStringLiteral(":sourceBookId"), sourceBookId);

    if (referenceQuery.exec()) {
        while (referenceQuery.next()) {
            QVariantMap reference;
            reference.insert(QStringLiteral("sourceBookId"), referenceQuery.value(QStringLiteral("sourceBookId")).toString());
            reference.insert(QStringLiteral("sourceAnchorId"), referenceQuery.value(QStringLiteral("sourceAnchorId")).toString());
            reference.insert(QStringLiteral("sourceAnchorTitle"), referenceQuery.value(QStringLiteral("sourceAnchorTitle")).toString());
            reference.insert(QStringLiteral("targetBookId"), referenceQuery.value(QStringLiteral("targetBookId")).toString());
            reference.insert(QStringLiteral("targetLocation"), referenceQuery.value(QStringLiteral("targetLocation")).toString());
            reference.insert(QStringLiteral("targetPreviewHtml"), referenceQuery.value(QStringLiteral("targetPreviewHtml")).toString());
            reference.insert(QStringLiteral("invalid"), false);
            reference.insert(QStringLiteral("created"), referenceQuery.value(QStringLiteral("created")).toString());
            reference.insert(QStringLiteral("modified"), referenceQuery.value(QStringLiteral("modified")).toString());
            references.append(reference);
        }
    } else {
        qCDebug(ARIANNA_LOG) << referenceQuery.lastError();
    }

    d->closeDb();
    return references;
}

QVariantMap BookDatabase::loadReferenceBySource(const QString &sourceBookId, const QString &sourceAnchorId)
{
    if (!d->prepareDb()) {
        return {};
    }

    QVariantMap reference;
    QSqlQuery referenceQuery;
    referenceQuery.prepare(
        QStringLiteral("SELECT sourceBookId, sourceAnchorId, "
                       "sourceAnchorTitle, targetBookId, targetLocation, targetPreviewHtml, created, modified "
                       "FROM \"references\" "
                       "WHERE sourceBookId=:sourceBookId AND sourceAnchorId=:sourceAnchorId "
                       "ORDER BY created ASC, rowid ASC"));
    referenceQuery.bindValue(QStringLiteral(":sourceBookId"), sourceBookId);
    referenceQuery.bindValue(QStringLiteral(":sourceAnchorId"), sourceAnchorId);

    if (referenceQuery.exec() && referenceQuery.next()) {
        reference.insert(QStringLiteral("sourceBookId"), referenceQuery.value("sourceBookId").toString());
        reference.insert(QStringLiteral("sourceAnchorId"), referenceQuery.value("sourceAnchorId").toString());
        reference.insert(QStringLiteral("sourceAnchorTitle"), referenceQuery.value("sourceAnchorTitle").toString());
        reference.insert(QStringLiteral("targetBookId"), referenceQuery.value("targetBookId").toString());
        reference.insert(QStringLiteral("targetLocation"), referenceQuery.value("targetLocation").toString());
        reference.insert(QStringLiteral("targetPreviewHtml"), referenceQuery.value("targetPreviewHtml").toString());
        reference.insert(QStringLiteral("invalid"), false);
        reference.insert(QStringLiteral("created"), referenceQuery.value("created").toString());
        reference.insert(QStringLiteral("modified"), referenceQuery.value("modified").toString());
    } else {
        qCDebug(ARIANNA_LOG) << referenceQuery.lastError();
    }

    d->closeDb();
    return reference;
}

void BookDatabase::saveReference(const QVariantMap &reference)
{
    const QString sourceBookId = reference.value(QStringLiteral("sourceBookId")).toString();
    const QString sourceAnchorId = reference.value(QStringLiteral("sourceAnchorId")).toString();
    const QString targetBookId = reference.value(QStringLiteral("targetBookId")).toString();
    const QString targetLocation = reference.value(QStringLiteral("targetLocation")).toString();

    if (sourceBookId.isEmpty() || sourceAnchorId.isEmpty() || targetBookId.isEmpty() || targetLocation.isEmpty()) {
        qDebug() << " BookDatabase::saveReference: Invalid reference data, missing required fields.";
        return;
    }

    if (!d->prepareDb()) {
        return;
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString created = reference.value(QStringLiteral("created")).toString().isEmpty() ? now : reference.value(QStringLiteral("created")).toString();
    const QString modified = reference.value(QStringLiteral("modified")).toString();

    QSqlQuery referenceQuery;
    referenceQuery.prepare(
        QStringLiteral("INSERT INTO \"references\"("
                       "sourceBookId, sourceAnchorId, sourceAnchorTitle, "
                       "targetBookId, targetLocation, targetPreviewHtml, created, modified) "
                       "VALUES("
                       ":sourceBookId, :sourceAnchorId, :sourceAnchorTitle, "
                       ":targetBookId, :targetLocation, :targetPreviewHtml, :created, :modified) "
                       "ON CONFLICT(sourceBookId, sourceAnchorId) "
                       "DO UPDATE SET "
                       "sourceAnchorTitle=excluded.sourceAnchorTitle, "
                       "targetBookId=excluded.targetBookId, "
                       "targetLocation=excluded.targetLocation, "
                       "targetPreviewHtml=excluded.targetPreviewHtml, "
                       "modified=excluded.modified"));

    referenceQuery.bindValue(QStringLiteral(":sourceBookId"), sourceBookId);
    referenceQuery.bindValue(QStringLiteral(":sourceAnchorId"), reference.value(QStringLiteral("sourceAnchorId")).toString());
    referenceQuery.bindValue(QStringLiteral(":sourceAnchorTitle"), reference.value(QStringLiteral("sourceAnchorTitle")).toString());
    referenceQuery.bindValue(QStringLiteral(":targetBookId"), targetBookId);
    referenceQuery.bindValue(QStringLiteral(":targetLocation"), targetLocation);
    referenceQuery.bindValue(QStringLiteral(":targetPreviewHtml"), reference.value(QStringLiteral("targetPreviewHtml")).toString());
    referenceQuery.bindValue(QStringLiteral(":created"), created);
    referenceQuery.bindValue(QStringLiteral(":modified"), modified);

    if (!referenceQuery.exec()) {
        qCDebug(ARIANNA_LOG) << referenceQuery.lastError();
        qCDebug(ARIANNA_LOG) << "Query failed, string:" << referenceQuery.lastQuery();
    }

    d->closeDb();
}

bool BookDatabase::updateReferenceTarget(const QVariantMap &expected, const QString &targetBookId, const QString &targetLocation, bool *conflict)
{
    if (conflict) {
        *conflict = false;
    }
    if (expected.value(QStringLiteral("sourceBookId")).toString().isEmpty() || expected.value(QStringLiteral("sourceAnchorId")).toString().isEmpty()
        || targetBookId.isEmpty() || targetLocation.isEmpty() || !d->prepareDb()) {
        return false;
    }
    QSqlQuery query;
    query.prepare(
        QStringLiteral("UPDATE \"references\" SET "
                       "targetPreviewHtml = CASE WHEN COALESCE(targetBookId, '') = :targetBookId "
                       "AND COALESCE(targetLocation, '') = :targetLocation THEN targetPreviewHtml ELSE '' END, "
                       "targetBookId = :targetBookId, targetLocation = :targetLocation, modified = :modified "
                       "WHERE sourceBookId = :sourceBookId AND sourceAnchorId = :sourceAnchorId "
                       "AND COALESCE(targetBookId, '') = COALESCE(:oldBookId, '') "
                       "AND COALESCE(targetLocation, '') = COALESCE(:oldLocation, '')"));
    query.bindValue(QStringLiteral(":targetBookId"), targetBookId);
    query.bindValue(QStringLiteral(":targetLocation"), targetLocation);
    query.bindValue(QStringLiteral(":modified"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":sourceBookId"), expected.value(QStringLiteral("sourceBookId")).toString());
    query.bindValue(QStringLiteral(":sourceAnchorId"), expected.value(QStringLiteral("sourceAnchorId")).toString());
    query.bindValue(QStringLiteral(":oldBookId"), expected.value(QStringLiteral("targetBookId")).toString());
    query.bindValue(QStringLiteral(":oldLocation"), expected.value(QStringLiteral("targetLocation")).toString());
    const bool executed = query.exec();
    const bool updated = executed && query.numRowsAffected() == 1;
    if (conflict) {
        *conflict = executed && !updated;
    }
    if (!executed) {
        qCDebug(ARIANNA_LOG) << query.lastError();
    }
    query.finish();
    d->closeDb();
    if (updated) {
        Q_EMIT databaseChanged();
    }
    return updated;
}

bool BookDatabase::updateReferenceTitle(const QVariantMap &expected, const QString &title)
{
    if (expected.value(QStringLiteral("sourceBookId")).toString().isEmpty() || expected.value(QStringLiteral("sourceAnchorId")).toString().isEmpty()
        || !expected.contains(QStringLiteral("sourceAnchorTitle")) || !d->prepareDb()) {
        return false;
    }
    QSqlQuery query;
    query.prepare(QStringLiteral("UPDATE \"references\" SET sourceAnchorTitle = :title, modified = :modified "
                                 "WHERE sourceBookId = :sourceBookId AND sourceAnchorId = :sourceAnchorId "
                                 "AND COALESCE(targetBookId, '') = COALESCE(:oldBookId, '') "
                                 "AND COALESCE(targetLocation, '') = COALESCE(:oldLocation, '') "
                                 "AND COALESCE(sourceAnchorTitle, '') = COALESCE(:oldTitle, '')"));
    query.bindValue(QStringLiteral(":title"), title);
    query.bindValue(QStringLiteral(":modified"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":sourceBookId"), expected.value(QStringLiteral("sourceBookId")).toString());
    query.bindValue(QStringLiteral(":sourceAnchorId"), expected.value(QStringLiteral("sourceAnchorId")).toString());
    query.bindValue(QStringLiteral(":oldBookId"), expected.value(QStringLiteral("targetBookId")).toString());
    query.bindValue(QStringLiteral(":oldLocation"), expected.value(QStringLiteral("targetLocation")).toString());
    query.bindValue(QStringLiteral(":oldTitle"), expected.value(QStringLiteral("sourceAnchorTitle")).toString());
    const bool updated = query.exec() && query.numRowsAffected() == 1;
    if (query.lastError().isValid()) {
        qCDebug(ARIANNA_LOG) << query.lastError();
    }
    query.finish();
    d->closeDb();
    if (updated) {
        Q_EMIT databaseChanged();
    }
    return updated;
}

QVariantList BookDatabase::loadReferencesTargeting(const QString &targetBookId)
{
    if (!d->prepareDb()) {
        return {};
    }

    QVariantList references;
    QSqlQuery referenceQuery;
    referenceQuery.prepare(
        QStringLiteral("SELECT sourceBookId, sourceAnchorId, "
                       "sourceAnchorTitle, "
                       "targetBookId, targetLocation, targetPreviewHtml, "
                       "created, modified "
                       "FROM \"references\" "
                       "WHERE targetBookId=:targetBookId "
                       "ORDER BY created ASC, rowid ASC"));
    referenceQuery.bindValue(QStringLiteral(":targetBookId"), targetBookId);

    if (referenceQuery.exec()) {
        while (referenceQuery.next()) {
            QVariantMap reference;
            reference.insert(QStringLiteral("sourceBookId"), referenceQuery.value(QStringLiteral("sourceBookId")).toString());
            reference.insert(QStringLiteral("sourceAnchorId"), referenceQuery.value(QStringLiteral("sourceAnchorId")).toString());
            reference.insert(QStringLiteral("sourceAnchorTitle"), referenceQuery.value(QStringLiteral("sourceAnchorTitle")).toString());
            reference.insert(QStringLiteral("targetBookId"), referenceQuery.value(QStringLiteral("targetBookId")).toString());
            reference.insert(QStringLiteral("targetLocation"), referenceQuery.value(QStringLiteral("targetLocation")).toString());
            reference.insert(QStringLiteral("targetPreviewHtml"), referenceQuery.value(QStringLiteral("targetPreviewHtml")).toString());
            reference.insert(QStringLiteral("invalid"), false);
            reference.insert(QStringLiteral("created"), referenceQuery.value(QStringLiteral("created")).toString());
            reference.insert(QStringLiteral("modified"), referenceQuery.value(QStringLiteral("modified")).toString());
            references.append(reference);
        }
    } else {
        qCDebug(ARIANNA_LOG) << referenceQuery.lastError();
    }

    d->closeDb();
    return references;
}

QVariantList BookDatabase::loadAllReferences(bool *success)
{
    if (success) {
        *success = false;
    }
    if (!d->prepareDb()) {
        return {};
    }
    const QVariantList references = readAllReferences(success);
    d->closeDb();
    return references;
}

bool BookDatabase::removeReferenceAndSaveStates(const QVariantMap &expected, const QList<BookState> &states)
{
    if (expected.value(QStringLiteral("sourceBookId")).toString().isEmpty() || expected.value(QStringLiteral("sourceAnchorId")).toString().isEmpty()
        || !d->prepareDb()) {
        return false;
    }
    auto db = QSqlDatabase::database();
    if (!db.transaction()) {
        d->closeDb();
        return false;
    }
    QSqlQuery query(db);
    QStringList conditions;
    const QStringList fields{QStringLiteral("sourceBookId"),
                             QStringLiteral("sourceAnchorId"),
                             QStringLiteral("sourceAnchorTitle"),
                             QStringLiteral("targetBookId"),
                             QStringLiteral("targetLocation"),
                             QStringLiteral("targetPreviewHtml"),
                             QStringLiteral("created"),
                             QStringLiteral("modified")};
    for (const auto &field : fields) {
        conditions.append(QStringLiteral("COALESCE(%1, '') = COALESCE(:%1, '')").arg(field));
    }
    query.prepare(QStringLiteral("DELETE FROM \"references\" WHERE ") + conditions.join(QStringLiteral(" AND ")));
    for (const auto &field : fields) {
        query.bindValue(QLatin1Char(':') + field, expected.value(field).toString());
    }
    bool success = query.exec() && query.numRowsAffected() == 1;
    query.finish();
    for (const auto &state : states) {
        if (!success)
            break;
        success = writeBookState(state);
    }
    success = success && db.commit();
    if (!success)
        db.rollback();
    d->closeDb();
    if (success)
        Q_EMIT databaseChanged();
    return success;
}

void BookDatabase::removeReference(const QString &sourceBookId, const QString &sourceAnchorId)
{
    if (sourceBookId.isEmpty() || sourceAnchorId.isEmpty()) {
        return;
    }

    if (!d->prepareDb()) {
        return;
    }

    QSqlQuery referenceQuery;
    referenceQuery.prepare(
        QStringLiteral("DELETE FROM \"references\" "
                       "WHERE sourceBookId=:sourceBookId "
                       "AND sourceAnchorId=:sourceAnchorId"));

    referenceQuery.bindValue(QStringLiteral(":sourceBookId"), sourceBookId);
    referenceQuery.bindValue(QStringLiteral(":sourceAnchorId"), sourceAnchorId);

    if (!referenceQuery.exec()) {
        qCDebug(ARIANNA_LOG) << referenceQuery.lastError();
    }

    d->closeDb();
}

#include "moc_bookdatabase.cpp"
