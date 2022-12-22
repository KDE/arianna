// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "database.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlRecord>
#include <QStandardPaths>

const QString DRIVER(QStringLiteral("QSQLITE"));

Database *Database::self()
{
    static Database _instance;
    return &_instance;
}

Database::Database(const QString &path)
{
    if (path.isEmpty()) {
        m_databasePath =
            QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QLatin1Char('/') + qGuiApp->applicationName());
    } else {
        m_databasePath = path;
    }

    init();
}

void Database::init()
{
    if (QFileInfo::exists(m_databasePath)) {
        checkColumns();
    } else {
        checkDatabase();
    }

    setTable(QStringLiteral("book"));
    setEditStrategy(QSqlTableModel::OnManualSubmit);
    select();
}

void Database::checkColumns()
{
}

void Database::checkDatabase()
{
    Q_ASSERT(QSqlDatabase::isDriverAvailable(DRIVER));
    Q_ASSERT(QDir().mkpath(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))));
    m_db = QSqlDatabase::addDatabase(DRIVER);
    const auto path = m_databasePath;
    qWarning() << m_databasePath;
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        qCritical() << m_db.lastError() << "while opening database at" << path;
    }

    auto query = createTableStatement(QStringLiteral("book"), bookTableColumns);
    if (!query.exec()) {
        qCritical() << query.lastError() << "while creating book table" << m_databasePath;
    }
    query = createTableStatement(QStringLiteral("directory"), directoryTableColumns);
    if (!query.exec()) {
        qCritical() << query.lastError() << "while creating directory table";
    }
    qWarning() << "Create table";
}

QSqlQuery Database::createTableStatement(const QString &table, const QHash<QByteArray, QByteArray> &columns)
{
    QStringList mergedColumns;
    for (const auto [column, type] : asKeyValueRange(columns)) {
        mergedColumns << (QString::fromUtf8(column) + QLatin1Char(' ') + QString::fromUtf8(type));
    }
    return QSqlQuery{QStringLiteral("CREATE TABLE IF NOT EXISTS ") + table + QLatin1Char('(') + mergedColumns.join(QLatin1Char(',')) + QLatin1String(");")};
}

bool Database::addBook(const QString &fileName, const QString &jsonMetadata)
{
    const auto doc = QJsonDocument::fromJson(jsonMetadata.toUtf8());
    Q_ASSERT(doc.isObject());
    const auto obj = doc.object();

    QSqlRecord newRecord;
    newRecord.setValue(QStringLiteral("Title"), obj[QStringLiteral("title")].toString());
    newRecord.setValue(QStringLiteral("Author"), obj[QStringLiteral("creator")].toString());
    newRecord.setValue(QStringLiteral("Year"), QDate::fromString(obj[QStringLiteral("pubdata")].toString(), Qt::ISODate).year());
    newRecord.setValue(QStringLiteral("DateAdded"), QDateTime::currentSecsSinceEpoch());
    newRecord.setValue(QStringLiteral("PATH"), fileName);
    newRecord.setValue(QStringLiteral("Position"), QVariant());
    newRecord.setValue(QStringLiteral("ISBN"), obj[QStringLiteral("identifier")].toString());
    newRecord.setValue(QStringLiteral("Tags"), QString());
    newRecord.setValue(QStringLiteral("Hash"), QString());
    newRecord.setValue(QStringLiteral("LastAccessed"), QDateTime::currentSecsSinceEpoch());
    newRecord.setValue(QStringLiteral("Bookmarks"), QByteArray());
    newRecord.setValue(QStringLiteral("CoverImage"), QByteArray());
    newRecord.setValue(QStringLiteral("Addition"), QString());
    newRecord.setValue(QStringLiteral("Annotations"), QByteArray());

    bool result = insertRecord(rowCount(), newRecord);
    if (!result) {
        qWarning() << m_db.lastError();
    }
    result &= submitAll();
    if (!result) {
        qWarning() << m_db.lastError();
    }
    return result;
}
