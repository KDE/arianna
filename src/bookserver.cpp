// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "bookserver.h"
#include "ariannatrace.h"
#include "bookdatabase.h"
#include "bookserverconfig.h"
#include "booktruthstore.h"
#include "categoryentriesmodel.h"
#include "config.h"
#include "okularpdfsupport.h"
#include "pdfwatermarkfilter.h"
#include "referencestore.h"

#include <QAbstractSocket>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QTcpServer>

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KZip>

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <qcoreapplication.h>
#include <qdeadlinetimer.h>
#include <qdebug.h>
#include <qdir.h>

#include <KLocalizedString>

#include <algorithm>

static QString discoveryFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver.json");
}

static void removeDiscoveryFile()
{
    const QString path = discoveryFilePath();
    if (QFile::exists(path))
        QFile::remove(path);
}

static void writeDiscoveryFile(quint16 port, const QString &serverToken)
{
    const QString path = discoveryFilePath();

    QJsonObject obj;
    obj.insert(QStringLiteral("baseUrl"), BookServerConfig::baseUrl(port));
    obj.insert(QStringLiteral("serverToken"), serverToken);
    obj.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Could not write BookServer discovery file:" << path;
        return;
    }

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    file.close();

    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    qWarning() << "BookServer discovery file:" << path;
    AriannaTrace::event(QStringLiteral("bookserver.discovery_file.written"),
                        {{QStringLiteral("path"), path},
                         {QStringLiteral("host"), BookServerConfig::listenAddress().toString()},
                         {QStringLiteral("port"), port},
                         {QStringLiteral("baseUrl"), BookServerConfig::baseUrl(port)}});
}

static QString
manualBookrefAnchorOpenTag(const QString &sourceAnchorId, const QString &targetLocation, const bool isCrossReference, const QString &sourceAnchorTitle)
{
    QString tag = QStringLiteral("<a id=\"%1\" data-role=\"anchor\"").arg(sourceAnchorId.toHtmlEscaped());
    if (isCrossReference) {
        tag += QStringLiteral(" data-anchor-type=\"crossref\"");
    } else if (!targetLocation.isEmpty()) {
        tag += QStringLiteral(" href=\"%1\"").arg(targetLocation.toHtmlEscaped());
    }
    if (!sourceAnchorTitle.trimmed().isEmpty()) {
        tag += QStringLiteral(" title=\"%1\"").arg(sourceAnchorTitle.trimmed().toHtmlEscaped());
    }
    tag += QStringLiteral(">");
    return tag;
}

static QString fragmentFromLocation(const QString &location)
{
    const int fragmentIndex = location.indexOf(QLatin1Char('#'));
    if (fragmentIndex < 0) {
        return {};
    }

    return QUrl::fromPercentEncoding(location.mid(fragmentIndex + 1).toUtf8()).trimmed();
}

static bool findReferenceableTarget(const std::shared_ptr<EPubContainer> &container, const QString &refOrLocation, TargetAnchorInfo *target)
{
    const QString lookup = refOrLocation.trimmed();
    if (!container || lookup.isEmpty()) {
        return false;
    }

    const QString lookupFragment = fragmentFromLocation(lookup);
    for (const TargetAnchorInfo &candidate : container->referenceableTargets()) {
        if (candidate.ref == lookup || candidate.location == lookup || candidate.cfiLocation == lookup
            || (!lookupFragment.isEmpty() && candidate.ref == lookupFragment)) {
            if (target) {
                *target = candidate;
            }
            return true;
        }
    }

    return false;
}

static bool isFragmentPreciseLocation(const QString &location)
{
    const QString trimmedLocation = location.trimmed();
    return trimmedLocation.startsWith(QStringLiteral("epubcfi(")) || trimmedLocation.contains(QLatin1Char('#'));
}

static bool isCfiLocation(const QString &location)
{
    return location.trimmed().startsWith(QStringLiteral("epubcfi("));
}

static QString readerLocationForTarget(const TargetAnchorInfo &target)
{
    if (!target.location.trimmed().isEmpty() && !isCfiLocation(target.location)) {
        return target.location;
    }

    return target.cfiLocation.isEmpty() ? target.location : target.cfiLocation;
}

static QString deliveryQueryValue(const QHttpServerRequest &request, const QStringList &names, const QString &fallback)
{
    const auto query = request.query();
    for (const QString &name : names) {
        const QString value = query.queryItemValue(name).trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }

    return fallback;
}

static QString normalizeResourceMode(QString mode)
{
    mode = mode.trimmed().toLower();
    if (mode == QStringLiteral("include") || mode == QStringLiteral("inline") || mode == QStringLiteral("off") || mode == QStringLiteral("none")) {
        return QStringLiteral("include");
    }
    if (mode == QStringLiteral("outsource") || mode == QStringLiteral("server") || mode == QStringLiteral("external")) {
        return QStringLiteral("outsource");
    }
    return QStringLiteral("auto");
}

static QString normalizeReferencingMode(QString mode)
{
    mode = mode.trimmed().toLower();
    if (mode == QStringLiteral("include")) {
        return QStringLiteral("include");
    }
    if (mode == QStringLiteral("none") || mode == QStringLiteral("off")) {
        return QStringLiteral("none");
    }
    return QStringLiteral("reader");
}

static QVector<EpubReference> epubReferencesFromVariantList(const QVariantList &values)
{
    QVector<EpubReference> references;
    references.reserve(values.size());

    for (const QVariant &value : values) {
        const QVariantMap map = value.toMap();
        EpubReference reference;
        reference.sourceAnchorId = map.value(QStringLiteral("sourceAnchorId")).toString();
        reference.sourceAnchorTitle = map.value(QStringLiteral("sourceAnchorTitle")).toString();
        reference.targetBookId = map.value(QStringLiteral("targetBookId")).toString();
        reference.targetLocation = map.value(QStringLiteral("targetLocation")).toString();
        reference.targetPreviewHtml = map.value(QStringLiteral("targetPreviewHtml")).toString();
        if (!reference.sourceAnchorId.isEmpty()) {
            references.append(reference);
        }
    }

    return references;
}

static QString uuidString(const QUuid &uuid)
{
    return uuid.isNull() ? QString() : uuid.toString(QUuid::WithoutBraces);
}

static QJsonArray anchorValidationIssuesToJson(const QList<BookAnchorValidationIssue> &issues)
{
    QJsonArray array;
    for (const BookAnchorValidationIssue &issue : issues) {
        QJsonObject object;
        object.insert(QStringLiteral("type"), issue.type);
        object.insert(QStringLiteral("bookId"), issue.bookId);
        object.insert(QStringLiteral("anchorId"), issue.anchorId);
        object.insert(QStringLiteral("objectId"), issue.objectId);
        object.insert(QStringLiteral("message"), issue.message);
        array.append(object);
    }
    return array;
}

static void appendUniqueString(QStringList &values, const QString &value)
{
    if (!value.isEmpty() && !values.contains(value)) {
        values.append(value);
    }
}

static QJsonArray stringListToJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values) {
        array.append(value);
    }
    return array;
}

static QString pdfLocationForPage(int page)
{
    QJsonObject location;
    location.insert(QStringLiteral("format"), QStringLiteral("pdf"));
    location.insert(QStringLiteral("page"), page);
    location.insert(QStringLiteral("pageNumber"), page + 1);
    return QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact));
}

