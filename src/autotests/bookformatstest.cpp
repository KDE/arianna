// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookformats.h"
#include "bookdatabase.h"
#include "booklistmodel.h"
#include "config.h"
#include "librarymaintenance.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QAbstractItemModelTester>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>
Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
BookEntry book(const QString &file)
{
    BookEntry result;
    result.filename = file;
    result.title = QStringLiteral("The Book");
    result.author = {QStringLiteral("An Author")};
    result.uniqueIdentifier = result.identifier = file;
    result.genres = {QStringLiteral("Fiction/Adventure")};
    result.currentLocation = file.endsWith(QLatin1String("pdf")) ? QStringLiteral("8") : QStringLiteral("chapter.xhtml#here");
    return result;
}
QVariantMap plan(const QVariantMap &report)
{
    for (const auto &value : report.value(QStringLiteral("results")).toList()) {
        const auto candidate = value.toMap().value(QStringLiteral("formatGroupPlan")).toMap();
        if (!candidate.isEmpty())
            return candidate;
    }
    return {};
}
QQuickItem *visualItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name)
        return root;
    for (auto *child : root->childItems())
        if (auto *item = visualItem(child, name))
            return item;
    return nullptr;
}
}

class BookFormatsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_data;
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_data.isValid());
        qputenv("XDG_DATA_HOME", QFile::encodeName(m_data.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_data.path()));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_data.path()));
        QCoreApplication::setApplicationName(QStringLiteral("arianna-bookformatstest"));
        KLocalizedString::setApplicationDomain("arianna");
        BookDatabase::self().loadEntries();
    }
    void init()
    {
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        for (const auto &table : {"books", "annotations", "references", "book_current_state"})
            QVERIFY(query.exec(QStringLiteral("DELETE FROM \"%1\"").arg(QString::fromLatin1(table))));
        query.finish();
        connection.close();
    }
    void matchingRequiresTitleAuthorAndDifferentFormats()
    {
        auto epub = book(QStringLiteral("/missing/a.epub"));
        auto pdf = book(QStringLiteral("/missing/a.PDF"));
        QCOMPARE(BookFormats::candidates({epub, pdf}).size(), 1);
        pdf.title = QStringLiteral("  THE   BOOK ");
        pdf.author = {QStringLiteral("AN AUTHOR ")};
        QCOMPARE(BookFormats::candidates({epub, pdf}).size(), 1);
        pdf.author.clear();
        QVERIFY(BookFormats::candidates({epub, pdf}).isEmpty());
        pdf.author = {QStringLiteral("Different Author")};
        QVERIFY(BookFormats::candidates({epub, pdf}).isEmpty());
        pdf = epub;
        pdf.filename = QStringLiteral("/missing/b.epub");
        QVERIFY(BookFormats::candidates({epub, pdf}).isEmpty());
        pdf.filename = QStringLiteral("/missing/a.pdf");
        epub.title.clear();
        pdf.title.clear();
        QVERIFY(BookFormats::candidates({epub, pdf}).isEmpty());
    }
    void combinePersistsCountsAndIndependentData()
    {
        auto epub = book(QStringLiteral("/missing/a.epub"));
        auto pdf = book(QStringLiteral("/missing/a.pdf"));
        auto &db = BookDatabase::self();
        db.addEntry(epub);
        db.addEntry(pdf);
        db.saveAnnotation(epub.identifier,
                          {{QStringLiteral("annotationId"), QStringLiteral("note")},
                           {QStringLiteral("anchorId"), QStringLiteral("note")},
                           {QStringLiteral("note"), QStringLiteral("Keep me")}});
        db.saveReference({{QStringLiteral("sourceBookId"), epub.identifier},
                          {QStringLiteral("sourceAnchorId"), QStringLiteral("source")},
                          {QStringLiteral("targetBookId"), pdf.identifier},
                          {QStringLiteral("targetLocation"), QStringLiteral("page=1")}});
        const auto notes = db.loadAnnotations(epub.identifier), refs = db.loadReferences(epub.identifier);
        QCOMPARE(notes.size(), 1);
        QCOMPARE(refs.size(), 1);
        BookListModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.componentComplete();
        QTRY_VERIFY(model.cacheLoaded());
        QCOMPARE(model.count(), 2);
        LibraryMaintenance checker;
        checker.start(QStringLiteral("database"));
        QTRY_VERIFY(!checker.running());
        const auto grouping = plan(checker.report());
        QVERIFY(!grouping.isEmpty());
        QCOMPARE(checker.report().value(QStringLiteral("books")).toInt(), 2);
        QVERIFY(db.loadEntry(epub.filename)->formatGroup.isEmpty()); // Check alone never combines.
        QVERIFY(checker.combineFormats(grouping).value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QTRY_COMPARE(model.count(), 1);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.knownBookFiles().size(), 2);
        QCOMPARE(model.newlyAddedCategoryModel()->bookCount(), 1);
        QCOMPARE(checker.report().value(QStringLiteral("books")).toInt(), 1);
        QCOMPARE(checker.report().value(QStringLiteral("files")).toInt(), 2);
        QVERIFY(plan(checker.report()).isEmpty());
        QVERIFY(!db.loadEntry(epub.filename)->formatGroup.isEmpty());
        QCOMPARE(db.loadEntry(epub.filename)->formatGroup, db.loadEntry(pdf.filename)->formatGroup);
        QCOMPARE(db.loadAnnotations(epub.identifier), notes);
        QCOMPARE(db.loadReferences(epub.identifier), refs);
        QCOMPARE(model.bookFromFile(epub.filename)->currentLocation, epub.currentLocation);
        QCOMPARE(model.bookFromFile(pdf.filename)->currentLocation, pdf.currentLocation);
        QVERIFY(!checker.combineFormats(grouping).value(QStringLiteral("success")).toBool());

        // The preferred format follows actual reader use and survives reload.
        model.setBookData(pdf.filename, QStringLiteral("lastOpenedTime"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        QCOMPARE(model.getBookEntry(0)->filename, pdf.filename);
        QCOMPARE(model.getBookEntry(0)->formatVariants.size(), 2);
        BookListModel reloaded;
        reloaded.componentComplete();
        QTRY_VERIFY(reloaded.cacheLoaded());
        QCOMPARE(reloaded.count(), 1);
        QCOMPARE(reloaded.getBookEntry(0)->filename, pdf.filename);
        reloaded.populateSubjectCategoryModel();
        QTRY_VERIFY(reloaded.subjectCategoryModelPopulated());
        auto *subjects = qobject_cast<CategoryEntriesModel *>(reloaded.subjectCategoryModel());
        QVERIFY(subjects);
        const auto categories = subjects->flatCategoryEntries();
        QCOMPARE(categories.size(), 2);
        for (const auto &category : categories)
            QCOMPARE(category.toMap().value(QStringLiteral("categoryEntriesCount")).toInt(), 1);
    }
    void stalePlanIsRejectedAtomically()
    {
        auto &db = BookDatabase::self();
        const auto epub = book(QStringLiteral("/missing/a.epub"));
        auto pdf = book(QStringLiteral("/missing/a.pdf"));
        db.addEntry(epub);
        db.addEntry(pdf);
        const auto grouping = BookFormats::candidates(db.loadEntries()).first().toMap();
        pdf.author = {QStringLiteral("Someone Else")};
        db.updateEntry(pdf);
        QVERIFY(!db.combineBookFormats(grouping));
        QVERIFY(db.loadEntry(epub.filename)->formatGroup.isEmpty());
        QVERIFY(db.loadEntry(pdf.filename)->formatGroup.isEmpty());
    }
    void maintenanceButtonUpdatesVisibleLibrary()
    {
        QTemporaryDir files;
        auto epub = book(files.filePath(QStringLiteral("book.epub")));
        auto pdf = book(files.filePath(QStringLiteral("book.pdf")));
        for (const auto &entry : {epub, pdf}) {
            QFile file(entry.filename);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(entry.filename.toUtf8()) > 0);
            file.close();
            BookDatabase::self().addEntry(entry);
        }
        Config::self()->setBookLocations({m_data.path()});
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/org/kde/arianna/qml/Main.qml")));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        quickWindow->resize(1200, 900);
        quickWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
        auto *root = quickWindow->contentItem();
        auto *checker = window->findChild<LibraryMaintenance *>();
        QVERIFY(checker);
        QQuickItem *grid = nullptr;
        QTRY_VERIFY(grid = visualItem(root, QStringLiteral("libraryBookGrid")));
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        auto *action = visualItem(root, QStringLiteral("maintenancePageButton"));
        QVERIFY(action);
        QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
        QTRY_VERIFY(!checker->running() && !checker->report().isEmpty());
        auto *results = visualItem(root, QStringLiteral("maintenanceResults"));
        QVERIFY(results);
        QVERIFY(QMetaObject::invokeMethod(results, "positionViewAtEnd"));
        QQuickItem *combine = nullptr;
        // The non-actionable rows contain invisible buttons with the same objectName.
        const auto findButton = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == QLatin1String("combineBookFormats") && item->isVisible())
                return item;
            for (auto *child : item->childItems())
                if (auto *found = self(self, child))
                    return found;
            return nullptr;
        };
        QTRY_VERIFY(combine = findButton(findButton, root));
        QTRY_VERIFY(combine->isEnabled() && combine->width() > 100 && combine->height() > 20);
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier, combine->mapToScene(QPointF(combine->width() / 2, combine->height() / 2)).toPoint());
        QTRY_VERIFY(!checker->running() && checker->report().value(QStringLiteral("books")).toInt() == 1);
        auto *maintenance = visualItem(root, QStringLiteral("maintenancePage"));
        QVERIFY(maintenance);
        QVERIFY(!maintenance->property("exportMessage").toString().isEmpty());
        auto *home = visualItem(root, QStringLiteral("goHomeButton"));
        QVERIFY(home);
        QVERIFY(QMetaObject::invokeMethod(home, "triggered"));
        QTRY_VERIFY(!maintenance->property("visible").toBool());
        QTRY_VERIFY(grid = visualItem(root, QStringLiteral("libraryBookGrid")));
        QTRY_COMPARE(grid->property("count").toInt(), 1);
        QTRY_VERIFY(quickWindow->title().contains(QStringLiteral("(1)")));
        QQuickItem *epubTag = nullptr, *pdfTag = nullptr;
        QTRY_VERIFY(epubTag = visualItem(root, QStringLiteral("formatTagEPUB")));
        QTRY_VERIFY(pdfTag = visualItem(root, QStringLiteral("formatTagPDF")));
        QTRY_VERIFY(epubTag->isVisible() && pdfTag->isVisible());
        QTest::mouseClick(quickWindow,
                          Qt::LeftButton,
                          Qt::ControlModifier,
                          epubTag->mapToScene(QPointF(epubTag->width() / 2, epubTag->height() / 2)).toPoint());
        auto *chooser = window->findChild<QObject *>(QStringLiteral("bookFormatDialog"));
        QVERIFY(chooser);
        QTRY_VERIFY(chooser->property("opened").toBool());
        QTest::keyClick(quickWindow, Qt::Key_Escape);
        QTRY_VERIFY(!chooser->property("visible").toBool());
    }
    void groupedModelUpdatesRemovesAndResolvesEachFile()
    {
        CategoryEntriesModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto epub = book(QStringLiteral("/missing/a.epub"));
        auto pdf = book(QStringLiteral("/missing/a.pdf"));
        epub.formatGroup = pdf.formatGroup = QStringLiteral("group");
        epub.thumbnail = QStringLiteral("epub-cover");
        pdf.thumbnail = QStringLiteral("pdf-cover");
        model.append(pdf);
        model.append(epub);
        QCOMPARE(model.bookCount(), 1);
        QCOMPARE(model.indexOfFile(epub.filename), model.indexOfFile(pdf.filename));
        QCOMPARE(model.leafModelForEntry(pdf), &model);
        QCOMPARE(model.getBookEntry(0)->filename, epub.filename); // EPUB is the initial default.
        pdf.lastOpenedTime = QDateTime::currentDateTimeUtc();
        Q_EMIT model.entryDataUpdated(pdf);
        QCOMPARE(model.getBookEntry(0)->filename, pdf.filename);
        QCOMPARE(model.getBookEntry(0)->thumbnail, epub.thumbnail);
        QCOMPARE(model.bookFromFile(pdf.filename)->thumbnail, pdf.thumbnail);
        QCOMPARE(model.typeBookGroups().size(), 1);
        QCOMPARE(model.typeBookGroups().first().toMap().value(QStringLiteral("bookCount")).toInt(), 1);
        QCOMPARE(model.titleBookGroups().first().toMap().value(QStringLiteral("bookCount")).toInt(), 1);
        Q_EMIT model.entryRemoved(pdf);
        QCOMPARE(model.bookCount(), 1);
        QCOMPARE(model.getBookEntry(0)->filename, epub.filename);
        QVERIFY(model.getBookEntry(0)->formatVariants.isEmpty());
        QVERIFY(!model.bookFromFile(pdf.filename));
        model.append(pdf);
        pdf.title = QStringLiteral("Changed title");
        Q_EMIT model.entryDataUpdated(pdf);
        QCOMPARE(model.bookCount(), 2);
        QCOMPARE(model.bookFromFile(pdf.filename)->title, pdf.title);
    }
    void coverClickAndControlClickChooseFormat()
    {
        auto epub = book(QStringLiteral("/missing/a.epub"));
        auto pdf = book(QStringLiteral("/missing/a.pdf"));
        pdf.lastOpenedTime = QDateTime::currentDateTimeUtc();
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testBook"), QVariant::fromValue(BookFormats::combined({epub, pdf})));
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import org.kde.arianna
Window {
    id: win
    width: 800; height: 700; visible: true
    property string openedFile: ""
    property string openedLocation: ""
    property int opens: 0
    GridBrowserDelegate {
        objectName: "cover"
        width: 200; height: 270; x: 50; y: 50
        imageUrl: ""; iconName: ""; mainText: testBook.title; secondaryText: ""; currentProgress: 0
        formatVariants: testBook.formatVariants
        onActivated: modifiers => chooser.openBook(testBook, modifiers)
    }
    BookFormatDialog {
        id: chooser
        onBookChosen: entry => {
            win.openedFile = entry.filename; win.openedLocation = entry.currentLocation; win.opens++;
        }
    }
})",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));
        auto *root = quickWindow->contentItem();
        auto *cover = visualItem(root, QStringLiteral("cover"));
        auto *epubTag = visualItem(root, QStringLiteral("formatTagEPUB"));
        auto *pdfTag = visualItem(root, QStringLiteral("formatTagPDF"));
        QVERIFY(cover && epubTag && pdfTag);
        QVERIFY(epubTag->isVisible() && pdfTag->isVisible());
        QVERIFY(epubTag->mapToScene(QPointF()).x() < pdfTag->mapToScene(QPointF()).x());
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier, QPoint(130, 130));
        QTRY_COMPARE(window->property("opens").toInt(), 1);
        QCOMPARE(window->property("openedFile").toString(), pdf.filename);
        QCOMPARE(window->property("openedLocation").toString(), pdf.currentLocation);
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::ControlModifier, QPoint(130, 130));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("bookFormatDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(window->property("opens").toInt(), 1); // Ctrl must not also open the default.
        auto *choose = visualItem(root, QStringLiteral("chooseFormatEPUB"));
        QVERIFY(choose);
        QTRY_VERIFY(choose->isVisible() && choose->width() > dialog->property("width").toReal() / 2 && choose->height() > 30);
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier, choose->mapToScene(QPointF(choose->width() / 2, choose->height() / 2)).toPoint());
        QTRY_COMPARE(window->property("opens").toInt(), 2);
        QCOMPARE(window->property("openedFile").toString(), epub.filename);
        QCOMPARE(window->property("openedLocation").toString(), epub.currentLocation);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::ControlModifier, QPoint(130, 130));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(quickWindow, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(window->property("opens").toInt(), 2);
    }
    void lockedPdfWithoutThumbnailShowsCoverIcon()
    {
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import org.kde.arianna
Window {
    width: 240; height: 300; visible: true
    GridBrowserDelegate {
        objectName: "lockedPdfBook"
        anchors.fill: parent
        imageUrl: "file:" + "/" + "/undefined"
        iconName: "application-epub+zip"
        mainText: "Locked PDF"
        secondaryText: ""
        currentProgress: 0
        showPasswordBadge: true
        typeIconSource: ""
        formatVariants: []
    }
})",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));

        auto *root = quickWindow->contentItem();
        auto *lockedCover = visualItem(root, QStringLiteral("lockedPdfCoverIcon"));
        auto *fallbackIcon = visualItem(root, QStringLiteral("bookCoverFallbackIcon"));
        auto *passwordBadge = visualItem(root, QStringLiteral("passwordBadge"));
        QVERIFY(lockedCover);
        QVERIFY(fallbackIcon);
        QVERIFY(passwordBadge);
        QTRY_VERIFY(lockedCover->property("visible").toBool());
        QTRY_COMPARE(lockedCover->property("status").toInt(), 1);
        QVERIFY(lockedCover->property("source").toUrl().toString().endsWith(QStringLiteral("/icons/locked-pdf-cover.svg")));
        QVERIFY(!fallbackIcon->isVisible());
        QVERIFY(!passwordBadge->isVisible());
    }
};
QTEST_MAIN(BookFormatsTest)
#include "bookformatstest.moc"
