// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLockFile>
#include <QElapsedTimer>
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
#include <QTextStream>
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
#include "booklistmodel.h"
#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "navigation.h"
#include "readerhistory.h"
#include "readercontrol.h"
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

static QString registerReadOnlyBook(const QString &sessionToken, const QString &fileName, const QString &identifier = {})
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
    if (!identifier.isEmpty()) payload.insert(QStringLiteral("identifier"), identifier);

    auto *reply = manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    bool ok = false;
    const QByteArray body = waitForNetworkReply(reply, &ok);
    if (!ok) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    const QString registeredIdentifier = document.object().value(QStringLiteral("identifier")).toString();
    if (registeredIdentifier.isEmpty()) {
        qWarning() << "BookServer did not return a read-only book identifier";
    }

    return registeredIdentifier;
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
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(arguments);
    // A command-only caller must be able to exit and close its output pipes.
    // Persistent diagnostics remain available through --flight-log.
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    const bool started = process.startDetached(&pid);
    AriannaTrace::event(QStringLiteral("bookserver.spawn.finished"), {{QStringLiteral("started"), started}, {QStringLiteral("pid"), pid}});
    if (!started) {
        qWarning() << "Unable to start detached Arianna BookServer";
    }

    return started;
}

static QString ensureBookServerSessionToken(const QString &serverToken)
{
    QLockFile launchLock(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver-start.lock"));
    if (!launchLock.tryLock(10000)) {
        qWarning() << "Another Arianna process is starting/restarting the BookServer";
        return {};
    }
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

static void waitForControlPoll()
{
    QEventLoop loop;
    QTimer::singleShot(50, &loop, &QEventLoop::quit);
    loop.exec();
}

static QJsonObject bookServerControl(const QString &serverToken, bool stop, bool *notListening = nullptr)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(BookServerConfig::baseUrl() + QStringLiteral("/server-control")));
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Server-Token"), serverToken.toUtf8());
    auto *reply = stop ? manager.deleteResource(request) : manager.get(request);
    bool ok = false;
    const auto body = waitForNetworkReply(reply, &ok, 1000, false);
    if (notListening) *notListening = reply->error() == QNetworkReply::ConnectionRefusedError;
    return ok ? QJsonDocument::fromJson(body).object() : QJsonObject();
}

static bool refreshRunningReaderProcesses();

static bool restartBookServer(const QString &serverToken)
{
    // Use the same lock as ordinary reader startup: one owner stops and starts.
    QLockFile launchLock(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver-start.lock"));
    if (!launchLock.tryLock(10000)) {
        qWarning() << "Timed out waiting for BookServer startup/restart lock";
        return false;
    }
    bool notListening = false;
    const auto current = bookServerControl(serverToken, false, &notListening);
    if (current.isEmpty() && !notListening) {
        qWarning() << "The running BookServer does not support authenticated restart or uses a different credential; close the older BookServer first";
        return false;
    }
    if (!current.isEmpty()) {
        if (bookServerControl(serverToken, true).value(QStringLiteral("state")).toString() != QStringLiteral("Stopping")) {
            qWarning() << "BookServer rejected the shutdown request";
            return false;
        }
        QElapsedTimer stopTimer;
        stopTimer.start();
        while (!notListening && stopTimer.elapsed() < 5000) {
            waitForControlPoll();
            bookServerControl(serverToken, false, &notListening);
        }
        if (!notListening) {
            qWarning() << "Timed out waiting for the old BookServer to stop";
            return false;
        }
    }
    if (!startDetachedBookServer()) return false;
    QElapsedTimer startTimer;
    startTimer.start();
    while (startTimer.elapsed() < 10000) {
        const auto running = bookServerControl(serverToken, false);
        if (!running.isEmpty()) {
            qInfo() << "New BookServer ready, PID:" << running.value(QStringLiteral("pid")).toInteger();
            return refreshRunningReaderProcesses();
        }
        waitForControlPoll();
    }
    qWarning() << "Timed out waiting for the new BookServer";
    return false;
}

static bool refreshRunningReaderProcesses()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        qWarning() << "Unable to access the session D-Bus for reader refresh";
        return false;
    }
    const QDBusReply<QStringList> names = bus.interface()->registeredServiceNames();
    if (!names.isValid()) return false;
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool success = true;
    int processes = 0;
    for (const auto &name : names.value()) {
        if (name != QStringLiteral("org.kde.arianna") && !name.startsWith(QStringLiteral("org.kde.arianna-"))) continue;
        QDBusInterface reader(name, QStringLiteral("/Reader"), QStringLiteral("org.kde.arianna.Reader1"), bus);
        reader.setTimeout(20000);
        const QDBusReply<int> reply = reader.call(QStringLiteral("RefreshReaders"), requestId);
        if (!reply.isValid() || reply.value() < 0) {
            qWarning() << "Reader refresh failed (older readers may need restarting):" << name << reply.error().message();
            success = false;
        } else {
            qInfo() << "Refreshed" << reply.value() << "reader(s) in" << name;
        }
        ++processes;
    }
    if (!processes) qInfo() << "No running Arianna reader sessions found";
    return success;
}

