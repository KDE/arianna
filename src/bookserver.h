#pragma once

#include <QDateTime>
#include <QHash>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QMap>
#include <QSet>
#include <QString>

#include <memory>

#include "epubcontainer.h"
class BookServer
{
public:
    explicit BookServer(const QString &token = QString(), bool quitWhenUnused = false);
    ~BookServer();
    using ResourceMap = QMap<QString, QString>;
    bool isRunning() const;
    void stop();
    void addSessionToken(const QString &token);
    void removeToken(const QString &token);
    void registerReaderSession(const QString &token);
    bool unregisterReaderSession(const QString &token);
    bool hasRegisteredReaders() const;
    int registeredReaderCount() const;
    ResourceMap makeResourceMap(const QSet<QString> &servedFiles, const QString &identifier, const QString &sessionToken = QString());

private:
    bool isValidToken(const QString &token) const;
    std::shared_ptr<EPubContainer> containerForIdentifier(const QString &identifier, const QString &cacheVersion = QString());
    QString pdfFileForIdentifier(const QString &identifier) const;
    void clearServedResourcesForIdentifier(const QString &identifier);
    void clearServedResourcesForSessionIdentifier(const QString &sessionToken, const QString &identifier);
    void registerReaderSessionForIdentifier(const QString &sessionToken, const QString &identifier);
    void releaseReaderSessionResources(const QString &sessionToken);
    void scheduleStopIfUnused();
    struct ServedResource {
        QString identifier;
        QString path;
        QString sessionToken;
    };
    struct CachedContainer {
        std::shared_ptr<EPubContainer> container;
        QString filename;
        QString cacheVersion;
        QDateTime lastModified;
        qint64 size = -1;
    };
    QHttpServer server;
    QString m_serverToken;
    QSet<QString> m_validTokens;
    QHash<QString, ServedResource> m_resourceByUuid;
    QMap<QString, QSet<QString>> m_servedFilesByIdentifier;
    QMap<QString, QMap<QString, QString>> m_resourceMapByIdentifier;
    QMap<QString, CachedContainer> m_containerCache;
    QHash<QString, QString> m_readOnlyFilesByIdentifier;
    QHash<QString, int> m_readerSessionRefCount;
    QHash<QString, QSet<QString>> m_identifiersByReaderSession;
    QHash<QString, QSet<QString>> m_readerSessionsByIdentifier;

    bool m_running = false;
    bool m_quitWhenUnused = false;
    bool m_stopScheduled = false;
};
