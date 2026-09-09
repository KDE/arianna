// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "booklistmodel.h"

#include "bookdatabase.h"
#include "booktruthstore.h"
#include "config.h"
#include "okularpdfpagepainter.h"
#include "okularpdfsupport.h"
#include "pdfvalidationreportparser.h"
#include "pdfversionupdater.h"
#include "pdfwatermarkfilter.h"

#include <KFileMetaData/UserMetaData>
#include <KLocalizedString>

#include <algorithm>

#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QPoint>
#include <QProcess>
#include <QRect>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "epubcontainer.h"

#include <QChar>
#include <QLoggingCategory>
#include <arianna_debug.h>

#include <core/area.h>
#include <core/document.h>
#include <core/generator.h>
#include <core/observer.h>
#include <core/page.h>

static QString normalizeBookServerIdentifier(QString identifier)
{
    identifier = identifier.trimmed();

    const QString uuidUrnPrefix = QStringLiteral("urn:uuid:");
    if (identifier.startsWith(uuidUrnPrefix, Qt::CaseInsensitive)) {
        identifier = identifier.mid(uuidUrnPrefix.size());
    }

    const QUuid uuid(identifier);
    if (!uuid.isNull()) {
        return uuid.toString(QUuid::WithoutBraces);
    }

    return identifier;
}

static QStringList normalizedCommaSeparatedList(QString value)
{
    value.replace(QLatin1Char(';'), QLatin1Char(','));

    QStringList result;
    QSet<QString> seen;
    const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString item : items) {
        item = item.trimmed();
        if (item.isEmpty()) {
            continue;
        }

        const QString key = item.toCaseFolded();
        if (seen.contains(key)) {
            continue;
        }

        seen.insert(key);
        result.append(item);
    }

    return result;
}

static QString stablePdfIdentifier(const QFileInfo &fileInfo)
{
    const QByteArray pathHash = QCryptographicHash::hash(QDir::cleanPath(fileInfo.absoluteFilePath()).toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("arianna-pdf-%1").arg(QString::fromLatin1(pathHash.left(32)));
}

static QString pdfMetadataString(const Okular::DocumentInfo &info, Okular::DocumentInfo::Key key)
{
    return info.get(key).trimmed();
}

static QDateTime pdfMetadataDate(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        return {};
    }

    const QLocale locale;
    QDateTime result = locale.toDateTime(value, QLocale::LongFormat);
    if (result.isValid()) {
        return result;
    }

    const QDate localeDate = locale.toDate(value, QLocale::LongFormat);
    if (localeDate.isValid()) {
        return QDateTime(localeDate, QTime(0, 0));
    }

    result = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (result.isValid()) {
        return result;
    }

    result = QDateTime::fromString(value, Qt::ISODate);
    if (result.isValid()) {
        return result;
    }

    result = QDateTime::fromString(value, Qt::RFC2822Date);
    if (result.isValid()) {
        return result;
    }

    static const QRegularExpression pdfDateExpression(QStringLiteral(R"(^D:?(\d{4})(\d{2})?(\d{2})?(\d{2})?(\d{2})?(\d{2})?.*$)"));
    const QRegularExpressionMatch match = pdfDateExpression.match(value);
    if (!match.hasMatch()) {
        return {};
    }

    const int year = match.captured(1).toInt();
    const int month = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
    const int day = match.captured(3).isEmpty() ? 1 : match.captured(3).toInt();
    const int hour = match.captured(4).isEmpty() ? 0 : match.captured(4).toInt();
    const int minute = match.captured(5).isEmpty() ? 0 : match.captured(5).toInt();
    const int second = match.captured(6).isEmpty() ? 0 : match.captured(6).toInt();

    result = QDateTime(QDate(year, month, day), QTime(hour, minute, second));
    return result.isValid() ? result : QDateTime();
}

static QString pdfHeaderVersion(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray header = file.read(1024);
    const qsizetype markerIndex = header.indexOf("%PDF-");
    if (markerIndex < 0) {
        return {};
    }

    const qsizetype versionStart = markerIndex + 5;
    qsizetype versionEnd = versionStart;
    while (versionEnd < header.size()) {
        const char character = header.at(versionEnd);
        if ((character < '0' || character > '9') && character != '.') {
            break;
        }
        ++versionEnd;
    }

    return QString::fromLatin1(header.mid(versionStart, versionEnd - versionStart)).trimmed();
}

static void removeCachedCover(const QString &thumbnail, const QString &replacement);
static QString localFileNameFromUrlOrPath(const QString &fileName);

class PdfCoverRenderObserver : public Okular::DocumentObserver
{
public:
    explicit PdfCoverRenderObserver(Okular::Document &document)
        : m_document(&document)
    {
        m_document->addObserver(this);
    }

    ~PdfCoverRenderObserver() override
    {
        if (m_document) {
            m_document->removeObserver(this);
        }
    }

private:
    Okular::Document *m_document = nullptr;
};

static QImage pdfCoverImage(Okular::Document &document, PdfCoverRenderObserver &observer)
{
    if (document.pages() <= 0) {
        return {};
    }

    const Okular::Page *page = document.page(0);
    if (!page) {
        return {};
    }

    QSizeF pointSize(page->width(), page->height());
    if (!pointSize.isValid() || pointSize.isEmpty()) {
        pointSize = QSizeF(210, 297);
    }

    const int targetHeight = 512;
    QSize imageSize = pointSize.scaled(QSizeF(targetHeight, targetHeight), Qt::KeepAspectRatio).toSize();
    imageSize.setWidth(std::max(1, imageSize.width()));
    imageSize.setHeight(std::max(1, imageSize.height()));

    const Okular::NormalizedRect fullPageRect(0, 0, 1, 1);
    auto *request = new Okular::PixmapRequest(&observer, 0, imageSize.width(), imageSize.height(), 1.0, 0, Okular::PixmapRequest::NoFeature);
    request->setNormalizedRect(fullPageRect);
    document.requestPixmaps({request}, Okular::Document::NoOption);
    if (!page->hasPixmap(&observer, imageSize.width(), imageSize.height(), fullPageRect)) {
        return {};
    }

    QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    PagePainter::paintPageOnPainter(&painter,
                                    page,
                                    &observer,
                                    0,
                                    imageSize.width(),
                                    imageSize.height(),
                                    QRect(QPoint(0, 0), imageSize),
                                    QColor(Qt::black),
                                    QColor(Qt::white),
                                    QVector<PagePainter::ColorRegion>(),
                                    QVector<PagePainter::ColorRegion>());
    return image;
}

static bool saveRenderedPdfCover(BookEntry &entry, Okular::Document &document, PdfCoverRenderObserver &observer)
{
    const QImage coverImage = pdfCoverImage(document, observer);
    if (coverImage.isNull()) {
        return false;
    }
    const QString previousThumbnail = entry.thumbnail;
    const QString thumbnail = entry.saveCover(coverImage);
    if (thumbnail.isEmpty()) {
        return false;
    }

    entry.thumbnail = thumbnail;
    removeCachedCover(previousThumbnail, entry.thumbnail);
    return true;
}

static bool pdfIsInCalibreBookFolder(const QFileInfo &fileInfo)
{
    const QString calibreLibraryFolder = localFileNameFromUrlOrPath(Config::calibreLibraryFolder()).trimmed();
    if (calibreLibraryFolder.isEmpty()) {
        return false;
    }

    const QFileInfo calibreLibraryInfo(calibreLibraryFolder);
    if (!calibreLibraryInfo.exists() || !calibreLibraryInfo.isDir()) {
        return false;
    }

    const QString calibreLibraryPath = QDir(calibreLibraryInfo.absoluteFilePath()).canonicalPath();
    const QString pdfDirectoryPath = fileInfo.dir().canonicalPath();
    if (calibreLibraryPath.isEmpty() || pdfDirectoryPath.isEmpty() || pdfDirectoryPath == calibreLibraryPath) {
        return false;
    }

    QString calibreLibraryPrefix = calibreLibraryPath;
    if (!calibreLibraryPrefix.endsWith(QLatin1Char('/'))) {
        calibreLibraryPrefix.append(QLatin1Char('/'));
    }
    if (!pdfDirectoryPath.startsWith(calibreLibraryPrefix)) {
        return false;
    }

    const QFileInfo metadataInfo(fileInfo.dir().filePath(QStringLiteral("metadata.opf")));
    return metadataInfo.exists() && metadataInfo.isFile();
}