static int refreshReaderProcess(QObject *root, const QString &serverToken, const QString &sessionToken)
{
    // Preserve the token bound into QML and WebEngine instead of invalidating URLs.
    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerSessionUrl());
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Server-Token"), serverToken.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    auto *reply = manager.post(request, QJsonDocument(QJsonObject{{QStringLiteral("sessionToken"), sessionToken}}).toJson(QJsonDocument::Compact));
    bool ok = false;
    const auto body = waitForNetworkReply(reply, &ok);
    if (!ok || QJsonDocument::fromJson(body).object().value(QStringLiteral("sessionToken")).toString() != sessionToken) return -1;
    QVariant filesJson;
    if (!QMetaObject::invokeMethod(root, "readerSessionFiles", Q_RETURN_ARG(QVariant, filesJson))) return -1;
    const auto files = QJsonDocument::fromJson(filesJson.toString().toUtf8()).array();
    for (const auto &value : files) {
        const auto file = value.toObject();
        const auto identifier = file.value(QStringLiteral("identifier")).toString();
        if (identifier.isEmpty() || registerReadOnlyBook(sessionToken, file.value(QStringLiteral("fileName")).toString(), identifier) != identifier) {
            qWarning() << "Unable to restore transient reader registration:" << file.value(QStringLiteral("fileName")).toString();
            return -1;
        }
    }
    QVariant count;
    if (!QMetaObject::invokeMethod(root, "refreshReaderSessions", Q_RETURN_ARG(QVariant, count))) return -1;
    return count.toInt();
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

static int checkAndRemovePdfWatermarks(const QStringList &fileArguments)
{
    QTextStream output(stdout);
    if (fileArguments.isEmpty()) {
        output << i18n("No PDF files were specified.") << Qt::endl;
        return 1;
    }

    QString pattern = Config::pdfWatermarkFilterPattern().trimmed();
    if (pattern.isEmpty()) {
        pattern = Config::defaultPdfWatermarkFilterPatternValue();
    }

    BookListModel bookListModel;
    int exitCode = 0;
    for (const QString &argument : fileArguments) {
        const QString fileName = localFilePathFromArgument(argument);
        output << fileName << Qt::endl;

        const QVariantMap checkResult = bookListModel.createWatermarkFreePdf(fileName, pattern);
        output << QStringLiteral("  %1").arg(checkResult.value(QStringLiteral("message")).toString()) << Qt::endl;
        if (!checkResult.value(QStringLiteral("success")).toBool()) {
            exitCode = 1;
            continue;
        }

        if (!checkResult.value(QStringLiteral("filtered")).toBool()) {
            continue;
        }

        const QString replacementFileName = checkResult.value(QStringLiteral("replacementFileName")).toString();
        const QVariantMap replacementResult = bookListModel.replaceBookFileWithWatermarkFreePdf(fileName, replacementFileName, false);
        bookListModel.discardWatermarkFreePdf(replacementFileName);

        const bool replacementSucceeded = replacementResult.value(QStringLiteral("success")).toBool();
        const QString replacementMessage =
            replacementSucceeded ? i18n("PDF replaced with watermark-free version.") : replacementResult.value(QStringLiteral("message")).toString();
        output << QStringLiteral("  %1").arg(replacementMessage) << Qt::endl;
        if (!replacementSucceeded) {
            exitCode = 1;
        }
    }

    output.flush();
    return exitCode;
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

static bool argumentsRequestNoLibraryImport(const QStringList &arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i) == QStringLiteral("--")) return false;
        if (arguments.at(i) == QStringLiteral("--no-library-import")) return true;
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
        if (!endOfOptions && argument == QStringLiteral("--open-last-closed-book")) {
            ++i; // Its count is an option value, not a file name.
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

static void openLastClosedBooks(Navigation *navigation, const QList<BookEntry> &entries)
{
    if (!navigation) return;
    if (entries.isEmpty()) {
        qWarning() << "No previously closed Arianna library books found";
    }
    // Keep the snapshot: opening a reader must not change this selection/order.
    for (const auto &entry : entries) {
        qInfo() << "Reopening last closed book:" << entry.filename;
        Q_EMIT navigation->openBook(entry.filename, entry.locations, entry.currentLocation, entry, false);
    }
}

static std::optional<int> lastClosedBookCountFromActivation(const QStringList &arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QStringLiteral("--")) break;
        QString value;
        if (argument == QStringLiteral("--open-last-closed-book")) {
            value = arguments.value(i + 1);
        } else if (argument.startsWith(QStringLiteral("--open-last-closed-book="))) {
            value = argument.mid(QStringLiteral("--open-last-closed-book=").size());
        } else {
            continue;
        }
        bool ok = false;
        const int count = value.toInt(&ok);
        if (ok && count > 0) return count;
        qWarning() << "--open-last-closed-book requires a positive integer";
        break;
    }
    return std::nullopt;
}

