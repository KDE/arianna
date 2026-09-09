// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "categoryentriesmodel.h"
#include "arianna_debug.h"
#include "epubcontainer.h"

#include <KFileMetaData/UserMetaData>
#include <KLocalizedString>

#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <functional>

class CategoryEntriesModel::Private
{
public:
    Private(CategoryEntriesModel *qq)
        : q(qq) { };
    ~Private() = default;

    struct BookGroupCache {
        QString filterText;
        QVariantList groups;
        QDateTime createdAt;
        bool valid = false;

        bool matches(const QString &filter, int maximumAgeMsec = -1) const
        {
            if (!valid || filterText != filter) {
                return false;
            }

            return maximumAgeMsec < 0 || createdAt.msecsTo(QDateTime::currentDateTime()) <= maximumAgeMsec;
        }

        void store(const QString &filter, const QVariantList &newGroups)
        {
            filterText = filter;
            groups = newGroups;
            createdAt = QDateTime::currentDateTime();
            valid = true;
        }

        void clear()
        {
            groups.clear();
            filterText.clear();
            createdAt = {};
            valid = false;
        }
    };

    CategoryEntriesModel *q;
    QString name;
    Roles role = UnknownRole;
    QList<BookEntry> entries;
    QList<CategoryEntriesModel *> categoryModels;
    BookGroupCache mainSubjectGroupCache;
    BookGroupCache titleGroupCache;
    BookGroupCache authorGroupCache;
    BookGroupCache lastOpenedGroupCache;
    BookGroupCache typeGroupCache;

    void invalidateBookGroupCaches()
    {
        mainSubjectGroupCache.clear();
        titleGroupCache.clear();
        authorGroupCache.clear();
        lastOpenedGroupCache.clear();
        typeGroupCache.clear();
    }
};

static QString normalizedSubjectKey(QString subject)
{
    subject = subject.trimmed().toCaseFolded();
    subject.replace(QStringLiteral("\u00e4"), QStringLiteral("ae"));
    subject.replace(QStringLiteral("\u00f6"), QStringLiteral("oe"));
    subject.replace(QStringLiteral("\u00fc"), QStringLiteral("ue"));
    subject.replace(QStringLiteral("\u00df"), QStringLiteral("ss"));
    subject.replace(QLatin1Char('_'), QLatin1Char(' '));
    subject.replace(QLatin1Char('-'), QLatin1Char(' '));
    return subject.simplified();
}

