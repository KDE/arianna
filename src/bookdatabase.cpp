// SPDX-FileCopyrightText: 2017 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "bookdatabase.h"

#include "categoryentriesmodel.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>

#include <QDir>

#include <arianna_debug.h>
#include <qstringliteral.h>

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
    }

    QSqlDatabase db;
    QString dbfile;
    QStringList fieldNames;

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
            return true;
        }

        QSqlQuery q;
        QStringList entryNames;
        // clang-format off
        entryNames << QStringLiteral("fileName varchar primary key")
                   << QStringLiteral("fileTitle varchar")
                   << QStringLiteral("title varchar")
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
                   << QStringLiteral("comment varchar")
                   << QStringLiteral("tags varchar")
                   << QStringLiteral("rating varchar")
                   << QStringLiteral("locations text")
                   << QStringLiteral("currentLocation varchar")
                   << QStringLiteral("currentProgress int")
                   << QStringLiteral("rights varchar")
                   << QStringLiteral("source varchar")
                   << QStringLiteral("identifier varchar")
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

        return true;
    }

    void closeDb()
    {
        db.close();
    }

    BookEntry *fromSqlQuery(const QSqlQuery &query)
    {
        auto entry = new BookEntry();
        entry->filename = query.value(fieldNames.indexOf(QStringLiteral("fileName"))).toString();
        entry->filetitle = query.value(fieldNames.indexOf(QStringLiteral("fileTitle"))).toString();
        entry->title = query.value(fieldNames.indexOf(QStringLiteral("title"))).toString();
        entry->series = query.value(fieldNames.indexOf(QStringLiteral("series"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->author = query.value(fieldNames.indexOf(QStringLiteral("author"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->publisher = query.value(fieldNames.indexOf(QStringLiteral("publisher"))).toString();
        entry->created = query.value(fieldNames.indexOf(QStringLiteral("created"))).toDateTime();
        entry->lastOpenedTime = query.value(fieldNames.indexOf(QStringLiteral("lastOpenedTime"))).toDateTime();
        entry->currentLocation = query.value(fieldNames.indexOf(QStringLiteral("currentLocation"))).toString();
        entry->currentProgress = query.value(fieldNames.indexOf(QStringLiteral("currentProgress"))).toInt();
        entry->thumbnail = query.value(fieldNames.indexOf(QStringLiteral("thumbnail"))).toString();
        entry->description = query.value(fieldNames.indexOf(QStringLiteral("description"))).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        entry->comment = query.value(fieldNames.indexOf(QStringLiteral("comment"))).toString();
        entry->tags = query.value(fieldNames.indexOf(QStringLiteral("tags"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->rating = query.value(fieldNames.indexOf(QStringLiteral("rating"))).toInt();
        entry->seriesNumbers = query.value(fieldNames.indexOf(QStringLiteral("seriesNumbers"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->seriesVolumes = query.value(fieldNames.indexOf(QStringLiteral("seriesVolumes"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->genres = query.value(fieldNames.indexOf(QStringLiteral("genres"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->keywords = query.value(fieldNames.indexOf(QStringLiteral("keywords"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->characters = query.value(fieldNames.indexOf(QStringLiteral("characters"))).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        entry->locations = query.value(fieldNames.indexOf(QStringLiteral("locations"))).toString();
        entry->language = query.value(fieldNames.indexOf(QStringLiteral("language"))).toString();
        entry->identifier = query.value(fieldNames.indexOf(QStringLiteral("identifier"))).toString();
        entry->rights = query.value(fieldNames.indexOf(QStringLiteral("rights"))).toString();
        entry->source = query.value(fieldNames.indexOf(QStringLiteral("source"))).toString();
        return entry;
    }
};

BookDatabase::BookDatabase(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

BookDatabase::~BookDatabase() = default;

QList<BookEntry *> BookDatabase::loadEntries()
{
    if (!d->prepareDb()) {
        return QList<BookEntry *>();
    }

    QList<BookEntry *> entries;
    QStringList entryNames = d->fieldNames;
    QSqlQuery allEntries(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(" FROM books"));

    while (allEntries.next()) {
        entries.append(d->fromSqlQuery(allEntries));
    }

    d->closeDb();
    return entries;
}

BookEntry *BookDatabase::loadEntry(const QString &fileName)
{
    if (!d->prepareDb()) {
        return nullptr;
    }
    QSqlQuery entry;
    entry.prepare(QStringLiteral("SELECT ") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(" FROM books WHERE fileName = :fileName LIMIT 1"));
    entry.bindValue(QLatin1String(":fileName"), fileName);
    if (entry.exec()) {
        entry.first();
        return d->fromSqlQuery(entry);
    }
    return nullptr;
}

void BookDatabase::addEntry(BookEntry *entry)
{
    if (!d->prepareDb()) {
        return;
    }
    qCDebug(ARIANNA_LOG) << "Adding newly discovered book to the database" << entry->filename;

    QStringList valueNames;
    for (int i = 0; i < d->fieldNames.size(); i++) {
        valueNames.append(QStringLiteral(":").append(d->fieldNames.at(i)));
    }
    QSqlQuery newEntry;
    newEntry.prepare(QStringLiteral("INSERT INTO books (") + d->fieldNames.join(QStringLiteral(", ")) + QStringLiteral(") ") + QStringLiteral("VALUES (")
                     + valueNames.join(QStringLiteral(", ")) + QLatin1Char(')'));
    newEntry.bindValue(QStringLiteral(":fileName"), entry->filename);
    newEntry.bindValue(QStringLiteral(":fileTitle"), entry->filetitle);
    newEntry.bindValue(QStringLiteral(":title"), entry->title);
    newEntry.bindValue(QStringLiteral(":series"), entry->series.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":author"), entry->author.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":publisher"), entry->publisher);
    newEntry.bindValue(QStringLiteral(":publisher"), entry->publisher);
    newEntry.bindValue(QStringLiteral(":created"), entry->created);
    newEntry.bindValue(QStringLiteral(":lastOpenedTime"), entry->lastOpenedTime);
    newEntry.bindValue(QStringLiteral(":currentLocation"), entry->currentLocation);
    newEntry.bindValue(QStringLiteral(":currentProgress"), entry->currentProgress);
    newEntry.bindValue(QStringLiteral(":thumbnail"), entry->thumbnail);
    newEntry.bindValue(QStringLiteral(":description"), entry->description.join(QLatin1Char('\n')));
    newEntry.bindValue(QStringLiteral(":comment"), entry->comment);
    newEntry.bindValue(QStringLiteral(":tags"), entry->tags.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":rating"), entry->rating);
    newEntry.bindValue(QStringLiteral(":seriesNumbers"), entry->seriesNumbers.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":seriesVolumes"), entry->seriesVolumes.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":genres"), entry->genres.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":keywords"), entry->keywords.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":characters"), entry->characters.join(QLatin1Char(',')));
    newEntry.bindValue(QStringLiteral(":locations"), entry->locations);
    newEntry.bindValue(QStringLiteral(":rights"), entry->rights);
    newEntry.bindValue(QStringLiteral(":source"), entry->source);
    newEntry.bindValue(QStringLiteral(":identifier"), entry->identifier);
    newEntry.bindValue(QStringLiteral(":language"), entry->language);
    newEntry.exec();

    d->closeDb();
}

void BookDatabase::removeEntry(BookEntry *entry)
{
    if (!d->prepareDb()) {
        return;
    }
    qCDebug(ARIANNA_LOG) << "Removing book from the database" << entry->filename;

    QSqlQuery removeEntry;
    removeEntry.prepare(QStringLiteral("DELETE FROM books WHERE fileName='") + entry->filename + QStringLiteral("';"));
    removeEntry.exec();

    d->closeDb();
}

void BookDatabase::updateEntry(QString fileName, QString property, QVariant value)
{
    if (!d->prepareDb()) {
        return;
    }
    // qCDebug(QTQUICK_LOG) << "Updating book in the database" << fileName << property << value;

    if (!d->fieldNames.contains(property)) {
        return;
    }

    QStringList stringListValues;
    // clang-format off
    stringListValues << QStringLiteral("series")
                     << QStringLiteral("author")
                     << QStringLiteral("characters")
                     << QStringLiteral("genres")
                     << QStringLiteral("keywords")
                     << QStringLiteral("tags");
    // clang-format on
    QString val;
    if (stringListValues.contains(property)) {
        val = value.toStringList().join(QLatin1Char(','));
    } else if (property == QStringLiteral("description")) {
        val = value.toStringList().join(QLatin1Char('\n'));
    }

    QSqlQuery updateEntry;
    updateEntry.prepare(QStringLiteral("UPDATE books SET %1=:value WHERE fileName=:filename ").arg(property));
    updateEntry.bindValue(QStringLiteral(":value"), value);
    if (!val.isEmpty()) {
        updateEntry.bindValue(QStringLiteral(":value"), val);
    }
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
