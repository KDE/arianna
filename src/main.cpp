// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVector>
#include <QtWebEngineQuick>

#include <KAboutData>
#include <KConfig>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedContext>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KWindowConfig>
#include <KWindowSystem>
#include <QStringLiteral>

#include <QCoreApplication>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <optional>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

#include <KConfigGroup>
#include <QCommandLineOption>
#include <QUuid>

#include "arianna-version.h"
#include "ariannatrace.h"
#include "bookdatabase.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "navigation.h"
#include <KConfig>
#include <KConfigGroup>
#include <QUuid>
#include <qlogging.h>

static void handleUnixSignal(int)
{
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        QCoreApplication::quit();
    });
}

static QString persistentServerToken()
{
    KConfig config(QStringLiteral("ariannarc"));
    KConfigGroup group(&config, QStringLiteral("BookServer"));

    QString token = group.readEntry(QStringLiteral("ServerToken"), QString());

    if (token.isEmpty()) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        group.writeEntry(QStringLiteral("ServerToken"), token);
        group.sync();
    }

    return token;
}

static QUrl bookServerSessionUrl()
{
    return QUrl(BookServerConfig::baseUrl() + QStringLiteral("/session"));
}

static QUrl bookServerReadOnlyBooksUrl()
{
    return QUrl(BookServerConfig::baseUrl() + QStringLiteral("/read-only-books"));
}

static QByteArray waitForNetworkReply(QNetworkReply *reply, bool *ok, int timeoutMs = 1000, bool warnOnFailure = true)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeout.start(timeoutMs);
    loop.exec();

    if (!timeout.isActive()) {
        reply->abort();
        if (ok) {
            *ok = false;
        }
        reply->deleteLater();
        return {};
    }

    const bool success = reply->error() == QNetworkReply::NoError;
    const QByteArray body = success ? reply->readAll() : QByteArray();

    if (!success && warnOnFailure) {
        qWarning() << "BookServer session request failed:" << reply->errorString();
    }

    if (ok) {
        *ok = success;
    }

    reply->deleteLater();
    return body;
}

static QString requestBookServerSessionToken(const QString &serverToken, bool warnOnFailure = true)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerSessionUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Server-Token"), serverToken.toUtf8());

    auto *reply = manager.post(request, QByteArrayLiteral("{}"));
    bool ok = false;
    const QByteArray body = waitForNetworkReply(reply, &ok, 1000, warnOnFailure);
    if (!ok) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    const QString sessionToken = document.object().value(QStringLiteral("sessionToken")).toString();
    if (sessionToken.isEmpty()) {
        qWarning() << "BookServer did not return a session token";
    }

    return sessionToken;
}

static bool unregisterBookServerSessionToken(const QString &sessionToken)
{
    if (sessionToken.isEmpty()) {
        return true;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerSessionUrl());
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Session-Token"), sessionToken.toUtf8());

    auto *reply = manager.deleteResource(request);
    bool ok = false;
    waitForNetworkReply(reply, &ok);
    return ok;
}

static QString registerReadOnlyBook(const QString &sessionToken, const QString &fileName)
{
    if (sessionToken.isEmpty() || fileName.isEmpty()) {
        return {};
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerReadOnlyBooksUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Session-Token"), sessionToken.toUtf8());

    QJsonObject payload;
    payload.insert(QStringLiteral("fileName"), fileName);

    auto *reply = manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    bool ok = false;
    const QByteArray body = waitForNetworkReply(reply, &ok);
    if (!ok) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    const QString identifier = document.object().value(QStringLiteral("identifier")).toString();
    if (identifier.isEmpty()) {
        qWarning() << "BookServer did not return a read-only book identifier";
    }

    return identifier;
}

static bool startDetachedBookServer()
{
    qint64 pid = -1;
    QStringList arguments{QStringLiteral("--bookserver")};
    if (AriannaTrace::isEnabled()) {
        arguments.append(QStringLiteral("--flight-log"));
    }

    AriannaTrace::event(QStringLiteral("bookserver.spawn.requested"),
                        {{QStringLiteral("executable"), QCoreApplication::applicationFilePath()}, {QStringLiteral("arguments"), arguments}});
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments, QString(), &pid);
    AriannaTrace::event(QStringLiteral("bookserver.spawn.finished"), {{QStringLiteral("started"), started}, {QStringLiteral("pid"), pid}});
    if (!started) {
        qWarning() << "Unable to start detached Arianna BookServer";
    }

    return started;
}

