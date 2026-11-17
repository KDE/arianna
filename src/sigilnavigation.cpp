// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "sigilnavigation.h"

#include <QCoreApplication>
#include <QDBusError>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>

namespace
{
const QString serviceName = QStringLiteral("org.sigil_ebook.Sigil");

QDBusMessage busCall(const QString &method, const QVariantList &arguments = {})
{
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                  QStringLiteral("/org/freedesktop/DBus"),
                                                  QStringLiteral("org.freedesktop.DBus"),
                                                  method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    return message;
}

QDBusMessage editorCall(const QString &owner, const QString &method, const QVariantList &arguments = {})
{
    auto message = QDBusMessage::createMethodCall(owner, QStringLiteral("/Editor"), QStringLiteral("org.sigil_ebook.Editor1"), method);
    message.setArguments(arguments);
    message.setAutoStartService(false);
    return message;
}

SigilNavigation::Outcome outcomeForError(const QDBusError &error)
{
    if (error.type() == QDBusError::ServiceUnknown || error.name() == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")) {
        return SigilNavigation::ServiceUnavailable;
    }
    if (error.name() == QLatin1String("org.sigil_ebook.Editor1.Error.BookNotOpen") || error.name() == QLatin1String("org.sigil_ebook.Editor1.BookNotOpen")
        || error.name() == QLatin1String("org.sigil_ebook.Editor1.WrongBook")) {
        return SigilNavigation::BookNotOpen;
    }
    if (error.name() == QLatin1String("org.sigil_ebook.Editor1.Error.Busy")) {
        return SigilNavigation::Busy;
    }
    if (error.name() == QLatin1String("org.sigil_ebook.Editor1.Error.AmbiguousBook")) {
        return SigilNavigation::AmbiguousBook;
    }
    if (error.name() == QLatin1String("org.sigil_ebook.Editor1.Error.InvalidArguments")) {
        return SigilNavigation::Rejected;
    }
    if (error.name() == QLatin1String("org.sigil_ebook.Editor1.Error.OpenFailed")) {
        return SigilNavigation::OpenFailed;
    }
    switch (error.type()) {
    case QDBusError::UnknownMethod:
    case QDBusError::UnknownInterface:
    case QDBusError::UnknownObject:
    case QDBusError::NotSupported:
    case QDBusError::AccessDenied:
    case QDBusError::InvalidArgs:
        return SigilNavigation::Rejected;
    default:
        // Navigation may already have run when its reply is lost or malformed.
        return SigilNavigation::IpcError;
    }
}

bool compatible(const QVariantMap &capabilities)
{
    const auto version = capabilities.value(QStringLiteral("protocolVersion"));
    const auto methods = capabilities.value(QStringLiteral("methods"));
    const auto navigation = capabilities.value(QStringLiteral("navigateToText"));
    const auto ready = capabilities.value(QStringLiteral("ready"));
    return version.metaType() == QMetaType::fromType<uint>() && version.toUInt() == 1 && methods.metaType() == QMetaType::fromType<QStringList>()
        && methods.toStringList().contains(QStringLiteral("NavigateToText")) && navigation.metaType() == QMetaType::fromType<bool>() && navigation.toBool()
        && ready.metaType() == QMetaType::fromType<bool>();
}

qint64 startingSigilProcess(const QDBusConnection &connection)
{
#ifdef Q_OS_LINUX
    // A manually launched Sigil may still be initializing Python/WebEngine.
    // Only consider this user's session bus, never another private test bus.
    const auto address = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    if (address.isEmpty() || connection.name() != QDBusConnection::sessionBus().name())
        return -1;
    for (const auto &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const auto pid = entry.toLongLong(&numeric);
        if (!numeric || pid == QCoreApplication::applicationPid())
            continue;
        const QString base = QStringLiteral("/proc/") + entry;
        const QString executable = QFileInfo(base + QStringLiteral("/exe")).symLinkTarget();
        if (QFileInfo(executable).completeBaseName().compare(QLatin1String("sigil"), Qt::CaseInsensitive) != 0)
            continue;
        QFile environment(base + QStringLiteral("/environ"));
        if (environment.open(QIODevice::ReadOnly) && environment.readAll().split('\0').contains("DBUS_SESSION_BUS_ADDRESS=" + address))
            return pid;
    }
#else
    Q_UNUSED(connection);
#endif
    return -1;
}
}

SigilNavigation::SigilNavigation(QObject *parent)
    : SigilNavigation(QDBusConnection::sessionBus(), 3000, parent)
{
}