static QString localizedSubjectTitle(const QString &subject)
{
    const QString key = normalizedSubjectKey(subject);

    if (key == QStringLiteral("adventure") || key == QStringLiteral("abenteuer")) {
        return i18nc("@item:inlistbox EPUB subject", "Adventure");
    } else if (key == QStringLiteral("art") || key == QStringLiteral("kunst")) {
        return i18nc("@item:inlistbox EPUB subject", "Art");
    } else if (key == QStringLiteral("autobiography") || key == QStringLiteral("autobiografie") || key == QStringLiteral("autobiographie")) {
        return i18nc("@item:inlistbox EPUB subject", "Autobiography");
    } else if (key == QStringLiteral("bible") || key == QStringLiteral("bibel")) {
        return i18nc("@item:inlistbox EPUB subject", "Bible");
    } else if (key == QStringLiteral("biography") || key == QStringLiteral("biografie") || key == QStringLiteral("biographie")) {
        return i18nc("@item:inlistbox EPUB subject", "Biography");
    } else if (key == QStringLiteral("business") || key == QStringLiteral("wirtschaft")) {
        return i18nc("@item:inlistbox EPUB subject", "Business");
    } else if (key == QStringLiteral("children") || key == QStringLiteral("children's") || key == QStringLiteral("kinder")) {
        return i18nc("@item:inlistbox EPUB subject", "Children");
    } else if (key == QStringLiteral("christianity") || key == QStringLiteral("christentum")) {
        return i18nc("@item:inlistbox EPUB subject", "Christianity");
    } else if (key == QStringLiteral("classics") || key == QStringLiteral("klassiker")) {
        return i18nc("@item:inlistbox EPUB subject", "Classics");
    } else if (key == QStringLiteral("computer science") || key == QStringLiteral("informatik")) {
        return i18nc("@item:inlistbox EPUB subject", "Computer Science");
    } else if (key == QStringLiteral("crime") || key == QStringLiteral("krimi") || key == QStringLiteral("verbrechen")) {
        return i18nc("@item:inlistbox EPUB subject", "Crime");
    } else if (key == QStringLiteral("drama")) {
        return i18nc("@item:inlistbox EPUB subject", "Drama");
    } else if (key == QStringLiteral("economics") || key == QStringLiteral("oekonomie")) {
        return i18nc("@item:inlistbox EPUB subject", "Economics");
    } else if (key == QStringLiteral("education") || key == QStringLiteral("bildung") || key == QStringLiteral("paedagogik")) {
        return i18nc("@item:inlistbox EPUB subject", "Education");
    } else if (key == QStringLiteral("fantasy") || key == QStringLiteral("phantastik")) {
        return i18nc("@item:inlistbox EPUB subject", "Fantasy");
    } else if (key == QStringLiteral("fiction") || key == QStringLiteral("belletristik")) {
        return i18nc("@item:inlistbox EPUB subject", "Fiction");
    } else if (key == QStringLiteral("health") || key == QStringLiteral("gesundheit")) {
        return i18nc("@item:inlistbox EPUB subject", "Health");
    } else if (key == QStringLiteral("history") || key == QStringLiteral("geschichte")) {
        return i18nc("@item:inlistbox EPUB subject", "History");
    } else if (key == QStringLiteral("horror")) {
        return i18nc("@item:inlistbox EPUB subject", "Horror");
    } else if (key == QStringLiteral("islam")) {
        return i18nc("@item:inlistbox EPUB subject", "Islam");
    } else if (key == QStringLiteral("judaism") || key == QStringLiteral("judentum")) {
        return i18nc("@item:inlistbox EPUB subject", "Judaism");
    } else if (key == QStringLiteral("law") || key == QStringLiteral("recht")) {
        return i18nc("@item:inlistbox EPUB subject", "Law");
    } else if (key == QStringLiteral("literature") || key == QStringLiteral("literatur")) {
        return i18nc("@item:inlistbox EPUB subject", "Literature");
    } else if (key == QStringLiteral("mathematics") || key == QStringLiteral("math") || key == QStringLiteral("mathematik")) {
        return i18nc("@item:inlistbox EPUB subject", "Mathematics");
    } else if (key == QStringLiteral("medicine") || key == QStringLiteral("medizin")) {
        return i18nc("@item:inlistbox EPUB subject", "Medicine");
    } else if (key == QStringLiteral("memoir") || key == QStringLiteral("memoiren")) {
        return i18nc("@item:inlistbox EPUB subject", "Memoir");
    } else if (key == QStringLiteral("music") || key == QStringLiteral("musik")) {
        return i18nc("@item:inlistbox EPUB subject", "Music");
    } else if (key == QStringLiteral("mystery") || key == QStringLiteral("raetsel")) {
        return i18nc("@item:inlistbox EPUB subject", "Mystery");
    } else if (key == QStringLiteral("nonfiction") || key == QStringLiteral("non fiction") || key == QStringLiteral("sachbuch")) {
        return i18nc("@item:inlistbox EPUB subject", "Nonfiction");
    } else if (key == QStringLiteral("philosophy") || key == QStringLiteral("philosophie")) {
        return i18nc("@item:inlistbox EPUB subject", "Philosophy");
    } else if (key == QStringLiteral("poetry") || key == QStringLiteral("lyrik") || key == QStringLiteral("poesie")) {
        return i18nc("@item:inlistbox EPUB subject", "Poetry");
    } else if (key == QStringLiteral("politics") || key == QStringLiteral("politik")) {
        return i18nc("@item:inlistbox EPUB subject", "Politics");
    } else if (key == QStringLiteral("programming") || key == QStringLiteral("programmierung")) {
        return i18nc("@item:inlistbox EPUB subject", "Programming");
    } else if (key == QStringLiteral("psychology") || key == QStringLiteral("psychologie")) {
        return i18nc("@item:inlistbox EPUB subject", "Psychology");
    } else if (key == QStringLiteral("reference") || key == QStringLiteral("nachschlagewerk")) {
        return i18nc("@item:inlistbox EPUB subject", "Reference");
    } else if (key == QStringLiteral("religion")) {
        return i18nc("@item:inlistbox EPUB subject", "Religion");
    } else if (key == QStringLiteral("romance") || key == QStringLiteral("liebesroman")) {
        return i18nc("@item:inlistbox EPUB subject", "Romance");
    } else if (key == QStringLiteral("science") || key == QStringLiteral("wissenschaft")) {
        return i18nc("@item:inlistbox EPUB subject", "Science");
    } else if (key == QStringLiteral("science fiction") || key == QStringLiteral("sci fi") || key == QStringLiteral("scifi")) {
        return i18nc("@item:inlistbox EPUB subject", "Science Fiction");
    } else if (key == QStringLiteral("sociology") || key == QStringLiteral("soziologie")) {
        return i18nc("@item:inlistbox EPUB subject", "Sociology");
    } else if (key == QStringLiteral("technology") || key == QStringLiteral("technik") || key == QStringLiteral("technologie")) {
        return i18nc("@item:inlistbox EPUB subject", "Technology");
    } else if (key == QStringLiteral("theology") || key == QStringLiteral("theologie")) {
        return i18nc("@item:inlistbox EPUB subject", "Theology");
    } else if (key == QStringLiteral("travel") || key == QStringLiteral("reisen")) {
        return i18nc("@item:inlistbox EPUB subject", "Travel");
    } else if (key == QStringLiteral("young adult") || key == QStringLiteral("jugendbuch")) {
        return i18nc("@item:inlistbox EPUB subject", "Young Adult");
    }

    return subject;
}

static QString localizedCategoryTitle(const QString &name, CategoryEntriesModel::Roles role)
{
    switch (role) {
    case CategoryEntriesModel::SubjectRole:
    case CategoryEntriesModel::GenreRole:
        return localizedSubjectTitle(name);
    default:
        return name;
    }
}

static QString localizedSubjectPathTitle(const QString &path, CategoryEntriesModel::Roles role)
{
    QStringList localizedSegments;
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        localizedSegments.append(localizedCategoryTitle(segment, role));
    }
    return localizedSegments.join(QStringLiteral(" / "));
}

