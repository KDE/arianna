// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <QAbstractItemModelTester>
#include <QtTest/QtTest>
#include <tableofcontentmodel.h>

class TableOfContentTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
    }

    void testModel()
    {
        TableOfContentModel tocModel;
        QAbstractItemModelTester tester(&tocModel);
        QFile json;
        json.setFileName(QLatin1String(DATA_DIR) + QLatin1String("/mobidick-toc.json"));
        json.open(QIODevice::ReadOnly);

        tocModel.importFromJson(json.readAll());

        QCOMPARE(tocModel.rowCount(), 142);
        QCOMPARE(tocModel.rowCount(tocModel.index(4, 0)), 1);
        QVERIFY(tocModel.hasChildren(tocModel.index(4, 0)));
    }
};

QTEST_MAIN(TableOfContentTest)
#include "tableofcontenttest.moc"