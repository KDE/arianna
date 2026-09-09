// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "categoryentriesmodel.h"

#include <QDateTime>
#include <QObject>
#include <QTest>
#include <QVariantMap>

static QVariantList groupBooks(const QVariantMap &group)
{
    return group.value(QStringLiteral("books")).toList();
}

static QString groupBookTitle(const QVariantList &books, int index)
{
    return books.at(index).toMap().value(QStringLiteral("title")).toString();
}

static QStringList groupBookTitles(const QVariantMap &group)
{
    QStringList titles;
    const QVariantList books = groupBooks(group);
    for (const QVariant &book : books) {
        titles.append(book.toMap().value(QStringLiteral("title")).toString());
    }
    return titles;
}

class CategoryEntriesModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSubjectPathCreatesVisibleAncestorLevels();
    void testMainSubjectSortRoleUsesFirstHierarchyLevel();
    void testTypeSortRoleUsesFileTypeThenTitle();
    void testTypeBookGroups();
    void testMainSubjectBookGroups();
    void testAuthorBookGroups();
    void testLastOpenedBookGroups();
    void testTitleBookGroups();
    void testBookGroupCacheInvalidatesOnAppend();
};

void CategoryEntriesModelTest::testSubjectPathCreatesVisibleAncestorLevels()
{
    CategoryEntriesModel model;
    BookEntry entry;
    entry.filename = QStringLiteral("/tmp/spirituality-buddhism.epub");
    entry.title = QStringLiteral("Buddhism Test");

    model.addCategoryEntry(QStringLiteral("Spirituality/Buddhism"), entry, CategoryEntriesModel::SubjectRole);

    const QVariantList entries = model.flatCategoryEntries();
    QCOMPARE(entries.size(), 2);

    const QVariantMap first = entries.at(0).toMap();
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Spirituality"));
    QCOMPARE(first.value(QStringLiteral("localizedTitle")).toString(), QStringLiteral("Spirituality"));
    QCOMPARE(first.value(QStringLiteral("subjectLevel")).toInt(), 1);
    QCOMPARE(first.value(QStringLiteral("categoryEntriesCount")).toInt(), 1);

    const QVariantMap second = entries.at(1).toMap();
    QCOMPARE(second.value(QStringLiteral("title")).toString(), QStringLiteral("Spirituality/Buddhism"));
    QCOMPARE(second.value(QStringLiteral("localizedTitle")).toString(), QStringLiteral("Buddhism"));
    QCOMPARE(second.value(QStringLiteral("localizedPathTitle")).toString(), QStringLiteral("Spirituality / Buddhism"));
    QCOMPARE(second.value(QStringLiteral("subjectLevel")).toInt(), 2);
    QCOMPARE(second.value(QStringLiteral("categoryEntriesCount")).toInt(), 1);
}

void CategoryEntriesModelTest::testMainSubjectSortRoleUsesFirstHierarchyLevel()
{
    CategoryEntriesModel model;
    BookEntry entry;
    entry.filename = QStringLiteral("/tmp/spirituality-buddhism.epub");
    entry.title = QStringLiteral("Buddhism Test");
    entry.genres = {QStringLiteral("Spirituality/Buddhism")};

    model.append(entry);

    QCOMPARE(model.data(model.index(0), CategoryEntriesModel::MainSubjectRole).toString(), QStringLiteral("Spirituality"));

    const QString sortKey = model.data(model.index(0), CategoryEntriesModel::MainSubjectSortRole).toString();
    QVERIFY(sortKey.startsWith(QStringLiteral("Spirituality\t")));
    QVERIFY(sortKey.endsWith(QStringLiteral("Buddhism Test")));
}