static QImage calibreSiblingPdfCoverImage(const QFileInfo &fileInfo)
{
    if (!pdfIsInCalibreBookFolder(fileInfo)) {
        return {};
    }

    const QFileInfo coverInfo(fileInfo.dir().filePath(QStringLiteral("cover.jpg")));
    if (!coverInfo.exists() || !coverInfo.isFile()) {
        return {};
    }

    QImage image(coverInfo.absoluteFilePath());
    if (image.isNull()) {
        qWarning() << "Unable to load Calibre PDF cover image:" << coverInfo.absoluteFilePath();
    }
    return image;
}

static bool saveCalibrePdfCover(BookEntry &entry, const QFileInfo &fileInfo)
{
    const QImage coverImage = calibreSiblingPdfCoverImage(fileInfo);
    if (coverImage.isNull()) {
        return false;
    }

    const QString previousThumbnail = entry.thumbnail;
    const QString thumbnail = entry.saveCover(coverImage);
    if (thumbnail.isEmpty()) {
        return false;
    }

    entry.thumbnail = thumbnail;
    removeCachedCover(previousThumbnail, entry.thumbnail);
    return true;
}

static void clearCachedCover(BookEntry &entry)
{
    const QString previousThumbnail = entry.thumbnail;
    entry.thumbnail.clear();
    removeCachedCover(previousThumbnail, entry.thumbnail);
}

static bool pdfEntryNeedsCoverRepair(const BookEntry &entry, const QFileInfo &fileInfo)
{
    const QString pdfIdentifier = stablePdfIdentifier(fileInfo);
    return entry.passwordProtected || entry.thumbnail.isEmpty() || !QFileInfo::exists(entry.thumbnail)
        || (entry.identifier != pdfIdentifier && entry.uniqueIdentifier != pdfIdentifier);
}

static bool applyPdfMetadata(BookEntry &entry, bool refreshCover)
{
    const QFileInfo fileInfo(entry.filename);
    const bool repairCover = pdfEntryNeedsCoverRepair(entry, fileInfo);
    Arianna::initializeOkularPdfSupport();
    Okular::Document document(nullptr);
    const QUrl url = QUrl::fromLocalFile(fileInfo.absoluteFilePath());
    const Okular::Document::OpenResult result = Arianna::openOkularPdfDocument(document, fileInfo.absoluteFilePath(), url);
    if (result == Okular::Document::OpenNeedsPassword) {
        entry.identifier = stablePdfIdentifier(fileInfo);
        entry.uniqueIdentifier = entry.identifier;
        entry.bookVersion = pdfHeaderVersion(fileInfo.absoluteFilePath());
        entry.passwordProtected = true;
        clearCachedCover(entry);
        return true;
    }
    if (result != Okular::Document::OpenSuccess || !document.isOpened()) {
        return false;
    }
    PdfCoverRenderObserver observer(document);

    const Okular::DocumentInfo info = document.documentInfo();
    const QString title = pdfMetadataString(info, Okular::DocumentInfo::Title);
    entry.title = title.isEmpty() ? fileInfo.completeBaseName() : title;
    entry.bookVersion = pdfHeaderVersion(fileInfo.absoluteFilePath());

    const QString author = pdfMetadataString(info, Okular::DocumentInfo::Author);
    if (!author.isEmpty()) {
        entry.author = QStringList{author};
    }

    const QString subject = pdfMetadataString(info, Okular::DocumentInfo::Subject);
    if (!subject.isEmpty() && entry.genres.isEmpty()) {
        entry.genres = QStringList{subject};
    }

    const QString keywords = pdfMetadataString(info, Okular::DocumentInfo::Keywords);
    if (!keywords.isEmpty()) {
        entry.keywords = normalizedCommaSeparatedList(keywords);
    }

    const QDateTime created = pdfMetadataDate(pdfMetadataString(info, Okular::DocumentInfo::CreationDate));
    if (created.isValid()) {
        entry.created = created;
    }

    entry.identifier = stablePdfIdentifier(fileInfo);
    entry.uniqueIdentifier = entry.identifier;
    entry.passwordProtected = false;

    if (!refreshCover && !repairCover) {
        return true;
    }

    if (saveCalibrePdfCover(entry, fileInfo) || saveRenderedPdfCover(entry, document, observer)) {
        return true;
    }

    clearCachedCover(entry);
    return true;
}

static void applyEpubIdentifiers(BookEntry &entry, EPubContainer &epub)
{
    const QStringList identifiers = epub.metadata(QStringLiteral("identifiers"));
    entry.identifier = identifiers.join(QStringLiteral(", "));
    entry.uniqueIdentifier = normalizeBookServerIdentifier(epub.metadata(QStringLiteral("unique-identifier")).value(0));
    if (entry.uniqueIdentifier.isEmpty()) {
        entry.uniqueIdentifier = normalizeBookServerIdentifier(identifiers.value(0));
    }
}

static bool refreshEpubIdentifiers(BookEntry &entry)
{
    QMimeDatabase db;
    if (db.mimeTypeForFile(entry.filename).name() != QStringLiteral("application/epub+zip")) {
        return false;
    }

    EPubContainer epub(nullptr);
    if (!epub.openFile(entry.filename)) {
        return false;
    }

    applyEpubIdentifiers(entry, epub);
    return !entry.uniqueIdentifier.isEmpty();
}

static bool refreshEpubThumbnail(BookEntry &entry)
{
    QMimeDatabase db;
    if (db.mimeTypeForFile(entry.filename).name() != QStringLiteral("application/epub+zip")) {
        return false;
    }

    EPubContainer epub(nullptr);
    if (!epub.openFile(entry.filename)) {
        return false;
    }

    const QString thumbnail = entry.saveCover(epub.coverImage());
    if (thumbnail.isEmpty()) {
        return false;
    }

    entry.thumbnail = thumbnail;
    return true;
}

static void removeCachedCover(const QString &thumbnail, const QString &replacement = {})
{
    if (thumbnail.isEmpty()) {
        return;
    }

    const QFileInfo thumbnailInfo(thumbnail);
    if (!thumbnailInfo.exists()) {
        return;
    }

    if (!replacement.isEmpty() && thumbnailInfo.absoluteFilePath() == QFileInfo(replacement).absoluteFilePath()) {
        return;
    }

    const QString coversPath = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).absoluteFilePath(QStringLiteral("covers"));
    const QString normalizedCoversPath = QDir(coversPath).absolutePath();
    if (!thumbnailInfo.absoluteFilePath().startsWith(normalizedCoversPath + QLatin1Char('/'))) {
        return;
    }

    QFile::remove(thumbnailInfo.absoluteFilePath());
}

static void applyEpubMetadata(BookEntry &entry, EPubContainer &epub, bool refreshCover)
{
    const auto titles = epub.metadata(QStringLiteral("title"));
    entry.title = titles.isEmpty() ? QFileInfo(entry.filename).completeBaseName() : titles[0];
    entry.author = epub.metadata(QStringLiteral("creator"));
    entry.rights = epub.metadata(QStringLiteral("rights")).join(QStringLiteral(", "));
    entry.source = epub.metadata(QStringLiteral("source")).join(QStringLiteral(", "));
    applyEpubIdentifiers(entry, epub);
    entry.bookVersion = epub.metadata(QStringLiteral("epub-version")).value(0);
    entry.language = epub.metadata(QStringLiteral("language")).join(QStringLiteral(", "));
    entry.genres = epub.metadata(QStringLiteral("subject"));
    entry.publisher = epub.metadata(QStringLiteral("publisher")).join(QStringLiteral(", "));

    entry.series.clear();
    entry.seriesNumbers.clear();
    entry.seriesVolumes.clear();
    const auto collections = epub.collections();
    for (const auto &collection : collections) {
        entry.series.append(collection.name);
        entry.seriesNumbers.append(QStringLiteral("0"));
        entry.seriesVolumes.append(QString::number(collection.position));
    }

    if (!refreshCover) {
        return;
    }

    const QString previousThumbnail = entry.thumbnail;
    const auto image = epub.coverImage();
    entry.thumbnail = entry.saveCover(image);
    removeCachedCover(previousThumbnail, entry.thumbnail);
}

static QString localFileNameFromUrlOrPath(const QString &fileName)
{
    const QUrl fileUrl(fileName);
    return fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileName;
}

static bool hasSupportedCalibreImportSuffix(const QFileInfo &fileInfo)
{
    const QString suffix = fileInfo.suffix().toLower();
    return suffix == QStringLiteral("epub") || suffix == QStringLiteral("pdf");
}

