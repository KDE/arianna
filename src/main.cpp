// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include <QCommandLineParser>
#include <QFontDatabase>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtWebEngineQuick>

#include <QApplication>

#include <KAboutData>
#include <KConfig>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedContext>
#include <KLocalizedString>
#include <KWindowConfig>
#include <KWindowSystem>
#include <qstringliteral.h>

#include "arianna-version.h"
#include "bookdatabase.h"
#include "booklistmodel.h"
#include "bookserver.h"
#include "clipboard.h"
#include "colorschemer.h"
#include "config.h"
#include "contentlist/contentlist.h"
#include "contentlist/contentquery.h"
#include "format.h"
#include "navigation.h"
#include "tableofcontentmodel.h"

int main(int argc, char *argv[])
{
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    QtWebEngineQuick::initialize();

    QApplication app(argc, argv);
    // Default to org.kde.desktop style unless the user forces another style
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

#ifdef Q_OS_WINDOWS
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    QApplication::setStyle(QStringLiteral("breeze"));
    auto font = app.font();
    font.setPointSize(10);
    app.setFont(font);
#endif

    KLocalizedString::setApplicationDomain(QByteArrayLiteral("arianna"));

    KAboutData about(QStringLiteral("arianna"),
                     i18n("Arianna"),
                     QStringLiteral(ARIANNA_VERSION_STRING),
                     i18n("EPub Reader"),
                     KAboutLicense::GPL_V3,
                     i18n("2022 Niccolò Venerandi <niccolo@venerandi.com>"));
    about.addAuthor(i18n("Niccolò Venerandi"), i18n("Maintainer"), QStringLiteral("niccolo@venerandi.com"));
    about.addAuthor(i18n("Carl Schwan"), i18n("Maintainer"), QStringLiteral("carl@carlschwan.eu"));
    about.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));
    about.setOrganizationDomain("kde.org");
    about.setBugAddress("https://bugs.kde.org/describecomponents.cgi?product=arianna");

    KAboutData::setApplicationData(about);
    KCrash::initialize();

    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.arianna")));

    KDBusService service(KDBusService::Unique);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Epub reader"));
    parser.addPositionalArgument(QStringLiteral("file"), i18n("Epub file to open"));

    about.setupCommandLine(&parser);
    parser.process(app);
    about.processCommandLine(&parser);

    Clipboard clipboard;
    ColorSchemer colorScheme;
    Format format;
    Navigation navigation;
    qmlRegisterSingletonInstance("org.kde.arianna", 1, 0, "Config", Config::self());
    qmlRegisterSingletonInstance("org.kde.arianna", 1, 0, "Clipboard", &clipboard);
    qmlRegisterSingletonInstance("org.kde.arianna", 1, 0, "Format", &format);
    qmlRegisterSingletonInstance("org.kde.arianna", 1, 0, "Navigation", &navigation);
    qmlRegisterType<BookListModel>("org.kde.arianna", 1, 0, "BookListModel");
    qmlRegisterType<ContentList>("org.kde.arianna", 1, 0, "ContentList");
    qmlRegisterType<QSortFilterProxyModel>("org.kde.arianna", 1, 0, "SortFilterProxyModel");
    qmlRegisterType<ContentQuery>("org.kde.arianna", 1, 0, "ContentQuery");
    qmlRegisterType<TableOfContentModel>("org.kde.arianna", 1, 0, "TableOfContentModel");
    qmlRegisterType<CategoryEntriesModel>("org.kde.arianna", 1, 0, "CategoryEntriesModel");
    qmlRegisterSingletonType("org.kde.arianna", 1, 0, "About", [](QQmlEngine *engine, QJSEngine *) -> QJSValue {
        return engine->toScriptValue(KAboutData::applicationData());
    });

    BookServer bookServer;

    engine.load(QUrl(QStringLiteral("qrc:/content/ui/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    QObject::connect(&service,
                     &KDBusService::activateRequested,
                     &engine,
                     [&engine, &navigation](const QStringList &arguments, const QString & /*workingDirectory*/) {
                         const auto rootObjects = engine.rootObjects();
                         for (auto obj : rootObjects) {
                             auto view = qobject_cast<QQuickWindow *>(obj);
                             if (view) {
                                 KWindowSystem::updateStartupId(view);
                                 KWindowSystem::activateWindow(view);

                                 if (arguments.count() > 1) {
                                     const auto entry = BookDatabase::self().loadEntry(arguments[1]);
                                     if (entry) {
                                         Q_EMIT navigation.openBook(arguments[1], entry->locations, entry->currentLocation, *entry);
                                     } else {
                                         Q_EMIT navigation.openBook(arguments[1], {}, {}, BookEntry{});
                                     }
                                 }
                                 return;
                             }
                         }
                     });

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        const auto entry = BookDatabase::self().loadEntry(args[0]);
        if (entry) {
            Q_EMIT navigation.openBook(args[0], entry->locations, entry->currentLocation, *entry);
        } else {
            Q_EMIT navigation.openBook(args[0], {}, {}, BookEntry{});
        }
    }

    return QCoreApplication::exec();
}
