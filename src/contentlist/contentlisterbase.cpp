// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "contentlisterbase.h"

#include <QDateTime>
#include <QFileInfo>
#include <QVariantMap>

#include <KFileMetaData/UserMetaData>

ContentListerBase::ContentListerBase(QObject *parent)
    : QObject(parent)
{
}

ContentListerBase::~ContentListerBase() = default;

void ContentListerBase::startSearch(const QList<ContentQuery *> &queries)
{
    Q_UNUSED(queries);
}

QVariantMap ContentListerBase::metaDataForFile(const QString &file)
{
    QVariantMap metadata;

    // TODO: This should include the same information for both the Baloo and
    // File searchers. Unfortunately, currently KFileMetaData does not seem able
    // to provide this. So this needs changes at a lower level.

    QFileInfo info(file);
    metadata[QStringLiteral("lastModified")] = info.lastModified();
    metadata[QStringLiteral("created")] = info.birthTime();
    metadata[QStringLiteral("lastRead")] = info.lastRead();

    KFileMetaData::UserMetaData data(file);
    if (data.hasAttribute(QStringLiteral("peruse.currentPage"))) {
        int currentPage = data.attribute(QStringLiteral("peruse.currentPage")).toInt();
        metadata[QStringLiteral("currentPage")] = QVariant::fromValue<int>(currentPage);
    }
    if (data.hasAttribute(QStringLiteral("peruse.totalPages"))) {
        int totalPages = data.attribute(QStringLiteral("peruse.totalPages")).toInt();
        metadata[QStringLiteral("totalPages")] = QVariant::fromValue<int>(totalPages);
    }
    if (!data.tags().isEmpty()) {
        metadata[QStringLiteral("tags")] = QVariant::fromValue<QStringList>(data.tags());
    }
    if (!data.userComment().isEmpty()) {
        metadata[QStringLiteral("comment")] = QVariant::fromValue<QString>(data.userComment());
    }
    metadata[QStringLiteral("rating")] = QVariant::fromValue<int>(data.rating());

    return metadata;
}

#include "moc_contentlisterbase.cpp"
