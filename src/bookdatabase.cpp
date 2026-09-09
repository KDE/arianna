// SPDX-FileCopyrightText: 2017 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "bookdatabase.h"

#include "categoryentriesmodel.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMetaType>
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
            ensureColumn(QStringLiteral("bookVersion"), QStringLiteral("bookVersion varchar"));
            ensureColumn(QStringLiteral("uniqueIdentifier"), QStringLiteral("uniqueIdentifier varchar"));
            ensureColumn(QStringLiteral("subjects"), QStringLiteral("subjects varchar"));
            ensureColumn(QStringLiteral("pdfNotInvertedRegions"), QStringLiteral("pdfNotInvertedRegions text"));
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
                   << QStringLiteral("bookVersion varchar")
                   << QStringLiteral("rights varchar")
                   << QStringLiteral("source varchar")
                   << QStringLiteral("identifier varchar")
                   << QStringLiteral("uniqueIdentifier varchar")
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

        return true;
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
                                   "created datetime, "
                                   "modified datetime, "
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
            QStringLiteral("created"),
            QStringLiteral("modified"),
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
                                                 "created datetime, "
                                                 "modified datetime, "
                                                 "primary key(bookId, annotationId))"))
            && execMigrationQuery(QStringLiteral("INSERT OR REPLACE INTO annotations_new("
                                                 "bookId, annotationId, cfiRange, anchorId, "
                                                 "color, text, note, created, modified) "
                                                 "SELECT bookId, %1, %2, %3, %4, %5, %6, %7, %8 "
                                                 "FROM annotations "
                                                 "WHERE bookId IS NOT NULL AND bookId != ''")
                                      .arg(annotationIdExpression,
                                           cfiRangeExpression,
                                           columnOrNull(QStringLiteral("anchorId")),
                                           columnOrNull(QStringLiteral("color")),
                                           columnOrNull(QStringLiteral("text")),
                                           columnOrNull(QStringLiteral("note")),
                                           columnOrNull(QStringLiteral("created")),
                                           columnOrNull(QStringLiteral("modified"))))
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
        entry.filetitle = query.value(fieldNames.indexOf(QStringLiteral("fileTitle"))).toString();
        entry.title = query.value(fieldNames.indexOf(QStringLiteral("title"))).toString();
        entry.series = query.value(fieldNames.indexOf(QStringLiteral("series"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.author = query.value(fieldNames.indexOf(QStringLiteral("author"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry.publisher = query.value(fieldNames.indexOf(QStringLiteral("publisher"))).toString();
        entry.created = query.value(fieldNames.indexOf(QStringLiteral("created"))).toDateTime();
        entry.lastOpenedTime = query.value(fieldNames.indexOf(QStringLiteral("lastOpenedTime"))).toDateTime();
        entry.currentLocation = query.value(fieldNames.indexOf(QStringLiteral("currentLocation"))).toString();
        entry.currentProgress = query.value(fieldNames.indexOf(QStringLiteral("currentProgress"))).toInt();
        entry.zoomLevel = query.value(fieldNames.indexOf(QStringLiteral("zoomLevel"))).toDouble();
        if (entry.zoomLevel <= 0.0) {
            entry.zoomLevel = 1.0;
        }
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

    void bindEntry(QSqlQuery &query, const BookEntry &entry)
    {
        query.bindValue(QStringLiteral(":fileName"), entry.filename);
        query.bindValue(QStringLiteral(":fileTitle"), entry.filetitle);
        query.bindValue(QStringLiteral(":title"), entry.title);
        query.bindValue(QStringLiteral(":series"), entry.series.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":author"), entry.author.join(QLatin1Char(',')));
        query.bindValue(QStringLiteral(":publisher"), entry.publisher);
        query.bindValue(QStringLiteral(":created"), entry.created);
        query.bindValue(QStringLiteral(":lastOpenedTime"), entry.lastOpenedTime);
        query.bindValue(QStringLiteral(":currentLocation"), entry.currentLocation);
        query.bindValue(QStringLiteral(":currentProgress"), entry.currentProgress);
        query.bindValue(QStringLiteral(":zoomLevel"), entry.zoomLevel);
        query.bindValue(QStringLiteral(":pageMode"), entry.pageMode);
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

QList<BookEntry> BookDatabase::loadEntries()
{
    if (!d->prepareDb()) {
        return {};
    }

    QList<BookEntry> entries;
    QSqlQuery allEntries(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(" FROM books"));

    while (allEntries.next()) {
        entries.append(d->fromSqlQuery(allEntries));
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

    QStringList valueNames;
    for (int i = 0; i < d->fieldNames.size(); i++) {
        valueNames.append(QStringLiteral(":").append(d->fieldNames.at(i)));
    }
    QSqlQuery newEntry;
    newEntry.prepare(QStringLiteral("INSERT INTO books (") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(") ") + QStringLiteral("VALUES (")
                     + valueNames.join(QStringLiteral(", ")) + QLatin1Char(')'));
    d->bindEntry(newEntry, entry);
    newEntry.exec();

    d->closeDb();
}

void BookDatabase::updateEntry(const BookEntry &entry)
{
    if (!d->prepareDb()) {
        return;
    }

    QStringList assignments;
    for (const QString &fieldName : std::as_const(d->fieldNames)) {
        if (fieldName == QStringLiteral("fileName")) {
            continue;
        }
        assignments.append(QStringLiteral("%1=:%1").arg(fieldName));
    }

    QSqlQuery updateEntry;
    updateEntry.prepare(QStringLiteral("UPDATE books SET ") + assignments.join(QStringLiteral(", ")) + QStringLiteral(" WHERE fileName=:fileName"));
    d->bindEntry(updateEntry, entry);
    if (!updateEntry.exec()) {
        qCDebug(ARIANNA_LOG) << updateEntry.lastError();
        qCDebug(ARIANNA_LOG) << "Query failed, string:" << updateEntry.lastQuery();
    }

    d->closeDb();
}

void BookDatabase::removeEntry(const BookEntry &entry)
{
    if (!d->prepareDb()) {
        return;
    }
    qCDebug(ARIANNA_LOG) << "Removing book from the database" << entry.filename;

    QSqlQuery removeAnnotations;
    removeAnnotations.prepare(QStringLiteral("DELETE FROM annotations WHERE bookId=:bookId"));
    removeAnnotations.bindValue(QStringLiteral(":bookId"), entry.uniqueIdentifier);
    if (!removeAnnotations.exec()) {
        qCDebug(ARIANNA_LOG) << removeAnnotations.lastError();
    }

    QSqlQuery removeEntry;
    removeEntry.prepare(QStringLiteral("DELETE FROM books WHERE fileName=:fileName"));
    removeEntry.bindValue(QStringLiteral(":fileName"), entry.filename);
    if (!removeEntry.exec()) {
        qCDebug(ARIANNA_LOG) << removeEntry.lastError();
    }

    d->closeDb();
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

bool BookDatabase::saveBookState(const BookState &state)
{
    if (state.stateId.isNull() || state.bookId.isEmpty() || state.textContentHash.isEmpty() || state.documentStateHash.isEmpty()
        || state.epubFileHash.isEmpty()) {
        return false;
    }

    if (!d->prepareDb()) {
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

    d->closeDb();
    return saved;
}

QVariantList BookDatabase::loadAnnotations(const QString &bookId)
{
    if (!d->prepareDb()) {
        return {};
    }

    QVariantList annotations;
    QSqlQuery annotationQuery;
    annotationQuery.prepare(
        QStringLiteral("SELECT cfiRange, color, text, note, created, modified, annotationId, anchorId "
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
            annotation.insert(QStringLiteral("created"), annotationQuery.value(4).toString());
            annotation.insert(QStringLiteral("modified"), annotationQuery.value(5).toString());
            annotation.insert(QStringLiteral("annotationId"), annotationQuery.value(6).toString());
            annotation.insert(QStringLiteral("anchorId"), annotationQuery.value(7).toString());
            annotation.insert(QStringLiteral("invalid"), false);
            annotations.append(annotation);
        }
    } else {
        qCDebug(ARIANNA_LOG) << annotationQuery.lastError();
    }

    d->closeDb();
    return annotations;
}

void BookDatabase::saveAnnotation(const QString &bookId, const QVariantMap &annotation)
{
    const QString cfiRange = annotation.value(QStringLiteral("cfiRange")).toString().isEmpty() ? annotation.value(QStringLiteral("value")).toString()
                                                                                               : annotation.value(QStringLiteral("cfiRange")).toString();
    QString annotationId = annotation.value(QStringLiteral("annotationId")).toString().trimmed();
    const QString anchorId = annotation.value(QStringLiteral("anchorId")).toString().trimmed();
    if (annotationId.isEmpty()) {
        annotationId = anchorId.isEmpty() ? QUuid::createUuidV7().toString(QUuid::WithoutBraces) : anchorId;
    }
    if (bookId.isEmpty() || annotationId.isEmpty() || (cfiRange.isEmpty() && anchorId.isEmpty())) {
        return;
    }

    if (!d->prepareDb()) {
        return;
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString created = annotation.value(QStringLiteral("created")).toString().isEmpty() ? now : annotation.value(QStringLiteral("created")).toString();
    const QString modified = annotation.value(QStringLiteral("modified")).toString();

    QSqlQuery annotationQuery;
    annotationQuery.prepare(
        QStringLiteral("INSERT INTO annotations(bookId, annotationId, cfiRange, anchorId, color, text, note, created, modified) "
                       "VALUES(:bookId, :annotationId, :cfiRange, :anchorId, :color, :text, :note, :created, :modified) "
                       "ON CONFLICT(bookId, annotationId) DO UPDATE SET "
                       "cfiRange=CASE WHEN excluded.cfiRange IS NOT NULL AND excluded.cfiRange != '' "
                       "THEN excluded.cfiRange ELSE annotations.cfiRange END, "
                       "anchorId=CASE WHEN excluded.anchorId IS NOT NULL AND excluded.anchorId != '' "
                       "THEN excluded.anchorId ELSE annotations.anchorId END, "
                       "color=excluded.color, "
                       "text=excluded.text, "
                       "note=excluded.note, "
                       "modified=excluded.modified"));
    annotationQuery.bindValue(QStringLiteral(":bookId"), bookId);
    annotationQuery.bindValue(QStringLiteral(":annotationId"), annotationId);
    annotationQuery.bindValue(QStringLiteral(":cfiRange"), cfiRange);
    annotationQuery.bindValue(QStringLiteral(":anchorId"), anchorId);
    annotationQuery.bindValue(QStringLiteral(":color"), annotation.value(QStringLiteral("color"), QStringLiteral("yellow")).toString());
    annotationQuery.bindValue(QStringLiteral(":text"), annotation.value(QStringLiteral("text")).toString());
    annotationQuery.bindValue(QStringLiteral(":note"), annotation.value(QStringLiteral("note")).toString());
    annotationQuery.bindValue(QStringLiteral(":created"), created);
    annotationQuery.bindValue(QStringLiteral(":modified"), modified);

    if (!annotationQuery.exec()) {
        qCDebug(ARIANNA_LOG) << annotationQuery.lastError();
        qCDebug(ARIANNA_LOG) << "Query failed, string:" << annotationQuery.lastQuery();
    }

    d->closeDb();
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
