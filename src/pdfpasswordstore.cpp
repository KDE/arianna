// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pdfpasswordstore.h"

#include <KLocalizedString>
#include <KWallet>
#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QThread>
#include <atomic>
#include <exception>
#include <podofo/main/PdfMemDocument.h>

struct PdfPasswordRequest {
    quint64 id;
    QUrl source;
    QString bookId;
    QString key;
    QString password;
    WId windowId;
    bool write;
    bool check;
    bool allowWalletOpen;
    quint64 walletRevision = 0;
    QByteArray fileRevision;
    std::atomic_bool cancelled = false;
};

namespace
{
const QString folder = QStringLiteral("arianna");

QByteArray fileRevision(const QUrl &source, const QString &bookId)
{
    const QFileInfo info(source.toLocalFile());
    if (!source.isLocalFile() || !info.isFile()) {
        return {};
    }
    return source.toEncoded() + '\0' + bookId.toUtf8() + '\0' + QByteArray::number(info.size())
        + ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch())
        + ':' + QByteArray::number(info.metadataChangeTime().toMSecsSinceEpoch());
}

QString documentKey(const std::shared_ptr<PdfPasswordRequest> &request)
{
    if (!request->key.isEmpty()) {
        return request->key;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Generated arianna-pdf identifiers contain a path hash. For those books,
    // hash the original file instead, so renames and copies retain the password.
    if (!request->bookId.isEmpty() && !request->bookId.startsWith(QLatin1String("arianna-pdf-"))) {
        hash.addData(QByteArrayLiteral("book:"));
        hash.addData(request->bookId.toUtf8());
    } else if (request->source.isLocalFile() || request->source.scheme().isEmpty()) {
        QFile file(request->source.isLocalFile() ? request->source.toLocalFile() : request->source.toString());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        hash.addData(QByteArrayLiteral("file:"));
        while (!file.atEnd()) {
            if (request->cancelled) {
                return {};
            }
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFile::NoError) {
                return {};
            }
            hash.addData(chunk);
        }
    } else {
        hash.addData(QByteArrayLiteral("url:"));
        hash.addData(request->source.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toEncoded());
    }
    return QStringLiteral("pdf-v1:%1").arg(QString::fromLatin1(hash.result().toHex()));
}
}