static QString processOutputString(const QByteArray &output)
{
    QString text = QString::fromUtf8(output);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.trimmed();
}

static BookEntry createBookEntryFromFile(const QString &fileName, const QVariantHash &metadata = {}, bool refreshCover = true)
{
    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return BookEntry();
    }

    BookEntry entry;
    entry.filename = fileInfo.absoluteFilePath();
    entry.filetitle = fileInfo.fileName();
    entry.title = fileInfo.completeBaseName();
    entry.series = QStringList{};
    entry.seriesNumbers = QStringList{QStringLiteral("0")};
    entry.seriesVolumes = QStringList{QStringLiteral("0")};
    entry.lastOpenedTime = fileInfo.lastRead();

    KFileMetaData::UserMetaData data(entry.filename);
    entry.rating = data.rating();
    entry.comment = data.userComment();
    entry.tags = data.tags();

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
        } else if (it.key() == QLatin1String("lastRead")) {
            const QDateTime lastRead = it.value().toDateTime();
            if (lastRead.isValid()) {
                entry.lastOpenedTime = lastRead;
            }
        } else if (it.key() == QLatin1String("lastOpenedTime")) {
            const QDateTime lastOpenedTime = it.value().toDateTime();
            if (lastOpenedTime.isValid()) {
                entry.lastOpenedTime = lastOpenedTime;
            }
        } else if (it.key() == QLatin1String("currentLocation")) {
            entry.currentLocation = it.value().toString();
        } else if (it.key() == QLatin1String("currentProgress")) {
            entry.currentProgress = it.value().toInt();
        } else if (it.key() == QLatin1String("pageMode")) {
            entry.pageMode = it.value().toString();
            if (entry.pageMode != QStringLiteral("single") && entry.pageMode != QStringLiteral("two")) {
                entry.pageMode.clear();
            }
        } else if (it.key() == QLatin1String("comments")) {
            entry.comment = it.value().toString();
        } else if (it.key() == QLatin1String("tags")) {
            entry.tags = it.value().toStringList();
        } else if (it.key() == QLatin1String("rating")) {
            entry.rating = it.value().toInt();
        }
    }

    QMimeDatabase db;
    const QString mimetype = db.mimeTypeForFile(fileInfo).name();
    if (mimetype == QStringLiteral("application/epub+zip")) {
        EPubContainer epub(nullptr);
        if (epub.openFile(entry.filename)) {
            applyEpubMetadata(entry, epub, refreshCover);
        }
    } else if (mimetype == QStringLiteral("application/pdf")) {
        if (!applyPdfMetadata(entry, refreshCover)) {
            entry.identifier = stablePdfIdentifier(fileInfo);
            entry.uniqueIdentifier = entry.identifier;
            if (refreshCover && !saveCalibrePdfCover(entry, fileInfo)) {
                clearCachedCover(entry);
            }
        }
    } else {
        return BookEntry();
    }

    return entry;
}