static QString ensureBookServerSessionToken(const QString &serverToken)
{
    bool triedStartingServer = false;

    for (int attempt = 0; attempt < 30; ++attempt) {
        const QString sessionToken = requestBookServerSessionToken(serverToken, false);
        if (!sessionToken.isEmpty()) {
            return sessionToken;
        }

        if (!triedStartingServer) {
            triedStartingServer = true;
            startDetachedBookServer();
        }

        QThread::msleep(100);
    }

    return {};
}

static bool hasOptionArgument(int argc, char *argv[], const QStringList &optionNames)
{
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--")) {
            return false;
        }

        if (optionNames.contains(argument)) {
            return true;
        }
    }

    return false;
}

static bool hasBookServerOnlyArgument(int argc, char *argv[])
{
    return hasOptionArgument(argc, argv, {QStringLiteral("--bookserver")});
}

static bool hasFlightLogArgument(int argc, char *argv[])
{
    return hasOptionArgument(argc, argv, {QStringLiteral("--flight-log")});
}

static QString bookServerDiscoveryFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver.json");
}

struct AriannaProcessInfo {
    qint64 pid = -1;
    QString commandLine;
};

static QString processCommandLine(const qint64 pid)
{
    QFile cmdlineFile(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (cmdlineFile.open(QIODevice::ReadOnly)) {
        QByteArray data = cmdlineFile.readAll();
        data.replace('\0', ' ');
        const QString commandLine = QString::fromLocal8Bit(data).simplified();
        if (!commandLine.isEmpty()) {
            return commandLine;
        }
    }

    return QStringLiteral("arianna");
}

#ifdef Q_OS_UNIX
static bool processExists(const qint64 pid)
{
    errno = 0;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }

    return errno == EPERM;
}
#endif

static QVector<AriannaProcessInfo> runningAriannaProcesses()
{
    QVector<AriannaProcessInfo> processes;

#ifdef Q_OS_UNIX
    const qint64 ownPid = QCoreApplication::applicationPid();
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool ok = false;
        const qint64 pid = entry.toLongLong(&ok);
        if (!ok || pid <= 0 || pid == ownPid) {
            continue;
        }

        QFile commFile(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!commFile.open(QIODevice::ReadOnly)) {
            continue;
        }

        const QString commandName = QString::fromLocal8Bit(commFile.readAll()).trimmed();
        if (commandName != QStringLiteral("arianna")) {
            continue;
        }

        processes.append(AriannaProcessInfo{pid, processCommandLine(pid)});
    }
#endif

    return processes;
}

static int stopExistingAriannaInstances()
{
#ifndef Q_OS_UNIX
    qWarning() << "--start-new is only implemented for Unix-like systems";
    return 0;
#else
    const QVector<AriannaProcessInfo> processes = runningAriannaProcesses();
    if (processes.isEmpty()) {
        return 0;
    }

    qWarning().noquote() << QStringLiteral("Stopping existing Arianna instance(s):");
    for (const AriannaProcessInfo &process : processes) {
        qWarning().noquote() << QStringLiteral("  %1 %2").arg(process.pid).arg(process.commandLine);
        if (::kill(static_cast<pid_t>(process.pid), SIGTERM) != 0 && errno != ESRCH) {
            qWarning().noquote() << QStringLiteral("Unable to stop Arianna instance %1: %2").arg(process.pid).arg(QString::fromLocal8Bit(strerror(errno)));
        }
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        bool anyRunning = false;
        for (const AriannaProcessInfo &process : processes) {
            if (processExists(process.pid)) {
                anyRunning = true;
                break;
            }
        }

        if (!anyRunning) {
            break;
        }

        QThread::msleep(100);
    }

    for (const AriannaProcessInfo &process : processes) {
        if (!processExists(process.pid)) {
            continue;
        }

        qWarning().noquote() << QStringLiteral("Force stopping remaining Arianna instance: %1").arg(process.pid);
        if (::kill(static_cast<pid_t>(process.pid), SIGKILL) != 0 && errno != ESRCH) {
            qWarning().noquote()
                << QStringLiteral("Unable to force stop Arianna instance %1: %2").arg(process.pid).arg(QString::fromLocal8Bit(strerror(errno)));
        }
    }

    return processes.size();
#endif
}

