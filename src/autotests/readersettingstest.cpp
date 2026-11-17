// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookdatabase.h"
#include "booklistmodel.h"
#include "config.h"
#include "fakespeechdispatcher.h"
#include "okularpdfpageitem.h"
#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QtQml/QQmlExtensionPlugin>
#include <memory>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QQuickItem *namedItem(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems()) {
        if (auto *result = namedItem(child, name)) return result;
    }
    return nullptr;
}

void wheel(QQuickItem *item, const QPoint &delta)
{
    const QPointF point = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    auto *window = item->window();
    QWheelEvent event(point, window->mapToGlobal(point.toPoint()), {}, delta, Qt::NoButton,
                      Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
}
}

class ReaderSettingsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        auto &database = BookDatabase::self();
        if (qEnvironmentVariableIsSet("ARIANNA_TEST_LEGACY_DATABASE")) {
            // A pre-upgrade schema with no pageWidthDistance column.
            auto legacy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy"));
            legacy.setDatabaseName(database.databaseFileName());
            QVERIFY(legacy.open());
            QSqlQuery query(legacy);
            QStringList definitions{QStringLiteral("fileName TEXT PRIMARY KEY"), QStringLiteral("zoomLevel REAL DEFAULT 1.0")};
            const QStringList fields = QStringLiteral("fileTitle title subjects genres keywords characters description series seriesNumbers seriesVolumes author "
                "publisher created lastOpenedTime thumbnail passwordProtected comment tags rating locations pdfNotInvertedRegions currentLocation currentProgress "
                "pageMode bookVersion rights source identifier uniqueIdentifier formatGroup language").split(QLatin1Char(' '));
            for (const auto &field : fields)
                definitions.append(field + QStringLiteral(" TEXT"));
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE books (") + definitions.join(QLatin1Char(',')) + QLatin1Char(')')));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO books (fileName,title,zoomLevel,pageMode) VALUES ('/missing/legacy.epub','Legacy',1.5,'two')")));
            query.finish();
            legacy.close();
        }
        bool loaded = false;
        database.loadEntries(&loaded);
        QVERIFY(loaded);
        if (qEnvironmentVariableIsSet("ARIANNA_TEST_LEGACY_DATABASE")) {
            const auto legacy = database.loadEntry(QStringLiteral("/missing/legacy.epub"));
            QVERIFY(legacy);
            QCOMPARE(legacy->pageWidthDistance, -1);
            QCOMPARE(legacy->zoomLevel, 1.5);
            QCOMPARE(legacy->pageMode, QStringLiteral("two"));
            QVERIFY(!legacy->lastClosedTime.isValid());
            QVERIFY(!legacy->lastClosedOpenedTime.isValid());
        }
    }

    void settingsRemainIndependentAfterReload()
    {
        auto &database = BookDatabase::self();
        BookEntry epub;
        epub.filename = QStringLiteral("/missing/layout.epub");
        epub.title = QStringLiteral("EPUB layout");
        BookEntry pdf;
        pdf.filename = QStringLiteral("/missing/layout.pdf");
        pdf.title = QStringLiteral("PDF layout");
        database.addEntry(epub);
        database.addEntry(pdf);
        QCOMPARE(database.loadEntry(epub.filename)->pageWidthDistance, -1);
        QCOMPARE(database.loadEntry(pdf.filename)->pageWidthDistance, -1);
        {
            BookListModel model;
            model.componentComplete();
            QTRY_VERIFY(model.cacheLoaded());
            model.setBookData(epub.filename, QStringLiteral("pageWidthDistance"), QStringLiteral("525"));
            model.setBookData(epub.filename, QStringLiteral("zoomLevel"), QStringLiteral("1.25"));
            model.setBookData(epub.filename, QStringLiteral("pageMode"), QStringLiteral("two"));
            QCOMPARE(database.loadEntry(pdf.filename)->pageWidthDistance, -1);
            model.setBookData(pdf.filename, QStringLiteral("pageWidthDistance"), QStringLiteral("0"));
            model.setBookData(pdf.filename, QStringLiteral("zoomLevel"), QStringLiteral("0.75"));
        }
        BookListModel reopened;
        reopened.componentComplete();
        QTRY_VERIFY(reopened.cacheLoaded());
        const auto epubEntry = reopened.bookEntryFromFile(epub.filename);
        const auto pdfEntry = reopened.bookEntryFromFile(pdf.filename);
        QCOMPARE(epubEntry.pageWidthDistance, 525);
        QCOMPARE(epubEntry.zoomLevel, 1.25);
        QCOMPARE(epubEntry.pageMode, QStringLiteral("two"));
        QCOMPARE(pdfEntry.pageWidthDistance, 0); // Zero is a saved gap, not the default sentinel.
        QCOMPARE(pdfEntry.zoomLevel, 0.75);
        const auto index = reopened.index(reopened.indexOfFile(epub.filename), 0);
        QCOMPARE(reopened.data(index, CategoryEntriesModel::PageWidthDistanceRole).toInt(), 525);
        auto updated = epubEntry;
        updated.title = QStringLiteral("Updated metadata");
        database.updateEntry(updated);
        QCOMPARE(database.loadEntry(epub.filename)->pageWidthDistance, 525);
        const auto inserted = database.loadEntry(pdf.filename);
        QCOMPARE(inserted->pageWidthDistance, 0);
    }

    void maintenanceBackupSettings()
    {
        QVERIFY(Config::defaultMathJaxSupportValue());
        QVERIFY(Config::mathJaxSupport());
        QCOMPARE(Config::defaultMathJaxInlineScaleValue(), 100);
        QCOMPARE(Config::mathJaxInlineScale(), 100);
        QCOMPARE(Config::defaultBackupOnImageInversionValue(), false);
        QCOMPARE(Config::backupOnImageInversion(), false);
        QVERIFY(Config::defaultBackupOnAnchoringValue());
        QVERIFY(Config::backupOnAnchoring());
        QTemporaryDir directory;
        FakeSpeechDispatcher server(directory.filePath(QStringLiteral("speech.sock")));
        QVERIFY(server.startServer());
        qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(directory.filePath(QStringLiteral("speech.sock")))));
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 900; height: 900; visible: true
                pageStack.initialPage: SettingsPage { objectName: "settingsPage" }
            }
        )", QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *page = object->findChild<QObject *>(QStringLiteral("settingsPage"));
        auto *tabs = object->findChild<QObject *>(QStringLiteral("settingsTabs"));
        auto *tab = object->findChild<QQuickItem *>(QStringLiteral("maintenanceSettingsTabButton"));
        auto *header = object->findChild<QQuickItem *>(QStringLiteral("safetyBackupsHeader"));
        auto *setting = object->findChild<QQuickItem *>(QStringLiteral("imageInversionBackupSwitch"));
        auto *anchorSetting = object->findChild<QQuickItem *>(QStringLiteral("anchoringBackupSwitch"));
        auto *mathSetting = object->findChild<QQuickItem *>(QStringLiteral("mathJaxSupportSwitch"));
        auto *mathDisplay = namedItem(window->contentItem(), QStringLiteral("mathJaxDisplaySettings"));
        auto *mathScale = namedItem(window->contentItem(), QStringLiteral("mathJaxScaleSlider"));
        auto *mathInlineScale = namedItem(window->contentItem(), QStringLiteral("mathJaxInlineScaleSlider"));
        auto *mathRight = namedItem(window->contentItem(), QStringLiteral("mathJaxAlignment_right"));
        auto *mathCache = namedItem(window->contentItem(), QStringLiteral("mathJaxFontCache_none"));
        auto *mathInline = namedItem(window->contentItem(), QStringLiteral("mathJaxBreakInlineSwitch"));
        auto *mathOverflow = namedItem(window->contentItem(), QStringLiteral("mathJaxOverflowChoice"));
        QVERIFY(page && tabs && tab && header && setting && anchorSetting);
        QVERIFY(mathDisplay && mathScale && mathInlineScale && mathRight && mathCache && mathInline && mathOverflow);
        QVERIFY(mathDisplay->isEnabled());
        mathScale->forceActiveFocus();
        for (int i = 0; i < 3; ++i) QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(Config::mathJaxScale(), 115);
        QCOMPARE(Config::mathJaxInlineScale(), 100);
        mathInlineScale->forceActiveFocus();
        for (int i = 0; i < 2; ++i) QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(Config::mathJaxInlineScale(), 90);
        QCOMPARE(Config::mathJaxScale(), 115);
        mathRight->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(Config::mathJaxAlignment(), QStringLiteral("right"));
        mathCache->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(Config::mathJaxFontCache(), QStringLiteral("none"));
        mathInline->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!Config::mathJaxBreakInline());
        mathOverflow->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(Config::mathJaxOverflow(), QStringLiteral("scroll"));
        QVERIFY(mathSetting && mathSetting->isVisible());
        QCOMPARE(mathSetting->property("text").toString(), i18nc("@option:check", "mathjax Support"));
        QVERIFY(mathSetting->property("checked").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            mathSetting->mapToScene(QPointF(mathSetting->width() / 2, mathSetting->height() / 2)).toPoint());
        QTRY_VERIFY(!Config::mathJaxSupport());
        QVERIFY(!mathDisplay->isEnabled() && !mathScale->isEnabled() && !mathInlineScale->isEnabled() && !mathRight->isEnabled());
        KConfig saved(Config::self()->config()->name(), KConfig::SimpleConfig);
        QVERIFY(!KConfigGroup(&saved, QStringLiteral("Appearance")).readEntry(QStringLiteral("mathJaxSupport"), true));
        QCOMPARE(tabs->property("count").toInt(), 5);
        QCOMPARE(tab->property("text").toString(), i18nc("@title:tab", "Maintenance"));
        QCOMPARE(header->property("title").toString(), i18nc("@title:group", "Create safety backups"));
        QCOMPARE(setting->property("text").toString(), i18nc("@option:check", "When inverting images"));
        QCOMPARE(anchorSetting->property("text").toString(), i18nc("@option:check", "When anchoring"));
        QVERIFY(anchorSetting->property("checked").toBool());
        QVERIFY(page->property("readerSettingsVisible").toBool());
        QVERIFY(!setting->isVisible());
        QVERIFY(!setting->property("checked").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            tab->mapToScene(QPointF(tab->width() / 2, tab->height() / 2)).toPoint());
        QTRY_VERIFY(page->property("maintenanceSettingsVisible").toBool());
        QTRY_VERIFY(header->isVisible() && setting->isVisible() && anchorSetting->isVisible());
        QVERIFY(!mathSetting->isVisible());
        QVERIFY(!page->property("readerSettingsVisible").toBool());
        // Let the form layout update before mapping the switch's click position.
        QTest::qWait(50);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            setting->mapToScene(QPointF(setting->width() / 2, setting->height() / 2)).toPoint());
        QTRY_VERIFY(Config::backupOnImageInversion());
        QVERIFY(setting->property("checked").toBool());
        saved.reparseConfiguration();
        QVERIFY(KConfigGroup(&saved, QStringLiteral("Maintenance")).readEntry(QStringLiteral("backupOnImageInversion"), false));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            anchorSetting->mapToScene(QPointF(anchorSetting->width() / 2, anchorSetting->height() / 2)).toPoint());
        QTRY_VERIFY(!Config::backupOnAnchoring());
        saved.reparseConfiguration();
        QVERIFY(!KConfigGroup(&saved, QStringLiteral("Maintenance")).readEntry(QStringLiteral("backupOnAnchoring"), true));
        object.reset();
        Config::setBackupOnImageInversion(false);
        Config::setBackupOnAnchoring(true);
        Config::setMathJaxSupport(true);
        Config::self()->load();
        QVERIFY(Config::backupOnImageInversion());
        QVERIFY(!Config::backupOnAnchoring());
        QVERIFY(!Config::mathJaxSupport());
        QCOMPARE(Config::mathJaxScale(), 115);
        QCOMPARE(Config::mathJaxInlineScale(), 90);
        QCOMPARE(Config::mathJaxAlignment(), QStringLiteral("right"));
        QCOMPARE(Config::mathJaxFontCache(), QStringLiteral("none"));
        QCOMPARE(Config::mathJaxOverflow(), QStringLiteral("scroll"));
        QVERIFY(!Config::mathJaxBreakInline());
        object.reset(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(object.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        tabs = object->findChild<QObject *>(QStringLiteral("settingsTabs"));
        tab = object->findChild<QQuickItem *>(QStringLiteral("maintenanceSettingsTabButton"));
        setting = object->findChild<QQuickItem *>(QStringLiteral("imageInversionBackupSwitch"));
        anchorSetting = object->findChild<QQuickItem *>(QStringLiteral("anchoringBackupSwitch"));
        mathSetting = object->findChild<QQuickItem *>(QStringLiteral("mathJaxSupportSwitch"));
        QVERIFY(tabs && tab && setting && anchorSetting);
        QVERIFY(!anchorSetting->property("checked").toBool());
        QVERIFY(mathSetting && mathSetting->isVisible());
        QVERIFY(!mathSetting->property("checked").toBool());
        QVERIFY(setting->property("checked").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            tab->mapToScene(QPointF(tab->width() / 2, tab->height() / 2)).toPoint());
        QTRY_COMPARE(tabs->property("currentIndex").toInt(), 3);
        QTRY_VERIFY(setting->isVisible());
        QTest::qWait(50);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            setting->mapToScene(QPointF(setting->width() / 2, setting->height() / 2)).toPoint());
        QTRY_VERIFY(!Config::backupOnImageInversion());
        saved.reparseConfiguration();
        QVERIFY(!KConfigGroup(&saved, QStringLiteral("Maintenance")).readEntry(QStringLiteral("backupOnImageInversion"), false));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            anchorSetting->mapToScene(QPointF(anchorSetting->width() / 2, anchorSetting->height() / 2)).toPoint());
        QTRY_VERIFY(Config::backupOnAnchoring());
        object.reset();
        Config::setBackupOnImageInversion(true);
        Config::setBackupOnAnchoring(false);
        Config::self()->load();
        QVERIFY(!Config::backupOnImageInversion());
        QVERIFY(Config::backupOnAnchoring());
        Config::setMathJaxSupport(true);
        Config::setMathJaxScale(100);
        Config::setMathJaxInlineScale(100);
        Config::setMathJaxAlignment(QStringLiteral("center"));
        Config::setMathJaxFontCache(QStringLiteral("local"));
        Config::setMathJaxOverflow(QStringLiteral("linebreak"));
        Config::setMathJaxBreakInline(true);
        Config::self()->save();
    }

    void pdfWheelChangesRenderedGapAndRestoresIt()
    {
        QTemporaryDir directory;
        const QString file = directory.filePath(QStringLiteral("spread.pdf"));
        {
            QPdfWriter writer(file);
            writer.setResolution(72);
            QPainter painter(&writer);
            painter.drawText(80, 150, QStringLiteral("First page"));
            QVERIFY(writer.newPage());
            painter.drawText(80, 150, QStringLiteral("Second page"));
        }
        BookEntry book;
        book.filename = file;
        book.title = QStringLiteral("Spread");
        book.pageMode = QStringLiteral("two");
        book.pageWidthDistance = 40;
        book.zoomLevel = 0.5;
        BookDatabase::self().addEntry(book);
        BookListModel library;
        library.componentComplete();
        QTRY_VERIFY(library.cacheLoaded());
        for (int pass = 0; pass < 2; ++pass) {
            QQmlEngine engine;
            engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
            engine.rootContext()->setContextProperty(QStringLiteral("testLibrary"), &library);
            engine.rootContext()->setContextProperty(QStringLiteral("testBook"), QVariant::fromValue(*BookDatabase::self().loadEntry(file)));
            engine.rootContext()->setContextProperty(QStringLiteral("testUrl"), QUrl::fromLocalFile(file));
            QQmlComponent component(&engine);
            component.setData(R"(
                import QtQuick
                import org.kde.kirigami as Kirigami
                import org.kde.arianna
                Kirigami.ApplicationWindow {
                    width: 1000; height: 800; visible: true
                    property var bookListModel: testLibrary
                    contextDrawer: Kirigami.ContextDrawer {
                        property string currentTocId: ""
                        property string currentHref: ""
                        property var model: QtObject { function importFromJson(json) {} }
                        function clearCurrentItem() {}
                        function setCurrentItem(id) {}
                        signal goTo(var location)
                        signal removePdfTableOfContentsItem(string tocId)
                        signal editPdfTableOfContentsItem(string tocId, string title)
                    }
                    function pdfWatermarkFilterMode() { return "off" }
                    function fileUrlFromLocalPath(fileName) { return testUrl }
                    pageStack.initialPage: PdfViewerPage {
                        objectName: "reader"
                        entry: testBook
                        filename: testBook.filename
                        pageMode: testBook.pageMode
                        zoomLevel: testBook.zoomLevel
                        url: testUrl
                        onPageWidthDistanceSaved: value => testLibrary.setBookData(filename, "pageWidthDistance", value)
                        onZoomLevelSaved: value => testLibrary.setBookData(filename, "zoomLevel", value)
                    }
                }
            )", QUrl());
            QTRY_VERIFY(component.status() != QQmlComponent::Loading);
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> object(component.create());
            QVERIFY2(object != nullptr, qPrintable(component.errorString()));
            auto *window = qobject_cast<QQuickWindow *>(object.get());
            auto *page = object->findChild<QQuickItem *>(QStringLiteral("reader"));
            QVERIFY(page && window);
            QVERIFY(QTest::qWaitForWindowExposed(window));
            QTRY_VERIFY(page->property("documentReady").toBool());
            QTRY_VERIFY(page->property("twoPageMode").toBool());
            QQuickItem *view = nullptr;
            for (auto *child : page->findChildren<QQuickItem *>()) {
                if (child->property("renderScale").isValid() && child->property("pageSpacing").isValid()) {
                    view = child;
                    break;
                }
            }
            QVERIFY(view);
            const auto pages = view->findChildren<OkularPdfPageItem *>();
            QCOMPARE(pages.size(), 2);
            const auto gap = [&] {
                auto *first = pages[0];
                auto *second = pages[1];
                if (first->mapToScene(QPointF()).x() > second->mapToScene(QPointF()).x())
                    std::swap(first, second);
                return second->mapToScene(QPointF()).x() - first->mapToScene(QPointF(first->width(), 0)).x();
            };
            QTRY_COMPARE(gap(), pass == 0 ? 40.0 : 0.0);
            if (pass == 1)
                continue;
            wheel(view, QPoint(120, 0));
            QTRY_COMPARE(gap(), 50.0);
            QCOMPARE(BookDatabase::self().loadEntry(file)->pageWidthDistance, 50);
            // A width save must not undo a zoom change still awaiting its save timer.
            wheel(view, QPoint(0, 120));
            const double zoom = page->property("zoomLevel").toDouble();
            QVERIFY(zoom > 0.5);
            wheel(view, QPoint(-120, 0));
            QCOMPARE(page->property("zoomLevel").toDouble(), zoom);
            QTRY_COMPARE(BookDatabase::self().loadEntry(file)->zoomLevel, zoom);
            for (int i = 0; i < 5; ++i)
                wheel(view, QPoint(-120, 0));
            QTRY_COMPARE(gap(), 0.0);
            QCOMPARE(BookDatabase::self().loadEntry(file)->pageWidthDistance, 0);
        }
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid())
        return 1;
    const QByteArray path = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", path);
    qputenv("XDG_CONFIG_HOME", path);
    qputenv("XDG_CACHE_HOME", path);
    qputenv("XDG_RUNTIME_DIR", path);
    QGuiApplication application(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    ReaderSettingsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "readersettingstest.moc"
