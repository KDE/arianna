// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "categoryentriesmodel.h"
#include "propertycontainer.h"
#include <KFileMetaData/UserMetaData>
#include <QDir>
#include <QFileInfo>

class CategoryEntriesModel::Private
{
public:
    Private(CategoryEntriesModel *qq)
        : q(qq){};
    ~Private()
    {
        // No deleting the entries - this is done by the master BookListModel already, so do that at your own risk
        // If we have manually unwrapped any books, though, those need removing
        qDeleteAll(unwrappedBooks);
    }
    CategoryEntriesModel *q;
    QString name;
    QList<BookEntry *> entries;
    QList<CategoryEntriesModel *> categoryModels;

    QObject *wrapBookEntry(const BookEntry *entry)
    {
        PropertyContainer *obj = new PropertyContainer(QStringLiteral("book"), q);
        obj->setProperty("author", entry->author);
        obj->setProperty("currentPage", QString::number(entry->currentPage));
        obj->setProperty("filename", entry->filename);
        obj->setProperty("filetitle", entry->filetitle);
        obj->setProperty("genres", entry->genres);
        obj->setProperty("keywords", entry->keywords);
        obj->setProperty("characters", entry->characters);
        obj->setProperty("created", entry->created);
        obj->setProperty("lastOpenedTime", entry->lastOpenedTime);
        obj->setProperty("publisher", entry->publisher);
        obj->setProperty("series", entry->series);
        obj->setProperty("title", entry->title);
        obj->setProperty("totalPages", entry->totalPages);
        obj->setProperty("thumbnail", entry->thumbnail);
        obj->setProperty("description", entry->description);
        obj->setProperty("comment", entry->comment);
        obj->setProperty("tags", entry->tags);
        obj->setProperty("rating", QString::number(entry->rating));
        return obj;
    }

    QList<BookEntry *> unwrappedBooks;
    BookEntry *unwrapBookEntry(const QObject *obj)
    {
        BookEntry *entry = new BookEntry;
        entry->author = obj->property("author").toStringList();
        entry->currentPage = obj->property("currentPage").toInt();
        entry->filename = obj->property("filename").toString();
        entry->filetitle = obj->property("filetitle").toString();
        entry->genres = obj->property("genres").toStringList();
        entry->keywords = obj->property("keywords").toStringList();
        entry->characters = obj->property("characters").toStringList();
        entry->created = obj->property("created").toDateTime();
        entry->lastOpenedTime = obj->property("lastOpenedTime").toDateTime();
        entry->publisher = obj->property("publisher").toString();
        entry->series = obj->property("series").toStringList();
        entry->title = obj->property("title").toString();
        entry->totalPages = obj->property("totalPages").toInt();
        entry->thumbnail = obj->property("thumbnail").toString();
        entry->description = obj->property("description").toStringList();
        entry->comment = obj->property("comment").toString();
        entry->tags = obj->property("tags").toStringList();
        entry->rating = obj->property("rating").toInt();
        unwrappedBooks << entry;
        return entry;
    }
};

CategoryEntriesModel::CategoryEntriesModel(QObject *parent)
    : QAbstractListModel(parent)
    , d(std::make_unique<Private>(this))
{
    connect(this, &CategoryEntriesModel::entryDataUpdated, this, &CategoryEntriesModel::entryDataChanged);
    connect(this, &CategoryEntriesModel::entryRemoved, this, &CategoryEntriesModel::entryRemove);
}

CategoryEntriesModel::~CategoryEntriesModel() = default;

QHash<int, QByteArray> CategoryEntriesModel::roleNames() const
{
    return {
        {FilenameRole, "filename"},
        {FiletitleRole, "filetitle"},
        {TitleRole, "title"},
        {GenreRole, "genres"},
        {KeywordRole, "keywords"},
        {SeriesRole, "series"},
        {SeriesNumbersRole, "seriesNumber"},
        {SeriesVolumesRole, "seriesVolume"},
        {AuthorRole, "author"},
        {PublisherRole, "publisher"},
        {CreatedRole, "created"},
        {LastOpenedTimeRole, "lastOpenedTime"},
        {TotalPagesRole, "totalPages"},
        {CurrentPageRole, "currentPage"},
        {CategoryEntriesModelRole, "categoryEntriesModel"},
        {CategoryEntryCountRole, "categoryEntriesCount"},
        {ThumbnailRole, "thumbnail"},
        {DescriptionRole, "description"},
        {CommentRole, "comment"},
        {TagsRole, "tags"},
        {RatingRole, "rating"},
    };
}

