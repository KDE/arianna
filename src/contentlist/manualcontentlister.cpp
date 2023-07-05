// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "manualcontentlister.h"
#include "contentquery.h"
#include <QDebug>
#include <QMimeDatabase>
#include <QUrl>
#include <QVariant>

class ManualContentLister::Private
{
public:
    Private(ManualContentLister *qq)
        : q(qq)
    {
    }
    ManualContentLister *q = nullptr;
    QList<ContentQuery *> queries;
    QMimeDatabase mimeDatabase;
};

ManualContentLister::ManualContentLister(QObject *parent)
    : ContentListerBase(parent)
    , d(std::make_unique<ManualContentLister::Private>(this))
{
}

ManualContentLister::~ManualContentLister() = default;

void ManualContentLister::startSearch(const QList<ContentQuery *> &queries)
{
    d->queries = queries;
}

bool ManualContentLister::addFile(const QUrl &filePath)
{
    const auto &mimeType = d->mimeDatabase.mimeTypeForFile(filePath.toLocalFile()).name();
    bool mimeTypeAccepted = false;
    for (const auto &query : std::as_const(d->queries)) {
        if (query->mimeTypes().contains(mimeType)) {
            mimeTypeAccepted = true;
        }
    }
    if (!mimeTypeAccepted) {
        return false;
    }

    auto metadata = ContentListerBase::metaDataForFile(filePath.toLocalFile());

    Q_EMIT fileFound(filePath.toLocalFile(), metadata);

    return true;
}

#include "moc_manualcontentlister.cpp"
