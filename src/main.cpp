// SPDX-FileCopyrightText: 2018-2019 Black Hat <bhat@encom.eu.org>
// SPDX-License-Identifier: GPL-3.0-only

#include <QCommandLineParser>
#include <QFontDatabase>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtWebEngine>

#include <QApplication>

#include <KAboutData>
#include <KConfig>
#include <KDBusService>
#include <KLocalizedContext>
#include <KLocalizedString>
#include <KWindowConfig>
#include <KWindowSystem>
#include <qstringliteral.h>

#include "arianna-version.h"

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QNetworkProxyFactory::setUseSystemConfiguration(true);
    QtWebEngine::initialize();

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

    KLocalizedString::setApplicationDomain("arianna");

    KAboutData about(QStringLiteral("arianna"),
                     i18n("arianna"),
                     QStringLiteral(ARIANNA_VERSION_STRING),
                     i18n("Mastodon client"),
                     KAboutLicense::GPL_V3,
                     i18n("2022 Niccolò Venerandi <niccolo@venerandi.com>"));
    about.addAuthor(i18n("Niccolò Venerandi"), i18n("Maintainer"), QStringLiteral("niccolo@venerandi.com"));
    about.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));
    about.setOrganizationDomain("kde.org");
    about.setBugAddress("https://bugs.kde.org/describecomponents.cgi?product=arianna");

    KAboutData::setApplicationData(about);
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.arianna")));

    KDBusService service(KDBusService::Unique);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));
    QObject::connect(&engine, &QQmlApplicationEngine::quit, &app, &QCoreApplication::quit);

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Epub reader"));

    about.setupCommandLine(&parser);
    parser.process(app);
    about.processCommandLine(&parser);

    // Controller::instance().setAboutData(about);

    engine.load(QUrl(QStringLiteral("qrc:/content/ui/main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    QObject::connect(&service, &KDBusService::activateRequested, &engine, [&engine](const QStringList & /*arguments*/, const QString & /*workingDirectory*/) {
        const auto rootObjects = engine.rootObjects();
        for (auto obj : rootObjects) {
            auto view = qobject_cast<QQuickWindow *>(obj);
            if (view) {
                KWindowSystem::updateStartupId(view);
                KWindowSystem::activateWindow(view);
                return;
            }
        }
    });
    const auto rootObjects = engine.rootObjects();
    for (auto obj : rootObjects) {
        auto view = qobject_cast<QQuickWindow *>(obj);
        if (view) {
            KConfig dataResource(QStringLiteral("data"), KConfig::SimpleConfig, QStandardPaths::AppDataLocation);
            KConfigGroup windowGroup(&dataResource, "Window");
            KWindowConfig::restoreWindowSize(view, windowGroup);
            KWindowConfig::restoreWindowPosition(view, windowGroup);
            break;
        }
    }
    return QCoreApplication::exec();
}
