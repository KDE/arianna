// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "categoryentriesmodel.h"
#include "arianna_debug.h"
#include "epubcontainer.h"

#include <KFileMetaData/UserMetaData>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>
#include <QUuid>

class CategoryEntriesModel::Private
{
public:
    Private(CategoryEntriesModel *qq)
        : q(qq) {};
    ~Private() = default;
    CategoryEntriesModel *q;
    QString name;
    Roles role;
    QList<BookEntry> entries;
    QList<CategoryEntriesModel *> categoryModels;
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
        {CurrentProgressRole, "currentProgress"},
        {CurrentLocationRole, "currentLocation"},
        {CategoryEntriesModelRole, "categoryEntriesModel"},
        {CategoryEntryCountRole, "categoryEntriesCount"},
        {ThumbnailRole, "thumbnail"},
        {DescriptionRole, "description"},
        {CommentRole, "comment"},
        {TagsRole, "tags"},
        {RatingRole, "rating"},
        {LocationsRole, "locations"},
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
        case ThumbnailRole:
            switch (model->role()) {
            case SeriesRole:
                return QStringLiteral("edit-group");
            case PublisherRole:
                return QStringLiteral("view-media-publisher");
            case GenreRole:
                return name() == QStringLiteral("Characters") ? QStringLiteral("actor") : QStringLiteral("tag-symbolic");
            default:
                return QStringLiteral("actor");
            }
        default:
            return QString();
        }
    } else {
        const BookEntry &entry = d->entries[index.row() - d->categoryModels.count()];
        switch (role) {
        case Qt::DisplayRole:
        case FilenameRole:
            return entry.filename;
        case FiletitleRole:
            return entry.filetitle;
        case TitleRole:
            return entry.title;
        case GenreRole:
            return entry.genres;
        case KeywordRole:
            return entry.keywords;
        case CharacterRole:
            return entry.characters;
        case SeriesRole:
            return entry.series;
        case SeriesNumbersRole:
            return entry.seriesNumbers;
        case SeriesVolumesRole:
            return entry.seriesVolumes;
        case AuthorRole:
            return entry.author;
        case PublisherRole:
            return entry.publisher;
        case CreatedRole:
            return entry.created;
        case LastOpenedTimeRole:
            return entry.lastOpenedTime;
        case CurrentProgressRole:
            return entry.currentProgress;
        case CurrentLocationRole:
            return entry.currentLocation;
        case CategoryEntriesModelRole:
            // Nothing, if we're not equipped with one such...
            return QString{};
        case CategoryEntryCountRole:
            return QVariant::fromValue<int>(0);
        case ThumbnailRole: {
            if (entry.thumbnail.isEmpty()) {
                return {};
            }
            if (QFileInfo::exists(entry.thumbnail)) {
                return entry.thumbnail;
            }

            QFile file(entry.thumbnail);
            EPubContainer epub(nullptr);
            epub.openFile(entry.filename);
            auto image = epub.getImage(epub.getMetadata(QStringLiteral("cover")).join(QChar()));
            entry.saveCover(image, entry.thumbnail);
            return entry.thumbnail;
        }
        case DescriptionRole:
            return entry.description;
        case CommentRole:
            return entry.comment;
        case TagsRole:
            return entry.tags;
        case RatingRole:
            return entry.rating;
        case LocationsRole:
            return entry.locations;
        default:
            return QString();
        }
    }
}

int CategoryEntriesModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return d->categoryModels.count() + d->entries.count();
}

int CategoryEntriesModel::count() const
{
    return rowCount();
}

void CategoryEntriesModel::append(const BookEntry &entry, Roles compareRole)
{
    int insertionIndex = 0;
    if (compareRole == UnknownRole) {
        // If we don't know what order to sort by, literally just append the entry
        insertionIndex = d->entries.count();
    } else {
        int seriesOne = -1;
        int seriesTwo = -1;
        if (compareRole == SeriesRole) {
            seriesOne = entry.series.indexOf(name());
            if (entry.series.contains(name(), Qt::CaseInsensitive) && seriesOne == -1) {
                for (int s = 0; s < entry.series.size(); s++) {
                    if (QString::compare(name(), entry.series.at(s), Qt::CaseInsensitive)) {
                        seriesOne = s;
                    }
                }
            }
        }
        for (; insertionIndex < d->entries.count(); ++insertionIndex) {
            if (compareRole == SeriesRole) {
                seriesTwo = d->entries.at(insertionIndex).series.indexOf(name());
                if (d->entries.at(insertionIndex).series.contains(name(), Qt::CaseInsensitive) && seriesTwo == -1) {
                    for (int s = 0; s < d->entries.at(insertionIndex).series.size(); s++) {
                        if (QString::compare(name(), d->entries.at(insertionIndex).series.at(s), Qt::CaseInsensitive)) {
                            seriesTwo = s;
                        }
                    }
                }
            }
            if (compareRole == CreatedRole) {
                if (entry.created <= d->entries.at(insertionIndex).created) {
                    continue;
                }
                break;
            } else if ((seriesOne > -1 && seriesTwo > -1) && entry.seriesNumbers.count() > -1 && entry.seriesNumbers.count() > seriesOne
                       && d->entries.at(insertionIndex).seriesNumbers.count() > -1 && d->entries.at(insertionIndex).seriesNumbers.count() > seriesTwo
                       && entry.seriesNumbers.at(seriesOne).toInt() > 0 && d->entries.at(insertionIndex).seriesNumbers.at(seriesTwo).toInt() > 0) {
                if (entry.seriesVolumes.count() > -1 && entry.seriesVolumes.count() > seriesOne && d->entries.at(insertionIndex).seriesVolumes.count() > -1
                    && d->entries.at(insertionIndex).seriesVolumes.count() > seriesTwo
                    && entry.seriesVolumes.at(seriesOne).toInt() >= d->entries.at(insertionIndex).seriesVolumes.at(seriesTwo).toInt()
                    && entry.seriesNumbers.at(seriesOne).toInt() > d->entries.at(insertionIndex).seriesNumbers.at(seriesTwo).toInt()) {
                    continue;
                }
                break;
            } else {
                if (QString::localeAwareCompare(d->entries.at(insertionIndex).title, entry.title) > 0) {
                    break;
                }
            }
        }
    }
    beginInsertRows({}, insertionIndex, insertionIndex);
    d->entries.insert(insertionIndex, entry);
    Q_EMIT countChanged();
    endInsertRows();
}

