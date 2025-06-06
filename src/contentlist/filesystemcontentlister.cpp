// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "filesystemcontentlister.h"

#include <KFileMetaData/UserMetaData>

#include <QCoreApplication>
#include <QDateTime>
#include <QDirIterator>
#include <QMimeDatabase>
#include <QThreadPool>
#include <QTimer>
#include <QVariantHash>

#include "contentquery.h"

class FileSystemSearcher : public QObject, public QRunnable
{
    Q_OBJECT
public:
    FileSystemSearcher(ContentQuery *query)
        : QObject()
    {
        m_query = query;
    }

    void run() override
    {
        QMimeDatabase mimeDb;

        auto locations = m_query->locations();
        if (locations.isEmpty())
            locations.append(QDir::homePath());

        for (const auto &location : std::as_const(locations)) {
            QDirIterator it(location, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                auto filePath = it.next();

                if (it.fileInfo().isDir())
                    continue;

                if (!m_query->mimeTypes().isEmpty()) {
                    QString mimeType = mimeDb.mimeTypeForName(filePath).name();
                    if (!m_query->mimeTypes().contains(mimeType)) {
                        continue;
                    }
                }

                auto metadata = ContentListerBase::metaDataForFile(filePath);

                Q_EMIT fileFound(filePath, metadata);
            }
        }

        Q_EMIT finished(this);
    }

Q_SIGNALS:
    void fileFound(const QString &path, const QVariantMap &metaData);
    void finished(FileSystemSearcher *searcher);

private:
    ContentQuery *m_query;
};

class FilesystemContentLister::Private
{
public:
    Private()
    {
    }

    QList<FileSystemSearcher *> runnables;
};

FilesystemContentLister::FilesystemContentLister(QObject *parent)
    : ContentListerBase(parent)
    , d(std::make_unique<Private>())
{
}

FilesystemContentLister::~FilesystemContentLister()
{
    QThreadPool::globalInstance()->waitForDone();
}

void FilesystemContentLister::startSearch(const QList<ContentQuery *> &queries)
{
    for (const auto &query : queries) {
        auto runnable = new FileSystemSearcher{query};
        connect(runnable, &FileSystemSearcher::fileFound, this, &FilesystemContentLister::fileFound);
        connect(runnable, &FileSystemSearcher::finished, this, &FilesystemContentLister::queryFinished);

        d->runnables.append(runnable);
    }

    if (!d->runnables.isEmpty()) {
        QThreadPool::globalInstance()->start(d->runnables.first());
    }
}

void FilesystemContentLister::queryFinished(QRunnable *runnable)
{
    d->runnables.removeAll(static_cast<FileSystemSearcher *>(runnable));

    if (!d->runnables.isEmpty()) {
        QThreadPool::globalInstance()->start(d->runnables.first());
    } else {
        Q_EMIT searchCompleted();
    }
}

#include "filesystemcontentlister.moc"

#include "moc_filesystemcontentlister.cpp"