// KWallet's asynchronous open still has synchronous D-Bus setup, read and write
// calls. All KWallet objects live on this one thread, including their destruction.
class PdfWalletWorker : public QObject
{
    Q_OBJECT
public:
    PdfWalletWorker()
    {
        auto bus = QDBusConnection::sessionBus();
        const QString service = QStringLiteral("org.kde.kwalletd6");
        const QString path = QStringLiteral("/modules/kwalletd6");
        const QString interface = QStringLiteral("org.kde.KWallet");
        bus.connect(service, path, interface, QStringLiteral("folderUpdated"), this, SLOT(folderUpdated(QString,QString)));
        bus.connect(service, path, interface, QStringLiteral("folderListUpdated"), this, SLOT(folderListUpdated(QString)));
        bus.connect(service, path, interface, QStringLiteral("walletClosed"), this, SLOT(walletClosed(QString)));
        bus.connect(service, path, interface, QStringLiteral("walletDeleted"), this, SLOT(walletClosed(QString)));
        bus.connect(service, path, interface, QStringLiteral("walletOpened"), this, SLOT(walletOpened(QString)));
        auto *watcher = new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForUnregistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { invalidate(false); });
    }

    void start(const std::shared_ptr<PdfPasswordRequest> &request)
    {
        if (request->cancelled) {
            return;
        }
        request->walletRevision = m_revision;
        if (request->check) {
            request->fileRevision = fileRevision(request->source, request->bookId);
            if (request->fileRevision.isEmpty()) {
                Q_EMIT finished(request->id, {}, {}, false, i18n("The PDF file is unavailable."));
                return;
            }
            if (const auto *cached = m_checks.object(request->fileRevision)) {
                Q_EMIT finished(request->id, cached->key, {}, cached->valid, {});
                return;
            }
        }
        const QString key = documentKey(request);
        if (request->cancelled) {
            return;
        }
        if (key.isEmpty()) {
            Q_EMIT finished(request->id, {}, {}, false, i18n("Unable to identify the PDF for KWallet."));
            return;
        }
        if (!QDBusConnection::sessionBus().isConnected() || !KWallet::Wallet::isEnabled()) {
            Q_EMIT finished(request->id, key, {}, false, i18n("KWallet is unavailable or disabled."));
            return;
        }
        const QString name = KWallet::Wallet::LocalWallet();
        if (name.isEmpty()) {
            Q_EMIT finished(request->id, key, {}, false, i18n("KWallet is unavailable or disabled."));
            return;
        }
        if (!request->write && KWallet::Wallet::keyDoesNotExist(name, folder, key)) {
            Q_EMIT finished(request->id, key, {}, false, {});
            return;
        }
        // An explicit cover check may request access. Refreshes after wallet or
        // desktop events must not reopen a wallet the user deliberately closed.
        if (request->check && !request->allowWalletOpen && (m_coverAccessDenied || !KWallet::Wallet::isOpen(name))) {
            Q_EMIT finished(request->id, key, {}, false, {});
            return;
        }
        if (request->cancelled) {
            return;
        }
        auto *wallet = KWallet::Wallet::openWallet(name, request->windowId, KWallet::Wallet::Asynchronous);
        if (!wallet) {
            Q_EMIT finished(request->id, key, {}, false, i18n("KWallet could not be opened. Access may have been denied."));
            return;
        }
        wallet->setParent(this);
        m_wallets.insert(request->id, wallet);
        connect(wallet, &KWallet::Wallet::walletOpened, this, [this, wallet, request, key](bool success) {
            if (request->cancelled) {
                release(request->id);
                return;
            }
            QString error;
            QString password;
            bool found = false;
            const bool opened = success && wallet->isOpen();
            if (request->check || opened) m_coverAccessDenied = !opened;
            if (!opened) {
                error = i18n("KWallet could not be opened. Access may have been denied.");
            } else if (!wallet->hasFolder(folder) && (!request->write || !wallet->createFolder(folder))) {
                if (request->write) {
                    error = i18n("The arianna folder could not be created in KWallet.");
                }
            } else if (!wallet->setFolder(folder)) {
                error = i18n("The arianna folder could not be opened in KWallet.");
            } else if (!request->cancelled) {
                if (request->write) {
                    if (wallet->writePassword(key, request->password) != 0) {
                        error = i18n("The PDF password could not be saved in KWallet.");
                    } else {
                        invalidate(true);
                    }
                } else if (wallet->hasEntry(key)) {
                    if (wallet->readPassword(key, password) != 0) {
                        error = i18n("The PDF password could not be read from KWallet.");
                    } else {
                        found = true;
                    }
                }
            }
            release(request->id);
            if (request->check) {
                bool valid = false;
                if (found && error.isEmpty() && !request->cancelled) {
                    try {
                        PoDoFo::PdfMemDocument document;
                        document.Load(request->source.toLocalFile().toStdString(), password.toUtf8().toStdString());
                        valid = document.IsEncrypted();
                    } catch (const std::exception &) {
                        // Missing/wrong passwords and unreadable PDFs stay locked.
                    }
                }
                password.clear();
                found = valid && request->walletRevision == m_revision
                    && request->fileRevision == fileRevision(request->source, request->bookId);
                if (!request->cancelled && request->walletRevision == m_revision && error.isEmpty()
                    && request->fileRevision == fileRevision(request->source, request->bookId)) {
                    m_checks.insert(request->fileRevision, new CheckResult{key, found});
                }
            }
            if (!request->cancelled) {
                Q_EMIT finished(request->id, key, password, found, error);
            }
        });
        connect(wallet, &KWallet::Wallet::walletClosed, this, [this, request, key] {
            if (m_wallets.contains(request->id)) {
                release(request->id);
                Q_EMIT finished(request->id, key, {}, false, i18n("The KWallet connection was closed."));
            }
        });
        // walletClosed is not emitted if the service disappears during open.
        auto *watcher = new QDBusServiceWatcher(QStringLiteral("org.kde.kwalletd6"), QDBusConnection::sessionBus(),
                                                QDBusServiceWatcher::WatchForUnregistration, wallet);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this, request, key] {
            if (m_wallets.contains(request->id)) {
                release(request->id);
                Q_EMIT finished(request->id, key, {}, false, i18n("The KWallet connection was closed."));
            }
        });
    }

    void release(quint64 id)
    {
        if (auto *wallet = m_wallets.take(id)) {
            wallet->disconnect(this);
            wallet->deleteLater();
        }
    }

    void forget(const QByteArray &revision) { m_checks.remove(revision); }

