// SPDX-License-Identifier: GPL-3.0-or-later
#include "referencetargetmodel.h"
#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QTest>

static QVariantMap target(const QString &value, const QString &label, const QStringList &path = {}, const QStringList &ids = {}, const QString &type = QStringLiteral("element"))
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("label"), label}, {QStringLiteral("searchKey"), (label + u' ' + path.join(u' ')).toLower()},
            {QStringLiteral("data"), QVariantMap{{QStringLiteral("hrefLocation"), value}, {QStringLiteral("tocPath"), path},
                {QStringLiteral("tocIdPath"), ids}, {QStringLiteral("type"), type}, {QStringLiteral("file"), value.section(u'#', 0, 0)}}}};
}

class ReferenceTargetModelTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void ancestorsAndExactTargets();
    void fallbackAndPdf();
    void largeTree();
};

void ReferenceTargetModelTest::ancestorsAndExactTargets()
{
    ReferenceTargetModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const auto verse = target(QStringLiteral("one.xhtml#verse_18-20"), QStringLiteral("Needle कृष्ण Śrī"),
                              {QStringLiteral("Part"), QStringLiteral("Chapter")}, {QStringLiteral("p1"), QStringLiteral("c1")});
    const auto chapter = target(QStringLiteral("one.xhtml#chapter"), QStringLiteral("Chapter"),
                                {QStringLiteral("Part"), QStringLiteral("Chapter")}, {QStringLiteral("p1"), QStringLiteral("c1")}, QStringLiteral("navigation"));
    const auto repeated = target(QStringLiteral("two.xhtml#verse"), QStringLiteral("Other"),
                                 {QStringLiteral("Part"), QStringLiteral("Chapter")}, {QStringLiteral("p2"), QStringLiteral("c2")});
    model.setRows({verse, repeated, chapter}); // TOC target may arrive after its anchors.
    QCOMPARE(model.matchCount(), 3);
    QCOMPARE(model.rowCount(), 2); // Equal captions do not merge their branches.
    auto selected = model.indexForValue(verse.value(QStringLiteral("value")).toString());
    QVERIFY(selected.isValid());
    QCOMPARE(model.data(selected, ReferenceTargetModel::TargetDataRole).toMap(), verse);
    QCOMPARE(model.data(model.parent(selected), ReferenceTargetModel::TargetValueRole).toString(), QStringLiteral("one.xhtml#chapter"));
    QVERIFY(!(model.flags(model.index(0, 0)) & Qt::ItemIsSelectable)); // Unlinked Part.
    QPersistentModelIndex oldSelection(selected);
    model.setFilterText(QStringLiteral("needle"));
    QVERIFY(!oldSelection.isValid());
    QCOMPARE(model.matchCount(), 1);
    QCOMPARE(model.rowCount(), 1);
    selected = model.indexForValue(QStringLiteral("one.xhtml#verse_18-20"));
    QCOMPARE(model.data(model.parent(selected)).toString(), QStringLiteral("Chapter"));
    QCOMPARE(model.data(model.parent(model.parent(selected))).toString(), QStringLiteral("Part"));
    QVERIFY(!model.indexForValue(QStringLiteral("two.xhtml#verse")).isValid());
    QCOMPARE(model.rowForValue(QStringLiteral("one.xhtml#verse_18-20")), verse);
    model.setFilterText(QStringLiteral("absent"));
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.matchCount(), 0);
    model.setFilterText(QString());
    QCOMPARE(model.rowCount(), 2);
    model.setRows({});
    QCOMPARE(model.matchCount(), 0);
    QVERIFY(model.rowForValue(QStringLiteral("one.xhtml#verse_18-20")).isEmpty());
}

void ReferenceTargetModelTest::fallbackAndPdf()
{
    ReferenceTargetModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QString pdf = QStringLiteral("{\"page\":3}");
    model.setRows({target(QStringLiteral("one.xhtml#exact"), QStringLiteral("An anchor")),
                   target(QStringLiteral("one.xhtml"), QStringLiteral("Document"), {}, {}, QStringLiteral("document")),
                   target(QStringLiteral("two.xhtml#exact"), QStringLiteral("Another anchor")),
                   target(pdf, QStringLiteral("PDF Part"), {QStringLiteral("PDF Part")}, {QStringLiteral("pdf-1")}, QStringLiteral("navigation")),
                   target(pdf, QStringLiteral("PDF Chapter on same page"), {QStringLiteral("PDF Part"), QStringLiteral("Chapter")},
                          {QStringLiteral("pdf-1"), QStringLiteral("pdf-2")}, QStringLiteral("navigation")),
                   target(QStringLiteral("legacy.xhtml#a"), QStringLiteral("First"), {QStringLiteral("Legacy")}),
                   target(QStringLiteral("legacy.xhtml#b"), QStringLiteral("Second"), {QStringLiteral("Legacy")})});
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.matchCount(), 7);
    auto index = model.indexForValue(QStringLiteral("one.xhtml#exact"));
    QCOMPARE(model.data(model.parent(index), ReferenceTargetModel::TargetValueRole).toString(), QStringLiteral("one.xhtml"));
    index = model.indexForValue(pdf);
    QCOMPARE(model.data(model.index(0, 0, index), ReferenceTargetModel::TargetValueRole).toString(), pdf);
    QCOMPARE(model.rowForValue(QStringLiteral("one.xhtml#exact")).value(QStringLiteral("value")).toString(), QStringLiteral("one.xhtml#exact"));
    model.setFilterText(QStringLiteral("second"));
    QCOMPARE(model.matchCount(), 1);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QStringLiteral("Legacy"));
}

void ReferenceTargetModelTest::largeTree()
{
    QVariantList rows;
    for (int chapter = 0; chapter < 300; ++chapter) {
        for (int anchor = 0; anchor < 100; ++anchor) {
            const QString id = QString::number(chapter);
            rows.append(target(id + QStringLiteral(".xhtml#v%1").arg(anchor), QStringLiteral("Verse%1").arg(anchor),
                               {QStringLiteral("Part"), QStringLiteral("Chapter ") + id}, {QStringLiteral("part"), id}));
        }
    }
    ReferenceTargetModel model;
    QElapsedTimer timer;
    timer.start();
    model.setRows(rows);
    QCOMPARE(model.matchCount(), 30000);
    QCOMPARE(model.rowCount(model.index(299, 0, model.index(0, 0))), 100); // No 40-target truncation.
    model.setFilterText(QStringLiteral("chapter 299 verse99"));
    QCOMPARE(model.matchCount(), 1);
    QVERIFY(model.indexForValue(QStringLiteral("299.xhtml#v99")).isValid());
    QCOMPARE(model.rowCount(), 1);
    qInfo() << "Build and filter 30,000 targets (ms):" << timer.elapsed();
}

QTEST_GUILESS_MAIN(ReferenceTargetModelTest)
#include "referencetargetmodeltest.moc"