static bool isSubjectCategoryRole(CategoryEntriesModel::Roles role)
{
    return role == CategoryEntriesModel::SubjectRole || role == CategoryEntriesModel::GenreRole;
}

static QStringList localizedMainSubjects(const QStringList &subjects)
{
    QStringList mainSubjects;
    QSet<QString> seen;

    for (const QString &subject : subjects) {
        QString mainSubject = subject.section(QLatin1Char('/'), 0, 0).trimmed().simplified();
        if (mainSubject.isEmpty()) {
            continue;
        }

        const QString key = mainSubject.toCaseFolded();
        if (seen.contains(key)) {
            continue;
        }

        seen.insert(key);
        mainSubjects.append(localizedCategoryTitle(mainSubject, CategoryEntriesModel::SubjectRole));
    }

    std::sort(mainSubjects.begin(), mainSubjects.end(), [](const QString &first, const QString &second) {
        return QString::localeAwareCompare(first, second) < 0;
    });

    return mainSubjects;
}

static QString mainSubjectTitle(const QStringList &subjects)
{
    const QStringList mainSubjects = localizedMainSubjects(subjects);
    return mainSubjects.isEmpty() ? i18nc("@item:inlistbox books without a main topic", "No Main Topic") : mainSubjects.join(QStringLiteral(", "));
}

static QVariantMap groupedBookMap(const BookEntry &entry)
{
    QVariantMap book;
    book.insert(QStringLiteral("thumbnail"), entry.thumbnail);
    book.insert(QStringLiteral("passwordProtected"), entry.passwordProtected);
    book.insert(QStringLiteral("title"), entry.title);
    book.insert(QStringLiteral("localizedTitle"), entry.title);
    book.insert(QStringLiteral("filename"), entry.filename);
    book.insert(QStringLiteral("mainSubject"), mainSubjectTitle(entry.genres));
    book.insert(QStringLiteral("author"), entry.author);
    book.insert(QStringLiteral("entry"), QVariant::fromValue(entry));
    book.insert(QStringLiteral("locations"), entry.locations);
    book.insert(QStringLiteral("pdfNotInvertedRegions"), entry.pdfNotInvertedRegions);
    book.insert(QStringLiteral("currentLocation"), entry.currentLocation);
    book.insert(QStringLiteral("currentProgress"), entry.currentProgress);
    book.insert(QStringLiteral("pageMode"), entry.pageMode);
    book.insert(QStringLiteral("subjects"), entry.genres);
    book.insert(QStringLiteral("categoryEntriesCount"), 0);
    book.insert(QStringLiteral("categoryEntriesModel"), QString());
    return book;
}

static QString mainSubjectSortKey(const QStringList &subjects, const QString &title)
{
    const QStringList mainSubjects = localizedMainSubjects(subjects);
    const QString subjectKey = mainSubjects.isEmpty() ? QString(QChar(0xffff)) : mainSubjects.join(QStringLiteral(", "));
    return subjectKey + QLatin1Char('\t') + title;
}

static QString titleGroupTitle(const QString &title)
{
    const QString normalizedTitle = title.trimmed().simplified();

    for (const QChar &character : normalizedTitle) {
        if (!character.isLetterOrNumber()) {
            continue;
        }

        if (character.isDigit()) {
            return QStringLiteral("0-9");
        }

        const QString normalizedLetter = QString(character).toUpper().normalized(QString::NormalizationForm_D);
        for (const QChar &letterCharacter : normalizedLetter) {
            if (letterCharacter.category() == QChar::Mark_NonSpacing || !letterCharacter.isLetter()) {
                continue;
            }

            return QString(letterCharacter);
        }
    }

    return QStringLiteral("#");
}

static QString titleWithoutLeadingArticle(const QString &title)
{
    const QString normalizedTitle = title.trimmed().simplified();
    int titleStart = 0;
    while (titleStart < normalizedTitle.size() && !normalizedTitle.at(titleStart).isLetterOrNumber()) {
        ++titleStart;
    }

    if (titleStart >= normalizedTitle.size()) {
        return normalizedTitle;
    }

    const QString sortableTitle = normalizedTitle.mid(titleStart);
    const QStringList articles = {
        QStringLiteral("the"),
        QStringLiteral("a"),
        QStringLiteral("an"),
        QStringLiteral("der"),
        QStringLiteral("die"),
        QStringLiteral("das"),
        QStringLiteral("ein"),
        QStringLiteral("eine"),
        QStringLiteral("sri"),
        QStringLiteral("shri"),
    };

    for (const QString &article : articles) {
        if (sortableTitle.size() <= article.size() || !sortableTitle.startsWith(article, Qt::CaseInsensitive)) {
            continue;
        }

        const QChar separator = sortableTitle.at(article.size());
        if (separator.isLetterOrNumber()) {
            continue;
        }

        int contentStart = article.size() + 1;
        while (contentStart < sortableTitle.size() && !sortableTitle.at(contentStart).isLetterOrNumber()) {
            ++contentStart;
        }

        if (contentStart < sortableTitle.size()) {
            return sortableTitle.mid(contentStart).trimmed();
        }
    }

    return normalizedTitle;
}

static QString titleGroupSortKey(const QString &groupTitle)
{
    return groupTitle == QStringLiteral("#") ? QString(QChar(0xffff)) : groupTitle;
}