static bool isPdfLocation(const QString &location)
{
    const QString trimmedLocation = location.trimmed();
    if (!trimmedLocation.startsWith(QLatin1Char('{'))) {
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(trimmedLocation.toUtf8());
    if (!document.isObject()) {
        return false;
    }

    const QJsonObject object = document.object();
    return object.value(QStringLiteral("format")).toString().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0
        && (object.contains(QStringLiteral("page")) || object.contains(QStringLiteral("pageNumber")));
}

static QString pdfSynopsisAttributeValue(const QDomElement &element, const QString &firstName, const QString &secondName)
{
    if (element.hasAttribute(firstName)) {
        return element.attribute(firstName);
    }
    return element.attribute(secondName);
}

static Okular::DocumentViewport viewportForPdfSynopsisElement(Okular::Document *document, const QDomElement &element)
{
    QString viewportString = pdfSynopsisAttributeValue(element, QStringLiteral("Viewport"), QStringLiteral("Destination"));
    if (viewportString.isEmpty()) {
        const QString viewportName = pdfSynopsisAttributeValue(element, QStringLiteral("ViewportName"), QStringLiteral("DestinationName"));
        if (!viewportName.isEmpty()) {
            viewportString = document->metaData(QStringLiteral("NamedViewport"), viewportName).toString();
        }
    }

    return viewportString.isEmpty() ? Okular::DocumentViewport() : Okular::DocumentViewport(viewportString);
}

static void appendPdfTargetLocation(QJsonArray &targetLocations,
                                    QJsonArray &locationStrings,
                                    const QString &id,
                                    const QString &title,
                                    int page,
                                    int tocDepth,
                                    const QStringList &tocPath)
{
    if (page < 0) {
        return;
    }

    const QString normalizedTitle = title.simplified();
    const QString label = normalizedTitle.isEmpty() ? i18nc("@item:inlistbox PDF page target", "Page %1", page + 1) : normalizedTitle;
    const QString location = pdfLocationForPage(page);

    QJsonObject item;
    item.insert(QStringLiteral("id"), id);
    item.insert(QStringLiteral("location"), location);
    item.insert(QStringLiteral("readerLocation"), location);
    item.insert(QStringLiteral("hrefLocation"), location);
    item.insert(QStringLiteral("cfiLocation"), QString());
    item.insert(QStringLiteral("file"), QString());
    item.insert(QStringLiteral("previewHtml"), label);
    item.insert(QStringLiteral("title"), label);
    item.insert(QStringLiteral("type"), QStringLiteral("navigation"));
    item.insert(QStringLiteral("tocTitle"), label);
    item.insert(QStringLiteral("tocDepth"), tocDepth);
    item.insert(QStringLiteral("tocPath"), stringListToJsonArray(tocPath.isEmpty() ? QStringList{label} : tocPath));

    targetLocations.append(item);
    locationStrings.append(location);
}

static void appendPdfSynopsisTargetLocations(Okular::Document *document,
                                             const QDomNode &parentNode,
                                             int pageCount,
                                             QStringList tocPath,
                                             int tocDepth,
                                             int &idCounter,
                                             QJsonArray &targetLocations,
                                             QJsonArray &locationStrings)
{
    for (QDomNode node = parentNode.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const QDomElement element = node.toElement();
        if (element.isNull()) {
            continue;
        }

        const QString label = element.tagName().simplified();
        QStringList itemPath = tocPath;
        if (!label.isEmpty()) {
            itemPath.append(label);
        }

        const Okular::DocumentViewport viewport = viewportForPdfSynopsisElement(document, element);
        if (viewport.isValid() && viewport.pageNumber >= 0 && viewport.pageNumber < pageCount) {
            appendPdfTargetLocation(targetLocations,
                                    locationStrings,
                                    QStringLiteral("pdf-toc-%1").arg(++idCounter),
                                    label,
                                    viewport.pageNumber,
                                    tocDepth,
                                    itemPath);
        }

        appendPdfSynopsisTargetLocations(document, element, pageCount, itemPath, tocDepth + 1, idCounter, targetLocations, locationStrings);
    }
}

static bool pdfTargetLocationsForFile(const QString &fileName, QJsonArray &targetLocations, QJsonArray &locationStrings)
{
    Arianna::initializeOkularPdfSupport();
    Okular::Document document(nullptr);
    const QFileInfo fileInfo(fileName);
    const QUrl url = QUrl::fromLocalFile(fileInfo.absoluteFilePath());
    const Okular::Document::OpenResult result = Arianna::openOkularPdfDocument(document, fileInfo.absoluteFilePath(), url);
    if (result != Okular::Document::OpenSuccess || !document.isOpened()) {
        return false;
    }

    const int pageCount = static_cast<int>(document.pages());
    const Okular::DocumentSynopsis *synopsis = document.documentSynopsis();
    if (synopsis) {
        int idCounter = 0;
        appendPdfSynopsisTargetLocations(&document, *synopsis, pageCount, QStringList(), 0, idCounter, targetLocations, locationStrings);
    }

    if (targetLocations.isEmpty()) {
        for (int page = 0; page < pageCount; ++page) {
            const QString label = i18nc("@item:inlistbox PDF page target", "Page %1", page + 1);
            appendPdfTargetLocation(targetLocations, locationStrings, QStringLiteral("pdf-page-%1").arg(page + 1), label, page, 0, QStringList{label});
        }
    }

    document.closeDocument();
    return true;
}

static QJsonObject activeFileImportObject(const QString &bookId, const BookCommitResult &result)
{
    QJsonObject object;
    object.insert(QStringLiteral("bookId"), bookId);
    object.insert(QStringLiteral("status"), result.unchanged ? QStringLiteral("unchanged") : QStringLiteral("imported"));
    object.insert(QStringLiteral("message"), result.errorMessage);
    object.insert(QStringLiteral("previousStateId"), uuidString(result.oldStateId));
    object.insert(QStringLiteral("newStateId"), uuidString(result.newStateId));
    object.insert(QStringLiteral("textContentHash"), QString::fromLatin1(result.textContentHash));
    object.insert(QStringLiteral("documentStateHash"), QString::fromLatin1(result.documentStateHash));
    object.insert(QStringLiteral("epubFileHash"), QString::fromLatin1(result.epubFileHash));
    object.insert(QStringLiteral("anchorValidationIssues"), anchorValidationIssuesToJson(result.anchorValidationIssues));
    return object;
}

static QHttpServerResponse activeFileImportConflictResponse(const QString &bookId, const BookCommitResult &result)
{
    QJsonObject object = activeFileImportObject(bookId, result);
    object.insert(QStringLiteral("status"), QStringLiteral("anchor-validation-failed"));
    return QHttpServerResponse(QByteArrayLiteral("application/json"),
                               QJsonDocument(object).toJson(QJsonDocument::Compact),
                               QHttpServerResponder::StatusCode::Conflict);
}

static void refreshBookStateAfterFileMutation(const QStringList &bookIds)
{
    BookTruthStore truthStore;
    for (const QString &bookId : bookIds) {
        if (bookId.trimmed().isEmpty()) {
            continue;
        }
        const BookCommitResult result = truthStore.commitActiveFileChangeIfNeeded(bookId, true);
        if (!result.success) {
            qWarning() << "Unable to refresh EPUB state after file mutation:" << bookId << result.errorMessage;
        }
    }
}

static QStringList targetAnchorCandidateIds(const QVariantMap &reference)
{
    QStringList candidates;
    auto append = [&candidates](QString value) {
        value = value.trimmed();
        if (value.isEmpty() || candidates.contains(value)) {
            return;
        }

        candidates.append(value);
    };

    const QString targetLocation = reference.value(QStringLiteral("targetLocation")).toString().trimmed();
    if (!targetLocation.contains(QLatin1Char('/')) && !targetLocation.contains(QLatin1Char('#')) && !targetLocation.startsWith(QStringLiteral("epubcfi("))) {
        append(targetLocation);
    }
    append(fragmentFromLocation(targetLocation));
    return candidates;
}

static bool isSameReference(const QVariantMap &reference, const QVariantMap &other)
{
    return reference.value(QStringLiteral("sourceBookId")).toString() == other.value(QStringLiteral("sourceBookId")).toString()
        && reference.value(QStringLiteral("sourceAnchorId")).toString() == other.value(QStringLiteral("sourceAnchorId")).toString();
}

static bool referenceTargetsAnchor(const QVariantMap &reference, const QString &bookId, const QString &anchorId)
{
    if (reference.value(QStringLiteral("targetBookId")).toString() != bookId) {
        return false;
    }

    const QString targetLocation = reference.value(QStringLiteral("targetLocation")).toString().trimmed();
    return targetLocation == anchorId || fragmentFromLocation(targetLocation) == anchorId;
}

static bool annotationUsesAnchor(const QVariantMap &annotation, const QString &anchorId)
{
    const QString annotationAnchorId = annotation.value(QStringLiteral("anchorId")).toString().trimmed();
    return annotationAnchorId == anchorId || annotationAnchorId + QStringLiteral("_begin") == anchorId
        || annotationAnchorId + QStringLiteral("_end") == anchorId;
}

static bool anchorHasOtherDatabaseUsers(const QString &bookId, const QString &anchorId, const QVariantMap &deletedReference)
{
    if (bookId.isEmpty() || anchorId.isEmpty()) {
        return true;
    }

    for (const QVariant &referenceValue : BookDatabase::self().loadReferencesTargeting(bookId)) {
        const QVariantMap reference = referenceValue.toMap();
        if (isSameReference(reference, deletedReference)) {
            continue;
        }
        if (referenceTargetsAnchor(reference, bookId, anchorId)) {
            return true;
        }
    }

    for (const QVariant &referenceValue : BookDatabase::self().loadReferences(bookId)) {
        const QVariantMap reference = referenceValue.toMap();
        if (isSameReference(reference, deletedReference)) {
            continue;
        }
        if (reference.value(QStringLiteral("sourceAnchorId")).toString().trimmed() == anchorId) {
            return true;
        }
    }

    for (const QVariant &annotationValue : BookDatabase::self().loadAnnotations(bookId)) {
        if (annotationUsesAnchor(annotationValue.toMap(), anchorId)) {
            return true;
        }
    }

    return false;
}

static bool isServerServedMimeType(const QByteArray &mime)
{
    // Nur große/problemlose Streaming-Medien externalisieren

    return mime == QByteArrayLiteral("video/mp4") || mime == QByteArrayLiteral("audio/mpeg") || mime == QByteArrayLiteral("audio/ogg")
        || mime == QByteArrayLiteral("audio/opus") || mime == QByteArrayLiteral("audio/mp4") || mime == QByteArrayLiteral("audio/webm")
        || mime == QByteArrayLiteral("video/webm");
}

static QSet<QString> makeServedFiles(const std::shared_ptr<EPubContainer> &container)
{
    QSet<QString> servedFiles;

    if (!container) {
        return servedFiles;
    }

    const QHash<QString, EpubItem> &manifest = container->manifestItems();

    for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
        const EpubItem &item = it.value();

        if (isServerServedMimeType(item.mimetype)) {
            servedFiles.insert(QDir::cleanPath(item.path));
        }
        if (item.mimetype == QByteArrayLiteral("video/mp4")) {
            const QString mp4Path = QDir::cleanPath(item.path);

            servedFiles.insert(mp4Path);

            QFileInfo fi(mp4Path);

            QString syncPath = fi.completeBaseName() + QStringLiteral("-sync.json");
            qDebug() << "Looking for sync file:" << syncPath;

            // oder je nach Schema:
            // karna-amrita_1.mp4
            // -> karna-amrita_1-sync.json

            for (auto jt = manifest.constBegin(); jt != manifest.constEnd(); ++jt) {
                const EpubItem &other = jt.value();

                if (QDir::cleanPath(other.path) == syncPath) {
                    servedFiles.insert(syncPath);

                    qDebug() << "Adding sync file:" << syncPath;

                    break;
                }
            }
        }
    }

    return servedFiles;
}

static bool shouldStripPath(const QString &path)
{
    const QString lower = path.toLower();

    return lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg")) || lower.endsWith(QStringLiteral(".png"))
        || lower.endsWith(QStringLiteral(".webp")) || lower.endsWith(QStringLiteral(".gif")) || lower.endsWith(QStringLiteral(".mp3"))
        || lower.endsWith(QStringLiteral(".ogg")) || lower.endsWith(QStringLiteral(".opus")) || lower.endsWith(QStringLiteral(".m4a"))
        || lower.endsWith(QStringLiteral(".mp4")) || lower.endsWith(QStringLiteral(".webm"));
}

static void copyDirectoryStripped(KZip &outZip, const KArchiveDirectory *dir, const QString &prefix, QSet<QString> *strippedFiles)
{
    const QStringList entries = dir->entries();

    for (const QString &entryName : entries) {
        const KArchiveEntry *entry = dir->entry(entryName);

        if (!entry) {
            continue;
        }

        const QString fullPath = prefix.isEmpty() ? entryName : prefix + QStringLiteral("/") + entryName;
        if (entry->isDirectory()) {
            auto subdir = dynamic_cast<const KArchiveDirectory *>(entry);

            if (subdir) {
                copyDirectoryStripped(outZip, subdir, fullPath, strippedFiles);
            }

            continue;
        }

        auto file = dynamic_cast<const KArchiveFile *>(entry);

        if (!file) {
            continue;
        }

        if (shouldStripPath(fullPath)) {
            if (strippedFiles) {
                strippedFiles->insert(QDir::cleanPath(fullPath));
            }

            qDebug() << "Stripping:" << fullPath;

            continue;
        }

        QScopedPointer<QIODevice> dev(file->createDevice());

        if (!dev) {
            continue;
        }

        outZip.writeFile(fullPath, dev->readAll());
    }
}
static QString makeResourceUrl(const QString &identifier, const QString &resourceUuid)
{
    return BookServerConfig::baseUrl() + QStringLiteral("/") + QString::fromUtf8(QUrl::toPercentEncoding(identifier)) + QStringLiteral("/res/")
        + QString::fromUtf8(QUrl::toPercentEncoding(resourceUuid));
}

static QString requestHeaderValue(const QHttpServerRequest &request, const QByteArray &name)
{
    const QByteArray exactValue = request.value(name);
    if (!exactValue.isEmpty()) {
        return QString::fromUtf8(exactValue);
    }

    for (const auto &[headerName, headerValue] : request.headers().toListOfPairs()) {
        if (headerName.compare(name, Qt::CaseInsensitive) == 0) {
            return QString::fromUtf8(headerValue);
        }
    }

    return {};
}

