// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "database.h"
#include <QtTest/QtTest>
#include <qstringliteral.h>

class TestDatabase : public Database
{
public:
    TestDatabase()
        : Database(QLatin1String("./test.db"))
    {
    }
};

class DatabaseTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
    }

    void testModel()
    {
        TestDatabase db;

        QFile file;
        file.setFileName(QLatin1String(DATA_DIR) + QLatin1Char('/') + QStringLiteral("book-metadata.json"));
        file.open(QIODevice::ReadOnly);

        QVERIFY(db.addBook(QStringLiteral("mybook.epub"), QString::fromUtf8(file.readAll())));
        QVERIFY(db.addBook(QStringLiteral("mybook.epub"), QString::fromUtf8(file.readAll())));

        QCOMPARE(db.rowCount(), 1);
    }
};

QTEST_MAIN(DatabaseTest)
#include "databasetest.moc"