static QString titleSortKey(const QString &title)
{
    const QString sortableTitle = titleWithoutLeadingArticle(title);
    return sortableTitle + QLatin1Char('\t') + title;
}

static QString fileTypeSortPrefix(const QString &filename)
{
    const QString suffix = QFileInfo(filename).suffix().toCaseFolded();
    if (suffix == QStringLiteral("epub")) {
        return QStringLiteral("0-epub");
    }
    if (suffix == QStringLiteral("pdf")) {
        return QStringLiteral("1-pdf");
    }

    return QStringLiteral("2-") + suffix;
}

static QString fileTypeGroupTitle(const QString &filename)
{
    const QString suffix = QFileInfo(filename).suffix().toCaseFolded();
    if (suffix == QStringLiteral("epub")) {
        return i18nc("@item:inlistbox EPUB documents", "Epub Documents");
    }
    if (suffix == QStringLiteral("pdf")) {
        return i18nc("@item:inlistbox PDF documents", "Pdf Documents");
    }

    return i18nc("@item:inlistbox documents with another file type", "Other Documents");
}

static QString typeSortKey(const BookEntry &entry)
{
    return fileTypeSortPrefix(entry.filename) + QLatin1Char('\t') + titleSortKey(entry.title);
}

static QString authorGroupTitle(const QStringList &authors)
{
    QStringList cleanedAuthors;
    for (const QString &author : authors) {
        const QString cleanedAuthor = author.trimmed().simplified();
        if (!cleanedAuthor.isEmpty()) {
            cleanedAuthors.append(cleanedAuthor);
        }
    }

    return cleanedAuthors.join(QStringLiteral(", "));
}

static int lastOpenedGroupIndex(const QDateTime &lastOpenedTime, const QDateTime &now)
{
    if (!lastOpenedTime.isValid()) {
        return 6;
    }

    const qint64 secondsSinceLastOpened = std::max<qint64>(0, lastOpenedTime.secsTo(now));
    constexpr qint64 secondsPerDay = 24 * 60 * 60;

    if (secondsSinceLastOpened <= secondsPerDay) {
        return 0;
    }
    if (secondsSinceLastOpened <= 7 * secondsPerDay) {
        return 1;
    }
    if (secondsSinceLastOpened <= 14 * secondsPerDay) {
        return 2;
    }
    if (secondsSinceLastOpened <= 21 * secondsPerDay) {
        return 3;
    }
    if (secondsSinceLastOpened <= 28 * secondsPerDay) {
        return 4;
    }

    return 5;
}

