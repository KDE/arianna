// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "tableofcontentmodel.h"

#include <QAbstractItemModelTester>
#include <QTest>

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
        QVERIFY(json.open(QIODevice::ReadOnly));

        tocModel.importFromJson(json.readAll());

        QCOMPARE(tocModel.rowCount(), 142);
        QCOMPARE(tocModel.rowCount(tocModel.index(4, 0)), 1);
        QVERIFY(tocModel.hasChildren(tocModel.index(4, 0)));
    }

    void testNumericIdRole()
    {
        TableOfContentModel tocModel;
        tocModel.importFromJson(QByteArrayLiteral(R"([
            {
                "label": "Chapter",
                "href": "chapter.xhtml",
                "id": 17,
                "subitems": [
                    {
                        "label": "Section",
                        "href": "chapter.xhtml#section",
                        "id": 18
                    }
                ]
            }
        ])"));

        const QModelIndex chapter = tocModel.index(0, 0);
        QVERIFY(chapter.isValid());
        QCOMPARE(tocModel.data(chapter, TableOfContentModel::IdRole).toString(), QStringLiteral("17"));
        QCOMPARE(tocModel.data(chapter, TableOfContentModel::TocIdRole).toString(), QStringLiteral("17"));

        const QModelIndex section = tocModel.index(0, 0, chapter);
        QVERIFY(section.isValid());
        QCOMPARE(tocModel.data(section, TableOfContentModel::IdRole).toString(), QStringLiteral("18"));
        QCOMPARE(tocModel.data(section, TableOfContentModel::TocIdRole).toString(), QStringLiteral("18"));
    }
};

QTEST_MAIN(TableOfContentTest)
#include "tableofcontenttest.moc"