static bool bookEntryDataMatches(const BookEntry &first, const BookEntry &second)
{
    return first.filename == second.filename && first.filetitle == second.filetitle && first.title == second.title && first.genres == second.genres
        && first.keywords == second.keywords && first.characters == second.characters && first.series == second.series
        && first.seriesNumbers == second.seriesNumbers && first.seriesVolumes == second.seriesVolumes && first.author == second.author
        && first.rights == second.rights && first.publisher == second.publisher && first.created == second.created
        && first.lastOpenedTime == second.lastOpenedTime && first.currentLocation == second.currentLocation && first.currentProgress == second.currentProgress
        && first.zoomLevel == second.zoomLevel && first.pageMode == second.pageMode && first.bookVersion == second.bookVersion
        && first.thumbnail == second.thumbnail && first.passwordProtected == second.passwordProtected && first.description == second.description
        && first.comment == second.comment && first.tags == second.tags && first.locations == second.locations
        && first.pdfNotInvertedRegions == second.pdfNotInvertedRegions && first.identifier == second.identifier
        && first.uniqueIdentifier == second.uniqueIdentifier && first.source == second.source && first.language == second.language
        && first.rating == second.rating;
}

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
        , cacheLoaded(false)
        , databaseSyncScheduled(false)
        , skipNextDatabaseSync(false)
        , secondaryCategoryPopulationScheduled(false)
        , secondaryCategoryPopulated(false)
        , secondaryCategoryPopulationIndex(0)
        , subjectCategoryPopulationScheduled(false)
        , subjectCategoryPopulated(false)
        , subjectCategoryPopulationIndex(0) { };

    QList<BookEntry> entries;

    ContentList *contentModel;
    CategoryEntriesModel *newlyAddedCategoryModel;
    CategoryEntriesModel *authorCategoryModel;
    CategoryEntriesModel *seriesCategoryModel;
    CategoryEntriesModel *publisherCategoryModel;
    CategoryEntriesModel *keywordCategoryModel;
    CategoryEntriesModel *folderCategoryModel;

    bool cacheLoaded;
    bool databaseSyncScheduled;
    bool skipNextDatabaseSync;
    bool secondaryCategoryPopulationScheduled;
    bool secondaryCategoryPopulated;
    qsizetype secondaryCategoryPopulationIndex;
    bool subjectCategoryPopulationScheduled;
    bool subjectCategoryPopulated;
    qsizetype subjectCategoryPopulationIndex;
    QHash<QString, QString> pendingWatermarkReplacements;

    void discardPendingWatermarkReplacement(const QString &replacementFileName)
    {
        const QString absoluteReplacementFileName = QFileInfo(localFileNameFromUrlOrPath(replacementFileName)).absoluteFilePath();
        if (absoluteReplacementFileName.isEmpty()) {
            return;
        }

        if (pendingWatermarkReplacements.remove(absoluteReplacementFileName) > 0) {
            QFile::remove(absoluteReplacementFileName);
        }
    }

    void discardPendingWatermarkReplacementsForOriginal(const QString &fileName)
    {
        const QString absoluteFileName = QFileInfo(localFileNameFromUrlOrPath(fileName)).absoluteFilePath();
        for (auto it = pendingWatermarkReplacements.begin(); it != pendingWatermarkReplacements.end();) {
            if (it.value() != absoluteFileName) {
                ++it;
                continue;
            }

            QFile::remove(it.key());
            it = pendingWatermarkReplacements.erase(it);
        }
    }

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
        if (!folderCategoryModel) {
            folderCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, folderCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, folderCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->folderCategoryModelChanged();
        }
    }

    void createSubjectCategoryModel(BookListModel *q)
    {
        if (keywordCategoryModel) {
            return;
        }

        keywordCategoryModel = new CategoryEntriesModel(q);
        connect(q, &CategoryEntriesModel::entryDataUpdated, keywordCategoryModel, &CategoryEntriesModel::entryDataUpdated);
        connect(q, &CategoryEntriesModel::entryRemoved, keywordCategoryModel, &CategoryEntriesModel::entryRemoved);
        Q_EMIT q->keywordCategoryModelChanged();
        Q_EMIT q->subjectCategoryModelChanged();
    }

    void addSubjectCategories(const BookEntry &entry)
    {
        if (!keywordCategoryModel) {
            return;
        }

        for (int i = 0; i < entry.genres.size(); i++) {
            keywordCategoryModel->addCategoryEntry(entry.genres.at(i), entry, SubjectRole);
        }
    }

    void addSecondaryCategories(const BookEntry &entry)
    {
        if (!authorCategoryModel || !seriesCategoryModel || !publisherCategoryModel || !folderCategoryModel) {
            return;
        }

        for (int i = 0; i < entry.author.size(); i++) {
            authorCategoryModel->addCategoryEntry(entry.author.at(i), entry);
        }
        for (int i = 0; i < entry.series.size(); i++) {
            seriesCategoryModel->addCategoryEntry(entry.series.at(i), entry, SeriesRole);
        }
        publisherCategoryModel->addCategoryEntry(entry.publisher, entry);

        QUrl url(entry.filename.left(entry.filename.lastIndexOf(QLatin1Char('/'))));
        folderCategoryModel->addCategoryEntry(url.path().mid(1), entry);
        if (folderCategoryModel->indexOfFile(entry.filename) == -1) {
            folderCategoryModel->append(entry);
        }
    }

    void scheduleSecondaryCategoryPopulation(BookListModel *q, int delay = 0)
    {
        if (secondaryCategoryPopulated || secondaryCategoryPopulationScheduled) {
            return;
        }

        secondaryCategoryPopulationScheduled = true;
        QTimer::singleShot(delay, q, [this, q]() {
            populateSecondaryCategoryModelsBatch(q);
        });
    }

    void populateSecondaryCategoryModelsBatch(BookListModel *q)
    {
        secondaryCategoryPopulationScheduled = false;
        if (secondaryCategoryPopulated) {
            return;
        }
        if (!cacheLoaded) {
            scheduleSecondaryCategoryPopulation(q, 100);
            return;
        }

        initializeSubModels(q);

        constexpr qsizetype secondaryPopulationBatchSize = 8;
        constexpr int secondaryPopulationBatchDelay = 40;
        const qsizetype batchEnd = std::min(secondaryCategoryPopulationIndex + secondaryPopulationBatchSize, entries.size());
        for (; secondaryCategoryPopulationIndex < batchEnd; ++secondaryCategoryPopulationIndex) {
            addSecondaryCategories(entries.at(secondaryCategoryPopulationIndex));
        }

        if (secondaryCategoryPopulationIndex < entries.size()) {
            scheduleSecondaryCategoryPopulation(q, secondaryPopulationBatchDelay);
            return;
        }

        secondaryCategoryPopulated = true;
    }

    void scheduleSubjectCategoryPopulation(BookListModel *q, int delay = 0)
    {
        if (subjectCategoryPopulated || subjectCategoryPopulationScheduled) {
            return;
        }

        subjectCategoryPopulationScheduled = true;
        QTimer::singleShot(delay, q, [this, q]() {
            populateSubjectCategoryModelBatch(q);
        });
    }

    void populateSubjectCategoryModelBatch(BookListModel *q)
    {
        subjectCategoryPopulationScheduled = false;
        if (subjectCategoryPopulated) {
            return;
        }
        if (!cacheLoaded) {
            scheduleSubjectCategoryPopulation(q, 100);
            return;
        }

        createSubjectCategoryModel(q);

        constexpr qsizetype subjectPopulationBatchSize = 20;
        constexpr int subjectPopulationBatchDelay = 40;
        const qsizetype batchEnd = std::min(subjectCategoryPopulationIndex + subjectPopulationBatchSize, entries.size());
        for (; subjectCategoryPopulationIndex < batchEnd; ++subjectCategoryPopulationIndex) {
            addSubjectCategories(entries.at(subjectCategoryPopulationIndex));
        }

        if (subjectCategoryPopulationIndex < entries.size()) {
            scheduleSubjectCategoryPopulation(q, subjectPopulationBatchDelay);
            return;
        }

        subjectCategoryPopulated = true;
        Q_EMIT q->subjectCategoryModelPopulatedChanged();
    }

    bool addEntry(BookListModel *q, const BookEntry &entry, bool populateSecondaryCategories = true)
    {
        if (q->indexOfFile(entry.filename) != -1) {
            return false;
        }

        entries.append(entry);
        q->append(entry);
        if (newlyAddedCategoryModel->indexOfFile(entry.filename) == -1) {
            newlyAddedCategoryModel->append(entry, CreatedRole);
        }
        if (populateSecondaryCategories) {
            addSecondaryCategories(entry);
        }
        if (subjectCategoryPopulated) {
            addSubjectCategories(entry);
        }

        return true;
    }

    qsizetype entryIndexForFile(const QString &fileName) const
    {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if (entries.at(i).filename == fileName) {
                return i;
            }
        }

        return -1;
    }

    QList<BookEntry> loadValidEntries(bool refreshMissingMetadata)
    {
        QList<BookEntry> validEntries;
        const QList<BookEntry> cachedEntries = BookDatabase::self().loadEntries();
        QSet<QString> usedThumbnails;
        for (const BookEntry &entry : std::as_const(cachedEntries)) {
            /*
             * This might turn out a little slow, but we should avoid having entries
             * that do not exist. If we end up with slowdown issues when loading the
             * cache this would be a good place to start investigating.
             */
            if (QFileInfo::exists(entry.filename)) {
                BookEntry cachedEntry = entry;
                if (refreshMissingMetadata && cachedEntry.uniqueIdentifier.isEmpty() && refreshEpubIdentifiers(cachedEntry)) {
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("identifier"), cachedEntry.identifier);
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("uniqueIdentifier"), cachedEntry.uniqueIdentifier);
                }

                const QString thumbnailPath = cachedEntry.thumbnail.isEmpty() ? QString() : QFileInfo(cachedEntry.thumbnail).absoluteFilePath();
                const bool thumbnailNeedsRefresh =
                    cachedEntry.thumbnail.isEmpty() || !QFileInfo::exists(cachedEntry.thumbnail) || usedThumbnails.contains(thumbnailPath);
                if (refreshMissingMetadata && thumbnailNeedsRefresh && refreshEpubThumbnail(cachedEntry)) {
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("thumbnail"), cachedEntry.thumbnail);
                }
                if (!cachedEntry.thumbnail.isEmpty()) {
                    usedThumbnails.insert(QFileInfo(cachedEntry.thumbnail).absoluteFilePath());
                }
                validEntries.append(cachedEntry);
            } else {
                BookDatabase::self().removeEntry(entry);
            }
        }

        return validEntries;
    }

    void loadCache(BookListModel *q)
    {
        const QList<BookEntry> validEntries = loadValidEntries(false);
        initializeSubModels(q);

        int i = 0;
        for (const BookEntry &entry : validEntries) {
            addEntry(q, entry, false);
            if (++i % 100 == 0) {
                Q_EMIT q->countChanged();
                qApp->processEvents();
            }
        }

        cacheLoaded = true;
        Q_EMIT q->cacheLoadedChanged();
        scheduleSecondaryCategoryPopulation(q, 4000);
    }

    void syncWithDatabase(BookListModel *q)
    {
        const QList<BookEntry> databaseEntries = loadValidEntries(false);

        QHash<QString, BookEntry> databaseEntriesByFile;
        databaseEntriesByFile.reserve(databaseEntries.size());
        for (const BookEntry &entry : databaseEntries) {
            databaseEntriesByFile.insert(entry.filename, entry);
        }

        bool bookCountChanged = false;
        for (qsizetype i = entries.size() - 1; i >= 0; --i) {
            const BookEntry entry = entries.at(i);
            if (databaseEntriesByFile.contains(entry.filename)) {
                continue;
            }

            entries.removeAt(i);
            Q_EMIT q->entryRemoved(entry);
            bookCountChanged = true;
        }

        if (!databaseEntries.isEmpty()) {
            initializeSubModels(q);
        }

        for (const BookEntry &databaseEntry : databaseEntries) {
            const qsizetype existingIndex = entryIndexForFile(databaseEntry.filename);
            if (existingIndex < 0) {
                if (addEntry(q, databaseEntry)) {
                    bookCountChanged = true;
                }
                continue;
            }

            const BookEntry existingEntry = entries.at(existingIndex);
            if (bookEntryDataMatches(existingEntry, databaseEntry)) {
                continue;
            }

            entries.removeAt(existingIndex);
            Q_EMIT q->entryRemoved(existingEntry);
            addEntry(q, databaseEntry);
        }

        if (contentModel) {
            contentModel->setKnownFiles(q->knownBookFiles());
        }

        if (bookCountChanged) {
            Q_EMIT q->countChanged();
        }
    }
};

BookListModel::BookListModel(QObject *parent)
    : CategoryEntriesModel(parent)
    , d(std::make_unique<Private>())
{
    connect(&BookDatabase::self(), &BookDatabase::databaseChanged, this, &BookListModel::scheduleDatabaseSync);
}

BookListModel::~BookListModel() = default;

void BookListModel::componentComplete()
{
    QTimer::singleShot(0, this, [this]() {
        d->loadCache(this);
    });
}