QVariant CategoryEntriesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() <= -1) {
        return {};
    }

    if (index.row() < d->categoryModels.count()) {
        CategoryEntriesModel *model = d->categoryModels[index.row()];
        switch (role) {
        case Qt::DisplayRole:
        case TitleRole:
            return model->name();
        case CategoryEntryCountRole:
            return model->bookCount();
        case CategoryEntriesModelRole:
            return QVariant::fromValue<CategoryEntriesModel *>(model);
        default:
            return QStringLiteral("Unknown role");
        }
    } else {
        const BookEntry *entry = d->entries[index.row() - d->categoryModels.count()];
        switch (role) {
        case Qt::DisplayRole:
        case FilenameRole:
            return entry->filename;
        case FiletitleRole:
            return entry->filetitle;
        case TitleRole:
            return entry->title;
        case GenreRole:
            return entry->genres;
        case KeywordRole:
            return entry->keywords;
        case CharacterRole:
            return entry->characters;
        case SeriesRole:
            return entry->series;
        case SeriesNumbersRole:
            return entry->seriesNumbers;
        case SeriesVolumesRole:
            return entry->seriesVolumes;
        case AuthorRole:
            return entry->author;
        case PublisherRole:
            return entry->publisher;
        case CreatedRole:
            return entry->created;
        case LastOpenedTimeRole:
            return entry->lastOpenedTime;
        case TotalPagesRole:
            return entry->totalPages;
        case CurrentPageRole:
            return entry->currentPage;
        case CategoryEntriesModelRole:
            // Nothing, if we're not equipped with one such...
            return {};
        case CategoryEntryCountRole:
            return QVariant::fromValue<int>(0);
        case ThumbnailRole:
            return entry->thumbnail;
        case DescriptionRole:
            return entry->description;
        case CommentRole:
            return entry->comment;
        case TagsRole:
            return entry->tags;
        case RatingRole:
            return entry->rating;
        default:
            return QStringLiteral("Unknown role");
        }
    }
}

int CategoryEntriesModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return d->categoryModels.count() + d->entries.count();
}

int CategoryEntriesModel::count() const
{
    return rowCount();
}

void CategoryEntriesModel::append(BookEntry *entry, Roles compareRole)
{
    int insertionIndex = 0;
    if (compareRole == UnknownRole) {
        // If we don't know what order to sort by, literally just append the entry
        insertionIndex = d->entries.count();
    } else {
        int seriesOne = -1;
        int seriesTwo = -1;
        if (compareRole == SeriesRole) {
            seriesOne = entry->series.indexOf(name());
            if (entry->series.contains(name(), Qt::CaseInsensitive) && seriesOne == -1) {
                for (int s = 0; s < entry->series.size(); s++) {
                    if (QString::compare(name(), entry->series.at(s), Qt::CaseInsensitive)) {
                        seriesOne = s;
                    }
                }
            }
        }
        for (; insertionIndex < d->entries.count(); ++insertionIndex) {
            if (compareRole == SeriesRole) {
                seriesTwo = d->entries.at(insertionIndex)->series.indexOf(name());
                if (d->entries.at(insertionIndex)->series.contains(name(), Qt::CaseInsensitive) && seriesTwo == -1) {
                    for (int s = 0; s < d->entries.at(insertionIndex)->series.size(); s++) {
                        if (QString::compare(name(), d->entries.at(insertionIndex)->series.at(s), Qt::CaseInsensitive)) {
                            seriesTwo = s;
                        }
                    }
                }
            }
            if (compareRole == CreatedRole) {
                if (entry->created <= d->entries.at(insertionIndex)->created) {
                    continue;
                }
                break;
            } else if ((seriesOne > -1 && seriesTwo > -1) && entry->seriesNumbers.count() > -1 && entry->seriesNumbers.count() > seriesOne
                       && d->entries.at(insertionIndex)->seriesNumbers.count() > -1 && d->entries.at(insertionIndex)->seriesNumbers.count() > seriesTwo
                       && entry->seriesNumbers.at(seriesOne).toInt() > 0 && d->entries.at(insertionIndex)->seriesNumbers.at(seriesTwo).toInt() > 0) {
                if (entry->seriesVolumes.count() > -1 && entry->seriesVolumes.count() > seriesOne && d->entries.at(insertionIndex)->seriesVolumes.count() > -1
                    && d->entries.at(insertionIndex)->seriesVolumes.count() > seriesTwo
                    && entry->seriesVolumes.at(seriesOne).toInt() >= d->entries.at(insertionIndex)->seriesVolumes.at(seriesTwo).toInt()
                    && entry->seriesNumbers.at(seriesOne).toInt() > d->entries.at(insertionIndex)->seriesNumbers.at(seriesTwo).toInt()) {
                    continue;
                }
                break;
            } else {
                if (QString::localeAwareCompare(d->entries.at(insertionIndex)->title, entry->title) > 0) {
                    break;
                }
            }
        }
    }
    beginInsertRows(QModelIndex(), insertionIndex, insertionIndex);
    d->entries.insert(insertionIndex, entry);
    Q_EMIT countChanged();
    endInsertRows();
}

