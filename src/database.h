// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlTableModel>

template<typename T>
class asKeyValueRange
{
public:
    asKeyValueRange(T &data)
        : m_data{data}
    {
    }

    auto begin()
    {
        return m_data.keyValueBegin();
    }
    auto end()
    {
        return m_data.keyValueEnd();
    }

private:
    T &m_data;
};
template<typename T>
asKeyValueRange(T &) -> asKeyValueRange<T>;

class Database : public QSqlTableModel
{
    Q_OBJECT

public:
    static Database *self();

    Q_INVOKABLE bool addBook(const QString &fileName, const QString &jsonMetadata);

protected:
    void init();
    Database(const QString &path = {});
    QString m_databasePath;

private:
    QSqlDatabase m_db;
    void checkColumns();
    void checkDatabase();
    QSqlQuery createTableStatement(const QString &table, const QHash<QByteArray, QByteArray> &columns);

    const QHash<QByteArray, QByteArray> bookTableColumns = {
        {"id", "INTEGER PRIMARY KEY AUTOINCREMENT"},
        {"Title", "TEXT"},
        {"Author", "TEXT"},
        {"Year", "INTEGER"},
        {"DateAdded", "INTEGER"},
        {"Path", "TEXT UNIQUE"},
        {"Position", "BLOB"},
        {"ISBN", "TEXT"},
        {"Tags", "TEXT"},
        {"Hash", "TEXT"},
        {"LastAccessed", "BLOB"},
        {"Bookmarks", "BLOB"},
        {"CoverImage", "BLOB"},
        {"Addition", "TEXT"},
        {"Annotations", "BLOB"},
    };

    const QHash<QByteArray, QByteArray> directoryTableColumns = {
        {"id", "INTEGER PRIMARY KEY"},
        {"Path", "TEXT"},
        {"Name", "TEXT"},
        {"Tags", "TEXT"},
        {"CheckState", "INTEGER"},
    };
};
