// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "booklistmodel.h"

#include "bookdatabase.h"

#include <kfilemetadata_version.h>
#if KFILEMETADATA_VERSION < QT_VERSION_CHECK(6, 14, 0)
#include "epubcontainer.h"
#else
#include <KFileMetaData/ExtractorCollection>
#include <KFileMetaData/Properties>
#include <KFileMetaData/UserMetaData>
#endif
#include <KFileMetaData/ExtractionResult>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <arianna_debug.h>

using namespace Qt::StringLiterals;

#if KFILEMETADATA_VERSION >= QT_VERSION_CHECK(6, 14, 0)
class AriannaExtractionResult : public KFileMetaData::ExtractionResult
{
public:
    AriannaExtractionResult(const QString &url, const QString &mimetype)
        : KFileMetaData::ExtractionResult(url, mimetype, KFileMetaData::ExtractionResult::ExtractMetaData | KFileMetaData::ExtractionResult::ExtractImageData)
    {
    }

    void add(KFileMetaData::Property::Property property, const QVariant &value) override
    {
        if (property == KFileMetaData::Property::Title) {
            title = value.toString();
        }

        if (property == KFileMetaData::Property::Author) {
            authors.append(value.toString());
        }

        if (property == KFileMetaData::Property::Publisher) {
            publishers.append(value.toString());
        }

        if (property == KFileMetaData::Property::Subject) {
            genres.append(value.toString());
        }

        if (property == KFileMetaData::Property::Identifier) {
            identifiers.append(value.toString());
        }

        if (property == KFileMetaData::Property::Language) {
            languages.append(value.toString());
        }

        if (property == KFileMetaData::Property::Description) {
            descriptions.append(value.toString());
        }

        if (property == KFileMetaData::Property::License) {
            rights.append(value.toString());
        }

        if (property == KFileMetaData::Property::CreationDate) {
            creation = value.toDateTime();
        }

        if (property == KFileMetaData::Property::OriginUrl) {
            sources.append(value.toString());
        }

        if (property == KFileMetaData::Property::Serie) {
            series.append(value.toString());
        }

        if (property == KFileMetaData::Property::VolumeNumber) {
            volumeNumbers.append(QString::number(value.toInt()));
        }
    }

    void addType(KFileMetaData::Type::Type) override
    {
    }

    void append(const QString &) override
    {
    }

    QString title;
    QStringList identifiers;
    QStringList authors;
    QStringList rights;
    QStringList languages;
    QStringList publishers;
    QStringList genres;
    QStringList sources;
    QStringList descriptions;
    QStringList series;
    QStringList volumeNumbers;
    QDateTime creation;
    QImage cover;
};
#endif

class BookListModel::Private
{
public:
    Private()
        : contentModel(nullptr)
        , newlyAddedCategoryModel(nullptr)
        , authorCategoryModel(nullptr)
        , seriesCategoryModel(nullptr)
        , publisherCategoryModel(nullptr)
        , keywordCategoryModel(nullptr)
        , folderCategoryModel(nullptr)
        , cacheLoaded(false) {};

    QList<BookEntry> entries;

    ContentList *contentModel;
    CategoryEntriesModel *newlyAddedCategoryModel;
    CategoryEntriesModel *authorCategoryModel;
    CategoryEntriesModel *seriesCategoryModel;
    CategoryEntriesModel *publisherCategoryModel;
    CategoryEntriesModel *keywordCategoryModel;
    CategoryEntriesModel *folderCategoryModel;

    bool cacheLoaded;

    void initializeSubModels(BookListModel *q)
    {
        if (!newlyAddedCategoryModel) {
            newlyAddedCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, newlyAddedCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, newlyAddedCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->newlyAddedCategoryModelChanged();
        }
        if (!authorCategoryModel) {
            authorCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, authorCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, authorCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->authorCategoryModelChanged();
        }
        if (!seriesCategoryModel) {
            seriesCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, seriesCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, seriesCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->seriesCategoryModelChanged();
        }
        if (!publisherCategoryModel) {
            publisherCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, publisherCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, publisherCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->publisherCategoryModelChanged();
        }
        if (!keywordCategoryModel) {
            keywordCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, keywordCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, keywordCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->keywordCategoryModelChanged();
        }
        if (!folderCategoryModel) {
            folderCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, folderCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, folderCategoryModel, &CategoryEntriesModel::entryRemoved);
            q->folderCategoryModel();
        }
    }

    void addEntry(BookListModel *q, const BookEntry &entry)
    {
        entries.append(entry);
        q->append(entry);
        for (int i = 0; i < entry.author.size(); i++) {
            authorCategoryModel->addCategoryEntry(entry.author.at(i), entry);
        }
        for (int i = 0; i < entry.series.size(); i++) {
            seriesCategoryModel->addCategoryEntry(entry.series.at(i), entry, SeriesRole);
        }
        if (newlyAddedCategoryModel->indexOfFile(entry.filename) == -1) {
            newlyAddedCategoryModel->append(entry, CreatedRole);
        }
        publisherCategoryModel->addCategoryEntry(entry.publisher, entry);
        QUrl url(entry.filename.left(entry.filename.lastIndexOf(QLatin1Char('/'))));
        folderCategoryModel->addCategoryEntry(url.path().mid(1), entry);
        if (folderCategoryModel->indexOfFile(entry.filename) == -1) {
            folderCategoryModel->append(entry);
        }
        for (int i = 0; i < entry.genres.size(); i++) {
            keywordCategoryModel->addCategoryEntry(entry.genres.at(i), entry, GenreRole);
        }
    }

    void loadCache(BookListModel *q)
    {
        QList<BookEntry> entries = BookDatabase::self().loadEntries();
        if (entries.count() > 0) {
            initializeSubModels(q);
        }
        int i = 0;
        for (const BookEntry &entry : std::as_const(entries)) {
            /*
             * This might turn out a little slow, but we should avoid having entries
             * that do not exist. If we end up with slowdown issues when loading the
             * cache this would be a good place to start investigating.
             */
            if (QFileInfo::exists(entry.filename)) {
                addEntry(q, entry);
                if (++i % 100 == 0) {
                    Q_EMIT q->countChanged();
                    qApp->processEvents();
                }
            } else {
                BookDatabase::self().removeEntry(entry);
            }
        }
        cacheLoaded = true;
        Q_EMIT q->cacheLoadedChanged();
    }
};

