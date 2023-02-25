// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "booklistmodel.h"

#include "bookdatabase.h"

#include <KFileMetaData/UserMetaData>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "epubcontainer.h"

#include <arianna_debug.h>
#include <qchar.h>
#include <qloggingcategory.h>

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
        , cacheLoaded(false){};
    ~Private()
    {
        qDeleteAll(entries);
    }
    QList<BookEntry *> entries;

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
            Q_EMIT q->folderCategoryModel();
        }
    }

    void addEntry(BookListModel *q, BookEntry *entry)
    {
        entries.append(entry);
        q->append(entry);
        for (int i = 0; i < entry->author.size(); i++) {
            authorCategoryModel->addCategoryEntry(entry->author.at(i), entry);
        }
        for (int i = 0; i < entry->series.size(); i++) {
            seriesCategoryModel->addCategoryEntry(entry->series.at(i), entry, SeriesRole);
        }
        if (newlyAddedCategoryModel->indexOfFile(entry->filename) == -1) {
            newlyAddedCategoryModel->append(entry, CreatedRole);
        }
        publisherCategoryModel->addCategoryEntry(entry->publisher, entry);
        QUrl url(entry->filename.left(entry->filename.lastIndexOf(QLatin1Char('/'))));
        folderCategoryModel->addCategoryEntry(url.path().mid(1), entry);
        if (folderCategoryModel->indexOfFile(entry->filename) == -1) {
            folderCategoryModel->append(entry);
        }
        for (int i = 0; i < entry->genres.size(); i++) {
            keywordCategoryModel->addCategoryEntry(entry->genres.at(i), entry, GenreRole);
        }
    }

    void loadCache(BookListModel *q)
    {
        QList<BookEntry *> entries = BookDatabase::self().loadEntries();
        if (entries.count() > 0) {
            initializeSubModels(q);
        }
        int i = 0;
        for (BookEntry *entry : entries) {
            /*
             * This might turn out a little slow, but we should avoid having entries
             * that do not exist. If we end up with slowdown issues when loading the
             * cache this would be a good place to start investigating.
             */
            if (QFileInfo::exists(entry->filename)) {
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

QString saveCover(const QString &identifier, const QImage &image)
{
    if (!image.isNull()) {
        const auto cacheLocation = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QString id = identifier;
        id.replace(QLatin1Char('/'), QLatin1Char('_'));
        QString fileName = cacheLocation + QLatin1String("/covers/") + id + QLatin1String(".jpg");
        fileName.replace(QLatin1Char(':'), QLatin1Char('_'));
        QDir dir(cacheLocation);
        if (!dir.exists(QLatin1String("covers"))) {
            dir.mkdir(QLatin1String("covers"));
        }
        if (!image.save(fileName)) {
            qCWarning(ARIANNA_LOG) << "Error saving image" << fileName;
        } else {
            qCDebug(ARIANNA_LOG) << "saving cover to" << fileName;
        }
        return fileName;
    } else {
        qCDebug(ARIANNA_LOG) << "cover is empty";
        // TODO generate generic cover
        return {};
    }
}

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
    int role = ContentList::FilePathRole;
    for (int i = first; i < last + 1; ++i) {
        QVariant filePath = d->contentModel->data(d->contentModel->index(first, 0, index), role);
        auto entry = new BookEntry();
        entry->filename = filePath.toUrl().toLocalFile();
        QStringList splitName = entry->filename.split(QLatin1Char('/'));
        if (!splitName.isEmpty())
            entry->filetitle = splitName.takeLast();
        if (!splitName.isEmpty()) {
            entry->series = QStringList();
            entry->seriesNumbers = QStringList() << QStringLiteral("0");
            entry->seriesVolumes = QStringList() << QStringLiteral("0");
        }
        // just in case we end up without a title... using complete basename here,
        // as we would rather have "book one. part two" and the odd "book one - part two.tar"
        QFileInfo fileinfo(entry->filename);
        entry->title = fileinfo.completeBaseName();

        if (entry->filename.toLower().endsWith(QStringLiteral("cbr")) || entry->filename.toLower().endsWith(QStringLiteral("cbz"))) {
            entry->thumbnail = QStringLiteral("image://comiccover/").append(entry->filename);
        }
#ifdef USE_PERUSE_PDFTHUMBNAILER
        else if (entry->filename.toLower().endsWith(QStringLiteral("pdf"))) {
            entry->thumbnail = QStringLiteral("image://pdfcover/").append(entry->filename);
        }
#endif
        else {
            entry->thumbnail = QStringLiteral("image://preview/").append(entry->filename);
        }

        KFileMetaData::UserMetaData data(entry->filename);
        entry->rating = data.rating();
        entry->comment = data.userComment();
        entry->tags = data.tags();

        QVariantHash metadata = d->contentModel->data(d->contentModel->index(first, 0, index), Qt::UserRole + 2).toHash();
        QVariantHash::const_iterator it = metadata.constBegin();
        for (; it != metadata.constEnd(); it++) {
            if (it.key() == QLatin1String("author")) {
                entry->author = it.value().toStringList();
            } else if (it.key() == QLatin1String("title")) {
                entry->title = it.value().toString().trimmed();
            } else if (it.key() == QLatin1String("publisher")) {
                entry->publisher = it.value().toString().trimmed();
            } else if (it.key() == QLatin1String("created")) {
                entry->created = it.value().toDateTime();
            } else if (it.key() == QLatin1String("currentLocation")) {
                entry->currentLocation = it.value().toString();
            } else if (it.key() == QLatin1String("currentProgress")) {
                entry->currentProgress = it.value().toInt();
            } else if (it.key() == QLatin1String("comments")) {
                entry->comment = it.value().toString();
            } else if (it.key() == QLatin1String("tags")) {
                entry->tags = it.value().toStringList();
            } else if (it.key() == QLatin1String("rating")) {
                entry->rating = it.value().toInt();
            }
        }
        QMimeDatabase db;
        QString mimetype = db.mimeTypeForFile(entry->filename).name();
        if (mimetype == QStringLiteral("application/epub+zip")) {
            EPubContainer epub(nullptr);
            epub.openFile(entry->filename);
            entry->title = epub.getMetadata(QStringLiteral("title"))[0];
            entry->author = epub.getMetadata(QStringLiteral("creator"));
            entry->rights = epub.getMetadata(QStringLiteral("rights")).join(QStringLiteral(", "));
            entry->source = epub.getMetadata(QStringLiteral("source")).join(QStringLiteral(", "));
            entry->identifier = epub.getMetadata(QStringLiteral("identifier")).join(QStringLiteral(", "));
            entry->language = epub.getMetadata(QStringLiteral("language")).join(QStringLiteral(", "));
            entry->genres = epub.getMetadata(QStringLiteral("subject"));
            entry->publisher = epub.getMetadata(QStringLiteral("publisher")).join(QStringLiteral(", "));

            auto image = epub.getImage(epub.getMetadata(QStringLiteral("cover")).join(QChar()));
            entry->thumbnail = saveCover(epub.getMetadata(QStringLiteral("identifier")).join(QChar()), image);

            const auto collections = epub.collections();
            for (const auto &collection : collections) {
                entry->series.append(collection.name);
                entry->seriesVolumes.append(QString::number(collection.position));
            }
        }

        d->addEntry(this, entry);
        BookDatabase::self().addEntry(entry);
    }
    Q_EMIT countChanged();
    qApp->processEvents();
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
    for (BookEntry *entry : d->entries) {
        if (entry->filename == fileName) {
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
    for (BookEntry *entry : d->entries) {
        if (entry->filename == fileName) {
            if (property == QStringLiteral("currentLocation")) {
                entry->currentLocation = value;
                BookDatabase::self().updateEntry(entry->filename, property, {value});
            } else if (property == QStringLiteral("currentProgress")) {
                entry->currentProgress = value.toInt();
                BookDatabase::self().updateEntry(entry->filename, property, QVariant(value.toInt()));
            } else if (property == QStringLiteral("locations")) {
                entry->locations = value;
                BookDatabase::self().updateEntry(entry->filename, property, {value});
            } else if (property == QStringLiteral("rating")) {
                entry->rating = value.toInt();
                BookDatabase::self().updateEntry(entry->filename, property, QVariant(value.toInt()));
            } else if (property == QStringLiteral("tags")) {
                entry->tags = value.split(QLatin1Char(','));
                BookDatabase::self().updateEntry(entry->filename, property, QVariant(value.split(QLatin1Char(','))));
            } else if (property == QStringLiteral("comment")) {
                entry->comment = value;
                BookDatabase::self().updateEntry(entry->filename, property, QVariant(value));
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

    for (BookEntry *entry : d->entries) {
        if (entry->filename == fileName) {
            Q_EMIT entryRemoved(entry);
            BookDatabase::self().removeEntry(entry);
            delete entry;
            break;
        }
    }
}

QStringList BookListModel::knownBookFiles() const
{
    QStringList files;
    for (BookEntry *entry : d->entries) {
        files.append(entry->filename);
    }
    return files;
}