void BookListModel::scheduleDatabaseSync()
{
    if (!d->cacheLoaded || d->databaseSyncScheduled) {
        return;
    }

    if (d->skipNextDatabaseSync) {
        d->skipNextDatabaseSync = false;
        return;
    }

    d->databaseSyncScheduled = true;
    QTimer::singleShot(250, this, [this]() {
        d->databaseSyncScheduled = false;
        if (!d->cacheLoaded) {
            return;
        }

        d->syncWithDatabase(this);
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
        QVariant filePath = d->contentModel->data(d->contentModel->index(i, 0, index), ContentList::FilePathRole);
        QVariantHash metadata = d->contentModel->data(d->contentModel->index(i, 0, index), Qt::UserRole + 2).toHash();
        const BookEntry entry = createBookEntryFromFile(filePath.toUrl().toLocalFile(), metadata);
        if (entry.filename.isEmpty()) {
            continue;
        }

        newEntries.append(entry);
    }

    // Batch process the entries
    for (const BookEntry &entry : std::as_const(newEntries)) {
        if (d->addEntry(this, entry)) {
            BookDatabase::self().addEntry(entry);
        }
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

CategoryEntriesModel *BookListModel::subjectCategoryModel() const
{
    return d->keywordCategoryModel;
}

bool BookListModel::subjectCategoryModelPopulated() const
{
    return d->subjectCategoryPopulated;
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
    for (qsizetype i = 0; i < d->entries.size(); ++i) {
        if (d->entries.at(i).filename != fileName) {
            continue;
        }

        d->skipNextDatabaseSync = true;
        BookEntry entry = d->entries.at(i);
        const BookEntry oldEntry = entry;
        bool categoryMembershipChanged = false;
        bool updated = true;

        if (property == QStringLiteral("currentLocation")) {
            entry.currentLocation = value;
            BookDatabase::self().updateEntry(entry.filename, property, {value});
        } else if (property == QStringLiteral("currentProgress")) {
            entry.currentProgress = value.toInt();
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.toInt()));
        } else if (property == QStringLiteral("zoomLevel")) {
            entry.zoomLevel = value.toDouble();
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(entry.zoomLevel));
        } else if (property == QStringLiteral("pageMode")) {
            entry.pageMode = value == QStringLiteral("single") || value == QStringLiteral("two") ? value : QString();
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(entry.pageMode));
        } else if (property == QStringLiteral("locations")) {
            entry.locations = value;
            BookDatabase::self().updateEntry(entry.filename, property, {value});
        } else if (property == QStringLiteral("pdfNotInvertedRegions")) {
            entry.pdfNotInvertedRegions = value;
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(value));
        } else if (property == QStringLiteral("lastOpenedTime")) {
            entry.lastOpenedTime = QDateTime::fromString(value, Qt::ISODateWithMs);
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(entry.lastOpenedTime));
        } else if (property == QStringLiteral("rating")) {
            entry.rating = value.toInt();
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.toInt()));
        } else if (property == QStringLiteral("tags")) {
            entry.tags = value.split(QLatin1Char(','));
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.split(QLatin1Char(','))));
        } else if (property == QStringLiteral("author")) {
            const QStringList requestedAuthors = normalizedCommaSeparatedList(value);
            entry.author = requestedAuthors;

            QMimeDatabase db;
            if (db.mimeTypeForFile(entry.filename).name() == QStringLiteral("application/epub+zip")) {
                bool epubUpdated = false;
                {
                    EPubContainer epub(nullptr);
                    if (!epub.openFile(entry.filename)) {
                        qCWarning(ARIANNA_LOG) << "Unable to open EPUB for creator metadata update" << entry.filename;
                        updated = false;
                    } else {
                        const EpubNormalizationResult creatorUpdate = epub.updateCreators(requestedAuthors);
                        if (!creatorUpdate.success) {
                            qCWarning(ARIANNA_LOG) << "Unable to update EPUB creator metadata" << entry.filename << creatorUpdate.errorMessage;
                            updated = false;
                        } else {
                            epubUpdated = true;
                            if (creatorUpdate.changed) {
                                const QString bookId = entry.uniqueIdentifier.isEmpty() ? entry.identifier : entry.uniqueIdentifier;
                                if (!bookId.isEmpty()) {
                                    BookTruthStore store;
                                    const BookCommitResult commit = store.commitActiveFileChangeIfNeeded(bookId);
                                    if (!commit.success) {
                                        qCWarning(ARIANNA_LOG)
                                            << "Updated EPUB creators, but could not refresh the book state" << bookId << commit.errorMessage;
                                    }
                                }
                            }
                        }
                    }
                }

                if (epubUpdated) {
                    EPubContainer refreshedEpub(nullptr);
                    if (refreshedEpub.openFile(entry.filename)) {
                        entry.author = refreshedEpub.metadata(QStringLiteral("creator"));
                    }
                }
            }

            if (updated) {
                categoryMembershipChanged = oldEntry.author != entry.author;
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(entry.author));
            }
        } else if (property == QStringLiteral("genres") || property == QStringLiteral("subjects")) {
            const QStringList requestedGenres = normalizedCommaSeparatedList(value);
            entry.genres = requestedGenres;

            QMimeDatabase db;
            if (db.mimeTypeForFile(entry.filename).name() == QStringLiteral("application/epub+zip")) {
                bool epubUpdated = false;
                {
                    EPubContainer epub(nullptr);
                    if (!epub.openFile(entry.filename)) {
                        qCWarning(ARIANNA_LOG) << "Unable to open EPUB for subject metadata update" << entry.filename;
                        updated = false;
                    } else {
                        const EpubNormalizationResult subjectUpdate = epub.updateSubjects(requestedGenres);
                        if (!subjectUpdate.success) {
                            qCWarning(ARIANNA_LOG) << "Unable to update EPUB subject metadata" << entry.filename << subjectUpdate.errorMessage;
                            updated = false;
                        } else {
                            epubUpdated = true;
                            if (subjectUpdate.changed) {
                                const QString bookId = entry.uniqueIdentifier.isEmpty() ? entry.identifier : entry.uniqueIdentifier;
                                if (!bookId.isEmpty()) {
                                    BookTruthStore store;
                                    const BookCommitResult commit = store.commitActiveFileChangeIfNeeded(bookId);
                                    if (!commit.success) {
                                        qCWarning(ARIANNA_LOG)
                                            << "Updated EPUB subjects, but could not refresh the book state" << bookId << commit.errorMessage;
                                    }
                                }
                            }
                        }
                    }
                }

                if (epubUpdated) {
                    EPubContainer refreshedEpub(nullptr);
                    if (refreshedEpub.openFile(entry.filename)) {
                        entry.genres = refreshedEpub.metadata(QStringLiteral("subject"));
                    }
                }
            }

            if (updated) {
                categoryMembershipChanged = oldEntry.genres != entry.genres;
                BookDatabase::self().updateEntry(entry.filename, QStringLiteral("subjects"), QVariant(entry.genres));
            }
        } else if (property == QStringLiteral("comment")) {
            entry.comment = value;
            BookDatabase::self().updateEntry(entry.filename, property, QVariant(value));
        } else {
            updated = false;
        }

        if (!updated) {
            break;
        }

        if (categoryMembershipChanged) {
            d->entries.removeAt(i);
            Q_EMIT entryRemoved(oldEntry);

            d->initializeSubModels(this);
            d->addEntry(this, entry);
            Q_EMIT entryDataUpdated(entry);
        } else {
            d->entries[i] = entry;
            Q_EMIT entryDataUpdated(entry);
        }
        break;
    }
}

void BookListModel::populateSubjectCategoryModel()
{
    if (!d->cacheLoaded) {
        d->scheduleSubjectCategoryPopulation(this, 100);
        return;
    }

    d->scheduleSubjectCategoryPopulation(this);
}

BookEntry BookListModel::addBookFromFile(const QString &fileName, bool refreshCover)
{
    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return BookEntry();
    }

    const QString absoluteFileName = fileInfo.absoluteFilePath();
    if (indexOfFile(absoluteFileName) != -1) {
        return bookEntryFromFile(absoluteFileName);
    }

    const BookEntry entry = createBookEntryFromFile(absoluteFileName, {}, refreshCover);
    if (entry.filename.isEmpty()) {
        return BookEntry();
    }

    d->initializeSubModels(this);
    if (!d->addEntry(this, entry)) {
        return bookEntryFromFile(absoluteFileName);
    }

    d->skipNextDatabaseSync = true;
    BookDatabase::self().addEntry(entry);
    Q_EMIT countChanged();
    return entry;
}