static void cleanupBookServerDiscoveryFile()
{
    const QString path = bookServerDiscoveryFilePath();
    if (!QFileInfo::exists(path)) {
        return;
    }

    qWarning().noquote() << QStringLiteral("Cleaning the bookserver launch pad: %1").arg(path);
    if (!QFile::remove(path)) {
        qWarning().noquote() << QStringLiteral("Unable to remove BookServer discovery file: %1").arg(path);
    }
}

static bool hasEarlyExitArgument(int argc, char *argv[])
{
    return hasOptionArgument(argc,
                             argv,
                             {
                                 QStringLiteral("-h"),
                                 QStringLiteral("--help"),
                                 QStringLiteral("--help-all"),
                                 QStringLiteral("-v"),
                                 QStringLiteral("--version"),
                                 QStringLiteral("--author"),
                                 QStringLiteral("--license"),
                             });
}

static QString localFilePathFromArgument(const QString &argument, const QString &workingDirectory = {})
{
    if (argument.isEmpty()) {
        return {};
    }

    const QUrl url(argument);
    if (url.isLocalFile()) {
        return QFileInfo(url.toLocalFile()).absoluteFilePath();
    }

    QFileInfo fileInfo(argument);
    if (fileInfo.isRelative() && !workingDirectory.isEmpty()) {
        fileInfo = QFileInfo(QDir(workingDirectory), argument);
    }

    return fileInfo.absoluteFilePath();
}

static bool argumentsRequestReadOnly(const QStringList &arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QStringLiteral("--")) {
            return false;
        }
        if (argument == QStringLiteral("--open-read-only")) {
            return true;
        }
    }

    return false;
}

static bool argumentsRequestOpenLastOpenedBook(const QStringList &arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QStringLiteral("--")) {
            return false;
        }
        if (argument == QStringLiteral("--open-last-opened-book")) {
            return true;
        }
    }

    return false;
}

static QString fileArgumentFromActivationArguments(const QStringList &arguments)
{
    bool endOfOptions = false;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (!endOfOptions && argument == QStringLiteral("--")) {
            endOfOptions = true;
            continue;
        }
        if (!endOfOptions && argument == QStringLiteral("--open-read-only")) {
            continue;
        }
        if (!endOfOptions && argument.startsWith(QStringLiteral("--"))) {
            continue;
        }

        return argument;
    }

    return {};
}

static std::optional<BookEntry> lastOpenedBookEntry()
{
    const QList<BookEntry> entries = BookDatabase::self().loadEntries();
    BookEntry lastOpenedEntry;
    bool found = false;

    for (const BookEntry &entry : entries) {
        if (!entry.lastOpenedTime.isValid() || entry.filename.isEmpty()) {
            continue;
        }
        if (!QFileInfo::exists(entry.filename)) {
            continue;
        }
        if (!found || entry.lastOpenedTime > lastOpenedEntry.lastOpenedTime) {
            lastOpenedEntry = entry;
            found = true;
        }
    }

    if (!found) {
        return std::nullopt;
    }

    return lastOpenedEntry;
}