static QString lastOpenedGroupTitle(int groupIndex)
{
    switch (groupIndex) {
    case 0:
        return i18nc("@item:inlistbox books last opened within the last day", "Last 24 Hours");
    case 1:
        return i18nc("@item:inlistbox books last opened within the last week", "Last 7 Days");
    case 2:
        return i18nc("@item:inlistbox books last opened within the last two weeks", "Last 2 Weeks");
    case 3:
        return i18nc("@item:inlistbox books last opened within the last three weeks", "Last 3 Weeks");
    case 4:
        return i18nc("@item:inlistbox books last opened within the last four weeks", "Last 4 Weeks");
    case 5:
        return i18nc("@item:inlistbox books last opened more than four weeks ago", "Older than 4 Weeks");
    default:
        return i18nc("@item:inlistbox books that have never been opened", "Never Opened");
    }
}

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
        {SubjectRole, "subjects"},
        {GenreRole, "genres"},
        {KeywordRole, "keywords"},
        {SeriesRole, "series"},
        {SeriesNumbersRole, "seriesNumber"},
        {SeriesVolumesRole, "seriesVolume"},
        {AuthorRole, "author"},
        {AuthorSortRole, "authorSort"},
        {PublisherRole, "publisher"},
        {CreatedRole, "created"},
        {LastOpenedTimeRole, "lastOpenedTime"},
        {CurrentProgressRole, "currentProgress"},
        {CurrentLocationRole, "currentLocation"},
        {ZoomLevelRole, "zoomLevel"},
        {PageModeRole, "pageMode"},
        {CategoryEntriesModelRole, "categoryEntriesModel"},
        {CategoryEntryCountRole, "categoryEntriesCount"},
        {ThumbnailRole, "thumbnail"},
        {DescriptionRole, "description"},
        {CommentRole, "comment"},
        {TagsRole, "tags"},
        {RatingRole, "rating"},
        {LocationsRole, "locations"},
        {PdfNotInvertedRegionsRole, "pdfNotInvertedRegions"},
        {EntryRole, "entry"},
        {LocalizedTitleRole, "localizedTitle"},
        {TitleSortRole, "titleSort"},
        {MainSubjectSortRole, "mainSubjectSort"},
        {MainSubjectRole, "mainSubject"},
        {BookVersionRole, "bookVersion"},
        {TypeSortRole, "typeSort"},
        {PasswordProtectedRole, "passwordProtected"},
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
        case AuthorSortRole:
        case TitleSortRole:
        case MainSubjectSortRole:
        case MainSubjectRole:
        case TypeSortRole:
            return model->name();
        case PasswordProtectedRole:
            return false;
        case LocalizedTitleRole:
            return localizedCategoryTitle(model->name(), model->role());
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
            case SubjectRole:
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
        case EntryRole:
            return QVariant::fromValue(entry);
        case Qt::DisplayRole:
        case FilenameRole:
            return entry.filename;
        case FiletitleRole:
            return entry.filetitle;
        case TitleRole:
            return entry.title;
        case LocalizedTitleRole:
            return entry.title;
        case TitleSortRole:
            return titleSortKey(entry.title);
        case TypeSortRole:
            return typeSortKey(entry);
        case SubjectRole:
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
        case AuthorSortRole:
            return entry.author.join(QStringLiteral(", "));
        case MainSubjectSortRole:
            return mainSubjectSortKey(entry.genres, entry.title);
        case MainSubjectRole:
            return mainSubjectTitle(entry.genres);
        case PublisherRole:
            return entry.publisher;
        case CreatedRole:
            return entry.created;
        case LastOpenedTimeRole:
            return entry.lastOpenedTime;
        case CurrentProgressRole:
            return entry.currentProgress;
        case ZoomLevelRole:
            return entry.zoomLevel;
        case PageModeRole:
            return entry.pageMode;
        case BookVersionRole:
            return entry.bookVersion;
        case PasswordProtectedRole:
            return entry.passwordProtected;
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
            if (!epub.openFile(entry.filename)) {
                return {};
            }
            const auto image = epub.coverImage();
            const QString savedThumbnail = entry.saveCover(image, entry.thumbnail);
            if (savedThumbnail.isEmpty()) {
                return {};
            }
            return savedThumbnail;
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
        case PdfNotInvertedRegionsRole:
            return entry.pdfNotInvertedRegions;
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
    d->invalidateBookGroupCaches();

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
            if (compareRole == CreatedRole || compareRole == LastOpenedTimeRole) {
                const QDateTime firstDate = compareRole == CreatedRole ? entry.created : entry.lastOpenedTime;
                const QDateTime secondDate = compareRole == CreatedRole ? d->entries.at(insertionIndex).created : d->entries.at(insertionIndex).lastOpenedTime;
                if (firstDate <= secondDate) {
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
                if (QString::localeAwareCompare(titleSortKey(d->entries.at(insertionIndex).title), titleSortKey(entry.title)) > 0) {
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
    d->invalidateBookGroupCaches();

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
            connect(categoryModel, &CategoryEntriesModel::countChanged, this, [this, categoryModel] {
                const int categoryIndex = d->categoryModels.indexOf(categoryModel);
                if (categoryIndex < 0) {
                    return;
                }

                const QModelIndex changedIndex = index(categoryIndex);
                Q_EMIT dataChanged(changedIndex, changedIndex, {CategoryEntryCountRole});
            });
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
            Q_EMIT countChanged();
        }
        if (splitPos > -1) {
            if (isSubjectCategoryRole(compareRole) && categoryModel->indexOfFile(entry.filename) == -1) {
                categoryModel->append(entry, compareRole);
            }
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
    int index = -1;
    if (QFile::exists(filename)) {
        int i = 0;
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

BookEntry CategoryEntriesModel::bookEntryFromFile(const QString &filename)
{
    return bookFromFile(filename).value_or(BookEntry{});
}

QVariantList CategoryEntriesModel::flatCategoryEntries(const QString &parentPath) const
{
    struct FlatCategoryEntry {
        QString path;
        QString title;
        QString localizedTitle;
        int level = 1;
        int bookCount = 0;
        CategoryEntriesModel *model = nullptr;
    };

    QList<FlatCategoryEntry> entries;
    const std::function<void(const CategoryEntriesModel *, const QString &)> collect = [&entries, &collect](const CategoryEntriesModel *model,
                                                                                                            const QString &pathPrefix) {
        for (CategoryEntriesModel *categoryModel : std::as_const(model->d->categoryModels)) {
            const QString path = pathPrefix.isEmpty() ? categoryModel->name() : pathPrefix + QLatin1Char('/') + categoryModel->name();
            entries.append({
                path,
                categoryModel->name(),
                localizedCategoryTitle(categoryModel->name(), categoryModel->role()),
                static_cast<int>(path.count(QLatin1Char('/')) + 1),
                categoryModel->bookCount(),
                categoryModel,
            });
            collect(categoryModel, path);
        }
    };

    collect(this, parentPath);
    std::sort(entries.begin(), entries.end(), [](const FlatCategoryEntry &first, const FlatCategoryEntry &second) {
        if (first.level != second.level) {
            return first.level < second.level;
        }
        return QString::localeAwareCompare(first.localizedTitle, second.localizedTitle) < 0;
    });

    QVariantList result;
    result.reserve(entries.size());
    for (const FlatCategoryEntry &entry : std::as_const(entries)) {
        QVariantMap item;
        item.insert(QStringLiteral("title"), entry.path);
        item.insert(QStringLiteral("name"), entry.title);
        item.insert(QStringLiteral("localizedTitle"), entry.localizedTitle);
        item.insert(QStringLiteral("localizedPathTitle"), localizedSubjectPathTitle(entry.path, entry.model ? entry.model->role() : UnknownRole));
        item.insert(QStringLiteral("subjectLevel"), entry.level);
        item.insert(QStringLiteral("categoryEntriesCount"), entry.bookCount);
        item.insert(QStringLiteral("categoryEntriesModel"), QVariant::fromValue<CategoryEntriesModel *>(entry.model));
        result.append(item);
    }

    return result;
}

QVariantList CategoryEntriesModel::mainSubjectBookGroups(const QString &filterText)
{
    struct MainSubjectBookGroup {
        QString title;
        QString sortKey;
        QList<BookEntry> books;
    };

    const QString normalizedFilter = filterText.trimmed();
    if (d->mainSubjectGroupCache.matches(normalizedFilter)) {
        return d->mainSubjectGroupCache.groups;
    }

    d->mainSubjectGroupCache.clear();

    QList<MainSubjectBookGroup> groups;
    QHash<QString, int> groupIndexes;

    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (!normalizedFilter.isEmpty() && !entry.title.contains(normalizedFilter, Qt::CaseInsensitive)) {
            continue;
        }

        const QStringList mainSubjects = localizedMainSubjects(entry.genres);
        const QString groupTitle =
            mainSubjects.isEmpty() ? i18nc("@item:inlistbox books without a main topic", "No Main Topic") : mainSubjects.join(QStringLiteral(", "));
        const QString groupSortKey = mainSubjects.isEmpty() ? QString(QChar(0xffff)) : groupTitle;
        const QString groupKey = groupSortKey.toCaseFolded();

        auto groupIt = groupIndexes.constFind(groupKey);
        if (groupIt == groupIndexes.constEnd()) {
            groupIndexes.insert(groupKey, groups.size());
            groups.append({
                groupTitle,
                groupSortKey,
                {},
            });
            groupIt = groupIndexes.constFind(groupKey);
        }

        groups[*groupIt].books.append(entry);
    }

    std::sort(groups.begin(), groups.end(), [](const MainSubjectBookGroup &first, const MainSubjectBookGroup &second) {
        const int compare = QString::localeAwareCompare(first.sortKey, second.sortKey);
        if (compare != 0) {
            return compare < 0;
        }
        return QString::localeAwareCompare(first.title, second.title) < 0;
    });

    QVariantList result;
    result.reserve(groups.size());
    for (MainSubjectBookGroup &group : groups) {
        std::sort(group.books.begin(), group.books.end(), [](const BookEntry &first, const BookEntry &second) {
            const int compare = QString::localeAwareCompare(titleSortKey(first.title), titleSortKey(second.title));
            if (compare != 0) {
                return compare < 0;
            }
            return QString::localeAwareCompare(first.filename, second.filename) < 0;
        });

        QVariantList books;
        books.reserve(group.books.size());
        for (const BookEntry &entry : std::as_const(group.books)) {
            books.append(groupedBookMap(entry));
        }

        QVariantMap item;
        item.insert(QStringLiteral("title"), group.title);
        item.insert(QStringLiteral("books"), books);
        item.insert(QStringLiteral("bookCount"), books.size());
        result.append(item);
    }

    d->mainSubjectGroupCache.store(normalizedFilter, result);
    return d->mainSubjectGroupCache.groups;
}

QVariantList CategoryEntriesModel::titleBookGroups(const QString &filterText)
{
    struct TitleBookGroup {
        QString title;
        QString displayTitle;
        QString sortKey;
        QList<BookEntry> books;
    };

    const QString normalizedFilter = filterText.trimmed();
    if (d->titleGroupCache.matches(normalizedFilter)) {
        return d->titleGroupCache.groups;
    }

    d->titleGroupCache.clear();

    QList<TitleBookGroup> groups;
    QHash<QString, int> groupIndexes;

    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (!normalizedFilter.isEmpty() && !entry.title.contains(normalizedFilter, Qt::CaseInsensitive)) {
            continue;
        }

        const QString groupTitle = titleGroupTitle(titleWithoutLeadingArticle(entry.title));
        const QString groupSortKey = titleGroupSortKey(groupTitle);
        const QString groupKey = groupSortKey.toCaseFolded();

        auto groupIt = groupIndexes.constFind(groupKey);
        if (groupIt == groupIndexes.constEnd()) {
            groupIndexes.insert(groupKey, groups.size());
            groups.append({
                groupTitle,
                groupTitle,
                groupSortKey,
                {},
            });
            groupIt = groupIndexes.constFind(groupKey);
        }

        groups[*groupIt].books.append(entry);
    }

    std::sort(groups.begin(), groups.end(), [](const TitleBookGroup &first, const TitleBookGroup &second) {
        const int compare = QString::localeAwareCompare(first.sortKey, second.sortKey);
        if (compare != 0) {
            return compare < 0;
        }
        return QString::localeAwareCompare(first.title, second.title) < 0;
    });

    auto titleLetterIndex = [](const QString &title) -> int {
        if (title.size() != 1) {
            return -1;
        }

        const ushort code = title.at(0).unicode();
        if (code < 'A' || code > 'Z') {
            return -1;
        }

        return code - 'A';
    };

    auto appendLetters = [](QString &title, int firstLetterIndex, int lastLetterIndex) {
        QStringList titleParts = title.split(QStringLiteral(", "), Qt::SkipEmptyParts);
        for (int letterIndex = firstLetterIndex; letterIndex <= lastLetterIndex; ++letterIndex) {
            titleParts.append(QString(QChar(QLatin1Char('A').unicode() + letterIndex)));
        }
        title = titleParts.join(QStringLiteral(", "));
    };

    int previousLetterGroupIndex = -1;
    int previousLetterIndex = -1;
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const int letterIndex = titleLetterIndex(groups.at(groupIndex).title);
        if (letterIndex < 0) {
            continue;
        }

        if (previousLetterGroupIndex < 0) {
            if (letterIndex > 0) {
                QStringList titleParts;
                for (int missingLetterIndex = 0; missingLetterIndex < letterIndex; ++missingLetterIndex) {
                    titleParts.append(QString(QChar(QLatin1Char('A').unicode() + missingLetterIndex)));
                }
                titleParts.append(groups[groupIndex].displayTitle);
                groups[groupIndex].displayTitle = titleParts.join(QStringLiteral(", "));
            }
        } else if (letterIndex > previousLetterIndex + 1) {
            appendLetters(groups[previousLetterGroupIndex].displayTitle, previousLetterIndex + 1, letterIndex - 1);
        }

        previousLetterGroupIndex = groupIndex;
        previousLetterIndex = letterIndex;
    }

    if (previousLetterGroupIndex >= 0 && previousLetterIndex < 25) {
        appendLetters(groups[previousLetterGroupIndex].displayTitle, previousLetterIndex + 1, 25);
    }

    QVariantList result;
    result.reserve(groups.size());
    for (TitleBookGroup &group : groups) {
        std::sort(group.books.begin(), group.books.end(), [](const BookEntry &first, const BookEntry &second) {
            const int compare = QString::localeAwareCompare(titleSortKey(first.title), titleSortKey(second.title));
            if (compare != 0) {
                return compare < 0;
            }
            return QString::localeAwareCompare(first.filename, second.filename) < 0;
        });

        QVariantList books;
        books.reserve(group.books.size());
        for (const BookEntry &entry : std::as_const(group.books)) {
            books.append(groupedBookMap(entry));
        }

        QVariantMap item;
        item.insert(QStringLiteral("title"), group.displayTitle);
        item.insert(QStringLiteral("books"), books);
        item.insert(QStringLiteral("bookCount"), books.size());
        result.append(item);
    }

    d->titleGroupCache.store(normalizedFilter, result);
    return d->titleGroupCache.groups;
}

QVariantList CategoryEntriesModel::authorBookGroups(const QString &filterText)
{
    struct AuthorBookGroup {
        QString title;
        QString sortKey;
        QList<BookEntry> books;
    };

    const QString normalizedFilter = filterText.trimmed();
    if (d->authorGroupCache.matches(normalizedFilter)) {
        return d->authorGroupCache.groups;
    }

    d->authorGroupCache.clear();

    QList<AuthorBookGroup> groups;
    QHash<QString, int> groupIndexes;

    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (!normalizedFilter.isEmpty() && !entry.title.contains(normalizedFilter, Qt::CaseInsensitive)) {
            continue;
        }

        const QString authorsTitle = authorGroupTitle(entry.author);
        const QString groupTitle = authorsTitle.isEmpty() ? i18nc("@item:inlistbox books without an author", "No Author") : authorsTitle;
        const QString groupSortKey = authorsTitle.isEmpty() ? QString(QChar(0xffff)) : authorsTitle;
        const QString groupKey = groupSortKey.toCaseFolded();

        auto groupIt = groupIndexes.constFind(groupKey);
        if (groupIt == groupIndexes.constEnd()) {
            groupIndexes.insert(groupKey, groups.size());
            groups.append({
                groupTitle,
                groupSortKey,
                {},
            });
            groupIt = groupIndexes.constFind(groupKey);
        }

        groups[*groupIt].books.append(entry);
    }

    std::sort(groups.begin(), groups.end(), [](const AuthorBookGroup &first, const AuthorBookGroup &second) {
        const int compare = QString::localeAwareCompare(first.sortKey, second.sortKey);
        if (compare != 0) {
            return compare < 0;
        }
        return QString::localeAwareCompare(first.title, second.title) < 0;
    });

    QVariantList result;
    result.reserve(groups.size());
    for (AuthorBookGroup &group : groups) {
        std::sort(group.books.begin(), group.books.end(), [](const BookEntry &first, const BookEntry &second) {
            const int compare = QString::localeAwareCompare(titleSortKey(first.title), titleSortKey(second.title));
            if (compare != 0) {
                return compare < 0;
            }
            return QString::localeAwareCompare(first.filename, second.filename) < 0;
        });

        QVariantList books;
        books.reserve(group.books.size());
        for (const BookEntry &entry : std::as_const(group.books)) {
            books.append(groupedBookMap(entry));
        }

        QVariantMap item;
        item.insert(QStringLiteral("title"), group.title);
        item.insert(QStringLiteral("books"), books);
        item.insert(QStringLiteral("bookCount"), books.size());
        result.append(item);
    }

    d->authorGroupCache.store(normalizedFilter, result);
    return d->authorGroupCache.groups;
}