void CategoryEntriesModel::appendFakeBook(QObject *book, CategoryEntriesModel::Roles compareRole)
{
    append(d->unwrapBookEntry(book), compareRole);
}

void CategoryEntriesModel::clear()
{
    beginResetModel();
    qDeleteAll(d->unwrappedBooks);
    d->unwrappedBooks.clear();
    d->entries.clear();
    endResetModel();
}

const QString &CategoryEntriesModel::name() const
{
    return d->name;
}

void CategoryEntriesModel::setName(const QString &newName)
{
    d->name = newName;
}

CategoryEntriesModel *CategoryEntriesModel::leafModelForEntry(BookEntry *entry)
{
    CategoryEntriesModel *model = nullptr;
    if (d->categoryModels.count() == 0) {
        if (d->entries.contains(entry)) {
            model = this;
        }
    } else {
        for (CategoryEntriesModel *testModel : d->categoryModels) {
            model = testModel->leafModelForEntry(entry);
            if (model) {
                break;
            }
        }
    }
    return model;
}

void CategoryEntriesModel::addCategoryEntry(const QString &categoryName, BookEntry *entry, Roles compareRole)
{
    if (categoryName.length() > 0) {
        static const QString splitString = QStringLiteral("/");
        int splitPos = categoryName.indexOf(splitString);
        QString desiredCategory{categoryName};
        if (splitPos > -1) {
            desiredCategory = categoryName.left(splitPos);
        }
        CategoryEntriesModel *categoryModel = nullptr;
        for (CategoryEntriesModel *existingModel : qAsConst(d->categoryModels)) {
            if (QString::compare(existingModel->name(), desiredCategory, Qt::CaseInsensitive) == 0) {
                categoryModel = existingModel;
                break;
            }
        }
        if (!categoryModel) {
            categoryModel = new CategoryEntriesModel(this);
            connect(this, &CategoryEntriesModel::entryDataUpdated, categoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(this, &CategoryEntriesModel::entryRemoved, categoryModel, &CategoryEntriesModel::entryRemoved);
            categoryModel->setName(desiredCategory);

            int insertionIndex = 0;
            for (; insertionIndex < d->categoryModels.count(); ++insertionIndex) {
                if (QString::localeAwareCompare(d->categoryModels.at(insertionIndex)->name(), categoryModel->name()) > 0) {
                    break;
                }
            }
            beginInsertRows(QModelIndex(), insertionIndex, insertionIndex);
            d->categoryModels.insert(insertionIndex, categoryModel);
            endInsertRows();
        }
        if (categoryModel->indexOfFile(entry->filename) == -1) {
            categoryModel->append(entry, compareRole);
        }
        if (splitPos > -1)
            categoryModel->addCategoryEntry(categoryName.mid(splitPos + 1), entry);
    }
}

QObject *CategoryEntriesModel::get(int index)
{
    BookEntry *entry = new BookEntry();
    bool deleteEntry = true;
    if (index > -1 && index < d->entries.count()) {
        entry = d->entries.at(index);
        deleteEntry = false;
    }
    QObject *obj = d->wrapBookEntry(entry);
    if (deleteEntry) {
        delete entry;
    }
    return obj;
}

BookEntry *CategoryEntriesModel::getBookEntry(int index)
{
    BookEntry *entry{nullptr};
    if (index > -1 && index < d->entries.count()) {
        entry = d->entries.at(index);
    }
    return entry;
}

int CategoryEntriesModel::indexOfFile(const QString &filename)
{
    int index = -1, i = 0;
    if (QFile::exists(filename)) {
        for (BookEntry *entry : d->entries) {
            if (entry->filename == filename) {
                index = i;
                break;
            }
            ++i;
        }
    }
    return index;
}

bool CategoryEntriesModel::indexIsBook(int index)
{
    if (index < d->categoryModels.count() || index >= rowCount()) {
        return false;
    }
    return true;
}

int CategoryEntriesModel::bookCount() const
{
    return d->entries.count();
}

QObject *CategoryEntriesModel::getEntry(int index)
{
    PropertyContainer *obj = new PropertyContainer(QStringLiteral("book"), this);
    if (index > d->categoryModels.count() - 1 && index < rowCount()) {
        // This is a book - get a book!
        obj = qobject_cast<PropertyContainer *>(get(index - d->categoryModels.count()));
    } else if (index >= 0 && index < d->categoryModels.count()) {
        CategoryEntriesModel *catEntry = d->categoryModels.at(index);
        obj->setProperty("title", catEntry->name());
        obj->setProperty("categoryEntriesCount", catEntry->bookCount());
        obj->setProperty("entriesModel", QVariant::fromValue(catEntry));
    }
    return obj;
}

QObject *CategoryEntriesModel::bookFromFile(const QString &filename)
{
    PropertyContainer *obj = qobject_cast<PropertyContainer *>(get(indexOfFile(filename)));
    if (obj->property("filename").toString().isEmpty()) {
        if (QFileInfo::exists(filename)) {
            QFileInfo info(filename);
            obj->setProperty("title", info.completeBaseName());
            obj->setProperty("created", info.birthTime());

            KFileMetaData::UserMetaData data(filename);
            if (data.hasAttribute(QStringLiteral("peruse.currentPage"))) {
                int currentPage = data.attribute(QStringLiteral("peruse.currentPage")).toInt();
                obj->setProperty("currentPage", QVariant::fromValue<int>(currentPage));
            }
            if (data.hasAttribute(QStringLiteral("peruse.totalPages"))) {
                int totalPages = data.attribute(QStringLiteral("peruse.totalPages")).toInt();
                obj->setProperty("totalPages", QVariant::fromValue<int>(totalPages));
            }
            obj->setProperty("rating", QVariant::fromValue<int>(data.rating()));
            if (!data.tags().isEmpty()) {
                obj->setProperty("tags", QVariant::fromValue<QStringList>(data.tags()));
            }
            if (!data.userComment().isEmpty()) {
                obj->setProperty("comment", QVariant::fromValue<QString>(data.userComment()));
            }
            obj->setProperty("filename", filename);

            QString thumbnail;
            if (filename.toLower().endsWith(QStringLiteral("cbr")) || filename.toLower().endsWith(QStringLiteral("cbz"))) {
                thumbnail = QStringLiteral("image://comiccover/").append(filename);
            }
#ifdef USE_PERUSE_PDFTHUMBNAILER
            else if (filename.toLower().endsWith(QStringLiteral("pdf"))) {
                thumbnail = QStringLiteral("image://pdfcover/").append(filename);
            }
#endif
            else {
                thumbnail = QStringLiteral("image://preview/").append(filename);
            }
            obj->setProperty("thumbnail", thumbnail);
        }
    }
    return obj;
}

void CategoryEntriesModel::entryDataChanged(BookEntry *entry)
{
    int entryIndex = d->entries.indexOf(entry) + d->categoryModels.count();
    QModelIndex changed = index(entryIndex);
    dataChanged(changed, changed);
}

void CategoryEntriesModel::entryRemove(BookEntry *entry)
{
    int listIndex = d->entries.indexOf(entry);
    if (listIndex > -1) {
        int entryIndex = listIndex + d->categoryModels.count();
        beginRemoveRows(QModelIndex(), entryIndex, entryIndex);
        d->entries.removeAll(entry);
        endRemoveRows();
    }
}