static bool openLastOpenedBook(Navigation *navigation)
{
    if (!navigation) {
        return false;
    }

    const std::optional<BookEntry> entry = lastOpenedBookEntry();
    if (!entry) {
        qWarning() << "No last opened Arianna book found";
        return false;
    }

    Q_EMIT navigation->openBook(entry->filename, entry->locations, entry->currentLocation, *entry, false);
    return true;
}

static void
openBookArgument(Navigation *navigation, const QString &argument, const QString &workingDirectory = {}, bool readOnly = false, const QString &sessionToken = {})
{
    const QString fileName = localFilePathFromArgument(argument, workingDirectory);
    if (fileName.isEmpty()) {
        return;
    }

    const QFileInfo fileInfo(fileName);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "Book file does not exist:" << fileName;
        return;
    }

    if (readOnly) {
        const QString identifier = registerReadOnlyBook(sessionToken, fileName);
        if (identifier.isEmpty()) {
            qWarning() << "Unable to register read-only book with the Arianna BookServer:" << fileName;
            return;
        }

        BookEntry readOnlyEntry;
        readOnlyEntry.filename = fileName;
        readOnlyEntry.filetitle = fileInfo.fileName();
        readOnlyEntry.title = fileInfo.completeBaseName();
        readOnlyEntry.uniqueIdentifier = identifier;

        Q_EMIT navigation->openBook(fileName, {}, {}, readOnlyEntry, true);
        return;
    }

    const auto entry = BookDatabase::self().loadEntry(fileName);
    if (entry) {
        Q_EMIT navigation->openBook(fileName, entry->locations, entry->currentLocation, *entry, false);
    } else {
        Q_EMIT navigation->openBook(fileName, {}, {}, BookEntry{}, false);
    }
}