Q_SIGNALS:
    void finished(quint64 id, const QString &key, const QString &password, bool found, const QString &error);
    void invalidated(bool refresh);

private Q_SLOTS:
    void folderUpdated(const QString &, const QString &changedFolder)
    {
        if (changedFolder == folder) invalidate(true);
    }
    void walletClosed(const QString &) { invalidate(false); }
    void folderListUpdated(const QString &) { invalidate(true); }
    void walletOpened(const QString &) { m_coverAccessDenied = false; invalidate(true); }

private:
    void invalidate(bool refresh)
    {
        ++m_revision;
        m_checks.clear();
        Q_EMIT invalidated(refresh);
    }
    struct CheckResult { QString key; bool valid; };
    // Session-only booleans, never credentials or persistent analysis data.
    QCache<QByteArray, CheckResult> m_checks{256};
    quint64 m_revision = 0;
    bool m_coverAccessDenied = false;
    QHash<quint64, KWallet::Wallet *> m_wallets;
};

namespace
{
PdfWalletWorker *walletWorker()
{
    // Created on first use, never for PDFs that do not need a password.
    static auto *worker = [] {
        auto *thread = new QThread(qApp);
        auto *worker = new PdfWalletWorker;
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread] {
            thread->quit();
            thread->wait();
        });
        thread->start();
        return worker;
    }();
    return worker;
}
}

PdfPasswordStore::PdfPasswordStore(QObject *parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(60000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        const bool checking = m_request && m_request->check;
        cancel();
        setPasswordValid(false);
        if (checking) Q_EMIT checked(false);
        Q_EMIT failed(i18n("KWallet did not respond in time. You can enter the PDF password manually."));
    });
    const auto changed = [this](bool fileEvent) {
        if (!m_checking) return;
        const auto revision = fileRevision(m_source, m_bookId);
        if (!fileEvent && revision == m_fileRevision) return;
        cancel();
        m_key.clear();
        setPasswordValid(false);
        auto *worker = walletWorker();
        const auto previous = m_fileRevision;
        QMetaObject::invokeMethod(worker, [worker, previous, revision] {
            worker->forget(previous);
            worker->forget(revision);
        });
        watchFile();
        Q_EMIT invalidated(true);
    };
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this, [changed] { changed(true); });
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, [changed] { changed(false); });
}

PdfPasswordStore::~PdfPasswordStore()
{
    cancel();
}

QUrl PdfPasswordStore::source() const
{
    return m_source;
}

void PdfPasswordStore::setSource(const QUrl &source)
{
    if (m_source != source) {
        cancel();
        m_source = source;
        m_key.clear();
        m_checking = false;
        setPasswordValid(false);
        watchFile();
        Q_EMIT identityChanged();
    }
}

QString PdfPasswordStore::fileName() const
{
    return m_source.toLocalFile();
}

void PdfPasswordStore::setFileName(const QString &fileName)
{
    setSource(fileName.isEmpty() ? QUrl() : QUrl::fromLocalFile(fileName));
}

QString PdfPasswordStore::bookId() const
{
    return m_bookId;
}