BookListModel::BookListModel(QObject *parent)
    : CategoryEntriesModel(parent)
    , d(std::make_unique<Private>())
{
}

BookListModel::~BookListModel() = default;

void BookListModel::componentComplete()
{
    QTimer::singleShot(0, this, [this]() {
        d->loadCache(this);
    });
}

bool BookListModel::cacheLoaded() const
{
    return d->cacheLoaded;
}

void BookListModel::setContentModel(ContentList *newModel)
{
    if (d->contentModel) {
        d->contentModel->disconnect(this);
    }
    d->contentModel = newModel;
    if (d->contentModel) {
        connect(d->contentModel, &QAbstractItemModel::rowsInserted, this, &BookListModel::contentModelItemsInserted);
    }
    Q_EMIT contentModelChanged();
}

ContentList *BookListModel::contentModel() const
{
    return d->contentModel;
}

void BookListModel::contentModelItemsInserted(QModelIndex index, int first, int last)
{
    d->initializeSubModels(this);

    // Process items in batches for better performance
    QList<BookEntry> newEntries;
    newEntries.reserve(last - first + 1);

    for (int i = first; i < last + 1; ++i) {
        QVariant filePath = d->contentModel->data(d->contentModel->index(first, 0, index), ContentList::FilePathRole);
        BookEntry entry;
        entry.filename = filePath.toUrl().toLocalFile();
        QStringList splitName = entry.filename.split(QLatin1Char('/'));
        if (!splitName.isEmpty())
            entry.filetitle = splitName.takeLast();
        if (!splitName.isEmpty()) {
            entry.series = QStringList{};
            entry.seriesNumbers = QStringList{QStringLiteral("0")};
            entry.seriesVolumes = QStringList{QStringLiteral("0")};
        }
        // just in case we end up without a title... using complete basename here,
        // as we would rather have "book one. part two" and the odd "book one - part two.tar"
        QFileInfo fileinfo(entry.filename);
        entry.title = fileinfo.completeBaseName();

        KFileMetaData::UserMetaData data(entry.filename);
        entry.rating = data.rating();
        entry.comment = data.userComment();
        entry.tags = data.tags();

        QVariantHash metadata = d->contentModel->data(d->contentModel->index(first, 0, index), Qt::UserRole + 2).toHash();
        QVariantHash::const_iterator it = metadata.constBegin();
        for (; it != metadata.constEnd(); it++) {
            if (it.key() == QLatin1String("author")) {
                entry.author = it.value().toStringList();
            } else if (it.key() == QLatin1String("title")) {
                entry.title = it.value().toString().trimmed();
            } else if (it.key() == QLatin1String("publisher")) {
                entry.publisher = it.value().toString().trimmed();
            } else if (it.key() == QLatin1String("created")) {
                entry.created = it.value().toDateTime();
            } else if (it.key() == QLatin1String("currentLocation")) {
                entry.currentLocation = it.value().toString();
            } else if (it.key() == QLatin1String("currentProgress")) {
                entry.currentProgress = it.value().toInt();
            } else if (it.key() == QLatin1String("comments")) {
                entry.comment = it.value().toString();
            } else if (it.key() == QLatin1String("tags")) {
                entry.tags = it.value().toStringList();
            } else if (it.key() == QLatin1String("rating")) {
                entry.rating = it.value().toInt();
            }
        }
        static QMimeDatabase db;
        const QString mimetype = db.mimeTypeForFile(entry.filename).name();

#if KFILEMETADATA_VERSION >= QT_VERSION_CHECK(6, 14, 0)
        static KFileMetaData::ExtractorCollection extractorCollection;
        const auto extractors = extractorCollection.fetchExtractors(mimetype);

        if (!extractors.isEmpty()) {
            const auto &extractor = extractors.at(0);
            AriannaExtractionResult result(entry.filename, mimetype);
            extractor->extract(&result);

            entry.title = result.title;
            entry.author = result.authors;
            entry.publisher = result.publishers.join(u", "_s);
            entry.genres = result.genres;
            entry.language = result.languages.join(u", "_s);
            entry.source = result.sources.join(u", "_s);
            entry.identifier = result.identifiers.join(u", "_s);
            entry.rights = result.rights.join(u", "_s);
            entry.description = result.descriptions;
            entry.created = result.creation;
            const auto cover = result.imageData().value(KFileMetaData::EmbeddedImageData::FrontCover);
            if (!cover.isEmpty()) {
                const QImage image = image.fromData(cover);
                if (!image.isNull()) {
                    entry.thumbnail = entry.saveCover(image);
                }
            }

            entry.series = result.series;
            entry.seriesVolumes = result.volumeNumbers;
        }
#else
        if (mimetype == QStringLiteral("application/epub+zip")) {
            EPubContainer epub(nullptr);
            epub.openFile(entry.filename);
            const auto titles = epub.metadata(QStringLiteral("title"));
            if (!titles.isEmpty()) {
                entry.title = titles[0];
            }
            entry.author = epub.metadata(QStringLiteral("creator"));
            entry.rights = epub.metadata(QStringLiteral("rights")).join(QStringLiteral(", "));
            entry.source = epub.metadata(QStringLiteral("source")).join(QStringLiteral(", "));
            entry.identifier = epub.metadata(QStringLiteral("identifier")).join(QStringLiteral(", "));
            entry.language = epub.metadata(QStringLiteral("language")).join(QStringLiteral(", "));
            entry.genres = epub.metadata(QStringLiteral("subject"));
            entry.publisher = epub.metadata(QStringLiteral("publisher")).join(QStringLiteral(", "));

            auto image = epub.image(epub.metadata(QStringLiteral("cover")).join(QChar()));
            entry.thumbnail = entry.saveCover(image);

            const auto collections = epub.collections();
            for (const auto &collection : collections) {
                entry.series.append(collection.name);
                entry.seriesVolumes.append(QString::number(collection.position));
            }
        }
#endif

        newEntries.append(entry);
    }

    // Batch process the entries
    for (const BookEntry &entry : std::as_const(newEntries)) {
        d->addEntry(this, entry);
        BookDatabase::self().addEntry(entry);
    }

    Q_EMIT countChanged();
}