SigilNavigation::SigilNavigation(const QDBusConnection &connection, int timeoutMs, QObject *parent)
    : QObject(parent)
    , m_connection(connection)
    , m_timeoutMs(qMax(1, timeoutMs))
    , m_readinessTimeoutMs(m_timeoutMs * 5)
{
}

void SigilNavigation::cancel()
{
    ++m_request;
    for (auto *watcher : findChildren<QDBusPendingCallWatcher *>()) {
        watcher->disconnect(this);
        watcher->deleteLater();
    }
    m_owner.clear();
    m_bookPath.clear();
    m_text.clear();
    m_documentFile.clear();
    m_documentRequestId.clear();
    m_anchorId.clear();
    m_verseReference.clear();
    m_processId = -1;
    m_waitingForStartup = false;
    m_deliveredByCli = false;
    m_canOpenBook = false;
    m_readinessTimer.invalidate();
}

void SigilNavigation::call(const QDBusMessage &message, std::function<void(const QDBusMessage &)> callback, int timeoutMs)
{
    qInfo().noquote() << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << "Sigil IPC Arianna PID" << QCoreApplication::applicationPid()
                      << "request" << m_request << "path=SigilNavigation::call" << "service" << message.service() << "owner" << m_owner << "editor PID"
                      << m_processId << message.path() << message.member() << message.arguments();
    const auto request = m_request;
    auto *watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(message, timeoutMs < 0 ? m_timeoutMs : timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request, callback = std::move(callback)](QDBusPendingCallWatcher *watcher) {
        const auto reply = watcher->reply();
        watcher->deleteLater();
        if (request == m_request) {
            callback(reply);
        }
    });
}

void SigilNavigation::complete(Outcome outcome, const QString &errorName, const QString &message)
{
    const QString owner = m_owner;
    const qint64 processId = m_processId;
    cancel();
    Q_EMIT finished(outcome, owner, processId, errorName, message);
}

void SigilNavigation::navigate(const QString &bookPath, const QString &text)
{
    cancel();
    m_bookPath = bookPath;
    m_text = text;
    if (!m_connection.isConnected()) {
        const auto request = m_request;
        QTimer::singleShot(0, this, [this, request] {
            if (request == m_request) {
                const auto error = m_connection.lastError();
                complete(ServiceUnavailable, error.name(), error.message());
            }
        });
        return;
    }
    discover();
}

void SigilNavigation::awaitStartup(const QString &bookPath)
{
    cancel();
    m_bookPath = bookPath;
    m_waitingForStartup = true;
    m_deliveredByCli = true;
    m_readinessTimer.start();
    if (!m_connection.isConnected()) {
        const auto request = m_request;
        QTimer::singleShot(0, this, [this, request] {
            if (request == m_request)
                complete(ServiceUnavailable, {}, QStringLiteral("CLI started; no session bus for readiness verification."));
        });
        return;
    }
    resolveOwner();
}

void SigilNavigation::openDocument(const QString &bookPath, const QString &documentFile, const QString &requestId, bool deliveredByCli,
                                   const QString &anchorId, const QString &verseReference)
{
    cancel();
    m_bookPath = bookPath;
    m_documentFile = documentFile;
    m_documentRequestId = requestId;
    m_anchorId = anchorId;
    m_verseReference = verseReference;
    m_deliveredByCli = deliveredByCli;
    m_waitingForStartup = deliveredByCli;
    if (deliveredByCli) m_readinessTimer.start();
    if (!m_connection.isConnected()) {
        const auto request = m_request;
        QTimer::singleShot(0, this, [this, request] {
            if (request == m_request) complete(ServiceUnavailable, {}, QStringLiteral("Session bus unavailable."));
        });
    } else if (deliveredByCli) {
        resolveOwner();
    } else {
        discover();
    }
}