QVariantMap BookListModel::importCalibreLibrary(const QString &folderName, bool refreshCover)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    result.insert(QStringLiteral("scanned"), 0);
    result.insert(QStringLiteral("imported"), 0);
    result.insert(QStringLiteral("duplicates"), 0);
    result.insert(QStringLiteral("failed"), 0);

    if (!d->cacheLoaded) {
        result.insert(QStringLiteral("message"), i18n("The library is still loading."));
        return result;
    }

    const QString localFolderName = localFileNameFromUrlOrPath(folderName).trimmed();
    if (localFolderName.isEmpty()) {
        result.insert(QStringLiteral("message"), i18n("Calibre library folder is not configured."));
        return result;
    }

    const QFileInfo folderInfo(localFolderName);
    if (!folderInfo.exists() || !folderInfo.isDir()) {
        result.insert(QStringLiteral("message"), i18n("Calibre library folder does not exist."));
        return result;
    }

    QSet<QString> knownBookFiles;
    knownBookFiles.reserve(d->entries.size());
    for (const BookEntry &entry : std::as_const(d->entries)) {
        knownBookFiles.insert(QFileInfo(localFileNameFromUrlOrPath(entry.filename)).absoluteFilePath());
    }

    QStringList failedFiles;
    int scanned = 0;
    int imported = 0;
    int duplicates = 0;
    int failed = 0;

    QDirIterator it(folderInfo.absoluteFilePath(), QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();

        const QFileInfo bookFileInfo = it.fileInfo();
        if (!hasSupportedCalibreImportSuffix(bookFileInfo)) {
            continue;
        }

        ++scanned;
        const QString absoluteFileName = bookFileInfo.absoluteFilePath();
        if (knownBookFiles.contains(absoluteFileName)) {
            ++duplicates;
            continue;
        }

        const BookEntry entry = addBookFromFile(absoluteFileName, refreshCover);
        if (entry.filename.isEmpty()) {
            ++failed;
            if (failedFiles.size() < 20) {
                failedFiles.append(absoluteFileName);
            }
            continue;
        }

        ++imported;
        knownBookFiles.insert(QFileInfo(entry.filename).absoluteFilePath());
    }

    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("folder"), folderInfo.absoluteFilePath());
    result.insert(QStringLiteral("scanned"), scanned);
    result.insert(QStringLiteral("imported"), imported);
    result.insert(QStringLiteral("duplicates"), duplicates);
    result.insert(QStringLiteral("failed"), failed);
    result.insert(QStringLiteral("failedFiles"), failedFiles);

    if (scanned == 0) {
        result.insert(QStringLiteral("message"), i18n("No supported EPUB or PDF books were found in the Calibre library folder."));
    } else if (failed > 0) {
        result.insert(QStringLiteral("message"), i18n("Calibre import finished: %1 imported, %2 duplicates skipped, %3 failed.", imported, duplicates, failed));
    } else {
        result.insert(QStringLiteral("message"), i18n("Calibre import finished: %1 imported, %2 duplicates skipped.", imported, duplicates));
    }

    qCDebug(ARIANNA_LOG) << "Calibre library import finished" << folderInfo.absoluteFilePath() << "scanned:" << scanned << "imported:" << imported
                         << "duplicates:" << duplicates << "failed:" << failed;
    return result;
}

BookEntry BookListModel::refreshBookFromFile(const QString &fileName, bool refreshCover)
{
    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return BookEntry();
    }

    qsizetype entryIndex = -1;
    const QString absoluteFileName = fileInfo.absoluteFilePath();
    for (qsizetype i = 0; i < d->entries.size(); ++i) {
        if (QFileInfo(d->entries.at(i).filename).absoluteFilePath() == absoluteFileName) {
            entryIndex = i;
            break;
        }
    }

    if (entryIndex < 0) {
        return BookEntry();
    }

    QMimeDatabase db;
    const QString mimeType = db.mimeTypeForFile(fileInfo).name();
    if (mimeType != QStringLiteral("application/epub+zip") && mimeType != QStringLiteral("application/pdf")) {
        return BookEntry();
    }

    const BookEntry oldEntry = d->entries.at(entryIndex);
    BookEntry refreshedEntry = oldEntry;
    refreshedEntry.filetitle = QFileInfo(refreshedEntry.filename).fileName();
    if (mimeType == QStringLiteral("application/epub+zip")) {
        EPubContainer epub(nullptr);
        if (!epub.openFile(absoluteFileName)) {
            return BookEntry();
        }
        applyEpubMetadata(refreshedEntry, epub, refreshCover);
    } else if (!applyPdfMetadata(refreshedEntry, refreshCover)) {
        const bool repairCover = pdfEntryNeedsCoverRepair(refreshedEntry, fileInfo);
        refreshedEntry.identifier = stablePdfIdentifier(QFileInfo(refreshedEntry.filename));
        refreshedEntry.uniqueIdentifier = refreshedEntry.identifier;
        if ((refreshCover || repairCover) && !saveCalibrePdfCover(refreshedEntry, fileInfo)) {
            clearCachedCover(refreshedEntry);
        }
    }

    Q_EMIT bookRefreshAboutToUpdate(absoluteFileName);
    d->entries.removeAt(entryIndex);
    Q_EMIT entryRemoved(oldEntry);

    d->initializeSubModels(this);
    d->addEntry(this, refreshedEntry);
    Q_EMIT entryDataUpdated(refreshedEntry);
    d->skipNextDatabaseSync = true;
    BookDatabase::self().updateEntry(refreshedEntry);

    qCDebug(ARIANNA_LOG) << "Refreshed book metadata from file" << refreshedEntry.filename;

    return refreshedEntry;
}

QVariantMap BookListModel::checkAndRefreshBookFromFile(const QString &fileName, bool refreshCover)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    result.insert(QStringLiteral("changed"), false);
    result.insert(QStringLiteral("emptyTitlesFixed"), 0);
    result.insert(QStringLiteral("scriptedPropertiesAdded"), 0);

    auto finishWithMessage = [&result](const QString &message) {
        result.insert(QStringLiteral("message"), message);
        result.insert(QStringLiteral("messages"), QStringList{message});
        return result;
    };

    const QUrl fileUrl(fileName);
    const QString localFileName = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileName;
    const QFileInfo fileInfo(localFileName);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return finishWithMessage(i18n("Book file does not exist."));
    }

    qsizetype entryIndex = -1;
    const QString absoluteFileName = fileInfo.absoluteFilePath();
    for (qsizetype i = 0; i < d->entries.size(); ++i) {
        if (QFileInfo(d->entries.at(i).filename).absoluteFilePath() == absoluteFileName) {
            entryIndex = i;
            break;
        }
    }

    if (entryIndex < 0) {
        return finishWithMessage(i18n("Book is not in the library."));
    }

    QMimeDatabase db;
    const QString mimeType = db.mimeTypeForFile(fileInfo).name();
    if (mimeType == QStringLiteral("application/pdf")) {
        const BookEntry refreshedEntry = refreshBookFromFile(absoluteFileName, refreshCover);
        if (refreshedEntry.filename.isEmpty()) {
            return finishWithMessage(i18n("PDF metadata could not be refreshed."));
        }

        const QString message = i18n("PDF metadata refreshed.");
        result.insert(QStringLiteral("success"), true);
        result.insert(QStringLiteral("entry"), QVariant::fromValue(refreshedEntry));
        result.insert(QStringLiteral("message"), message);
        result.insert(QStringLiteral("messages"), QStringList{message});
        return result;
    }

    if (mimeType != QStringLiteral("application/epub+zip")) {
        return finishWithMessage(i18n("Only EPUB and PDF books can be checked."));
    }

    const BookEntry oldEntry = d->entries.at(entryIndex);
    EPubContainer epub(nullptr);
    if (!epub.openFile(absoluteFileName)) {
        return finishWithMessage(i18n("Unable to open EPUB file."));
    }

    const EpubNormalizationResult normalization = epub.normalizeForArianna(oldEntry.title);
    result.insert(QStringLiteral("changed"), normalization.changed);
    result.insert(QStringLiteral("emptyTitlesFixed"), normalization.emptyTitlesFixed);
    result.insert(QStringLiteral("scriptedPropertiesAdded"), normalization.scriptedPropertiesAdded);

    QStringList messages = normalization.messages;
    if (!normalization.success) {
        const QString message = normalization.errorMessage.isEmpty() ? i18n("Unable to normalize EPUB file.") : normalization.errorMessage;
        result.insert(QStringLiteral("message"), message);
        result.insert(QStringLiteral("messages"), messages.isEmpty() ? QStringList{message} : messages + QStringList{message});
        return result;
    }

    if (normalization.changed && !oldEntry.uniqueIdentifier.isEmpty()) {
        BookTruthStore store;
        const BookCommitResult commit = store.commitActiveFileChangeIfNeeded(oldEntry.uniqueIdentifier);
        if (commit.success && !commit.unchanged) {
            messages.append(i18n("Registered normalized EPUB as the current book state."));
            result.insert(QStringLiteral("newStateId"), commit.newStateId.toString(QUuid::WithoutBraces));
        } else if (!commit.success) {
            messages.append(commit.errorMessage.isEmpty() ? i18n("Normalized EPUB, but the book state could not be refreshed.") : commit.errorMessage);
            result.insert(QStringLiteral("stateRefreshFailed"), true);
        }
    }

    const BookEntry refreshedEntry = refreshBookFromFile(absoluteFileName, refreshCover);
    if (refreshedEntry.filename.isEmpty()) {
        const QString message = i18n("EPUB was checked, but metadata could not be refreshed.");
        result.insert(QStringLiteral("message"), message);
        result.insert(QStringLiteral("messages"), messages.isEmpty() ? QStringList{message} : messages + QStringList{message});
        return result;
    }

    if (messages.isEmpty()) {
        messages.append(i18n("EPUB checked and metadata refreshed."));
    } else {
        messages.append(i18n("Metadata refreshed."));
    }

    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("entry"), QVariant::fromValue(refreshedEntry));
    result.insert(QStringLiteral("message"), messages.join(QStringLiteral("\n")));
    result.insert(QStringLiteral("messages"), messages);
    return result;
}