QVariantList CategoryEntriesModel::lastOpenedBookGroups(const QString &filterText)
{
    struct LastOpenedBookGroup {
        int sortKey;
        QString title;
        QList<BookEntry> books;
    };

    const QString normalizedFilter = filterText.trimmed();
    if (d->lastOpenedGroupCache.matches(normalizedFilter, 5 * 60 * 1000)) {
        return d->lastOpenedGroupCache.groups;
    }

    d->lastOpenedGroupCache.clear();

    QList<LastOpenedBookGroup> groups;
    QHash<int, int> groupIndexes;
    const QDateTime now = QDateTime::currentDateTime();

    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (!normalizedFilter.isEmpty() && !entry.title.contains(normalizedFilter, Qt::CaseInsensitive)) {
            continue;
        }

        const int groupSortKey = lastOpenedGroupIndex(entry.lastOpenedTime, now);

        auto groupIt = groupIndexes.constFind(groupSortKey);
        if (groupIt == groupIndexes.constEnd()) {
            groupIndexes.insert(groupSortKey, groups.size());
            groups.append({
                groupSortKey,
                lastOpenedGroupTitle(groupSortKey),
                {},
            });
            groupIt = groupIndexes.constFind(groupSortKey);
        }

        groups[*groupIt].books.append(entry);
    }

    std::sort(groups.begin(), groups.end(), [](const LastOpenedBookGroup &first, const LastOpenedBookGroup &second) {
        return first.sortKey < second.sortKey;
    });

    QVariantList result;
    result.reserve(groups.size());
    for (LastOpenedBookGroup &group : groups) {
        std::sort(group.books.begin(), group.books.end(), [](const BookEntry &first, const BookEntry &second) {
            if (first.lastOpenedTime.isValid() && second.lastOpenedTime.isValid() && first.lastOpenedTime != second.lastOpenedTime) {
                return first.lastOpenedTime > second.lastOpenedTime;
            }
            if (first.lastOpenedTime.isValid() != second.lastOpenedTime.isValid()) {
                return first.lastOpenedTime.isValid();
            }

            const int compare = QString::localeAwareCompare(titleSortKey(first.title), titleSortKey(second.title));
            if (compare != 0) {
                return compare < 0;
            }
            return QString::localeAwareCompare(first.filename, second.filename) < 0;
        });

        QVariantList books;
        books.reserve(group.books.size());
        for (const BookEntry &entry : std::as_const(group.books)) {
            books.append(groupedBookMap(entry));
        }

        QVariantMap item;
        item.insert(QStringLiteral("title"), group.title);
        item.insert(QStringLiteral("books"), books);
        item.insert(QStringLiteral("bookCount"), books.size());
        result.append(item);
    }

    d->lastOpenedGroupCache.store(normalizedFilter, result);
    return d->lastOpenedGroupCache.groups;
}