void SigilNavigation::documentResult(const QVariantMap &result)
{
    const auto normalized = [](const QString &path) {
        const QFileInfo file(path);
        return file.canonicalFilePath().isEmpty() ? QDir::cleanPath(file.absoluteFilePath()) : file.canonicalFilePath();
    };
    if (result.value(QStringLiteral("requestId")).toString() != m_documentRequestId
        || result.value(QStringLiteral("documentFile")).toString() != m_documentFile
        || normalized(result.value(QStringLiteral("bookPath")).toString()) != normalized(m_bookPath)) {
        complete(IpcError, {}, QStringLiteral("Mismatched document request result."));
        return;
    }
    const auto status = result.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("DocumentOpened")) {
        if (!m_anchorId.isEmpty() && (!result.value(QStringLiteral("anchorEditPrepared")).toBool()
            || result.value(QStringLiteral("anchorId")).toString() != m_anchorId
            || result.value(QStringLiteral("verseReference")).toString() != m_verseReference)) {
            complete(IpcError, {}, QStringLiteral("Sigil did not confirm the requested AI anchor instruction. No retry."));
            return;
        }
        complete(m_anchorId.isEmpty() ? DocumentOpened : AnchorEditPrepared);
    } else if (status == QLatin1String("Received")) {
        m_deliveredByCli = true; // Accepted once; only query its result from now on.
        waitForReady([this] { queryCapabilities(); });
    } else if (status == QLatin1String("Failed")) {
        const auto error = result.value(QStringLiteral("errorCode")).toString();
        complete(error == QLatin1String("Busy") ? Busy : error == QLatin1String("AmbiguousBook") ? AmbiguousBook
                     : error == QLatin1String("InstructionConflict") ? Rejected : OpenFailed,
                 QStringLiteral("org.sigil_ebook.Editor1.Error.") + error, error);
    } else {
        complete(IpcError, {}, QStringLiteral("Invalid document request status."));
    }
}

void SigilNavigation::discover()
{
    // Asynchronous equivalent of registeredServiceNames(). Protocol 1 publishes
    // only this fixed service; local Sigil processes without it are not addressable.
    call(busCall(QStringLiteral("ListNames")), [this](const QDBusMessage &message) {
        const QDBusPendingReply<QStringList> reply(message);
        if (reply.isError()) {
            complete(QueryError, reply.error().name(), reply.error().message());
        } else if (reply.value().contains(serviceName)) {
            qInfo() << "Sigil D-Bus: preferring registered service" << serviceName;
            resolveOwner();
        } else {
            m_processId = startingSigilProcess(m_connection);
            if (m_processId > 0) {
                m_waitingForStartup = true;
                m_readinessTimer.start();
                qInfo() << "Sigil is already starting; waiting for registration, no launch. PID" << m_processId;
                resolveOwner();
            } else {
                complete(ServiceUnavailable);
            }
        }
    });
}

void SigilNavigation::resolveOwner()
{
    call(busCall(QStringLiteral("GetNameOwner"), {serviceName}), [this](const QDBusMessage &message) {
        const QDBusPendingReply<QString> reply(message);
        if (reply.isError()) {
            if (outcomeForError(reply.error()) == ServiceUnavailable) {
                if (m_waitingForStartup) {
                    waitForReady(
                        [this] {
                            resolveOwner();
                        },
                        ReadinessTimeout,
                        reply.error().name(),
                        reply.error().message());
                } else {
                    // The name vanished after discovery. A competing start may
                    // have run; do not turn this race into another process.
                    complete(QueryError, reply.error().name(), reply.error().message());
                }
            } else {
                complete(QueryError, reply.error().name(), reply.error().message());
            }
            return;
        }
        m_owner = reply.value();
        // All editor calls are pinned to this owner, including readiness polls.
        call(busCall(QStringLiteral("GetConnectionUnixProcessID"), {m_owner}), [this](const QDBusMessage &message) {
            const QDBusPendingReply<uint> reply(message);
            if (reply.isError()) {
                complete(QueryError, reply.error().name(), reply.error().message());
                return;
            }
            m_processId = reply.value();
            queryCapabilities();
        });
    });
}