int main(int argc, char *argv[])
{
    const bool requestedBookServerOnly = hasBookServerOnlyArgument(argc, argv);
    const bool flightLogRequested = hasFlightLogArgument(argc, argv);
    const bool exitsBeforeOpeningUi = hasEarlyExitArgument(argc, argv);
    if (!requestedBookServerOnly && !exitsBeforeOpeningUi) {
        QtWebEngineQuick::initialize();
    }

    QNetworkProxyFactory::setUseSystemConfiguration(true);

    std::unique_ptr<QCoreApplication> app;
    if (requestedBookServerOnly || exitsBeforeOpeningUi) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        auto guiApp = std::make_unique<QApplication>(argc, argv);

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
        auto font = guiApp->font();
        font.setPointSize(10);
        guiApp->setFont(font);
#endif
        app = std::move(guiApp);
    }

    const QString traceRole = requestedBookServerOnly ? QStringLiteral("bookserver") : exitsBeforeOpeningUi ? QStringLiteral("utility") : QStringLiteral("ui");
    AriannaTrace::initializeProcess(traceRole, QCoreApplication::arguments(), flightLogRequested);
    struct ProcessLandingLogger {
        ~ProcessLandingLogger()
        {
            AriannaTrace::land(QStringLiteral("main-return"));
        }
    };
    const ProcessLandingLogger processLandingLogger;

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

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Epub reader"));
    parser.addPositionalArgument(QStringLiteral("file"), i18n("Epub file to open"));
    QCommandLineOption bookServerOnlyOption(QStringLiteral("bookserver"), i18n("Start only the local BookServer and do not open the reader UI"));
    QCommandLineOption startNewOption(QStringLiteral("start-new"),
                                      i18n("Stop existing Arianna instances, remove stale BookServer discovery state, then start normally"));
    QCommandLineOption flightLogOption(QStringLiteral("flight-log"), i18n("Write persistent diagnostic flight log history"));
    QCommandLineOption readOnlyOption(QStringLiteral("open-read-only"), i18n("Open the given book read-only without adding it to the library"));
    QCommandLineOption openLastOpenedBookOption(QStringLiteral("open-last-opened-book"), i18n("Open the most recently opened library book"));
    parser.addOption(bookServerOnlyOption);
    parser.addOption(startNewOption);
    parser.addOption(flightLogOption);
    parser.addOption(readOnlyOption);
    parser.addOption(openLastOpenedBookOption);

    about.setupCommandLine(&parser);
    parser.process(*app);
    about.processCommandLine(&parser);

    const bool bookServerOnly = parser.isSet(bookServerOnlyOption);
    const bool startNew = parser.isSet(startNewOption);
    const bool readOnly = parser.isSet(readOnlyOption);
    const bool openLastOpenedBookRequested = parser.isSet(openLastOpenedBookOption);
    const QStringList args = parser.positionalArguments();
    const QString startupFileName = args.isEmpty() ? QString() : localFilePathFromArgument(args.at(0));
    const bool startupFileOpenRequested = !startupFileName.isEmpty() && QFileInfo::exists(startupFileName);
    const std::optional<BookEntry> startupLastOpenedEntry = openLastOpenedBookRequested ? lastOpenedBookEntry() : std::optional<BookEntry>();
    const bool startupDirectReaderMode = startupFileOpenRequested || startupLastOpenedEntry.has_value();
    if (startNew) {
        const int rocketCount = stopExistingAriannaInstances();
        cleanupBookServerDiscoveryFile();
        qWarning().noquote() << QStringLiteral("Starting %1 after shooting %2 rocket%3 before.")
                                    .arg(QCoreApplication::applicationFilePath())
                                    .arg(rocketCount)
                                    .arg(rocketCount == 1 ? QString() : QStringLiteral("s"));
    }

    const QString serverToken = persistentServerToken();
    std::signal(SIGINT, handleUnixSignal);
    std::signal(SIGTERM, handleUnixSignal);

    if (bookServerOnly) {
        BookServer bookServer(serverToken, true);
        if (!bookServer.isRunning()) {
            return 1;
        }
        return QCoreApplication::exec();
    }

    const QString sessionToken = ensureBookServerSessionToken(serverToken);
    if (sessionToken.isEmpty()) {
        qWarning() << "Unable to register this reader with the Arianna BookServer";
        return 1;
    }

    QObject::connect(app.get(), &QCoreApplication::aboutToQuit, app.get(), [sessionToken] {
        unregisterBookServerSessionToken(sessionToken);
    });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
    engine.rootContext()->setContextProperty(QStringLiteral("serverToken"), serverToken);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), sessionToken);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerAddress"), BookServerConfig::address());
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerPort"), BookServerConfig::port());
    engine.rootContext()->setContextProperty(QStringLiteral("startupDirectReaderMode"), startupDirectReaderMode);
    engine.loadFromModule("org.kde.arianna", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.arianna")));

    KDBusService service(KDBusService::Multiple);

    auto *navigation = engine.singletonInstance<Navigation *>("org.kde.arianna", "Navigation");
    if (navigation) {
        navigation->setWebEngineSessionToken(sessionToken);
    }
    qDebug() << "Arianna BookServer session registered:" << sessionToken;

    QObject::connect(&service,
                     &KDBusService::activateRequested,
                     &engine,
                     [&engine, navigation, sessionToken](const QStringList &arguments, const QString &workingDirectory) {
                         const auto rootObjects = engine.rootObjects();
                         for (auto obj : rootObjects) {
                             auto view = qobject_cast<QQuickWindow *>(obj);
                             if (view) {
                                 KWindowSystem::updateStartupId(view);
                                 KWindowSystem::activateWindow(view);

                                 const QString fileArgument = fileArgumentFromActivationArguments(arguments);
                                 if (!fileArgument.isEmpty()) {
                                     openBookArgument(navigation, fileArgument, workingDirectory, argumentsRequestReadOnly(arguments), sessionToken);
                                 } else if (argumentsRequestOpenLastOpenedBook(arguments)) {
                                     openLastOpenedBook(navigation);
                                 }
                                 return;
                             }
                         }
                     });

    if (!args.isEmpty()) {
        openBookArgument(navigation, args[0], {}, readOnly, sessionToken);
    } else if (openLastOpenedBookRequested) {
        openLastOpenedBook(navigation);
    }

    return QCoreApplication::exec();
}