QVariantMap BookListModel::checkEpubFile(const QString &fileName, const QString &command) const
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    result.insert(QStringLiteral("exitCode"), -1);

    auto finishWithMessage = [&result](const QString &message) {
        result.insert(QStringLiteral("message"), message);
        if (!result.contains(QStringLiteral("output"))) {
            result.insert(QStringLiteral("output"), QString());
        }
        return result;
    };

    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return finishWithMessage(i18n("Book file does not exist."));
    }

    QMimeDatabase db;
    if (db.mimeTypeForFile(fileInfo).name() != QStringLiteral("application/epub+zip")) {
        return finishWithMessage(i18n("Only EPUB files can be checked with EPUBCheck."));
    }

    QString program = command.trimmed();
    QStringList arguments{fileInfo.absoluteFilePath()};
    if (program.isEmpty()) {
        return finishWithMessage(i18n("No EPUB check command configured."));
    }

    if (!QFileInfo::exists(program) && program.contains(QLatin1Char(' '))) {
        const QStringList commandParts = QProcess::splitCommand(program);
        if (commandParts.isEmpty()) {
            return finishWithMessage(i18n("Unable to parse EPUB check command."));
        }

        program = commandParts.first();
        arguments = commandParts.mid(1) + arguments;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) {
        result.insert(QStringLiteral("output"), processOutputString(process.readAll()));
        return finishWithMessage(i18n("EPUB check could not be started."));
    }

    if (!process.waitForFinished(120000)) {
        process.terminate();
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished();
        }

        result.insert(QStringLiteral("output"), processOutputString(process.readAll()));
        return finishWithMessage(i18n("EPUB check timed out."));
    }

    const QString output = processOutputString(process.readAll());
    const int exitCode = process.exitCode();
    const bool success = process.exitStatus() == QProcess::NormalExit && exitCode == 0;
    result.insert(QStringLiteral("success"), success);
    result.insert(QStringLiteral("exitCode"), exitCode);
    result.insert(QStringLiteral("output"), output);

    if (process.exitStatus() != QProcess::NormalExit) {
        return finishWithMessage(i18n("EPUB check crashed."));
    }

    return finishWithMessage(success ? i18n("EPUB check passed.") : i18n("EPUB check reported problems."));
}

QVariantMap BookListModel::checkPdfFile(const QString &fileName, const QString &command) const
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    result.insert(QStringLiteral("exitCode"), -1);

    auto finishWithMessage = [&result](const QString &message) {
        result.insert(QStringLiteral("message"), message);
        if (!result.contains(QStringLiteral("output"))) {
            result.insert(QStringLiteral("output"), QString());
        }
        return result;
    };

    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return finishWithMessage(i18n("Book file does not exist."));
    }

    QMimeDatabase db;
    if (db.mimeTypeForFile(fileInfo).name() != QStringLiteral("application/pdf")) {
        return finishWithMessage(i18n("Only PDF files can be checked with the configured PDF check command."));
    }

    QString program = command.trimmed();
    QStringList arguments{fileInfo.absoluteFilePath()};
    if (program.isEmpty()) {
        return finishWithMessage(i18n("No PDF check command configured."));
    }

    if (!QFileInfo::exists(program) && program.contains(QLatin1Char(' '))) {
        const QStringList commandParts = QProcess::splitCommand(program);
        if (commandParts.isEmpty()) {
            return finishWithMessage(i18n("Unable to parse PDF check command."));
        }

        program = commandParts.first();
        arguments = commandParts.mid(1) + arguments;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) {
        result.insert(QStringLiteral("output"), processOutputString(process.readAll()));
        return finishWithMessage(i18n("PDF check could not be started."));
    }

    if (!process.waitForFinished(120000)) {
        process.terminate();
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished();
        }

        result.insert(QStringLiteral("output"), processOutputString(process.readAll()));
        return finishWithMessage(i18n("PDF check timed out."));
    }

    const QString output = processOutputString(process.readAll());
    const int exitCode = process.exitCode();
    const bool success = process.exitStatus() == QProcess::NormalExit && exitCode == 0;
    result.insert(QStringLiteral("success"), success);
    result.insert(QStringLiteral("exitCode"), exitCode);
    result.insert(QStringLiteral("output"), output);
    result.insert(QStringLiteral("report"), PdfValidationReportParser::parse(output));

    if (process.exitStatus() != QProcess::NormalExit) {
        return finishWithMessage(i18n("PDF check crashed."));
    }

    return finishWithMessage(success ? i18n("PDF check completed.") : i18n("PDF check reported problems."));
}

QVariantMap BookListModel::createWatermarkFreePdf(const QString &fileName, const QString &pattern)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);
    result.insert(QStringLiteral("filtered"), false);
    result.insert(QStringLiteral("occurrences"), 0);

    auto finishWithMessage = [&result](const QString &message, bool success = false) {
        result.insert(QStringLiteral("success"), success);
        result.insert(QStringLiteral("message"), message);
        return result;
    };

    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return finishWithMessage(i18n("Book file does not exist."));
    }

    QMimeDatabase db;
    if (db.mimeTypeForFile(fileInfo).name() != QStringLiteral("application/pdf")) {
        return finishWithMessage(i18n("Only PDF files can be processed."));
    }

    const QString trimmedPattern = pattern.trimmed();
    if (trimmedPattern.isEmpty()) {
        return finishWithMessage(i18n("The PDF watermark marker is empty."));
    }

    d->discardPendingWatermarkReplacementsForOriginal(fileInfo.absoluteFilePath());

    QTemporaryFile temporaryFile(fileInfo.dir().filePath(QStringLiteral(".arianna-watermark-free-XXXXXX.pdf")));
    temporaryFile.setAutoRemove(false);
    if (!temporaryFile.open()) {
        return finishWithMessage(i18n("Unable to create a temporary watermark-free PDF file next to the original."));
    }

    const QString replacementFileName = temporaryFile.fileName();
    temporaryFile.close();
    if (!QFile::remove(replacementFileName)) {
        return finishWithMessage(i18n("Unable to prepare the temporary watermark-free PDF file."));
    }

    const PdfWatermarkFilterResult filterResult = PdfWatermarkFilter::filterFile(fileInfo.absoluteFilePath(), replacementFileName, trimmedPattern);
    result.insert(QStringLiteral("occurrences"), filterResult.occurrences);

    if (!filterResult.filtered) {
        QFile::remove(replacementFileName);
        if (filterResult.errorString.isEmpty()) {
            return finishWithMessage(i18n("No PDF watermark marker found."), true);
        }

        return finishWithMessage(filterResult.errorString);
    }

    const QString absoluteReplacementFileName = QFileInfo(replacementFileName).absoluteFilePath();
    d->pendingWatermarkReplacements.insert(absoluteReplacementFileName, fileInfo.absoluteFilePath());

    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("filtered"), true);
    result.insert(QStringLiteral("replacementFileName"), absoluteReplacementFileName);
    result.insert(QStringLiteral("message"), i18np("Found one PDF watermark marker.", "Found %1 PDF watermark markers.", filterResult.occurrences));
    return result;
}