QVariantList CategoryEntriesModel::typeBookGroups(const QString &filterText)
{
    struct TypeBookGroup {
        QString sortKey;
        QString title;
        QList<BookEntry> books;
    };

    const QString normalizedFilter = filterText.trimmed();
    if (d->typeGroupCache.matches(normalizedFilter)) {
        return d->typeGroupCache.groups;
    }

    d->typeGroupCache.clear();

    QList<TypeBookGroup> groups;
    QHash<QString, int> groupIndexes;

    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (!normalizedFilter.isEmpty() && !entry.title.contains(normalizedFilter, Qt::CaseInsensitive)) {
            continue;
        }

        const QString groupSortKey = fileTypeSortPrefix(entry.filename);

        auto groupIt = groupIndexes.constFind(groupSortKey);
        if (groupIt == groupIndexes.constEnd()) {
            groupIndexes.insert(groupSortKey, groups.size());
            groups.append({
                groupSortKey,
                fileTypeGroupTitle(entry.filename),
                {},
            });
            groupIt = groupIndexes.constFind(groupSortKey);
        }

        groups[*groupIt].books.append(entry);
    }

    std::sort(groups.begin(), groups.end(), [](const TypeBookGroup &first, const TypeBookGroup &second) {
        const int compare = QString::localeAwareCompare(first.sortKey, second.sortKey);
        if (compare != 0) {
            return compare < 0;
        }
        return QString::localeAwareCompare(first.title, second.title) < 0;
    });

    QVariantList result;
    result.reserve(groups.size());
    for (TypeBookGroup &group : groups) {
        std::sort(group.books.begin(), group.books.end(), [](const BookEntry &first, const BookEntry &second) {
            const int compare = QString::localeAwareCompare(titleSortKey(first.title), titleSortKey(second.title));
            if (compare != 0) {
                return compare < 0;
            }
            return QString::localeAwareCompare(first.filename, second.filename) < 0;
        });

        QVariantList books;
        books.reserve(group.books.size());
        for (const BookEntry &entry : std::as_const(group.books)) {
            books.append(groupedBookMap(entry));
        }

        QVariantMap item;
        item.insert(QStringLiteral("title"), group.title);
        item.insert(QStringLiteral("books"), books);
        item.insert(QStringLiteral("bookCount"), books.size());
        result.append(item);
    }

    d->typeGroupCache.store(normalizedFilter, result);
    return d->typeGroupCache.groups;
}

void CategoryEntriesModel::entryDataChanged(const BookEntry &entry)
{
    d->invalidateBookGroupCaches();

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
    d->invalidateBookGroupCaches();

    int listIndex = d->entries.indexOf(entry);
    if (listIndex > -1) {
        int entryIndex = listIndex + d->categoryModels.count();
        beginRemoveRows(QModelIndex(), entryIndex, entryIndex);
        d->entries.removeAll(entry);
        endRemoveRows();
        Q_EMIT countChanged();
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
    if (!dir.exists(QStringLiteral("covers"))) {
        dir.mkdir(QStringLiteral("covers"));
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