void PdfPasswordStore::setBookId(const QString &bookId)
{
    if (m_bookId != bookId) {
        cancel();
        m_bookId = bookId;
        m_key.clear();
        setPasswordValid(false);
        watchFile();
        Q_EMIT identityChanged();
    }
}

bool PdfPasswordStore::busy() const
{
    return bool(m_request);
}

bool PdfPasswordStore::passwordValid() const
{
    return m_passwordValid;
}

void PdfPasswordStore::setPasswordValid(bool valid)
{
    if (m_passwordValid != valid) {
        m_passwordValid = valid;
        Q_EMIT passwordValidChanged();
    }
}

void PdfPasswordStore::watchFile()
{
    const auto paths = m_fileWatcher.files() + m_fileWatcher.directories();
    if (!paths.isEmpty()) m_fileWatcher.removePaths(paths);
    m_fileRevision = fileRevision(m_source, m_bookId);
    if (m_checking && m_source.isLocalFile()) {
        const QFileInfo info(m_source.toLocalFile());
        if (info.isFile()) m_fileWatcher.addPath(info.absoluteFilePath());
        m_fileWatcher.addPath(info.absolutePath());
    }
}

void PdfPasswordStore::check(bool allowWalletOpen)
{
    setPasswordValid(false);
    m_checking = true;
    watchFile();
    start(false, {}, true, allowWalletOpen);
}

void PdfPasswordStore::load()
{
    start(false);
}

void PdfPasswordStore::save(const QString &password)
{
    start(true, password);
}

void PdfPasswordStore::cancel()
{
    m_timeout.stop();
    if (m_request) {
        m_request->cancelled = true;
        const quint64 id = m_request->id;
        m_request.reset();
        auto *worker = walletWorker();
        QMetaObject::invokeMethod(worker, [worker, id] { worker->release(id); });
        Q_EMIT busyChanged();
    }
}

void PdfPasswordStore::start(bool write, const QString &password, bool check, bool allowWalletOpen)
{
    cancel();
    if (m_source.isEmpty()) {
        Q_EMIT failed(i18n("Unable to identify the PDF for KWallet."));
        return;
    }
    static quint64 nextId = 0;
    auto request = std::make_shared<PdfPasswordRequest>();
    request->id = ++nextId;
    request->source = m_source;
    request->bookId = m_bookId;
    // Cover checks identify the current file revision. A reader's previous key
    // must not validate credentials against changed bytes.
    request->key = check ? QString() : m_key;
    request->password = password;
    request->write = write;
    request->check = check;
    request->allowWalletOpen = allowWalletOpen;
    request->windowId = m_window ? m_window->winId() : 0;
    m_request = request;
    auto *worker = walletWorker();
    connect(worker, &PdfWalletWorker::finished, this, &PdfPasswordStore::finish, Qt::UniqueConnection);
    connect(worker, &PdfWalletWorker::invalidated, this, &PdfPasswordStore::walletInvalidated, Qt::UniqueConnection);
    m_timeout.start();
    Q_EMIT busyChanged();
    QMetaObject::invokeMethod(worker, [worker, request] { worker->start(request); });
}

void PdfPasswordStore::walletInvalidated(bool refresh)
{
    if (!m_checking) return;
    cancel();
    setPasswordValid(false);
    Q_EMIT invalidated(refresh);
}

void PdfPasswordStore::finish(quint64 id, const QString &key, const QString &password, bool found, const QString &error)
{
    if (!m_request || m_request->id != id) {
        return;
    }
    const bool write = m_request->write;
    const bool check = m_request->check;
    m_key = key;
    m_request.reset();
    m_timeout.stop();
    Q_EMIT busyChanged();
    if (check) {
        setPasswordValid(error.isEmpty() && found);
        Q_EMIT checked(m_passwordValid);
    }
    if (!error.isEmpty()) {
        Q_EMIT failed(error);
    } else if (write) {
        Q_EMIT saved();
    } else if (!check) {
        Q_EMIT loaded(password, found);
    }
}

#include "pdfpasswordstore.moc"