static QString requestToken(const QHttpServerRequest &request)
{
    const QString headerToken = requestHeaderValue(request, QByteArrayLiteral("X-Arianna-Session-Token"));
    if (!headerToken.isEmpty()) {
        return headerToken;
    }

    return request.query().queryItemValue(QStringLiteral("token"));
}

static bool isAllowedCorsOrigin(const QByteArray &origin)
{
    return origin == QByteArrayLiteral("qrc:") || origin == QByteArrayLiteral("foliate://") || origin == QByteArrayLiteral("foliate:");
}

static void addCorsHeaders(const QHttpServerRequest &request, QHttpServerResponse &response)
{
    const QByteArray origin = request.value("Origin");
    if (!isAllowedCorsOrigin(origin)) {
        return;
    }

    auto headers = response.headers();
    headers.replaceOrAppend("Access-Control-Allow-Origin", origin);
    headers.replaceOrAppend("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    headers.replaceOrAppend("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
    headers.replaceOrAppend("Access-Control-Expose-Headers",
                            "Accept-Ranges, Content-Length, Content-Range, X-Arianna-Resource-Mode, X-Arianna-Referencing-Mode");
    headers.replaceOrAppend("Vary", "Origin");
    response.setHeaders(headers);
}

static QHttpServerResponse corsPreflightResponse(const QHttpServerRequest &request)
{
    Q_UNUSED(request)
    QHttpServerResponse response(QHttpServerResponder::StatusCode::NoContent);
    return response;
}

static void addBookResponseHeaders(QHttpServerResponse &response, qint64 contentLength, const QByteArray &inlineFilename = QByteArrayLiteral("book.epub"))
{
    auto headers = response.headers();
    headers.replaceOrAppend("Content-Disposition", QByteArrayLiteral("inline; filename=\"") + inlineFilename + QByteArrayLiteral("\""));
    if (contentLength >= 0) {
        headers.replaceOrAppend("Content-Length", QByteArray::number(contentLength));
    }
    headers.replaceOrAppend("Cache-Control", "no-store");
    response.setHeaders(headers);
}

static void addBookDeliveryHeaders(QHttpServerResponse &response, const QString &resourceMode, const QString &referencingMode)
{
    auto headers = response.headers();
    headers.replaceOrAppend("X-Arianna-Resource-Mode", resourceMode.toUtf8());
    headers.replaceOrAppend("X-Arianna-Referencing-Mode", referencingMode.toUtf8());
    response.setHeaders(headers);
}

static QHttpServerResponse bookFileResponse(const QString &filename, const QByteArray &inlineFilename = QByteArrayLiteral("book.epub"))
{
    QHttpServerResponse response = QHttpServerResponse::fromFile(filename);
    addBookResponseHeaders(response, QFileInfo(filename).size(), inlineFilename);
    return response;
}

static QString pdfWatermarkFilterMode()
{
    const QString mode = Config::pdfWatermarkFilterMode();
    if (mode == QStringLiteral("permanent") || mode == QStringLiteral("temporary")) {
        return mode;
    }
    if (Config::pdfWatermarkFilterEnabled()) {
        return QStringLiteral("temporary");
    }
    return QStringLiteral("never");
}

static QHttpServerResponse pdfFileResponse(const QString &filename)
{
    if (pdfWatermarkFilterMode() != QStringLiteral("temporary")) {
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    const QString pattern = Config::pdfWatermarkFilterPattern().trimmed();
    if (pattern.isEmpty()) {
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    QTemporaryFile filteredFile(QDir::temp().filePath(QStringLiteral("arianna-server-pdf-XXXXXX.pdf")));
    filteredFile.setAutoRemove(true);
    if (!filteredFile.open()) {
        qWarning() << "Unable to create temporary server PDF for watermark filtering:" << filename;
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    const QString filteredFileName = filteredFile.fileName();
    filteredFile.close();
    if (!QFile::remove(filteredFileName)) {
        qWarning() << "Unable to prepare temporary server PDF for watermark filtering:" << filteredFileName;
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    const PdfWatermarkFilterResult filterResult = PdfWatermarkFilter::filterFile(filename, filteredFileName, pattern);
    if (!filterResult.filtered) {
        QFile::remove(filteredFileName);
        if (!filterResult.errorString.isEmpty()) {
            qWarning() << "Server PDF watermark filter not applied:" << filterResult.errorString;
        } else {
            qDebug() << "Server PDF watermark marker not found:" << filename;
        }
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    QFile filteredPdf(filteredFileName);
    if (!filteredPdf.open(QIODevice::ReadOnly)) {
        QFile::remove(filteredFileName);
        qWarning() << "Unable to read temporary server PDF after watermark filtering:" << filteredFileName;
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    const QByteArray data = filteredPdf.readAll();
    filteredPdf.close();
    QFile::remove(filteredFileName);
    if (data.isEmpty()) {
        qWarning() << "Temporary server PDF after watermark filtering is empty:" << filteredFileName;
        return bookFileResponse(filename, QByteArrayLiteral("book.pdf"));
    }

    qDebug() << "Server PDF watermark filter removed" << filterResult.occurrences << "object(s):" << filename;
    QHttpServerResponse response(QByteArrayLiteral("application/pdf"), data);
    addBookResponseHeaders(response, data.size(), QByteArrayLiteral("book.pdf"));
    return response;
}

void BookServer::clearServedResourcesForIdentifier(const QString &identifier)
{
    for (auto it = m_resourceByUuid.begin(); it != m_resourceByUuid.end();) {
        if (it.value().identifier == identifier) {
            it = m_resourceByUuid.erase(it);
        } else {
            ++it;
        }
    }

    m_servedFilesByIdentifier.remove(identifier);
    m_resourceMapByIdentifier.remove(identifier);
}

void BookServer::clearServedResourcesForSessionIdentifier(const QString &sessionToken, const QString &identifier)
{
    for (auto it = m_resourceByUuid.begin(); it != m_resourceByUuid.end();) {
        const ServedResource &served = it.value();
        if (served.identifier == identifier && served.sessionToken == sessionToken) {
            it = m_resourceByUuid.erase(it);
        } else {
            ++it;
        }
    }
}

void BookServer::registerReaderSessionForIdentifier(const QString &sessionToken, const QString &identifier)
{
    if (sessionToken.isEmpty() || identifier.isEmpty() || !m_readerSessionRefCount.contains(sessionToken)) {
        return;
    }

    m_identifiersByReaderSession[sessionToken].insert(identifier);
    m_readerSessionsByIdentifier[identifier].insert(sessionToken);
}

void BookServer::releaseReaderSessionResources(const QString &sessionToken)
{
    const QSet<QString> identifiers = m_identifiersByReaderSession.take(sessionToken);

    for (const QString &identifier : identifiers) {
        clearServedResourcesForSessionIdentifier(sessionToken, identifier);

        auto sessionsIt = m_readerSessionsByIdentifier.find(identifier);
        if (sessionsIt == m_readerSessionsByIdentifier.end()) {
            continue;
        }

        sessionsIt.value().remove(sessionToken);
        if (!sessionsIt.value().isEmpty()) {
            continue;
        }

        m_readerSessionsByIdentifier.erase(sessionsIt);
        clearServedResourcesForIdentifier(identifier);
        m_containerCache.remove(identifier);
        m_readOnlyFilesByIdentifier.remove(identifier);

        qDebug() << "BookServer cache removed for inactive identifier:" << identifier;
        AriannaTrace::event(QStringLiteral("bookserver.container.cache_removed"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("reason"), QStringLiteral("inactive-reader")},
                             {QStringLiteral("reader"), AriannaTrace::shortId(sessionToken)},
                             {QStringLiteral("cachedBooks"), m_containerCache.size()}});
    }
}

std::shared_ptr<EPubContainer> BookServer::containerForIdentifier(const QString &identifier, const QString &cacheVersion)
{
    QString bookFileName = m_readOnlyFilesByIdentifier.value(identifier);
    if (bookFileName.isEmpty()) {
        auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(identifier);

        if (!entry) {
            qWarning() << "Kein BookEntry für Identifier:" << identifier;
            AriannaTrace::event(QStringLiteral("bookserver.container.missing_entry"), {{QStringLiteral("bookId"), identifier}});
            m_containerCache.remove(identifier);
            clearServedResourcesForIdentifier(identifier);
            return {};
        }

        bookFileName = entry->filename;
    }

    const QFileInfo fileInfo(bookFileName);

    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "EPUB-Datei existiert nicht mehr:" << bookFileName;
        AriannaTrace::event(QStringLiteral("bookserver.container.missing_file"),
                            {{QStringLiteral("bookId"), identifier}, {QStringLiteral("file"), bookFileName}});
        m_containerCache.remove(identifier);
        clearServedResourcesForIdentifier(identifier);
        m_readOnlyFilesByIdentifier.remove(identifier);
        return {};
    }

    const QString filename = fileInfo.absoluteFilePath();
    const QDateTime lastModified = fileInfo.lastModified();
    const qint64 size = fileInfo.size();

    const auto cachedIt = m_containerCache.constFind(identifier);
    if (cachedIt != m_containerCache.constEnd()) {
        const CachedContainer &cached = cachedIt.value();
        const bool cacheVersionMatches = cacheVersion.isEmpty() || cached.cacheVersion == cacheVersion;
        if (cached.container && cached.filename == filename && cached.lastModified == lastModified && cached.size == size && cacheVersionMatches) {
            qDebug() << "Container aus Cache:" << identifier;
            AriannaTrace::event(QStringLiteral("bookserver.container.cache_hit"),
                                {{QStringLiteral("bookId"), identifier},
                                 {QStringLiteral("file"), filename},
                                 {QStringLiteral("cacheVersion"), cached.cacheVersion},
                                 {QStringLiteral("cachedBooks"), m_containerCache.size()}});
            return cached.container;
        }

        AriannaTrace::event(QStringLiteral("bookserver.container.cache_stale"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("file"), filename},
                             {QStringLiteral("oldMTime"), cached.lastModified.toString(Qt::ISODateWithMs)},
                             {QStringLiteral("newMTime"), lastModified.toString(Qt::ISODateWithMs)},
                             {QStringLiteral("oldSize"), cached.size},
                             {QStringLiteral("newSize"), size},
                             {QStringLiteral("oldCacheVersion"), cached.cacheVersion},
                             {QStringLiteral("newCacheVersion"), cacheVersion}});
        QDebug debug = qDebug() << "Container cache stale:" << identifier << filename << "old mtime:" << cached.lastModified << "new mtime:" << lastModified
                                << "old size:" << cached.size << "new size:" << size;
        if (!cached.cacheVersion.isEmpty() || !cacheVersion.isEmpty()) {
            debug << "old cache version:" << cached.cacheVersion << "new cache version:" << cacheVersion;
        }
        m_containerCache.remove(identifier);
        clearServedResourcesForIdentifier(identifier);
    }

    auto container = std::make_shared<EPubContainer>(nullptr);

    if (!container->openFile(filename)) {
        qWarning() << "EPUB konnte nicht geöffnet werden:" << filename;
        AriannaTrace::event(QStringLiteral("bookserver.container.open_failed"), {{QStringLiteral("bookId"), identifier}, {QStringLiteral("file"), filename}});
        return {};
    }

    m_containerCache.insert(identifier, CachedContainer{container, filename, cacheVersion, lastModified, size});
    AriannaTrace::event(QStringLiteral("bookserver.container.cache_put"),
                        {{QStringLiteral("bookId"), identifier},
                         {QStringLiteral("file"), filename},
                         {QStringLiteral("mtime"), lastModified.toString(Qt::ISODateWithMs)},
                         {QStringLiteral("size"), size},
                         {QStringLiteral("cacheVersion"), cacheVersion},
                         {QStringLiteral("cachedBooks"), m_containerCache.size()}});

    QDebug debug = qDebug() << "Container aus DB geladen:" << identifier << filename << "mtime:" << lastModified << "size:" << size;
    if (!cacheVersion.isEmpty()) {
        debug << "cache version:" << cacheVersion;
    }

    return container;
}

QString BookServer::pdfFileForIdentifier(const QString &identifier) const
{
    QString bookFileName = m_readOnlyFilesByIdentifier.value(identifier);
    if (bookFileName.isEmpty()) {
        const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(identifier);
        if (!entry) {
            qWarning() << "No BookEntry for PDF identifier:" << identifier;
            return {};
        }
        bookFileName = entry->filename;
    }

    const QFileInfo fileInfo(bookFileName);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "PDF file no longer exists:" << bookFileName;
        return {};
    }

    QMimeDatabase db;
    if (db.mimeTypeForFile(fileInfo).name() != QStringLiteral("application/pdf")) {
        qWarning() << "Requested PDF identifier does not point to a PDF:" << identifier << bookFileName;
        return {};
    }

    return fileInfo.absoluteFilePath();
}

static QString serverTokenFromRequest(const QHttpServerRequest &request)
{
    const QString headerToken = requestHeaderValue(request, QByteArrayLiteral("X-Arianna-Server-Token"));
    if (!headerToken.isEmpty()) {
        return headerToken;
    }

    return request.query().queryItemValue(QStringLiteral("serverToken"));
}

auto serveStaticFile = [](QString filePath) -> QHttpServerResponse {
    if (filePath.isEmpty())
        return {QHttpServerResponder::StatusCode::NotFound};

    const QUrl url(filePath);
    if (url.isLocalFile())
        filePath = url.toLocalFile();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {QHttpServerResponder::StatusCode::NotFound};

    const QByteArray data = file.readAll();

    QMimeDatabase mimeDatabase;
    const QByteArray mimeType = mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchExtension).name().toUtf8();

    QHttpServerResponse response(mimeType.isEmpty() ? QByteArrayLiteral("application/octet-stream") : mimeType, data);

    auto headers = response.headers();
    headers.append("Cache-Control", "no-store");
    response.setHeaders(headers);
    qDebug() << "Serving static file:" << filePath << "mime type:" << mimeType;
    return response;
};

BookServer::BookServer(const QString &serverToken, bool quitWhenUnused)
    : m_serverToken(serverToken)
    , m_quitWhenUnused(quitWhenUnused)
{
    addSessionToken(m_serverToken);

    server.route(QStringLiteral("/static/background-image"), [](const QHttpServerRequest &request) {
        Q_UNUSED(request)
        return serveStaticFile(Config::readerBackgroundPath());
    });

    server.route(QStringLiteral("/static/book-icon"), [](const QHttpServerRequest &) {
        qDebug() << "Serving book icon";
        return serveStaticFile(QStringLiteral(":/qt/qml/org/kde/arianna/qml/icons/book.svg"));
    });

    server.route(QStringLiteral("/session"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/session"), QHttpServerRequest::Method::Post, [this](const QHttpServerRequest &request) {
        qDebug() << "Request for session token" << serverTokenFromRequest(request);
        if (serverTokenFromRequest(request) != m_serverToken) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const QString sessionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
        registerReaderSession(sessionToken);

        QJsonObject obj;
        obj.insert(QStringLiteral("sessionToken"), sessionToken);
        obj.insert(QStringLiteral("headerName"), QStringLiteral("X-Arianna-Session-Token"));
        obj.insert(QStringLiteral("registeredReaders"), registeredReaderCount());

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.route(QStringLiteral("/session"), QHttpServerRequest::Method::Delete, [this](const QHttpServerRequest &request) {
        const QString sessionToken = requestToken(request);

        if (!isValidToken(sessionToken)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        if (!unregisterReaderSession(sessionToken)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const int remainingReaders = registeredReaderCount();
        const bool serverStopping = remainingReaders == 0;

        QJsonObject obj;
        obj.insert(QStringLiteral("registeredReaders"), remainingReaders);
        obj.insert(QStringLiteral("serverStopping"), serverStopping);

        if (serverStopping) {
            scheduleStopIfUnused();
        }

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.route(QStringLiteral("/read-only-books"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/read-only-books"), QHttpServerRequest::Method::Post, [this](const QHttpServerRequest &request) {
        const QString sessionToken = requestToken(request);

        if (!m_readerSessionRefCount.contains(sessionToken)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const QJsonDocument document = QJsonDocument::fromJson(request.body());
        const QString fileName = document.object().value(QStringLiteral("fileName")).toString();
        const QFileInfo fileInfo(fileName);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const QString identifier = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_readOnlyFilesByIdentifier.insert(identifier, fileInfo.absoluteFilePath());
        registerReaderSessionForIdentifier(sessionToken, identifier);

        QJsonObject obj;
        obj.insert(QStringLiteral("identifier"), identifier);

        qDebug() << "BookServer read-only book registered:" << identifier << fileInfo.absoluteFilePath();
        AriannaTrace::event(QStringLiteral("bookserver.readonly_book.registered"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("file"), fileInfo.absoluteFilePath()},
                             {QStringLiteral("reader"), AriannaTrace::shortId(sessionToken)}});

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    /* Route for EPUB files */
    server.route(QStringLiteral("/<arg>/book.epub"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/book.epub"), [this](const QString &identifier, const QHttpServerRequest &request) {
        QElapsedTimer deliveryTimer;
        deliveryTimer.start();
        qDebug() << "Request for book URL:" << request.url() << "request token" << requestToken(request);
        const QString token = requestToken(request);
        const QString requestedResourceMode = normalizeResourceMode(
            deliveryQueryValue(request,
                               {QStringLiteral("resourceMode"), QStringLiteral("resourceOutsourcing"), QStringLiteral("resource-outsourcing")},
                               QStringLiteral("auto")));
        const QString referencingMode = normalizeReferencingMode(
            deliveryQueryValue(request,
                               {QStringLiteral("referencingMode"), QStringLiteral("referenceMode"), QStringLiteral("referencing-mode")},
                               QStringLiteral("reader")));
        const QString deliveryVersion = deliveryQueryValue(request, {QStringLiteral("v"), QStringLiteral("version")}, QString());

        AriannaTrace::event(QStringLiteral("bookserver.book.request"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("url"), request.url().toString()},
                             {QStringLiteral("reader"), AriannaTrace::shortId(token)},
                             {QStringLiteral("resourceMode"), requestedResourceMode},
                             {QStringLiteral("referencingMode"), referencingMode},
                             {QStringLiteral("version"), deliveryVersion}});

        if (!isValidToken(token)) {
            AriannaTrace::event(QStringLiteral("bookserver.book.rejected"),
                                {{QStringLiteral("bookId"), identifier}, {QStringLiteral("reason"), QStringLiteral("unauthorized")}});
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const bool readerSessionToken = m_readerSessionRefCount.contains(token);
        const QString resourceSessionToken = readerSessionToken ? token : QString();
        if (readerSessionToken) {
            registerReaderSessionForIdentifier(token, identifier);
        }

        QString effectiveDeliveryVersion = deliveryVersion;
        if (!m_readOnlyFilesByIdentifier.contains(identifier)) {
            BookTruthStore truthStore;
            const BookCommitResult activeFileImport = truthStore.commitActiveFileChangeIfNeeded(identifier);
            if (!activeFileImport.success) {
                if (activeFileImport.validationFailed) {
                    return activeFileImportConflictResponse(identifier, activeFileImport);
                }

                qWarning() << "Unable to import externally changed active EPUB before delivery:" << identifier << activeFileImport.errorMessage;
                return QHttpServerResponse{activeFileImport.invalidCandidate ? QHttpServerResponder::StatusCode::BadRequest
                                                                             : QHttpServerResponder::StatusCode::InternalServerError};
            }

            if (!activeFileImport.unchanged) {
                m_containerCache.remove(identifier);
                clearServedResourcesForIdentifier(identifier);
                effectiveDeliveryVersion = uuidString(activeFileImport.newStateId);
                qDebug() << "Imported externally changed active EPUB before delivery:" << identifier << effectiveDeliveryVersion;
            }
        }

        auto container = containerForIdentifier(identifier, effectiveDeliveryVersion);
        if (!container) {
            AriannaTrace::event(QStringLiteral("bookserver.book.rejected"),
                                {{QStringLiteral("bookId"), identifier}, {QStringLiteral("reason"), QStringLiteral("container-not-found")}});
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }
        const bool outsourceResources = requestedResourceMode != QStringLiteral("include");
        const bool includeReferences = referencingMode == QStringLiteral("include");
        const QSet<QString> servedFiles = outsourceResources ? makeServedFiles(container) : QSet<QString>{};
        const QString effectiveResourceMode = servedFiles.isEmpty() ? QStringLiteral("include") : QStringLiteral("outsource");
        if (readerSessionToken) {
            clearServedResourcesForSessionIdentifier(token, identifier);
        } else {
            clearServedResourcesForIdentifier(identifier);
        }
        if (servedFiles.isEmpty() && !includeReferences && requestedResourceMode != QStringLiteral("outsource")) {
            m_containerCache.remove(identifier);
            qDebug() << "Serving original EPUB directly:" << container->filename();
            AriannaTrace::event(QStringLiteral("bookserver.book.served"),
                                {{QStringLiteral("bookId"), identifier},
                                 {QStringLiteral("file"), container->filename()},
                                 {QStringLiteral("resourceMode"), requestedResourceMode},
                                 {QStringLiteral("effectiveResourceMode"), effectiveResourceMode},
                                 {QStringLiteral("referencingMode"), referencingMode},
                                 {QStringLiteral("version"), effectiveDeliveryVersion},
                                 {QStringLiteral("bytes"), QFileInfo(container->filename()).size()},
                                 {QStringLiteral("resources"), 0},
                                 {QStringLiteral("references"), 0},
                                 {QStringLiteral("cachedBooks"), m_containerCache.size()},
                                 {QStringLiteral("directOriginal"), true},
                                 {QStringLiteral("durationMs"), deliveryTimer.elapsed()}});
            QHttpServerResponse response = bookFileResponse(container->filename());
            addBookDeliveryHeaders(response, effectiveResourceMode, referencingMode);
            return response;
        }

        m_servedFilesByIdentifier[identifier] = servedFiles;
        const ResourceMap resourceMap = makeResourceMap(servedFiles, identifier, resourceSessionToken);
        ServerReadyEpubOptions deliveryOptions;
        deliveryOptions.resourceMap = resourceMap;
        deliveryOptions.includeReferences = includeReferences;
        if (includeReferences) {
            ReferenceStore referenceStore;
            deliveryOptions.references = epubReferencesFromVariantList(referenceStore.loadReferences(identifier));
        }
        const QByteArray data = container->createServerReadyEpub(deliveryOptions);

        qDebug() << "Served files map for identifier:" << identifier << resourceMap << "referencing mode:" << referencingMode;

        if (data.isEmpty()) {
            AriannaTrace::event(QStringLiteral("bookserver.book.failed"),
                                {{QStringLiteral("bookId"), identifier},
                                 {QStringLiteral("reason"), QStringLiteral("server-ready-empty")},
                                 {QStringLiteral("durationMs"), deliveryTimer.elapsed()}});
            return QHttpServerResponse{QHttpServerResponder::StatusCode::InternalServerError};
        }

        QHttpServerResponse response(QByteArrayLiteral("application/epub+zip"), data);
        addBookResponseHeaders(response, data.size());
        addBookDeliveryHeaders(response, effectiveResourceMode, referencingMode);

        AriannaTrace::event(QStringLiteral("bookserver.book.served"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("file"), container->filename()},
                             {QStringLiteral("resourceMode"), requestedResourceMode},
                             {QStringLiteral("effectiveResourceMode"), effectiveResourceMode},
                             {QStringLiteral("referencingMode"), referencingMode},
                             {QStringLiteral("version"), effectiveDeliveryVersion},
                             {QStringLiteral("bytes"), data.size()},
                             {QStringLiteral("resources"), resourceMap.size()},
                             {QStringLiteral("references"), deliveryOptions.references.size()},
                             {QStringLiteral("cachedBooks"), m_containerCache.size()},
                             {QStringLiteral("directOriginal"), false},
                             {QStringLiteral("durationMs"), deliveryTimer.elapsed()}});
        return response;
    });

    /* Route for PDF files */
    server.route(QStringLiteral("/<arg>/book.pdf"), QHttpServerRequest::Method::Options, [](const QString & /*identifier*/, const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/<arg>/book.pdf"), [this](const QString &identifier, const QHttpServerRequest &request) {
        qDebug() << "Request for PDF book URL:" << request.url() << "request token" << requestToken(request);
        const QString token = requestToken(request);
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        if (m_readerSessionRefCount.contains(token)) {
            registerReaderSessionForIdentifier(token, identifier);
        }

        const QString pdfFileName = pdfFileForIdentifier(identifier);
        if (pdfFileName.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        QHttpServerResponse response = pdfFileResponse(pdfFileName);
        addBookDeliveryHeaders(response, QStringLiteral("include"), QStringLiteral("reader"));
        return response;
    });

    server.route(QStringLiteral("/<arg>/state/import-active-file"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/state/import-active-file"),
                 QHttpServerRequest::Method::Post,
                 [this](const QString &identifier, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }
                     if (m_readOnlyFilesByIdentifier.contains(identifier)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     const QJsonDocument document = QJsonDocument::fromJson(request.body());
                     const bool allowInvalidAnchors = document.object().value(QStringLiteral("allowInvalidAnchors")).toBool(false)
                         || request.query().queryItemValue(QStringLiteral("allowInvalidAnchors")).compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;

                     BookTruthStore truthStore;
                     const BookCommitResult result = truthStore.commitActiveFileChangeIfNeeded(identifier, allowInvalidAnchors);
                     if (!result.success) {
                         if (result.validationFailed) {
                             return activeFileImportConflictResponse(identifier, result);
                         }

                         qWarning() << "Unable to import active EPUB state:" << identifier << result.errorMessage;
                         return QHttpServerResponse{result.invalidCandidate ? QHttpServerResponder::StatusCode::BadRequest
                                                                            : QHttpServerResponder::StatusCode::InternalServerError};
                     }

                     if (!result.unchanged) {
                         m_containerCache.remove(identifier);
                         clearServedResourcesForIdentifier(identifier);
                     }

                     const QJsonObject root = activeFileImportObject(identifier, result);
                     return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
                 });

    server.route(QStringLiteral("/<arg>/image/not-inverse"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/image/not-inverse"),
                 QHttpServerRequest::Method::Post,
                 [this](const QString &identifier, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }
                     if (m_readOnlyFilesByIdentifier.contains(identifier)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     const QJsonDocument document = QJsonDocument::fromJson(request.body());
                     const QJsonObject object = document.object();
                     const QString cfi = object.value(QStringLiteral("cfi")).toString().trimmed();
                     const QString src = object.value(QStringLiteral("src")).toString().trimmed();
                     if (cfi.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     const auto container = containerForIdentifier(identifier);
                     if (!container) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     bool changed = false;
                     if (!container->setImageNotInverse(cfi, src, &changed)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     QStringList modifiedBookIds;
                     if (changed) {
                         m_containerCache.remove(identifier);
                         clearServedResourcesForIdentifier(identifier);
                         appendUniqueString(modifiedBookIds, identifier);
                         refreshBookStateAfterFileMutation(modifiedBookIds);
                     }

                     QJsonObject root;
                     root.insert(QStringLiteral("bookId"), identifier);
                     root.insert(QStringLiteral("status"), changed ? QStringLiteral("updated") : QStringLiteral("unchanged"));
                     root.insert(QStringLiteral("changed"), changed);
                     root.insert(QStringLiteral("modifiedBookIds"), stringListToJsonArray(modifiedBookIds));
                     return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
                 });

    server.route(QStringLiteral("/<arg>/refs"), QHttpServerRequest::Method::Options, [](const QString & /*identifier*/, const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/<arg>/refs"), QHttpServerRequest::Method::Get, [this](const QString &identifier, const QHttpServerRequest &request) {
        const QString token = requestToken(request);
        qDebug() << "Processing request to list references for book:" << identifier << "with token:" << token;
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        ReferenceStore referenceStore;
        const QVariantList refs = referenceStore.loadReferences(identifier);

        QJsonObject root;
        root.insert(QStringLiteral("bookId"), identifier);
        root.insert(QStringLiteral("refs"), QJsonArray::fromVariantList(refs));

        QJsonDocument doc(root);
        QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
        QHttpHeaders headers;
        headers.append("Cache-Control", "no-cache");
        response.setHeaders(headers);
        return response;
    });

    server.route(QStringLiteral("/<arg>/targetLocations"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/targetLocations"),
                 QHttpServerRequest::Method::Get,
                 [this](const QString &identifier, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request to list target locations for book:" << identifier << "with token:" << token;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }

                     QString bookFileName = m_readOnlyFilesByIdentifier.value(identifier);
                     if (bookFileName.isEmpty()) {
                         const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(identifier);
                         if (entry) {
                             bookFileName = entry->filename;
                         }
                     }
                     if (!bookFileName.isEmpty()) {
                         const QFileInfo fileInfo(bookFileName);
                         QMimeDatabase db;
                         if (fileInfo.exists() && fileInfo.isFile() && db.mimeTypeForFile(fileInfo).name() == QStringLiteral("application/pdf")) {
                             QJsonArray targetLocations;
                             QJsonArray locationStrings;
                             if (!pdfTargetLocationsForFile(fileInfo.absoluteFilePath(), targetLocations, locationStrings)) {
                                 return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                             }

                             QJsonObject root;
                             root.insert(QStringLiteral("bookId"), identifier);
                             root.insert(QStringLiteral("targetLocations"), targetLocations);
                             root.insert(QStringLiteral("locations"), locationStrings);

                             QJsonDocument doc(root);
                             QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
                             QHttpHeaders headers;
                             headers.append("Cache-Control", "no-cache");
                             response.setHeaders(headers);
                             return response;
                         }
                     }

                     const auto container = containerForIdentifier(identifier);
                     if (!container) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     QJsonArray targetLocations;
                     QJsonArray locationStrings;
                     for (const TargetAnchorInfo &target : container->referenceableTargets()) {
                         if (target.ref.isEmpty() || target.location.isEmpty()) {
                             continue;
                         }

                         const QString readerLocation = readerLocationForTarget(target);
                         QJsonObject item;
                         item.insert(QStringLiteral("id"), target.ref);
                         item.insert(QStringLiteral("location"), readerLocation);
                         item.insert(QStringLiteral("readerLocation"), readerLocation);
                         item.insert(QStringLiteral("hrefLocation"), target.location);
                         item.insert(QStringLiteral("cfiLocation"), target.cfiLocation);
                         item.insert(QStringLiteral("file"), target.file);
                         item.insert(QStringLiteral("previewHtml"), target.previewHtml);
                         item.insert(QStringLiteral("title"), target.title);
                         item.insert(QStringLiteral("type"), target.type);
                         item.insert(QStringLiteral("tocTitle"), target.tocTitle);
                         item.insert(QStringLiteral("tocDepth"), target.tocDepth);
                         QJsonArray tocPath;
                         for (const QString &tocSegment : target.tocPath) {
                             tocPath.append(tocSegment);
                         }
                         item.insert(QStringLiteral("tocPath"), tocPath);
                         targetLocations.append(item);
                         locationStrings.append(readerLocation);
                     }

                     QJsonObject root;
                     root.insert(QStringLiteral("bookId"), identifier);
                     root.insert(QStringLiteral("targetLocations"), targetLocations);
                     root.insert(QStringLiteral("locations"), locationStrings);

                     QJsonDocument doc(root);
                     QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
                     QHttpHeaders headers;
                     headers.append("Cache-Control", "no-cache");
                     response.setHeaders(headers);
                     return response;
                 });

    server.route(QStringLiteral("/<arg>/targetLocations"),
                 QHttpServerRequest::Method::Post,
                 [this](const QString &identifier, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request to create target location for book:" << identifier << "with token:" << token;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }

                     const QJsonDocument document = QJsonDocument::fromJson(request.body());
                     const QJsonObject obj = document.object();
                     const QString cfi = obj.value(QStringLiteral("targetCFI")).toString(obj.value(QStringLiteral("cfi")).toString()).trimmed();
                     if (cfi.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     const auto container = containerForIdentifier(identifier);
                     if (!container) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     const QString targetId = container->createAnchor(cfi, QString());
                     if (targetId.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     m_containerCache.remove(identifier);
                     clearServedResourcesForIdentifier(identifier);
                     refreshBookStateAfterFileMutation({identifier});

                     QJsonObject root;
                     root.insert(QStringLiteral("id"), targetId);
                     root.insert(QStringLiteral("anchorCreated"), true);
                     root.insert(QStringLiteral("modifiedBookIds"), stringListToJsonArray({identifier}));

                     const auto anchoredContainer = containerForIdentifier(identifier);
                     if (anchoredContainer) {
                         anchoredContainer->extractTargetAnchors();
                         if (const TargetAnchorInfo *anchor = anchoredContainer->targetAnchorByRef(targetId)) {
                             const QString readerLocation = readerLocationForTarget(*anchor);
                             root.insert(QStringLiteral("location"), readerLocation);
                             root.insert(QStringLiteral("readerLocation"), readerLocation);
                             root.insert(QStringLiteral("hrefLocation"), anchor->location);
                             root.insert(QStringLiteral("cfiLocation"), anchor->cfiLocation);
                             root.insert(QStringLiteral("previewHtml"), anchor->previewHtml);
                             root.insert(QStringLiteral("file"), anchor->file);
                             root.insert(QStringLiteral("title"), anchor->title);
                             root.insert(QStringLiteral("type"), anchor->type);
                         }
                     }

                     return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
                 });
    // Route for book resources
    server.route(QStringLiteral("/<arg>/res/<arg>"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QString & /*resourceUuid*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/res/<arg>"), [this](const QString &identifier, const QString &resourceUuid, const QHttpServerRequest &request) {
        qDebug() << "Request for resource URL:" << request.url() << "with token:" << requestToken(request);

        const QString token = requestToken(request);
        if (!isValidToken(token)) {
            qDebug() << "invalid token";
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        if (!m_resourceByUuid.contains(resourceUuid)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const ServedResource served = m_resourceByUuid.value(resourceUuid);

        if (served.identifier != identifier) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Forbidden};
        }

        if (!served.sessionToken.isEmpty() && served.sessionToken != token) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Forbidden};
        }

        const QString cleanPath = QDir::cleanPath(served.path);

        if (!m_servedFilesByIdentifier.value(identifier).contains(cleanPath)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Forbidden};
        }

        auto container = containerForIdentifier(identifier);
        if (!container) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const QByteArray data = container->readData(cleanPath);
        if (data.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const qint64 totalSize = data.size();
        const QByteArray rangeHeader = request.value("Range");

        qint64 start = 0;
        qint64 end = totalSize - 1;
        bool partial = false;

        if (rangeHeader.startsWith("bytes=")) {
            const QByteArray range = rangeHeader.mid(strlen("bytes="));
            const QList<QByteArray> parts = range.split('-');

            bool okStart = false;
            bool okEnd = false;

            if (!parts.value(0).isEmpty()) {
                start = parts.value(0).toLongLong(&okStart);
            }

            if (parts.size() > 1 && !parts.value(1).isEmpty()) {
                end = parts.value(1).toLongLong(&okEnd);
            }

            if (okStart) {
                if (!okEnd) {
                    end = totalSize - 1;
                }

                if (start >= 0 && end >= start && end < totalSize) {
                    partial = true;
                } else {
                    return QHttpServerResponse{QHttpServerResponder::StatusCode::RequestRangeNotSatisfiable};
                }
            }
        }
        qDebug() << "Serving resource:" << cleanPath << "size:" << totalSize << "partial:" << partial << "range:" << start << "-" << end;
        const QByteArray body = partial ? data.mid(start, end - start + 1) : data;

        QMimeDatabase db;
        const QString mime = db.mimeTypeForFile(cleanPath, QMimeDatabase::MatchExtension).name();

        QHttpServerResponse response(mime.isEmpty() ? QByteArrayLiteral("application/octet-stream") : mime.toUtf8(),
                                     body,
                                     partial ? QHttpServerResponder::StatusCode::PartialContent : QHttpServerResponder::StatusCode::Ok);

        auto headers = response.headers();
        headers.append("Accept-Ranges", "bytes");
        headers.append("Cache-Control", "public, max-age=3600");
        headers.append("Content-Length", QByteArray::number(body.size()));

        if (partial) {
            headers.append("Content-Range",
                           QByteArray("bytes ") + QByteArray::number(start) + "-" + QByteArray::number(end) + "/" + QByteArray::number(totalSize));
        }

        response.setHeaders(headers);
        AriannaTrace::event(QStringLiteral("bookserver.resource.served"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("resource"), AriannaTrace::shortId(resourceUuid)},
                             {QStringLiteral("path"), cleanPath},
                             {QStringLiteral("mime"), mime},
                             {QStringLiteral("bytes"), body.size()},
                             {QStringLiteral("totalBytes"), totalSize},
                             {QStringLiteral("partial"), partial},
                             {QStringLiteral("rangeStart"), start},
                             {QStringLiteral("rangeEnd"), end}});

        return response;
    });

    server.route(QStringLiteral("/<arg>/ref/<arg>"),
                 QHttpServerRequest::Method::Options,
                 [](const QString &, const QString &, const QHttpServerRequest &request) {
                     qDebug() << "Processing OPTIONS request for reference from book:";
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/ref/<arg>"),
                 QHttpServerRequest::Method::Get,
                 [this](const QString &sourceBookId, const QString &sourceAnchorId, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request for reference from book:" << sourceBookId << "anchor:" << sourceAnchorId;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }
                     qDebug() << "Request for reference from book:" << sourceBookId << "anchor:" << sourceAnchorId << "with token:" << token;
                     ReferenceStore referenceStore;
                     QVariantMap ref = referenceStore.loadReferenceBySource(sourceBookId, sourceAnchorId);
                     if (ref.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }
                     // Resolve target anchor details if we don't already have location or preview metadata.
                     QJsonObject obj;
                     const QString targetBookId = ref.value(QStringLiteral("targetBookId")).toString();
                     const QString targetLocation = ref.value(QStringLiteral("targetLocation")).toString();
                     const QString targetPreviewHtml = ref.value(QStringLiteral("targetPreviewHtml")).toString();
                     const QString sourceAnchorTitle = ref.value(QStringLiteral("sourceAnchorTitle")).toString();
                     TargetAnchorInfo resolvedTarget;
                     bool hasResolvedTarget = false;
                     QString resolvedTargetLocation = targetLocation;
                     QString resolvedTargetPreviewHtml = targetPreviewHtml;
                     const bool targetLocationIsPdf = isPdfLocation(targetLocation);
                     const bool requiredAnchorLookup = !targetLocationIsPdf && (targetLocation.isEmpty() || targetPreviewHtml.isEmpty());
                     const bool locationPrefersHref = !targetLocationIsPdf && !targetLocation.isEmpty() && isCfiLocation(targetLocation);
                     const bool needsAnchorLookup = (requiredAnchorLookup || locationPrefersHref) && !targetBookId.isEmpty();
                     if (needsAnchorLookup) {
                         const auto targetContainer = containerForIdentifier(targetBookId);
                         if (!targetContainer) {
                             return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                         }
                         hasResolvedTarget = findReferenceableTarget(targetContainer, targetLocation, &resolvedTarget);
                         if (!hasResolvedTarget) {
                             if (requiredAnchorLookup) {
                                 return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                             }
                         }

                         if (hasResolvedTarget) {
                             qDebug() << "Found target in EPUB:" << resolvedTarget.ref << "Location:" << resolvedTarget.location
                                      << "CFI:" << resolvedTarget.cfiLocation << "preview:" << resolvedTarget.previewHtml;
                             const QString bookTitle = targetContainer->metadata(QStringLiteral("title")).value(0, QStringLiteral("Unbekannter Titel"));
                             const QString shortName = bookTitle + QStringLiteral(" (") + resolvedTarget.ref + QStringLiteral(")");
                             obj.insert(QStringLiteral("shortName"), shortName);

                             const QString resolvedReaderLocation = readerLocationForTarget(resolvedTarget);
                             if ((targetLocation.isEmpty() || locationPrefersHref || resolvedReaderLocation != targetLocation)
                                 && !resolvedReaderLocation.isEmpty()) {
                                 resolvedTargetLocation = resolvedReaderLocation;
                             }
                             if (targetPreviewHtml.isEmpty() && !resolvedTarget.previewHtml.isEmpty()) {
                                 resolvedTargetPreviewHtml = resolvedTarget.previewHtml;
                             }

                             if (resolvedTargetLocation != targetLocation || resolvedTargetPreviewHtml != targetPreviewHtml) {
                                 qDebug() << "Updating reference store with resolved target details";
                                 ref.insert(QStringLiteral("targetLocation"), resolvedTargetLocation);
                                 ref.insert(QStringLiteral("targetPreviewHtml"), resolvedTargetPreviewHtml);
                                 referenceStore.saveReference(ref);
                             }
                         }
                     }

                     // obj[QStringLiteral("sourceBookId")] = sourceBookId;
                     // obj[QStringLiteral("targetBookId")] = targetBookId;
                     obj[QStringLiteral("targetBookId")] = targetBookId;
                     obj[QStringLiteral("location")] = resolvedTargetLocation;
                     if (hasResolvedTarget) {
                         obj[QStringLiteral("hrefLocation")] = resolvedTarget.location;
                         obj[QStringLiteral("cfiLocation")] = resolvedTarget.cfiLocation;
                     }
                     obj[QStringLiteral("previewHtml")] = resolvedTargetPreviewHtml;

                     QJsonObject entryObj;
                     const auto targetEntry = BookDatabase::self().loadEntryByUniqueIdentifier(targetBookId);
                     if (targetEntry) {
                         entryObj.insert(QStringLiteral("filename"), targetEntry->filename);
                         entryObj.insert(QStringLiteral("filetitle"), targetEntry->filetitle);
                         entryObj.insert(QStringLiteral("title"), targetEntry->title);
                         entryObj.insert(QStringLiteral("locations"), targetEntry->locations);
                         entryObj.insert(QStringLiteral("currentLocation"), targetEntry->currentLocation);
                         entryObj.insert(QStringLiteral("uniqueIdentifier"), targetEntry->uniqueIdentifier);
                         entryObj.insert(QStringLiteral("identifier"), targetEntry->identifier);
                         entryObj.insert(QStringLiteral("zoomLevel"), targetEntry->zoomLevel);
                         entryObj.insert(QStringLiteral("pageMode"), targetEntry->pageMode);
                     } else if (m_readOnlyFilesByIdentifier.contains(targetBookId)) {
                         entryObj.insert(QStringLiteral("filename"), m_readOnlyFilesByIdentifier.value(targetBookId));
                         entryObj.insert(QStringLiteral("uniqueIdentifier"), targetBookId);
                     }
                     obj.insert(QStringLiteral("entry"), entryObj);
                     obj.insert(QStringLiteral("readOnly"), m_readOnlyFilesByIdentifier.contains(targetBookId));
                     qDebug() << "opening referenced book at referenced location"
                              << "sourceBookId:" << sourceBookId << "sourceAnchorId:" << sourceAnchorId << "targetBookId:" << targetBookId
                              << "targetLocation:" << resolvedTargetLocation << "targetEntryFilename:" << entryObj.value(QStringLiteral("filename")).toString()
                              << "targetLocationIsPdf:" << targetLocationIsPdf << "lookupRequired:" << requiredAnchorLookup
                              << "locationPrefersHref:" << locationPrefersHref << "targetResolved:" << hasResolvedTarget;

                     QJsonObject root;
                     root.insert(QStringLiteral("sourceBookId"), sourceBookId);
                     root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                     root.insert(QStringLiteral("sourceAnchorTitle"), sourceAnchorTitle);
                     root.insert(QStringLiteral("targetBookId"), targetBookId);
                     root.insert(QStringLiteral("targetLocation"), resolvedTargetLocation);
                     if (hasResolvedTarget) {
                         root.insert(QStringLiteral("targetHrefLocation"), resolvedTarget.location);
                         root.insert(QStringLiteral("targetCfiLocation"), resolvedTarget.cfiLocation);
                     }
                     root.insert(QStringLiteral("targetPreviewHtml"), resolvedTargetPreviewHtml);
                     root[QStringLiteral("target")] = obj;

                     QJsonDocument doc(root);
                     QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
                     QHttpHeaders headers;
                     headers.append("Cache-Control", "no-cache");
                     response.setHeaders(headers);
                     AriannaTrace::event(QStringLiteral("bookserver.crossref.served"),
                                         {{QStringLiteral("sourceBookId"), sourceBookId},
                                          {QStringLiteral("sourceAnchorId"), sourceAnchorId},
                                          {QStringLiteral("sourceAnchorTitle"), sourceAnchorTitle},
                                          {QStringLiteral("targetBookId"), targetBookId},
                                          {QStringLiteral("targetLocation"), resolvedTargetLocation},
                                          {QStringLiteral("targetEntryFilename"), entryObj.value(QStringLiteral("filename")).toString()},
                                          {QStringLiteral("targetLocationIsPdf"), targetLocationIsPdf},
                                          {QStringLiteral("lookupRequired"), requiredAnchorLookup},
                                          {QStringLiteral("locationPrefersHref"), locationPrefersHref},
                                          {QStringLiteral("targetResolved"), hasResolvedTarget}});
                     return response;
                 });

    server.route(QStringLiteral("/<arg>/ref/<arg>"),
                 QHttpServerRequest::Method::Delete,
                 [this](const QString &sourceBookId, const QString &sourceAnchorId, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request to delete reference from book:" << sourceBookId << "anchor:" << sourceAnchorId;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }

                     ReferenceStore referenceStore;
                     const QVariantMap ref = referenceStore.loadReferenceBySource(sourceBookId, sourceAnchorId);
                     if (ref.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     const auto sourceContainer = containerForIdentifier(sourceBookId);
                     if (!sourceContainer) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     if (!sourceContainer->deleteReferenceAnchor(sourceAnchorId)) {
                         QJsonObject root;
                         root.insert(QStringLiteral("error"), QStringLiteral("referenceAnchorDeleteFailed"));
                         root.insert(QStringLiteral("sourceBookId"), sourceBookId);
                         root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                         return QHttpServerResponse(QByteArrayLiteral("application/json"),
                                                    QJsonDocument(root).toJson(QJsonDocument::Compact),
                                                    QHttpServerResponder::StatusCode::InternalServerError);
                     }

                     m_containerCache.remove(sourceBookId);
                     clearServedResourcesForIdentifier(sourceBookId);

                     QStringList modifiedBookIds;
                     appendUniqueString(modifiedBookIds, sourceBookId);
                     bool targetAnchorDeleted = false;
                     const QString targetBookId = ref.value(QStringLiteral("targetBookId")).toString();
                     if (!targetBookId.isEmpty() && !isPdfLocation(ref.value(QStringLiteral("targetLocation")).toString())) {
                         for (const QString &candidateAnchorId : targetAnchorCandidateIds(ref)) {
                             if (!candidateAnchorId.startsWith(QStringLiteral("uuid_"))) {
                                 qDebug() << "Keeping target anchor because it is not an Arianna-generated range anchor:" << targetBookId << candidateAnchorId;
                                 continue;
                             }
                             if (anchorHasOtherDatabaseUsers(targetBookId, candidateAnchorId, ref)) {
                                 qDebug() << "Keeping target anchor because it is still used:" << targetBookId << candidateAnchorId;
                                 continue;
                             }

                             m_containerCache.remove(targetBookId);
                             const auto targetContainer = containerForIdentifier(targetBookId);
                             if (!targetContainer) {
                                 qWarning() << "Unable to open target book for unused target anchor cleanup:" << targetBookId << candidateAnchorId;
                                 continue;
                             }
                             if (!targetContainer->deleteTargetRangeAnchor(candidateAnchorId)) {
                                 qWarning() << "Unable to clean unused target range anchor:" << targetBookId << candidateAnchorId;
                                 continue;
                             }

                             m_containerCache.remove(targetBookId);
                             clearServedResourcesForIdentifier(targetBookId);
                             targetAnchorDeleted = true;
                             appendUniqueString(modifiedBookIds, targetBookId);
                             break;
                         }
                     }

                     referenceStore.removeReference(sourceBookId, sourceAnchorId);

                     QJsonObject root;
                     root.insert(QStringLiteral("deleted"), true);
                     root.insert(QStringLiteral("sourceAnchorDeleted"), true);
                     root.insert(QStringLiteral("sourceBookId"), sourceBookId);
                     root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                     root.insert(QStringLiteral("targetAnchorDeleted"), targetAnchorDeleted);
                     root.insert(QStringLiteral("modifiedBookIds"), stringListToJsonArray(modifiedBookIds));
                     refreshBookStateAfterFileMutation(modifiedBookIds);
                     return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
                 });
    /* route for all books*/
    server.route(QStringLiteral("/books"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });
    server.route(QStringLiteral("/books"), QHttpServerRequest::Method::Get, [this](const QHttpServerRequest &request) {
        const QString token = requestToken(request);
        qDebug() << "Processing request to list all books with token:" << token;
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }
        qDebug() << "   Request to list all books with token:" << token;

        QJsonArray books;
        QList<BookEntry> entries = BookDatabase::self().loadEntries();
        std::sort(entries.begin(), entries.end(), [](const BookEntry &left, const BookEntry &right) {
            const QString leftId = left.uniqueIdentifier.isEmpty() ? left.identifier : left.uniqueIdentifier;
            const QString rightId = right.uniqueIdentifier.isEmpty() ? right.identifier : right.uniqueIdentifier;
            const QString leftTitle = left.title.isEmpty() ? leftId : left.title;
            const QString rightTitle = right.title.isEmpty() ? rightId : right.title;
            const int titleCompare = QString::localeAwareCompare(leftTitle, rightTitle);
            if (titleCompare != 0) {
                return titleCompare < 0;
            }

            return QString::localeAwareCompare(leftId, rightId) < 0;
        });
        for (const BookEntry &entry : entries) {
            QJsonObject book;
            const QString id = entry.uniqueIdentifier.isEmpty() ? entry.identifier : entry.uniqueIdentifier;
            book.insert(QStringLiteral("id"), id);
            book.insert(QStringLiteral("title"), entry.title);
            book.insert(QStringLiteral("author"), QJsonArray::fromStringList(entry.author));
            books.append(book);
        }

        QJsonObject root;
        root.insert(QStringLiteral("books"), books);

        QJsonDocument doc(root);
        QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
        QHttpHeaders headers;
        headers.append("Cache-Control", "no-cache");
        response.setHeaders(headers);
        return response;
    });

    /* route for reference (POST)*/
    server.route(QStringLiteral("/<arg>/ref"), QHttpServerRequest::Method::Options, [](const QString &sourceBookId, const QHttpServerRequest &request) {
        qDebug() << "Processing OPTIONS request to save reference for book:" << sourceBookId;
        return corsPreflightResponse(request);
    });
    server.route(QStringLiteral("/<arg>/ref"), QHttpServerRequest::Method::Post, [this](const QString &sourceBookId, const QHttpServerRequest &request) {
        const QString token = requestToken(request);
        qDebug() << "Processing request to save reference for book:" << sourceBookId;
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }
        qDebug() << "Request to save reference from book:" << sourceBookId << "with token:" << token;

        const QJsonDocument document = QJsonDocument::fromJson(request.body());
        qDebug() << "Request body:" << document.toJson(QJsonDocument::Compact);
        const QJsonObject obj = document.object();
        if (obj.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        ReferenceStore referenceStore;
        QVariantMap ref;
        QString sourceAnchorId = obj.value(QStringLiteral("sourceAnchorId")).toString().trimmed();
        const QString sourceAnchorTitle = obj.value(QStringLiteral("sourceAnchorTitle")).toString().trimmed();
        const QString sourceCfi = obj.value(QStringLiteral("cfi")).toString().trimmed();
        if (sourceCfi.isEmpty() && sourceAnchorId.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }
        const QString targetBookId = obj.value(QStringLiteral("targetBookId")).toString();

        QString targetLocation = obj.value(QStringLiteral("targetLocation")).toString();
        QString targetPreviewHtml = obj.value(QStringLiteral("targetPreviewHtml")).toString();
        const bool targetPreviewHtmlEdited = obj.value(QStringLiteral("targetPreviewHtmlEdited")).toBool(false);

        const QString targetCFI = obj.value(QStringLiteral("targetCFI")).toString().trimmed();
        const bool hasSelectedTargetLocation = !targetLocation.isEmpty();
        if (targetBookId.isEmpty() || (!hasSelectedTargetLocation && targetCFI.isEmpty())) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }
        const bool isCrossReference = targetBookId != sourceBookId;
        QStringList modifiedBookIds;
        bool targetAnchorCreated = false;
        bool sourceAnchorCreated = false;
        bool sourceAnchorChanged = false;

        const bool targetLocationIsPdf = isPdfLocation(targetLocation);

        if (!targetBookId.isEmpty() && !targetCFI.isEmpty() && !hasSelectedTargetLocation) {
            const auto targetContainer = containerForIdentifier(targetBookId);
            if (!targetContainer) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
            }

            const QString createdTargetAnchorId = targetContainer->createAnchor(targetCFI, QString());
            if (createdTargetAnchorId.isEmpty()) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
            }

            targetAnchorCreated = true;
            appendUniqueString(modifiedBookIds, targetBookId);
            m_containerCache.remove(targetBookId);
            clearServedResourcesForIdentifier(targetBookId);

            const auto anchoredTargetContainer = containerForIdentifier(targetBookId);
            if (anchoredTargetContainer) {
                anchoredTargetContainer->extractTargetAnchors();
                if (const TargetAnchorInfo *anchor = anchoredTargetContainer->targetAnchorByRef(createdTargetAnchorId)) {
                    targetLocation = isCrossReference ? readerLocationForTarget(*anchor) : anchor->location;
                    if (targetPreviewHtml.isEmpty() && !targetPreviewHtmlEdited) {
                        targetPreviewHtml = anchor->previewHtml;
                    }
                }
            }
        } else if (!targetLocationIsPdf) {
            const auto targetContainer = containerForIdentifier(targetBookId);
            TargetAnchorInfo target;
            if (findReferenceableTarget(targetContainer, targetLocation, &target)) {
                const QString preferredTargetLocation = isCrossReference ? readerLocationForTarget(target) : target.location;
                if (!preferredTargetLocation.isEmpty()
                    && (targetLocation.isEmpty() || !isFragmentPreciseLocation(targetLocation)
                        || (isCrossReference && targetLocation != preferredTargetLocation))) {
                    targetLocation = preferredTargetLocation;
                }
                if (targetPreviewHtml.isEmpty() && !targetPreviewHtmlEdited) {
                    targetPreviewHtml = target.previewHtml;
                }
            }
        }

        if (targetLocation.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        if (sourceAnchorId.isEmpty()) {
            const auto sourceContainer = containerForIdentifier(sourceBookId);
            if (!sourceContainer) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
            }

            sourceAnchorId = sourceContainer->createBookrefAnchor(sourceCfi, targetLocation, isCrossReference, sourceAnchorTitle);
            if (sourceAnchorId.isEmpty()) {
                sourceAnchorId = QStringLiteral("uuid_") + QUuid::createUuidV7().toString(QUuid::WithoutBraces);

                ref.insert(QStringLiteral("sourceBookId"), sourceBookId);
                ref.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                ref.insert(QStringLiteral("sourceAnchorTitle"), sourceAnchorTitle);
                ref.insert(QStringLiteral("targetBookId"), targetBookId);
                ref.insert(QStringLiteral("targetLocation"), targetLocation);
                ref.insert(QStringLiteral("targetPreviewHtml"), targetPreviewHtml);
                referenceStore.saveReference(ref);

                const QString selectedText = obj.value(QStringLiteral("text")).toString();
                QJsonObject root;
                root.insert(QStringLiteral("error"), QStringLiteral("manualAnchorRequired"));
                root.insert(QStringLiteral("reason"), QStringLiteral("anchorWrapFailed"));
                root.insert(QStringLiteral("bookId"), sourceBookId);
                root.insert(QStringLiteral("cfi"), sourceCfi);
                root.insert(QStringLiteral("text"), selectedText);
                root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                root.insert(QStringLiteral("sourceAnchorTitle"), sourceAnchorTitle);
                root.insert(QStringLiteral("referenceStored"), true);
                root.insert(QStringLiteral("isCrossReference"), isCrossReference);
                root.insert(QStringLiteral("targetBookId"), targetBookId);
                root.insert(QStringLiteral("sourceAnchorCreated"), false);
                root.insert(QStringLiteral("targetAnchorCreated"), targetAnchorCreated);
                root.insert(QStringLiteral("modifiedBookIds"), stringListToJsonArray(modifiedBookIds));
                root.insert(QStringLiteral("anchorOpenTag"), manualBookrefAnchorOpenTag(sourceAnchorId, targetLocation, isCrossReference, sourceAnchorTitle));
                root.insert(QStringLiteral("anchorCloseTag"), QStringLiteral("</a>"));
                refreshBookStateAfterFileMutation(modifiedBookIds);
                return QHttpServerResponse(QByteArrayLiteral("application/json"),
                                           QJsonDocument(root).toJson(QJsonDocument::Compact),
                                           QHttpServerResponder::StatusCode::Conflict);
            }

            m_containerCache.remove(sourceBookId);
            clearServedResourcesForIdentifier(sourceBookId);
            sourceAnchorCreated = true;
            appendUniqueString(modifiedBookIds, sourceBookId);
        } else {
            qDebug() << "Saving reference for existing source anchor:" << sourceAnchorId << "in book:" << sourceBookId;
            const auto sourceContainer = containerForIdentifier(sourceBookId);
            if (!sourceContainer) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
            }
            if (!sourceContainer->updateReferenceAnchor(sourceAnchorId, targetLocation, isCrossReference, sourceAnchorTitle, &sourceAnchorChanged)) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::InternalServerError};
            }
            if (sourceAnchorChanged) {
                m_containerCache.remove(sourceBookId);
                clearServedResourcesForIdentifier(sourceBookId);
                appendUniqueString(modifiedBookIds, sourceBookId);
            }
        }

        if (sourceAnchorId.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        ref.clear();
        ref.insert(QStringLiteral("sourceBookId"), sourceBookId);
        ref.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
        ref.insert(QStringLiteral("sourceAnchorTitle"), sourceAnchorTitle);
        ref.insert(QStringLiteral("targetBookId"), targetBookId);
        ref.insert(QStringLiteral("targetLocation"), targetLocation);
        ref.insert(QStringLiteral("targetPreviewHtml"), targetPreviewHtml);
        referenceStore.saveReference(ref);

        QJsonObject root;
        root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
        root.insert(QStringLiteral("sourceAnchorTitle"), sourceAnchorTitle);
        root.insert(QStringLiteral("targetBookId"), targetBookId);
        root.insert(QStringLiteral("targetLocation"), targetLocation);
        root.insert(QStringLiteral("targetPreviewHtml"), targetPreviewHtml);
        root.insert(QStringLiteral("isCrossReference"), isCrossReference);
        root.insert(QStringLiteral("sourceAnchorCreated"), sourceAnchorCreated);
        root.insert(QStringLiteral("targetAnchorCreated"), targetAnchorCreated);
        root.insert(QStringLiteral("modifiedBookIds"), stringListToJsonArray(modifiedBookIds));
        refreshBookStateAfterFileMutation(modifiedBookIds);
        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
    });
    server.addAfterRequestHandler(&server, [](const QHttpServerRequest &request, QHttpServerResponse &resp) {
        addCorsHeaders(request, resp);
    });

    auto tcpserver = std::make_unique<QTcpServer>();
    if (!tcpserver->listen(BookServerConfig::listenAddress(), BookServerConfig::port())) {
        if (tcpserver->serverError() == QAbstractSocket::AddressInUseError) {
            qWarning().noquote() << "BookServer already running";
        } else {
            qWarning().noquote() << "Server failed to listen on a port:" << tcpserver->errorString();
        }
        return;
    }
    if (!server.bind(tcpserver.get())) {
        qWarning().noquote() << "Server failed to bind to the listening socket.";
        return;
    }
    quint16 port = tcpserver->serverPort();
    auto s = tcpserver.release();
    Q_UNUSED(s);

    m_running = true;
    qWarning() << QStringLiteral("BookServer running on %1/ (Press CTRL+C to quit)").arg(BookServerConfig::baseUrl(port));
    AriannaTrace::event(QStringLiteral("bookserver.listen"),
                        {{QStringLiteral("host"), BookServerConfig::listenAddress().toString()},
                         {QStringLiteral("port"), port},
                         {QStringLiteral("baseUrl"), BookServerConfig::baseUrl(port)},
                         {QStringLiteral("discoveryFile"), discoveryFilePath()}});
    writeDiscoveryFile(port, m_serverToken);
}

bool BookServer::isRunning() const
{
    return m_running;
}

void BookServer::stop()
{
    if (!m_running) {
        return;
    }

    const auto tcpServers = server.servers();
    for (QTcpServer *tcpServer : tcpServers) {
        if (tcpServer) {
            tcpServer->close();
        }
    }

    m_validTokens.clear();
    m_readerSessionRefCount.clear();
    m_identifiersByReaderSession.clear();
    m_readerSessionsByIdentifier.clear();
    m_resourceByUuid.clear();
    m_servedFilesByIdentifier.clear();
    m_resourceMapByIdentifier.clear();
    m_containerCache.clear();
    m_readOnlyFilesByIdentifier.clear();
    m_running = false;

    removeDiscoveryFile();

    qWarning() << "BookServer stopped";
    AriannaTrace::event(QStringLiteral("bookserver.stopped"), {{QStringLiteral("reason"), QStringLiteral("stop")}});
}

void BookServer::addSessionToken(const QString &token)
{
    if (token.isEmpty()) {
        return;
    }

    m_validTokens.insert(token);

    qDebug() << "BookServer session token added:" << token;
}

void BookServer::removeToken(const QString &token)
{
    releaseReaderSessionResources(token);
    m_readerSessionRefCount.remove(token);
    m_validTokens.remove(token);

    qDebug() << "BookServer token removed:" << token;
}

void BookServer::registerReaderSession(const QString &token)
{
    if (token.isEmpty()) {
        return;
    }

    addSessionToken(token);
    m_readerSessionRefCount[token] = m_readerSessionRefCount.value(token) + 1;

    qDebug() << "BookServer reader registered:" << token << "registered readers:" << registeredReaderCount();
    AriannaTrace::event(QStringLiteral("bookserver.reader.registered"),
                        {{QStringLiteral("reader"), AriannaTrace::shortId(token)}, {QStringLiteral("registeredReaders"), registeredReaderCount()}});
}

bool BookServer::unregisterReaderSession(const QString &token)
{
    auto it = m_readerSessionRefCount.find(token);
    if (it == m_readerSessionRefCount.end()) {
        qDebug() << "BookServer reader session not registered:" << token;
        return false;
    }

    --it.value();
    if (it.value() <= 0) {
        m_readerSessionRefCount.erase(it);
        m_validTokens.remove(token);
        releaseReaderSessionResources(token);
    }

    qDebug() << "BookServer reader unregistered:" << token << "registered readers:" << registeredReaderCount();
    AriannaTrace::event(QStringLiteral("bookserver.reader.unregistered"),
                        {{QStringLiteral("reader"), AriannaTrace::shortId(token)}, {QStringLiteral("registeredReaders"), registeredReaderCount()}});
    return true;
}

bool BookServer::hasRegisteredReaders() const
{
    return registeredReaderCount() > 0;
}

int BookServer::registeredReaderCount() const
{
    int count = 0;
    for (auto it = m_readerSessionRefCount.constBegin(); it != m_readerSessionRefCount.constEnd(); ++it) {
        count += it.value();
    }

    return count;
}

void BookServer::scheduleStopIfUnused()
{
    if (hasRegisteredReaders() || m_stopScheduled) {
        return;
    }

    auto *app = QCoreApplication::instance();
    if (!app) {
        stop();
        return;
    }

    m_stopScheduled = true;
    QTimer::singleShot(100, app, [this] {
        m_stopScheduled = false;
        if (hasRegisteredReaders()) {
            return;
        }

        stop();

        if (m_quitWhenUnused) {
            QCoreApplication::quit();
        }
    });
}

bool BookServer::isValidToken(const QString &token) const
{
    if (token.isEmpty()) {
        return false;
    }

    if (m_validTokens.contains(token)) {
        qDebug() << "BookServer token valid:" << token;
        return true;
    }
    qWarning() << "BookServer token invalid:" << token;
    return false;
}

ResourceMap BookServer::makeResourceMap(const QSet<QString> &servedFiles, const QString &identifier, const QString &sessionToken)
{
    ResourceMap map;

    for (const QString &path : servedFiles) {
        const QString cleanPath = QDir::cleanPath(path);

        const QString resourceUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

        m_resourceByUuid.insert(resourceUuid, ServedResource{identifier, cleanPath, sessionToken});

        map.insert(cleanPath, makeResourceUrl(identifier, resourceUuid));
    }

    if (!map.isEmpty()) {
        AriannaTrace::event(QStringLiteral("bookserver.resource.map_created"),
                            {{QStringLiteral("bookId"), identifier},
                             {QStringLiteral("resources"), map.size()},
                             {QStringLiteral("sessionScoped"), !sessionToken.isEmpty()},
                             {QStringLiteral("reader"), AriannaTrace::shortId(sessionToken)}});
    }

    return map;
}

BookServer::~BookServer()
{
    stop();
}
