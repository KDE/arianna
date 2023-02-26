// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "baloocontentlister.h"

#include <Baloo/File>
#include <Baloo/IndexerConfig>
#include <KFileMetaData/PropertyInfo>
#include <KFileMetaData/UserMetaData>

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QList>
#include <QMimeDatabase>
#include <QProcess>
#include <QThreadPool>

#include "contentquery.h"

class BalooContentLister::Private
{
public:
    Private(BalooContentLister *qq)
        : q(qq)
    {
    }

    BalooContentLister *q = nullptr;

    Baloo::QueryRunnable *createQuery(ContentQuery *contentQuery, const QString &location = QString{});

    QStringList locations;
    QString searchString;
    QList<Baloo::QueryRunnable *> queries;
    QList<QString> queryLocations;

    QMimeDatabase mimeDatabase;
};

BalooContentLister::BalooContentLister(QObject *parent)
    : ContentListerBase(parent)
    , d(std::make_unique<Private>(this))
{
}

BalooContentLister::~BalooContentLister()
{
    QThreadPool::globalInstance()->waitForDone();
}

bool BalooContentLister::balooEnabled() const
{
    // Baloo is not intended to be used outside of Plasma sessions
    // and so we can bypass all the testing if we are not actually
    // in a full KDE session.
    bool result{qEnvironmentVariableIsSet("KDE_FULL_SESSION")};

    if (result) {
        Baloo::IndexerConfig config;
        result = config.fileIndexingEnabled();

        if (result) {
            // It would be terribly nice with a bit of baloo engine exporting, so
            // we can ask the database about whether or not it is accessible...
            // But, this is a catch-all check anyway, so we get a complete "everything's broken"
            // result if anything is broken... guess it will do :)
            QProcess statuscheck;
            statuscheck.start(QStringLiteral("balooctl"), QStringList() << QStringLiteral("status"));
            statuscheck.waitForFinished();
            if (statuscheck.exitStatus() == QProcess::CrashExit || statuscheck.exitCode() != 0) {
                result = false;
            }
        }
    }

    return result;
}

void BalooContentLister::startSearch(const QList<ContentQuery *> &queries)
{
    for (const auto &query : queries) {
        const auto locations = query->locations();
        for (const auto &location : locations) {
            d->queries.append(d->createQuery(query, location));
        }

        if (query->locations().isEmpty()) {
            d->queries.append(d->createQuery(query));
        }
    }

    if (!d->queries.empty()) {
        qWarning() << "starting query" << d->queries.first();
        QThreadPool::globalInstance()->start(d->queries.first());
    }
}

void BalooContentLister::queryCompleted(Baloo::QueryRunnable *query)
{
    d->queries.removeAll(query);
    if (d->queries.empty()) {
        Q_EMIT searchCompleted();
    } else {
        qWarning() << "starting query" << d->queries.first();
        QThreadPool::globalInstance()->start(d->queries.first());
    }
}

void BalooContentLister::queryResult(const ContentQuery *query, const QString &location, const QString &file)
{
    Q_UNUSED(location)
    if (knownFiles.contains(file)) {
        return;
    }

    // Like the one above, this is also not nice: apparently Baloo can return results to
    // files that no longer exist on the file system. So we have to check manually whether
    // the results provided are actually sensible results...
    if (!QFileInfo::exists(file)) {
        return;
    }

    // It would be nice if Baloo could do mime type filtering on its own...
    if (!query->mimeTypes().isEmpty()) {
        const auto &mimeType = d->mimeDatabase.mimeTypeForFile(file).name();
        if (!query->mimeTypes().contains(mimeType))
            return;
    }

    auto metadata = metaDataForFile(file);

    Baloo::File balooFile(file);
    balooFile.load();
    auto properties = balooFile.properties();
    auto it = properties.constBegin();
    for (; it != properties.constEnd(); it++) {
        KFileMetaData::PropertyInfo propInfo(it.key());
        metadata[propInfo.name()] = it.value();
    }

    knownFiles << file;
    Q_EMIT fileFound(file, metadata);
}

Baloo::QueryRunnable *BalooContentLister::Private::createQuery(ContentQuery *contentQuery, const QString &location)
{
    auto balooQuery = Baloo::Query{};

    switch (contentQuery->type()) {
    case ContentQuery::Audio:
        balooQuery.setType(QStringLiteral("Audio"));
        break;
    case ContentQuery::Documents:
    case ContentQuery::Epub:
        balooQuery.setType(QStringLiteral("Document"));
        break;
    case ContentQuery::Images:
        balooQuery.setType(QStringLiteral("Image"));
        break;
    case ContentQuery::Video:
        balooQuery.setType(QStringLiteral("Video"));
        break;
        break;
    default:
        break;
    }

    balooQuery.setSearchString(QStringLiteral("epub"));

    auto runnable = new Baloo::QueryRunnable{balooQuery};
    connect(runnable, &Baloo::QueryRunnable::queryResult, q, [this, contentQuery, location](QRunnable *, const QString &file) {
        q->queryResult(contentQuery, location, file);
    });
    connect(runnable, &Baloo::QueryRunnable::finished, q, &BalooContentLister::queryCompleted);

    return runnable;
}