static void
openBookArgument(Navigation *navigation, const QString &argument, const QString &workingDirectory = {}, bool readOnly = false, const QString &sessionToken = {}, bool noLibraryImport = false)
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

    if (noLibraryImport && fileInfo.suffix().compare(QLatin1String("epub"), Qt::CaseInsensitive) != 0) {
        qWarning() << "--no-library-import requires an EPUB file:" << fileName;
        return;
    }

    // Both modes use session-only, immutable BookServer delivery. The external
    // editor owns explicit saves in no-library mode; no BookTruthStore import.
    if (readOnly || noLibraryImport) {
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

        Q_EMIT navigation->openBook(fileName, {}, {}, readOnlyEntry, readOnly, noLibraryImport);
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
    const bool requestedWatermarkCheck = hasOptionArgument(argc, argv, {QStringLiteral("--check-and-remove-watermark-pdf-only")});
    const bool requestedReaderRefresh = hasOptionArgument(argc, argv,
        {QStringLiteral("--reopen-already-opened-books")});
    const bool requestedServerRestart = hasOptionArgument(argc, argv, {QStringLiteral("--restart-new-bookserver")});
    QStringList rawArguments;
    bool requestedHistoryOpen = false;
    for (int i = 0; i < argc; ++i) {
        const auto argument = QString::fromLocal8Bit(argv[i]);
        rawArguments.append(argument);
        if (argument == QStringLiteral("--open-last-opened-book") || argument.startsWith(QStringLiteral("--open-last-closed-book"))) requestedHistoryOpen = true;
    }
    const bool controlOnly = (requestedReaderRefresh || requestedServerRestart) && !requestedHistoryOpen
        && fileArgumentFromActivationArguments(rawArguments).isEmpty() && !requestedBookServerOnly;
    const bool flightLogRequested = hasFlightLogArgument(argc, argv);
    const bool exitsBeforeOpeningUi = hasEarlyExitArgument(argc, argv) || requestedWatermarkCheck || controlOnly;
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

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Epub reader"));
    parser.addPositionalArgument(QStringLiteral("file..."), i18n("Epub file to open, or PDF files to check for watermarks"));
    QCommandLineOption bookServerOnlyOption(QStringLiteral("bookserver"), i18n("Start only the local BookServer and do not open the reader UI"));
    QCommandLineOption startNewOption(QStringLiteral("start-new"),
                                      i18n("Stop existing Arianna instances, remove stale BookServer discovery state, then start normally"));
    QCommandLineOption flightLogOption(QStringLiteral("flight-log"), i18n("Write persistent diagnostic flight log history"));
    QCommandLineOption readOnlyOption(QStringLiteral("open-read-only"), i18n("Open the given book read-only without adding it to the library"));
    QCommandLineOption openLastOpenedBookOption(QStringLiteral("open-last-opened-book"), i18n("Open the most recently opened library book"));
    QCommandLineOption openLastClosedBookOption(QStringLiteral("open-last-closed-book"),
        i18n("Open the n most recently closed library books in their original opening order"), QStringLiteral("n"));
    QCommandLineOption refreshReadersOption(QStringLiteral("reopen-already-opened-books"),
        i18n("Refresh all running reader sessions at their current positions without opening another window"));
    QCommandLineOption restartBookServerOption(QStringLiteral("restart-new-bookserver"),
        i18n("Restart the BookServer and refresh all running reader sessions"));
    QCommandLineOption checkAndRemoveWatermarkPdfOnlyOption(
        QStringLiteral("check-and-remove-watermark-pdf-only"),
        i18n("Check the given PDF files for watermark markers, remove found markers, write the result to standard output, and exit"));
    parser.addOption(bookServerOnlyOption);
    parser.addOption(startNewOption);
    parser.addOption(flightLogOption);
    QCommandLineOption noLibraryImportOption(QStringLiteral("no-library-import"),
        i18n("Open the given EPUB for editing without adding it to the library"));
    parser.addOption(noLibraryImportOption);
    parser.addOption(readOnlyOption);
    parser.addOption(openLastOpenedBookOption);
    parser.addOption(openLastClosedBookOption);
    parser.addOption(refreshReadersOption);
    parser.addOption(restartBookServerOption);
    parser.addOption(checkAndRemoveWatermarkPdfOnlyOption);

    about.setupCommandLine(&parser);
    parser.process(*app);
    about.processCommandLine(&parser);

    const bool bookServerOnly = parser.isSet(bookServerOnlyOption);
    const bool startNew = parser.isSet(startNewOption);
    const bool readOnly = parser.isSet(readOnlyOption);
    const bool noLibraryImport = parser.isSet(noLibraryImportOption);
    const bool openLastOpenedBookRequested = parser.isSet(openLastOpenedBookOption);
    const bool openLastClosedBookRequested = parser.isSet(openLastClosedBookOption);
    bool lastClosedCountValid = false;
    const int lastClosedCount = parser.value(openLastClosedBookOption).toInt(&lastClosedCountValid);
    if (openLastClosedBookRequested && (!lastClosedCountValid || lastClosedCount <= 0)) {
        QTextStream(stderr) << "--open-last-closed-book requires a positive integer\n";
        return 1;
    }
    const QStringList args = parser.positionalArguments();
    if (parser.isSet(checkAndRemoveWatermarkPdfOnlyOption)) {
        return checkAndRemovePdfWatermarks(args);
    }

    if (parser.isSet(restartBookServerOption) || parser.isSet(refreshReadersOption)) {
        if (bookServerOnly || startNew) {
            QTextStream(stderr) << "Reader refresh/restart cannot be combined with --bookserver or --start-new\n";
            return 1;
        }
        const auto controlServerToken = persistentServerToken();
        if (parser.isSet(restartBookServerOption)) {
            if (!restartBookServer(controlServerToken)) return 1;
        } else {
            QLockFile launchLock(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver-start.lock"));
            if (!launchLock.tryLock(10000) || !refreshRunningReaderProcesses()) return 1;
        }
        if (controlOnly) return 0;
    }

    KCrash::initialize();

    const QString startupFileName = args.isEmpty() ? QString() : localFilePathFromArgument(args.at(0));
    const bool startupFileOpenRequested = !startupFileName.isEmpty() && QFileInfo::exists(startupFileName);
    const std::optional<BookEntry> startupLastOpenedEntry = openLastOpenedBookRequested ? lastOpenedBookEntry() : std::optional<BookEntry>();
    const QList<BookEntry> startupLastClosedEntries = openLastClosedBookRequested ? ReaderHistory::lastClosedBooks(lastClosedCount) : QList<BookEntry>();
    const bool startupDirectReaderMode = startupFileOpenRequested || startupLastOpenedEntry.has_value() || !startupLastClosedEntries.isEmpty();
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
    ReaderControl readerControl([&engine, serverToken, sessionToken] {
        if (engine.rootObjects().isEmpty()) return -1;
        return refreshReaderProcess(engine.rootObjects().first(), serverToken, sessionToken);
    });
    if (!QDBusConnection::sessionBus().registerObject(QStringLiteral("/Reader"), &readerControl, QDBusConnection::ExportAllSlots)) {
        qWarning() << "Unable to register reader refresh control on D-Bus";
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
                                     openBookArgument(navigation, fileArgument, workingDirectory, argumentsRequestReadOnly(arguments), sessionToken, argumentsRequestNoLibraryImport(arguments));
                                 } else if (const auto count = lastClosedBookCountFromActivation(arguments)) {
                                     openLastClosedBooks(navigation, ReaderHistory::lastClosedBooks(*count));
                                 } else if (argumentsRequestOpenLastOpenedBook(arguments)) {
                                     openLastOpenedBook(navigation);
                                 }
                                 return;
                             }
                         }
                     });

    if (!args.isEmpty()) {
        openBookArgument(navigation, args[0], {}, readOnly, sessionToken, noLibraryImport);
    } else if (openLastClosedBookRequested) {
        openLastClosedBooks(navigation, startupLastClosedEntries);
    } else if (openLastOpenedBookRequested) {
        openLastOpenedBook(navigation);
    }

    return QCoreApplication::exec();
}
