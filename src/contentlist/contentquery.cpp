// SPDX-FileCopyrightText: 2018 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "contentquery.h"

#include <QMimeDatabase>
#include <QtGui/QImageReader>

class ContentQuery::Private
{
public:
    QStringList mimeTypesForType(Type type);

    Type type = Any;
    QString searchString;
    QStringList locations;
    QStringList mimeTypes;
};

ContentQuery::ContentQuery(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

ContentQuery::~ContentQuery()
{
}

ContentQuery::Type ContentQuery::type() const
{
    return d->type;
}

QString ContentQuery::searchString() const
{
    return d->searchString;
}

QStringList ContentQuery::locations() const
{
    return d->locations;
}

QStringList ContentQuery::mimeTypes() const
{
    if (!d->mimeTypes.isEmpty())
        return d->mimeTypes;

    return d->mimeTypesForType(d->type);
}

void ContentQuery::setType(ContentQuery::Type type)
{
    if (type == d->type)
        return;

    d->type = type;
    Q_EMIT typeChanged();
}

void ContentQuery::setSearchString(const QString &searchString)
{
    if (searchString == d->searchString)
        return;

    d->searchString = searchString;
    Q_EMIT searchStringChanged();
}

void ContentQuery::setLocations(const QStringList &locations)
{
    if (locations == d->locations)
        return;

    d->locations = locations;
    Q_EMIT locationsChanged();
}

void ContentQuery::setMimeTypes(const QStringList &mimeTypes)
{
    if (mimeTypes == d->mimeTypes)
        return;

    d->mimeTypes = mimeTypes;
    Q_EMIT mimeTypesChanged();
}

namespace
{
QStringList contentQueryVideo()
{
    return {
        QStringLiteral("video/x-matroska"),
        QStringLiteral("video/mp4"),
        QStringLiteral("video/mpeg"),
        QStringLiteral("video/ogg"),
        QStringLiteral("video/quicktime"),
        QStringLiteral("video/webm"),
        QStringLiteral("video/x-ms-wmv"),
        QStringLiteral("video/x-msvideo"),
        QStringLiteral("video/x-ogm+ogg"),
        QStringLiteral("video/x-theora+ogg"),
    };
}

QStringList contentQueryAudio()
{
    return {
        QStringLiteral("audio/aac"),
        QStringLiteral("audio/flac"),
        QStringLiteral("audio/mp2"),
        QStringLiteral("audio/mp4"),
        QStringLiteral("audio/mpeg"),
        QStringLiteral("audio/ogg"),
        QStringLiteral("audio/webm"),
        QStringLiteral("audio/x-opus+ogg"),
        QStringLiteral("audio/x-ms-wma"),
        QStringLiteral("audio/x-vorbis+ogg"),
        QStringLiteral("audio/x-wav"),
    };
}

QStringList contentQueryDocuments()
{
    return {
        QStringLiteral("application/vnd.oasis.opendocument.text"),
        QStringLiteral("application/vnd.oasis.opendocument.spreadsheet"),
        QStringLiteral("application/vnd.oasis.opendocument.presentation"),
        QStringLiteral("application/vnd.ms-word"),
        QStringLiteral("application/vnd.ms-excel"),
        QStringLiteral("application/vnd.ms-powerpoint"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.xml"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document.xml"),
        QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation.xml"),
        QStringLiteral("text/plain"),
        QStringLiteral("application/pdf"),
    };
}

QStringList contentQueryImages()
{
    // only popylate once.
    static QStringList result;
    if (result.isEmpty()) {
        const auto mimeTypes = QImageReader::supportedMimeTypes();
        for (const auto &item : mimeTypes) {
            result << QString::fromUtf8(item);
        }
    }
    return result;
}

QStringList contentQueryEpub()
{
    return {
        QStringLiteral("application/epub+zip"),
    };
}

QStringList contentQueryComics()
{
    return {
        QStringLiteral("application/x-cbz"),
        QStringLiteral("application/x-cbr"),
        QStringLiteral("application/x-cb7"),
        QStringLiteral("application/x-cbt"),
        QStringLiteral("application/x-cba"),
        QStringLiteral("application/vnd.comicbook+zip"),
        QStringLiteral("application/vnd.comicbook+rar"),
        QStringLiteral("application/vnd.ms-htmlhelp"),
        QStringLiteral("image/vnd.djvu"),
        QStringLiteral("image/x-djvu"),
        QStringLiteral("application/epub+zip"),
        QStringLiteral("application/pdf"),
    };
}
}
QStringList ContentQuery::Private::mimeTypesForType(ContentQuery::Type type)
{
    switch (type) {
    case ContentQuery::Type::Video:
        return contentQueryVideo();
    case ContentQuery::Type::Audio:
        return contentQueryAudio();
    case ContentQuery::Type::Documents:
        return contentQueryDocuments();
    case ContentQuery::Type::Images:
        return contentQueryImages();
    case ContentQuery::Type::Comics:
        return contentQueryComics();
    case ContentQuery::Type::Epub:
        return contentQueryEpub();
    case ContentQuery::Type::Any: /* do nothing */
        return {};
    }
    return {};
}

#include "moc_contentquery.cpp"