void CategoryEntriesModelTest::testTypeSortRoleUsesFileTypeThenTitle()
{
    CategoryEntriesModel model;

    BookEntry pdfEntry;
    pdfEntry.filename = QStringLiteral("/tmp/a-pdf.pdf");
    pdfEntry.title = QStringLiteral("A PDF");
    model.append(pdfEntry);

    BookEntry epubEntry;
    epubEntry.filename = QStringLiteral("/tmp/the-epub.epub");
    epubEntry.title = QStringLiteral("The EPUB");
    model.append(epubEntry);

    const QString epubSortKey = model.data(model.index(0), CategoryEntriesModel::TypeSortRole).toString();
    const QString pdfSortKey = model.data(model.index(1), CategoryEntriesModel::TypeSortRole).toString();

    QVERIFY(pdfSortKey.startsWith(QStringLiteral("1-pdf\t")));
    QVERIFY(epubSortKey.startsWith(QStringLiteral("0-epub\t")));
    QVERIFY(QString::localeAwareCompare(epubSortKey, pdfSortKey) < 0);
    QVERIFY(epubSortKey.endsWith(QStringLiteral("EPUB\tThe EPUB")));
}

void CategoryEntriesModelTest::testTypeBookGroups()
{
    CategoryEntriesModel model;

    BookEntry betaEpubEntry;
    betaEpubEntry.filename = QStringLiteral("/tmp/beta.epub");
    betaEpubEntry.title = QStringLiteral("Beta");
    model.append(betaEpubEntry);

    BookEntry pdfEntry;
    pdfEntry.filename = QStringLiteral("/tmp/a-pdf.pdf");
    pdfEntry.title = QStringLiteral("A PDF");
    model.append(pdfEntry);

    BookEntry alphaEpubEntry;
    alphaEpubEntry.filename = QStringLiteral("/tmp/the-alpha.epub");
    alphaEpubEntry.title = QStringLiteral("The Alpha");
    model.append(alphaEpubEntry);

    const QVariantList groups = model.typeBookGroups();
    QCOMPARE(groups.size(), 2);

    const QVariantMap epubGroup = groups.at(0).toMap();
    QCOMPARE(epubGroup.value(QStringLiteral("title")).toString(), QStringLiteral("Epub Documents"));
    QVERIFY(!epubGroup.contains(QStringLiteral("categoryEntriesModel")));
    const QVariantList epubBooks = groupBooks(epubGroup);
    QCOMPARE(epubBooks.size(), 2);
    QCOMPARE(groupBookTitle(epubBooks, 0), QStringLiteral("The Alpha"));
    QCOMPARE(groupBookTitle(epubBooks, 1), QStringLiteral("Beta"));

    const QVariantMap pdfGroup = groups.at(1).toMap();
    QCOMPARE(pdfGroup.value(QStringLiteral("title")).toString(), QStringLiteral("Pdf Documents"));
    QVERIFY(!pdfGroup.contains(QStringLiteral("categoryEntriesModel")));
    const QVariantList pdfBooks = groupBooks(pdfGroup);
    QCOMPARE(pdfBooks.size(), 1);
    QCOMPARE(groupBookTitle(pdfBooks, 0), QStringLiteral("A PDF"));

    const QVariantList filteredGroups = model.typeBookGroups(QStringLiteral("PDF"));
    QCOMPARE(filteredGroups.size(), 1);
    QCOMPARE(filteredGroups.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Pdf Documents"));
}

void CategoryEntriesModelTest::testMainSubjectBookGroups()
{
    CategoryEntriesModel model;

    BookEntry bhaktiEntry;
    bhaktiEntry.filename = QStringLiteral("/tmp/bhakti.epub");
    bhaktiEntry.title = QStringLiteral("Bhakti Yoga");
    bhaktiEntry.genres = {QStringLiteral("Spirituality/Yoga/Bhakti")};
    model.append(bhaktiEntry);

    BookEntry buddhismEntry;
    buddhismEntry.filename = QStringLiteral("/tmp/buddhism.epub");
    buddhismEntry.title = QStringLiteral("Buddhism");
    buddhismEntry.genres = {QStringLiteral("Spirituality/Buddhism")};
    model.append(buddhismEntry);

    BookEntry unassignedEntry;
    unassignedEntry.filename = QStringLiteral("/tmp/unassigned.epub");
    unassignedEntry.title = QStringLiteral("Unassigned");
    model.append(unassignedEntry);

    const QVariantList groups = model.mainSubjectBookGroups();
    QCOMPARE(groups.size(), 2);

    const QVariantMap spiritualityGroup = groups.at(0).toMap();
    QCOMPARE(spiritualityGroup.value(QStringLiteral("title")).toString(), QStringLiteral("Spirituality"));
    const QVariantList spiritualityBooks = spiritualityGroup.value(QStringLiteral("books")).toList();
    QCOMPARE(spiritualityBooks.size(), 2);
    QCOMPARE(spiritualityBooks.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Bhakti Yoga"));
    QCOMPARE(spiritualityBooks.at(1).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Buddhism"));
    QVERIFY(!spiritualityGroup.contains(QStringLiteral("categoryEntriesModel")));

    const QVariantMap unassignedGroup = groups.at(1).toMap();
    QCOMPARE(unassignedGroup.value(QStringLiteral("title")).toString(), QStringLiteral("No Main Topic"));
    const QVariantList unassignedBooks = groupBooks(unassignedGroup);
    QCOMPARE(unassignedBooks.size(), 1);

    const QVariantList filteredGroups = model.mainSubjectBookGroups(QStringLiteral("Bhakti"));
    QCOMPARE(filteredGroups.size(), 1);
    QCOMPARE(filteredGroups.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Spirituality"));
    const QVariantList filteredBooks = groupBooks(filteredGroups.at(0).toMap());
    QCOMPARE(filteredBooks.size(), 1);
    QCOMPARE(groupBookTitle(filteredBooks, 0), QStringLiteral("Bhakti Yoga"));
}

void CategoryEntriesModelTest::testAuthorBookGroups()
{
    CategoryEntriesModel model;

    BookEntry betaEntry;
    betaEntry.filename = QStringLiteral("/tmp/beta.epub");
    betaEntry.title = QStringLiteral("Beta");
    betaEntry.author = {QStringLiteral("Alice Author")};
    model.append(betaEntry);

    BookEntry alphaEntry;
    alphaEntry.filename = QStringLiteral("/tmp/alpha.epub");
    alphaEntry.title = QStringLiteral("The Alpha");
    alphaEntry.author = {QStringLiteral("Alice Author")};
    model.append(alphaEntry);

    BookEntry bobEntry;
    bobEntry.filename = QStringLiteral("/tmp/bob.epub");
    bobEntry.title = QStringLiteral("Buddhism");
    bobEntry.author = {QStringLiteral("Bob Author")};
    model.append(bobEntry);

    BookEntry noAuthorEntry;
    noAuthorEntry.filename = QStringLiteral("/tmp/no-author.epub");
    noAuthorEntry.title = QStringLiteral("No Writer");
    model.append(noAuthorEntry);

    const QVariantList groups = model.authorBookGroups();
    QCOMPARE(groups.size(), 3);

    const QVariantMap aliceGroup = groups.at(0).toMap();
    QCOMPARE(aliceGroup.value(QStringLiteral("title")).toString(), QStringLiteral("Alice Author"));
    const QVariantList aliceBooks = aliceGroup.value(QStringLiteral("books")).toList();
    QCOMPARE(aliceBooks.size(), 2);
    QCOMPARE(aliceBooks.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("The Alpha"));
    QCOMPARE(aliceBooks.at(1).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Beta"));
    QVERIFY(!aliceGroup.contains(QStringLiteral("categoryEntriesModel")));

    const QVariantMap bobGroup = groups.at(1).toMap();
    QCOMPARE(bobGroup.value(QStringLiteral("title")).toString(), QStringLiteral("Bob Author"));
    const QVariantList bobBooks = groupBooks(bobGroup);
    QCOMPARE(bobBooks.size(), 1);

    const QVariantMap noAuthorGroup = groups.at(2).toMap();
    QCOMPARE(noAuthorGroup.value(QStringLiteral("title")).toString(), QStringLiteral("No Author"));
    const QVariantList noAuthorBooks = groupBooks(noAuthorGroup);
    QCOMPARE(noAuthorBooks.size(), 1);

    const QVariantList filteredGroups = model.authorBookGroups(QStringLiteral("Beta"));
    QCOMPARE(filteredGroups.size(), 1);
    QCOMPARE(filteredGroups.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Alice Author"));
    const QVariantList filteredBooks = groupBooks(filteredGroups.at(0).toMap());
    QCOMPARE(filteredBooks.size(), 1);
    QCOMPARE(groupBookTitle(filteredBooks, 0), QStringLiteral("Beta"));
}

void CategoryEntriesModelTest::testLastOpenedBookGroups()
{
    CategoryEntriesModel model;
    const QDateTime now = QDateTime::currentDateTime();

    auto appendBook = [&model](const QString &title, const QString &filename, const QDateTime &lastOpenedTime) {
        BookEntry entry;
        entry.filename = filename;
        entry.title = title;
        entry.author = {QStringLiteral("Test Author")};
        entry.lastOpenedTime = lastOpenedTime;
        model.append(entry);
    };

    appendBook(QStringLiteral("Zulu Recent"), QStringLiteral("/tmp/zulu-recent.epub"), now.addSecs(-2 * 60 * 60));
    appendBook(QStringLiteral("Alpha Recent"), QStringLiteral("/tmp/alpha-recent.epub"), now.addSecs(-12 * 60 * 60));
    appendBook(QStringLiteral("Week Book"), QStringLiteral("/tmp/week.epub"), now.addDays(-3));
    appendBook(QStringLiteral("Two Week Book"), QStringLiteral("/tmp/two-week.epub"), now.addDays(-10));
    appendBook(QStringLiteral("Three Week Book"), QStringLiteral("/tmp/three-week.epub"), now.addDays(-17));
    appendBook(QStringLiteral("Four Week Book"), QStringLiteral("/tmp/four-week.epub"), now.addDays(-24));
    appendBook(QStringLiteral("Older Book"), QStringLiteral("/tmp/older.epub"), now.addDays(-35));
    appendBook(QStringLiteral("Never Book"), QStringLiteral("/tmp/never.epub"), {});

    const QVariantList groups = model.lastOpenedBookGroups();
    QCOMPARE(groups.size(), 7);
    QCOMPARE(groups.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Last 24 Hours"));
    QCOMPARE(groups.at(1).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Last 7 Days"));
    QCOMPARE(groups.at(2).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Last 2 Weeks"));
    QCOMPARE(groups.at(3).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Last 3 Weeks"));
    QCOMPARE(groups.at(4).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Last 4 Weeks"));
    QCOMPARE(groups.at(5).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Older than 4 Weeks"));
    QCOMPARE(groups.at(6).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Never Opened"));

    const QVariantList recentBooks = groupBooks(groups.at(0).toMap());
    QCOMPARE(recentBooks.size(), 2);
    QCOMPARE(groupBookTitle(recentBooks, 0), QStringLiteral("Zulu Recent"));
    QCOMPARE(groupBookTitle(recentBooks, 1), QStringLiteral("Alpha Recent"));

    const QVariantList neverBooks = groupBooks(groups.at(6).toMap());
    QCOMPARE(neverBooks.size(), 1);
    QCOMPARE(groupBookTitle(neverBooks, 0), QStringLiteral("Never Book"));

    const QVariantList filteredGroups = model.lastOpenedBookGroups(QStringLiteral("Three"));
    QCOMPARE(filteredGroups.size(), 1);
    QCOMPARE(filteredGroups.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Last 3 Weeks"));
    const QVariantList filteredBooks = groupBooks(filteredGroups.at(0).toMap());
    QCOMPARE(filteredBooks.size(), 1);
    QCOMPARE(groupBookTitle(filteredBooks, 0), QStringLiteral("Three Week Book"));
}

void CategoryEntriesModelTest::testTitleBookGroups()
{
    CategoryEntriesModel model;

    BookEntry betaEntry;
    betaEntry.filename = QStringLiteral("/tmp/beta.epub");
    betaEntry.title = QStringLiteral("Beta");
    model.append(betaEntry);

    BookEntry deltaEntry;
    deltaEntry.filename = QStringLiteral("/tmp/delta.epub");
    deltaEntry.title = QStringLiteral("Delta");
    model.append(deltaEntry);

    BookEntry englishArticleEntry;
    englishArticleEntry.filename = QStringLiteral("/tmp/the-book.epub");
    englishArticleEntry.title = QStringLiteral("The Book");
    model.append(englishArticleEntry);

    BookEntry englishIndefiniteArticleEntry;
    englishIndefiniteArticleEntry.filename = QStringLiteral("/tmp/a-book.epub");
    englishIndefiniteArticleEntry.title = QStringLiteral("A Book");
    model.append(englishIndefiniteArticleEntry);

    BookEntry englishAnArticleEntry;
    englishAnArticleEntry.filename = QStringLiteral("/tmp/an-apple.epub");
    englishAnArticleEntry.title = QStringLiteral("An Apple");
    model.append(englishAnArticleEntry);

    BookEntry masculineArticleEntry;
    masculineArticleEntry.filename = QStringLiteral("/tmp/der-baum.epub");
    masculineArticleEntry.title = QStringLiteral("Der Baum");
    model.append(masculineArticleEntry);

    BookEntry feminineArticleEntry;
    feminineArticleEntry.filename = QStringLiteral("/tmp/die-blume.epub");
    feminineArticleEntry.title = QStringLiteral("Die Blume");
    model.append(feminineArticleEntry);

    BookEntry neuterArticleEntry;
    neuterArticleEntry.filename = QStringLiteral("/tmp/das-buch.epub");
    neuterArticleEntry.title = QStringLiteral("Das Buch");
    model.append(neuterArticleEntry);

    BookEntry germanIndefiniteArticleEntry;
    germanIndefiniteArticleEntry.filename = QStringLiteral("/tmp/ein-baum.epub");
    germanIndefiniteArticleEntry.title = QStringLiteral("Ein Baum");
    model.append(germanIndefiniteArticleEntry);

    BookEntry germanFeminineIndefiniteArticleEntry;
    germanFeminineIndefiniteArticleEntry.filename = QStringLiteral("/tmp/eine-blume.epub");
    germanFeminineIndefiniteArticleEntry.title = QStringLiteral("Eine Blume");
    model.append(germanFeminineIndefiniteArticleEntry);

    BookEntry sriArticleEntry;
    sriArticleEntry.filename = QStringLiteral("/tmp/sri-aurobindo.epub");
    sriArticleEntry.title = QStringLiteral("Sri Aurobindo");
    model.append(sriArticleEntry);

    BookEntry shriArticleEntry;
    shriArticleEntry.filename = QStringLiteral("/tmp/shri-bhagavad-gita.epub");
    shriArticleEntry.title = QStringLiteral("Shri Bhagavad Gita");
    model.append(shriArticleEntry);

    BookEntry numberEntry;
    numberEntry.filename = QStringLiteral("/tmp/1984.epub");
    numberEntry.title = QStringLiteral("1984");
    model.append(numberEntry);

    BookEntry accentEntry;
    accentEntry.filename = QStringLiteral("/tmp/aether.epub");
    accentEntry.title = QStringLiteral("Äther");
    model.append(accentEntry);

    BookEntry symbolEntry;
    symbolEntry.filename = QStringLiteral("/tmp/symbol.epub");
    symbolEntry.title = QStringLiteral("...");
    model.append(symbolEntry);

    const QVariantList groups = model.titleBookGroups();
    QCOMPARE(groups.size(), 5);
    QCOMPARE(groups.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("0-9"));
    QCOMPARE(groups.at(1).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("A"));
    QCOMPARE(groups.at(2).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("B, C"));
    QCOMPARE(groups.at(3).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z"));
    QCOMPARE(groups.at(4).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("#"));

    const QStringList accentTitles = groupBookTitles(groups.at(1).toMap());
    QCOMPARE(accentTitles.size(), 3);

    QVERIFY(accentTitles.contains(QStringLiteral("An Apple")));
    QVERIFY(accentTitles.contains(QStringLiteral("Sri Aurobindo")));
    QVERIFY(accentTitles.contains(QStringLiteral("Äther")));

    const QStringList bTitles = groupBookTitles(groups.at(2).toMap());
    QCOMPARE(bTitles.size(), 9);

    QVERIFY(bTitles.contains(QStringLiteral("Beta")));
    QVERIFY(bTitles.contains(QStringLiteral("The Book")));
    QVERIFY(bTitles.contains(QStringLiteral("A Book")));
    QVERIFY(bTitles.contains(QStringLiteral("Der Baum")));
    QVERIFY(bTitles.contains(QStringLiteral("Die Blume")));
    QVERIFY(bTitles.contains(QStringLiteral("Das Buch")));
    QVERIFY(bTitles.contains(QStringLiteral("Ein Baum")));
    QVERIFY(bTitles.contains(QStringLiteral("Eine Blume")));
    QVERIFY(bTitles.contains(QStringLiteral("Shri Bhagavad Gita")));

    const QVariantList filteredGroups = model.titleBookGroups(QStringLiteral("Beta"));
    QCOMPARE(filteredGroups.size(), 1);
    QCOMPARE(filteredGroups.at(0).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z"));

    const QVariantList filteredBooks = groupBooks(filteredGroups.at(0).toMap());
    QCOMPARE(filteredBooks.size(), 1);
    QCOMPARE(groupBookTitle(filteredBooks, 0), QStringLiteral("Beta"));
}

void CategoryEntriesModelTest::testBookGroupCacheInvalidatesOnAppend()
{
    CategoryEntriesModel model;

    BookEntry alphaEntry;
    alphaEntry.filename = QStringLiteral("/tmp/alpha.epub");
    alphaEntry.title = QStringLiteral("Alpha");
    model.append(alphaEntry);

    const QVariantList firstGroups = model.titleBookGroups();
    QCOMPARE(firstGroups.size(), 1);
    const QVariantList firstBooks = groupBooks(firstGroups.at(0).toMap());
    QCOMPARE(firstBooks.size(), 1);
    QCOMPARE(groupBookTitle(firstBooks, 0), QStringLiteral("Alpha"));

    const QVariantList cachedGroups = model.titleBookGroups();
    QCOMPARE(cachedGroups.size(), 1);
    const QVariantList cachedBooks = groupBooks(cachedGroups.at(0).toMap());
    QCOMPARE(cachedBooks.size(), 1);
    QCOMPARE(groupBookTitle(cachedBooks, 0), QStringLiteral("Alpha"));

    BookEntry anotherEntry;
    anotherEntry.filename = QStringLiteral("/tmp/another.epub");
    anotherEntry.title = QStringLiteral("Another");
    model.append(anotherEntry);

    const QVariantList refreshedGroups = model.titleBookGroups();
    QCOMPARE(refreshedGroups.size(), 1);
    const QVariantList refreshedBooks = groupBooks(refreshedGroups.at(0).toMap());
    QCOMPARE(refreshedBooks.size(), 2);
    QCOMPARE(groupBookTitle(refreshedBooks, 0), QStringLiteral("Alpha"));
    QCOMPARE(groupBookTitle(refreshedBooks, 1), QStringLiteral("Another"));
}

QTEST_MAIN(CategoryEntriesModelTest)
#include "categoryentriesmodeltest.moc"