void CategoryEntriesModel::clear()
{
    beginResetModel();
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

CategoryEntriesModel *CategoryEntriesModel::leafModelForEntry(const BookEntry &entry)
{
    CategoryEntriesModel *model = nullptr;
    if (d->categoryModels.count() == 0) {
        if (d->entries.contains(entry)) {
            model = this;
        }
    } else {
        for (CategoryEntriesModel *testModel : std::as_const(d->categoryModels)) {
            model = testModel->leafModelForEntry(entry);
            if (model) {
                break;
            }
        }
    }
    return model;
}

void CategoryEntriesModel::addCategoryEntry(const QString &categoryName, const BookEntry &entry, Roles compareRole)
{
    if (categoryName.length() > 0) {
        static const QString splitString = QStringLiteral("/");
        int splitPos = categoryName.indexOf(splitString);
        QString desiredCategory{categoryName};
        if (splitPos > -1) {
            desiredCategory = categoryName.left(splitPos);
        }
        CategoryEntriesModel *categoryModel = nullptr;
        for (CategoryEntriesModel *existingModel : std::as_const(d->categoryModels)) {
            if (QString::compare(existingModel->name(), desiredCategory, Qt::CaseInsensitive) == 0) {
                categoryModel = existingModel;
                break;
            }
        }
        if (!categoryModel) {
            categoryModel = new CategoryEntriesModel(this);
            categoryModel->setRole(compareRole);
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
        if (splitPos > -1) {
            categoryModel->addCategoryEntry(categoryName.mid(splitPos + 1), entry, compareRole);
        } else if (categoryModel->indexOfFile(entry.filename) == -1) {
            categoryModel->append(entry, compareRole);
        }
    }
}

std::optional<BookEntry> CategoryEntriesModel::getBookEntry(int index)
{
    if (index > -1 && index < d->entries.count()) {
        return d->entries.at(index);
    }
    return std::nullopt;
}

int CategoryEntriesModel::indexOfFile(const QString &filename)
{
    int index = -1, i = 0;
    if (QFile::exists(filename)) {
        for (const BookEntry &entry : std::as_const(d->entries)) {
            if (entry.filename == filename) {
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

std::optional<BookEntry> CategoryEntriesModel::bookFromFile(const QString &filename)
{
    const auto entry = getBookEntry(indexOfFile(filename));
    if (!entry) {
        return std::nullopt;
    }
    auto book = entry.value();
    if (book.filename.isEmpty()) {
        if (QFileInfo::exists(filename)) {
            QFileInfo info(filename);
            book.title = info.completeBaseName();
            book.created = info.birthTime();

            KFileMetaData::UserMetaData data(filename);
            if (data.hasAttribute(QStringLiteral("arianna.currentLocation"))) {
                book.currentLocation = data.attribute(QStringLiteral("arianna.currentLocation"));
            }
            book.rating = data.rating();
            if (!data.tags().isEmpty()) {
                book.tags = data.tags();
            }
            if (!data.userComment().isEmpty()) {
                book.comment = data.userComment();
            }
            book.filename = filename;
        }
    }
    return book;
}

void CategoryEntriesModel::entryDataChanged(const BookEntry &entry)
{
    int itemOffset = d->entries.indexOf(entry);
    if (itemOffset >= 0) {
        d->entries[itemOffset] = entry;
        int entryIndex = itemOffset + d->categoryModels.count();
        QModelIndex changed = index(entryIndex);
        Q_EMIT dataChanged(changed, changed);
    }
}

void CategoryEntriesModel::entryRemove(const BookEntry &entry)
{
    int listIndex = d->entries.indexOf(entry);
    if (listIndex > -1) {
        int entryIndex = listIndex + d->categoryModels.count();
        beginRemoveRows(QModelIndex(), entryIndex, entryIndex);
        d->entries.removeAll(entry);
        endRemoveRows();
    }
}

CategoryEntriesModel::Roles CategoryEntriesModel::role() const
{
    return d->role;
}

void CategoryEntriesModel::setRole(Roles role)
{
    d->role = role;
}

bool operator==(const BookEntry &b1, const BookEntry &b2) noexcept
{
    return b1.filename == b2.filename;
}

QString BookEntry::saveCover(const QImage &image, const QString &path) const
{
    if (image.isNull()) {
        qCDebug(ARIANNA_LOG) << "cover is empty";
        return {};
    }

    QString fileName;
    const auto cacheLocation = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);

    if (path.isEmpty()) {
        QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        fileName = cacheLocation + QLatin1String("/covers/") + id + QLatin1String(".jpg");
    } else {
        fileName = path;
    }

    QDir dir(cacheLocation);
    if (!dir.exists(QLatin1String("covers"))) {
        dir.mkdir(QLatin1String("covers"));
    }
    if (!image.save(fileName)) {
        qCWarning(ARIANNA_LOG) << "Error saving image" << fileName;
        return {};
    } else {
        qCDebug(ARIANNA_LOG) << "saving cover to" << fileName;
    }
    return fileName;
}

#include "moc_categoryentriesmodel.cpp"