QVariantMap BookListModel::replaceBookFileWithWatermarkFreePdf(const QString &fileName, const QString &replacementFileName, bool refreshCover)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);

    auto finishWithMessage = [&result](const QString &message, bool success = false) {
        result.insert(QStringLiteral("success"), success);
        result.insert(QStringLiteral("message"), message);
        return result;
    };

    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return finishWithMessage(i18n("Book file does not exist."));
    }

    const QFileInfo replacementInfo(localFileNameFromUrlOrPath(replacementFileName));
    if (!replacementInfo.exists() || !replacementInfo.isFile()) {
        return finishWithMessage(i18n("The watermark-free PDF file no longer exists."));
    }

    const QString absoluteFileName = fileInfo.absoluteFilePath();
    const QString absoluteReplacementFileName = replacementInfo.absoluteFilePath();
    if (d->pendingWatermarkReplacements.value(absoluteReplacementFileName) != absoluteFileName) {
        return finishWithMessage(i18n("The watermark-free PDF is no longer pending for this book."));
    }

    QTemporaryFile backupFile(fileInfo.dir().filePath(QStringLiteral(".arianna-watermark-original-XXXXXX.pdf")));
    backupFile.setAutoRemove(false);
    if (!backupFile.open()) {
        return finishWithMessage(i18n("Unable to create a backup before replacing the PDF."));
    }

    const QString backupFileName = backupFile.fileName();
    backupFile.close();
    if (!QFile::remove(backupFileName)) {
        return finishWithMessage(i18n("Unable to prepare the PDF backup file."));
    }

    const QFileDevice::Permissions originalPermissions = QFile::permissions(absoluteFileName);
    if (!QFile::rename(absoluteFileName, backupFileName)) {
        return finishWithMessage(i18n("Unable to move the original PDF to a backup file."));
    }

    QFile::setPermissions(absoluteReplacementFileName, originalPermissions);
    if (!QFile::rename(absoluteReplacementFileName, absoluteFileName)) {
        const bool restored = QFile::rename(backupFileName, absoluteFileName);
        if (!restored) {
            return finishWithMessage(i18n("Unable to replace the PDF and unable to restore the original PDF from backup."));
        }

        return finishWithMessage(i18n("Unable to replace the PDF with the watermark-free version."));
    }

    d->pendingWatermarkReplacements.remove(absoluteReplacementFileName);
    if (!QFile::remove(backupFileName)) {
        qCWarning(ARIANNA_LOG) << "Unable to remove watermark replacement backup" << backupFileName;
    }

    const BookEntry refreshedEntry = refreshBookFromFile(absoluteFileName, refreshCover);
    if (refreshedEntry.filename.isEmpty()) {
        return finishWithMessage(i18n("PDF replaced with watermark-free version, but metadata could not be refreshed."), true);
    }

    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("entry"), QVariant::fromValue(refreshedEntry));
    result.insert(QStringLiteral("message"), i18n("PDF replaced with watermark-free version."));
    return result;
}

void BookListModel::discardWatermarkFreePdf(const QString &replacementFileName)
{
    d->discardPendingWatermarkReplacement(replacementFileName);
}

QVariantMap BookListModel::setPdfVersion20(const QString &fileName, bool refreshCover)
{
    QVariantMap result;
    result.insert(QStringLiteral("success"), false);

    auto finishWithMessage = [&result](const QString &message, bool success = false) {
        result.insert(QStringLiteral("success"), success);
        result.insert(QStringLiteral("message"), message);
        return result;
    };

    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return finishWithMessage(i18n("Book file does not exist."));
    }

    QMimeDatabase db;
    if (db.mimeTypeForFile(fileInfo).name() != QStringLiteral("application/pdf")) {
        return finishWithMessage(i18n("Only PDF files can be processed."));
    }

    const QString absoluteFileName = fileInfo.absoluteFilePath();
    QTemporaryFile outputFile(fileInfo.dir().filePath(QStringLiteral(".arianna-pdf-2.0-XXXXXX.pdf")));
    outputFile.setAutoRemove(false);
    if (!outputFile.open()) {
        return finishWithMessage(i18n("Unable to create a temporary PDF file."));
    }
    const QString outputFileName = outputFile.fileName();
    outputFile.close();
    if (!QFile::remove(outputFileName)) {
        return finishWithMessage(i18n("Unable to prepare the temporary PDF file."));
    }

    const PdfVersionUpdateResult updateResult = PdfVersionUpdater::setVersion20(absoluteFileName, outputFileName);
    if (updateResult.alreadyVersion20) {
        QFile::remove(outputFileName);
        result.insert(QStringLiteral("version"), QStringLiteral("2.0"));
        return finishWithMessage(i18n("The PDF already uses version 2.0."), true);
    }
    if (!updateResult.updated) {
        QFile::remove(outputFileName);
        return finishWithMessage(updateResult.errorString.isEmpty() ? i18n("Unable to set the PDF version to 2.0.") : updateResult.errorString);
    }

    const QFileDevice::Permissions originalPermissions = QFile::permissions(absoluteFileName);
    if (!QFile::setPermissions(outputFileName, originalPermissions)) {
        QFile::remove(outputFileName);
        return finishWithMessage(i18n("Unable to preserve the original PDF file permissions."));
    }

    QTemporaryFile backupFile(fileInfo.dir().filePath(QStringLiteral(".arianna-pdf-version-backup-XXXXXX.pdf")));
    backupFile.setAutoRemove(false);
    if (!backupFile.open()) {
        QFile::remove(outputFileName);
        return finishWithMessage(i18n("Unable to create a backup before replacing the PDF."));
    }
    const QString backupFileName = backupFile.fileName();
    backupFile.close();
    if (!QFile::remove(backupFileName)) {
        QFile::remove(outputFileName);
        return finishWithMessage(i18n("Unable to prepare the PDF backup file."));
    }

    if (!QFile::rename(absoluteFileName, backupFileName)) {
        QFile::remove(outputFileName);
        return finishWithMessage(i18n("Unable to move the original PDF to a backup file."));
    }
    if (!QFile::rename(outputFileName, absoluteFileName)) {
        const bool restored = QFile::rename(backupFileName, absoluteFileName);
        QFile::remove(outputFileName);
        if (!restored) {
            return finishWithMessage(i18n("Unable to replace the PDF and unable to restore the original PDF from backup."));
        }

        return finishWithMessage(i18n("Unable to replace the original PDF."));
    }

    if (!QFile::remove(backupFileName)) {
        qCWarning(ARIANNA_LOG) << "Unable to remove PDF version update backup" << backupFileName;
    }

    result.insert(QStringLiteral("version"), QStringLiteral("2.0"));
    const BookEntry refreshedEntry = refreshBookFromFile(absoluteFileName, refreshCover);
    if (refreshedEntry.filename.isEmpty()) {
        return finishWithMessage(i18n("The PDF version was set to 2.0, but metadata could not be refreshed."), true);
    }

    result.insert(QStringLiteral("entry"), QVariant::fromValue(refreshedEntry));
    return finishWithMessage(i18n("The PDF header was set to version 2.0. This does not validate PDF 2.0 conformance."), true);
}

QString BookListModel::bookVersionFromFile(const QString &fileName) const
{
    const QFileInfo fileInfo(localFileNameFromUrlOrPath(fileName));
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return {};
    }

    QMimeDatabase db;
    const QString mimeType = db.mimeTypeForFile(fileInfo).name();
    if (mimeType == QStringLiteral("application/pdf")) {
        return pdfHeaderVersion(fileInfo.absoluteFilePath());
    }

    if (mimeType == QStringLiteral("application/epub+zip")) {
        EPubContainer epub(nullptr);
        if (epub.openFile(fileInfo.absoluteFilePath())) {
            return epub.metadata(QStringLiteral("epub-version")).value(0);
        }
    }

    return {};
}

void BookListModel::removeBook(const QString &fileName, bool deleteFile)
{
    for (qsizetype i = 0; i < d->entries.size(); ++i) {
        const BookEntry entry = d->entries.at(i);
        if (entry.filename != fileName) {
            continue;
        }

        if (deleteFile && !QFile::remove(entry.filename)) {
            qCWarning(ARIANNA_LOG) << "Could not delete book file" << entry.filename;
        }

        d->entries.removeAt(i);
        Q_EMIT entryRemoved(entry);
        Q_EMIT countChanged();
        BookDatabase::self().removeEntry(entry);
        break;
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