void SigilNavigation::queryCapabilities()
{
    call(editorCall(m_owner, QStringLiteral("GetCapabilities")), [this](const QDBusMessage &message) {
        const QDBusPendingReply<QVariantMap> reply(message);
        if (reply.isError()) {
            const auto error = reply.error();
            if (error.type() == QDBusError::UnknownObject || error.type() == QDBusError::UnknownInterface) {
                waitForReady(
                    [this] {
                        queryCapabilities();
                    },
                    ReadinessTimeout,
                    error.name(),
                    error.message());
            } else {
                complete(outcomeForError(error) == Rejected ? Rejected : QueryError, error.name(), error.message());
            }
            return;
        }
        const auto capabilities = reply.value();
        if (!m_documentFile.isEmpty()) {
            if (!compatible(capabilities) || !capabilities.value(QStringLiteral("openBookAtLocation")).toBool()
                || !capabilities.value(QStringLiteral("methods")).toStringList().contains(QStringLiteral("OpenBookAtLocation"))) {
                complete(Rejected, {}, QStringLiteral("Sigil does not support exact XHTML navigation. Install the patched version and restart Sigil."));
            } else if (!m_anchorId.isEmpty() && (!capabilities.value(QStringLiteral("prepareAnchorEdit")).toBool()
                       || !capabilities.value(QStringLiteral("methods")).toStringList().contains(QStringLiteral("PrepareAnchorEdit")))) {
                complete(Rejected, {}, QStringLiteral("Sigil does not support preparing the AI anchor instruction. Install the patched version and restart Sigil."));
            } else if (!capabilities.value(QStringLiteral("ready")).toBool()
                       || (!m_deliveredByCli && capabilities.value(QStringLiteral("navigationPending")).toBool())) {
                waitForReady([this] { queryCapabilities(); });
            } else if (m_deliveredByCli) {
                const auto requests = qdbus_cast<QVariantMap>(capabilities.value(QStringLiteral("documentRequests")));
                const auto result = qdbus_cast<QVariantMap>(requests.value(m_documentRequestId));
                if (result.isEmpty()) complete(IpcError, {}, QStringLiteral("Sigil has no result for the accepted document request. No retry."));
                else documentResult(result);
            } else {
                const bool prepareAnchor = !m_anchorId.isEmpty();
                call(editorCall(m_owner, prepareAnchor ? QStringLiteral("PrepareAnchorEdit") : QStringLiteral("OpenBookAtLocation"),
                                prepareAnchor ? QVariantList{m_bookPath, m_documentFile, m_anchorId, m_verseReference, m_documentRequestId}
                                              : QVariantList{m_bookPath, m_documentFile, m_documentRequestId}),
                     [this](const QDBusMessage &message) {
                         const QDBusPendingReply<QVariantMap> reply(message);
                         if (reply.isError()) complete(IpcError, reply.error().name(), reply.error().message());
                         else documentResult(reply.value());
                     });
            }
            return;
        }
        if (!compatible(capabilities)) {
            complete(Rejected, {}, QStringLiteral("Incompatible GetCapabilities reply: protocol 1 and NavigateToText are required."));
        } else if (!capabilities.value(QStringLiteral("ready")).toBool()
                   || (m_deliveredByCli && capabilities.value(QStringLiteral("navigationPending")).toBool())) {
            waitForReady([this] {
                queryCapabilities();
            });
        } else {
            qInfo() << "Sigil D-Bus: NavigateToText ready" << m_owner << "PID" << m_processId;
            const auto openBook = capabilities.value(QStringLiteral("openBook"));
            m_canOpenBook = openBook.metaType() == QMetaType::fromType<bool>() && openBook.toBool()
                && capabilities.value(QStringLiteral("methods")).toStringList().contains(QStringLiteral("OpenBookAndNavigate"));
            if (m_deliveredByCli)
                complete(Ready);
            else
                sendNavigation();
        }
    });
}

void SigilNavigation::waitForReady(std::function<void()> retry, Outcome timeoutOutcome, const QString &errorName, const QString &message)
{
    if (!m_readinessTimer.isValid()) {
        m_readinessTimer.start();
        qInfo() << "Sigil D-Bus: waiting asynchronously for editor readiness" << m_owner;
    }
    if (m_readinessTimer.elapsed() >= m_readinessTimeoutMs) {
        complete(timeoutOutcome, errorName, message + QStringLiteral(" Sigil editor did not become ready within the readiness deadline."));
        return;
    }
    const auto request = m_request;
    QTimer::singleShot(100, this, [this, request, retry = std::move(retry)] {
        if (request == m_request) {
            retry();
        }
    });
}

void SigilNavigation::sendNavigation()
{
    // Use the explicitly advertised opening method where available. Older
    // services only navigate already open books and may return BookNotOpen.
    // Sigil allows its asynchronous search 15 seconds; include one query-timeout
    // margin for delivery instead of cutting it off after the short bus-query budget.
    call(
        editorCall(m_owner, m_canOpenBook ? QStringLiteral("OpenBookAndNavigate") : QStringLiteral("NavigateToText"), {m_bookPath, m_text}),
        [this](const QDBusMessage &message) {
            const QDBusPendingReply<bool> reply(message);
            if (reply.isError()) {
                const auto outcome = outcomeForError(reply.error());
                // An owner disappearing after dispatch is an IPC failure,
                // never evidence that no editor start has been attempted.
                complete(outcome == ServiceUnavailable ? IpcError : outcome, reply.error().name(), reply.error().message());
            } else {
                complete(reply.value() ? Found : NotFound);
            }
        },
        m_readinessTimeoutMs + m_timeoutMs);
}

#include "moc_sigilnavigation.cpp"