CategoryEntriesModel *BookListModel::newlyAddedCategoryModel() const
{
    return d->newlyAddedCategoryModel;
}

CategoryEntriesModel *BookListModel::authorCategoryModel() const
{
    return d->authorCategoryModel;
}

CategoryEntriesModel *BookListModel::seriesCategoryModel() const
{
    return d->seriesCategoryModel;
}

CategoryEntriesModel *BookListModel::seriesModelForEntry(const QString &fileName)
{
    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (entry.filename == fileName) {
            return d->seriesCategoryModel->leafModelForEntry(entry);
        }
    }
    return nullptr;
}

CategoryEntriesModel *BookListModel::publisherCategoryModel() const
{
    return d->publisherCategoryModel;
}

CategoryEntriesModel *BookListModel::keywordCategoryModel() const
{
    return d->keywordCategoryModel;
}

CategoryEntriesModel *BookListModel::folderCategoryModel() const
{
    return d->folderCategoryModel;
}

int BookListModel::count() const
{
    return d->entries.count();
}

void BookListModel::setBookData(const QString &fileName, const QString &property, const QString &value)
{
    for (BookEntry &entry : d->entries) {
        if (entry.filename == fileName) {
            if (property == QStringLiteral("currentLocation")) {
                entry.currentLocation = value;
                BookDatabase::self().updateEntry(entry.filename, property, {value});
            } else if (property == QStringLiteral("currentProgress")) {
                entry.currentProgress = value.toInt();
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.toInt()));
            } else if (property == QStringLiteral("locations")) {
                entry.locations = value;
                BookDatabase::self().updateEntry(entry.filename, property, {value});
            } else if (property == QStringLiteral("rating")) {
                entry.rating = value.toInt();
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.toInt()));
            } else if (property == QStringLiteral("tags")) {
                entry.tags = value.split(QLatin1Char(','));
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.split(QLatin1Char(','))));
            } else if (property == QStringLiteral("comment")) {
                entry.comment = value;
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value));
            }
            Q_EMIT entryDataUpdated(entry);
            break;
        }
    }
}

void BookListModel::removeBook(const QString &fileName, bool deleteFile)
{
    if (deleteFile) {
        // KIO::DeleteJob *job = KIO::del(QUrl::fromLocalFile(fileName), KIO::HideProgressInfo);
        // job->start();
    }

    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (entry.filename == fileName) {
            Q_EMIT entryRemoved(entry);
            BookDatabase::self().removeEntry(entry);
            break;
        }
    }
}

QStringList BookListModel::knownBookFiles() const
{
    QStringList files;
    for (const auto &entry : std::as_const(d->entries)) {
        files.append(entry.filename);
    }
    return files;
}

#include "moc_booklistmodel.cpp"
