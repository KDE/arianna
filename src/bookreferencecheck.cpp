// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookreferencecheck.h"
#include "maintenancefiles.h"

#include "bookdatabase.h"
#include "booktruthstore.h"
#include "epubcontainer.h"
#include "pdfreferenceindex.h"

#include <KArchiveDirectory>
#include <KLocalizedString>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>

namespace
{
QString localPath(const QString &value)
{
    const QUrl url(value);
    return QFileInfo(url.isLocalFile() ? url.toLocalFile() : value).absoluteFilePath();
}

QString fileDigest(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

bool containsRemoval(const QVariantMap &report, const QVariantMap &removal)
{
    if (removal.isEmpty()) {
        return false;
    }
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        if (row.toMap().value(QStringLiteral("removal")).toMap() == removal) {
            return true;
        }
    }
    return false;
}

QString normalizedId(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QStringLiteral("urn:uuid:"), Qt::CaseInsensitive)) {
        value = value.mid(9);
    }
    const QUuid uuid(value);
    return uuid.isNull() ? value : uuid.toString(QUuid::WithoutBraces);
}

QList<int> matchingBooks(const QList<BookEntry> &books, const QString &bookId)
{
    const QString id = normalizedId(bookId);
    if (id.isEmpty()) {
        return {};
    }
    QList<int> matches;
    for (int i = 0; i < books.size(); ++i) {
        if (normalizedId(books.at(i).uniqueIdentifier) == id) {
            matches.append(i);
        }
    }
    if (matches.isEmpty()) {
        for (int i = 0; i < books.size(); ++i) {
            auto aliases = books.at(i).identifier.split(QLatin1Char(','), Qt::SkipEmptyParts);
            aliases.append(books.at(i).filename);
            aliases.append(books.at(i).identifier);
            for (const auto &alias : aliases) {
                if (normalizedId(alias) == id) {
                    matches.append(i);
                    break;
                }
            }
        }
    }
    return matches;
}

bool useWorkingCopy(QList<BookEntry> &books, const QString &bookId, const QString &fileName)
{
    if (bookId.isEmpty()) {
        return true;
    }
    const auto matches = matchingBooks(books, bookId);
    if (matches.size() != 1) {
        return false;
    }
    books[matches.first()].filename = localPath(fileName);
    return true;
}

QVariantMap editableReference(const QVariantMap &reference)
{
    QVariantMap result;
    for (const auto &key : {"sourceBookId", "sourceAnchorId", "targetBookId", "targetLocation"}) {
        result.insert(QString::fromLatin1(key), reference.value(QString::fromLatin1(key)).toString());
    }
    return result;
}

bool containsEditableReference(const QVariantMap &report, const QVariantMap &reference)
{
    if (reference.isEmpty()) {
        return false;
    }
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        if (row.toMap().value(QStringLiteral("referenceEdit")).toMap() == reference) {
            return true;
        }
    }
    return false;
}

QVariantMap titleReference(const QVariantMap &reference)
{
    auto result = editableReference(reference);
    result.insert(QStringLiteral("sourceAnchorTitle"), reference.value(QStringLiteral("sourceAnchorTitle")).toString());
    return result;
}

bool containsTitleEdit(const QVariantMap &report, const QVariantMap &edit)
{
    if (edit.isEmpty()) {
        return false;
    }
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        if (row.toMap().value(QStringLiteral("sourceTitleEdit")).toMap() == edit) {
            return true;
        }
    }
    return false;
}

bool replaceFileContents(const QString &fileName, const QString &candidatePath)
{
    QFile candidate(candidatePath);
    QSaveFile output(fileName);
    if (!candidate.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        return false;
    }
    while (!candidate.atEnd()) {
        const auto data = candidate.read(65536);
        if (candidate.error() != QFileDevice::NoError || output.write(data) != data.size()) {
            return false;
        }
    }
    return output.commit();
}

QString elementName(const QDomElement &element)
{
    return element.tagName().section(QLatin1Char(':'), -1);
}

QList<QDomElement> documentElements(const QDomDocument &document)
{
    QList<QDomElement> elements;
    QList<QDomElement> pending{document.documentElement()};
    while (!pending.isEmpty()) {
        const auto element = pending.takeLast();
        if (element.isNull()) {
            continue;
        }
        elements.append(element);
        for (auto child = element.lastChildElement(); !child.isNull(); child = child.previousSiblingElement()) {
            pending.append(child);
        }
    }
    return elements;
}

bool legacyReference(const QDomElement &element)
{
    return element.attribute(QStringLiteral("class")).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).contains(QStringLiteral("bookref"))
        && element.hasAttribute(QStringLiteral("data-ref"));
}

bool crossReferenceElement(const QDomElement &element)
{
    return legacyReference(element)
        || ((elementName(element) == QStringLiteral("a") || elementName(element) == QStringLiteral("span"))
            && element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor")
            && element.attribute(QStringLiteral("data-anchor-type")) == QStringLiteral("crossref"));
}

bool referenceElement(const QDomElement &element)
{
    // Also index local reference anchors to diagnose a wrong source type on a stored cross-reference.
    return crossReferenceElement(element)
        || (elementName(element) == QStringLiteral("a") && element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor")
            && element.hasAttribute(QStringLiteral("href")));
}

QString sourceId(const QDomElement &element)
{
    return element.attribute(legacyReference(element) ? QStringLiteral("data-ref") : QStringLiteral("id"));
}

struct Anchor {
    QString file;
    QDomElement element;
};

struct Document {
    QDomDocument dom;
    QString error;
    QHash<QString, QList<QDomElement>> ids;
};

struct BookIndex {
    EPubContainer container{nullptr};
    QString error;
    bool unverified = false;
    bool isPdf = false;
    PdfReferenceIndex pdf;
    QHash<QString, QList<PdfReferenceAnchor>> pdfSources;
    QMap<QString, Document> documents;
    QHash<QString, QList<Anchor>> ids;
    QHash<QString, QList<Anchor>> sources;
    QHash<QString, QString> cfiLocations;
    QHash<QString, int> textCounts;
    bool cfiLoaded = false;

    explicit BookIndex(const QString &fileName)
    {
        if (!QFileInfo(fileName).isFile()) {
            error = i18n("Book file does not exist: %1", fileName);
            return;
        }
        const QString mimeType = QMimeDatabase().mimeTypeForFile(fileName).name();
        if (mimeType == QStringLiteral("application/pdf")) {
            isPdf = true;
            pdf = readPdfReferenceIndex(fileName);
            error = pdf.error;
            unverified = pdf.needsPassword;
            for (const auto &anchor : pdf.anchors) {
                pdfSources[anchor.id].append(anchor);
            }
            return;
        }
        if (mimeType != QStringLiteral("application/epub+zip")) {
            unverified = true;
            error = i18n("Anchor validation is only available for EPUB and PDF files: %1", fileName);
            return;
        }
        QObject::connect(&container, &EPubContainer::errorOccured, &container, [this](const QString &message) {
            error = message;
        });
        if (!container.openFile(fileName)) {
            if (error.isEmpty()) {
                error = i18n("Unable to open EPUB: %1", fileName);
            }
            return;
        }
        QStringList itemIds = container.manifestItemIds();
        itemIds.sort();
        for (const auto &itemId : itemIds) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                return;
            }
            const auto item = container.epubItem(itemId);
            if (item.mimetype != "application/xhtml+xml" && item.mimetype != "text/html" && item.mimetype != "image/svg+xml") {
                continue;
            }
            const QString path = QDir::cleanPath(item.path);
            if (documents.contains(path)) {
                continue;
            }
            auto &document = documents[path];
            const auto parsed = document.dom.setContent(container.readData(path));
            if (!parsed) {
                document.error = i18n("Cannot parse %1 (line %2, column %3): %4", path, parsed.errorLine, parsed.errorColumn, parsed.errorMessage);
                continue;
            }
            for (const auto &element : documentElements(document.dom)) {
                QStringList elementIds{element.attribute(QStringLiteral("id")), element.attribute(QStringLiteral("xml:id"))};
                if (elementName(element) == QStringLiteral("a")) {
                    elementIds.append(element.attribute(QStringLiteral("name")));
                }
                elementIds.removeAll(QString());
                elementIds.removeDuplicates();
                for (const auto &id : elementIds) {
                    document.ids[id].append(element);
                    ids[id].append({path, element});
                }
                if (referenceElement(element)) {
                    sources[sourceId(element)].append({path, element});
                }
            }
        }
    }

    bool complete() const
    {
        for (const auto &document : documents) {
            if (!document.error.isEmpty()) {
                return false;
            }
        }
        return true;
    }
};

struct Finding {
    QVariantMap row;
    QStringList messages;
    QStringList codes;
    int severity = 0;

    void add(const QString &code, const QString &message, int level = 2)
    {
        codes.append(code);
        messages.append(message);
        severity = qMax(severity, level);
    }
};

void addAnchorRepair(Finding &finding, bool source, const QString &anchorId, const QString &documentFile = QString())
{
    const QString prefix = source ? QStringLiteral("source") : QStringLiteral("target");
    const QString fileName = finding.row.value(prefix + QStringLiteral("BookFile")).toString();
    const QString bookId = finding.row.value(prefix + QStringLiteral("EditorBookId")).toString();
    if (anchorId.isEmpty() || fileName.isEmpty() || bookId.isEmpty()) {
        return;
    }
    const QString tagName = source ? QStringLiteral("span") : QStringLiteral("a");
    QString openTag = QStringLiteral("<%1 id=\"%2\" data-role=\"anchor\" data-anchor-type=\"%3\"")
                          .arg(tagName, anchorId.toHtmlEscaped(), source ? QStringLiteral("crossref") : QStringLiteral("target"));
    const QString title = finding.row.value(QStringLiteral("sourceAnchorTitle")).toString();
    if (source && !title.isEmpty()) {
        openTag += QStringLiteral(" title=\"%1\"").arg(title.toHtmlEscaped());
    }
    openTag += QLatin1Char('>');
    const QString html = openTag + (source ? i18n("Reference text").toHtmlEscaped() : QString()) + QStringLiteral("</%1>").arg(tagName);
    QVariantList repairs = finding.row.value(QStringLiteral("repairs")).toList();
    repairs.append(QVariantMap{{QStringLiteral("kind"), prefix},
                               {QStringLiteral("bookId"), bookId},
                               {QStringLiteral("fileName"), fileName},
                               {QStringLiteral("documentFile"), documentFile},
                               {QStringLiteral("anchorId"), anchorId},
                               {QStringLiteral("htmlSource"), html}});
    finding.row.insert(QStringLiteral("repairs"), repairs);
    if (!source) {
        finding.row.insert(QStringLiteral("targetAnchorRepair"),
                           BookReferenceCheck::targetAnchorRepair(bookId, localPath(fileName), documentFile, anchorId));
    }
}

// Use a synthetic scheme so URI resolution never accesses the local filesystem or network.
QUrl archiveUrl(const QString &path)
{
    QUrl url;
    url.setScheme(QStringLiteral("epub"));
    url.setPath(QLatin1Char('/') + path);
    return url;
}

QUrl resolveLocation(BookIndex &book, const QString &location, Finding &finding)
{
    const QString value = location.trimmed();
    if (value.startsWith(QStringLiteral("epubcfi("))) {
        if (!book.cfiLoaded) {
            for (const auto &target : book.container.referenceableTargets()) {
                if (!target.cfiLocation.isEmpty()) {
                    book.cfiLocations.insert(target.cfiLocation, target.location);
                }
            }
            book.cfiLoaded = true;
        }
        const QString resolved = book.cfiLocations.value(value);
        if (resolved.isEmpty() || resolved.startsWith(QStringLiteral("epubcfi("))) {
            finding.add(QStringLiteral("cfi-unverified"), i18n("This CFI position could not be verified: %1", value), 1);
            return {};
        }
        return archiveUrl(QString()).resolved(QUrl(resolved));
    }
    const QUrl url(value);
    if (!url.isValid()) {
        finding.add(QStringLiteral("invalid-location"), i18n("Invalid target location: %1", value));
        return {};
    }
    if (!url.isRelative() || !url.authority().isEmpty()) {
        finding.add(QStringLiteral("external-unchecked"), i18n("External link; network availability was not checked: %1", value), 1);
        return {};
    }
    // Database locations may be bare anchor or manifest IDs, or paths from the archive root.
    if ((url.path().isEmpty() || (!url.hasFragment() && !value.contains(QLatin1Char('/')))) && !book.complete()) {
        finding.add(QStringLiteral("incomplete-target-index"), i18n("Some EPUB documents are unreadable; the book-wide anchor lookup is incomplete."), 1);
    }
    if (!url.hasFragment() && !value.contains(QLatin1Char('/'))) {
        const QString id = QUrl::fromPercentEncoding(value.toUtf8());
        const auto anchors = book.ids.value(id);
        if (anchors.size() > 1) {
            finding.add(QStringLiteral("ambiguous-target"), i18n("Target anchor occurs in multiple places: %1", id));
            return {};
        }
        if (anchors.size() == 1) {
            QUrl result = archiveUrl(anchors.first().file);
            result.setFragment(id);
            return result;
        }
        const auto item = book.container.epubItem(id);
        if (!item.path.isEmpty()) {
            return archiveUrl(item.path);
        }
    }
    if (url.path().isEmpty() && url.hasFragment()) {
        const auto anchors = book.ids.value(url.fragment(QUrl::FullyDecoded));
        if (anchors.size() != 1) {
            finding.add(QStringLiteral("ambiguous-target"), i18n("Target anchor is missing or ambiguous: %1", url.fragment(QUrl::FullyDecoded)));
            if (anchors.isEmpty() && book.complete()) {
                addAnchorRepair(finding, false, url.fragment(QUrl::FullyDecoded));
            }
            return {};
        }
        QUrl result = archiveUrl(anchors.first().file);
        result.setFragment(url.fragment(QUrl::FullyDecoded));
        return result;
    }
    return archiveUrl(QString()).resolved(url).adjusted(QUrl::NormalizePathSegments);
}

void checkTarget(BookIndex &book, const QUrl &url, Finding &finding)
{
    if (url.isEmpty()) {
        return;
    }
    QString path = url.path();
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }
    const QString resolvedTarget = path + (url.hasFragment() ? QLatin1Char('#') + url.fragment(QUrl::FullyDecoded) : QString());
    finding.row.insert(QStringLiteral("resolvedTarget"), resolvedTarget);
    const auto entry = book.container.rootDirectory()->entry(path);
    if (!entry || !entry->isFile()) {
        finding.add(QStringLiteral("missing-target-file"), i18n("Target document does not exist: %1", path));
        return;
    }
    const auto document = book.documents.constFind(path);
    if (document != book.documents.cend() && !document->error.isEmpty()) {
        finding.add(QStringLiteral("invalid-target-document"), document->error);
        return;
    }
    if (url.fragment(QUrl::FullyDecoded).isEmpty()) {
        return;
    }
    if (document == book.documents.cend()) {
        finding.add(QStringLiteral("fragment-unverified"),
                    i18n("The target exists, but its fragment cannot be checked: %1", url.fragment(QUrl::FullyDecoded)),
                    1);
        return;
    }
    const auto anchors = document->ids.value(url.fragment(QUrl::FullyDecoded));
    if (anchors.isEmpty()) {
        finding.add(QStringLiteral("missing-target-anchor"), i18n("Target anchor %1 does not exist in %2.", url.fragment(QUrl::FullyDecoded), path));
        addAnchorRepair(finding, false, url.fragment(QUrl::FullyDecoded), path);
    } else if (anchors.size() > 1) {
        finding.add(QStringLiteral("duplicate-target-anchor"), i18n("Target anchor %1 occurs more than once in %2.", url.fragment(QUrl::FullyDecoded), path));
    }
}

void checkPdfTarget(const BookIndex &book, const QString &location, Finding &finding)
{
    const auto document = QJsonDocument::fromJson(location.toUtf8());
    const auto target = document.object();
    auto pageValueValid = [](const QJsonValue &value, int minimum) {
        const double number = value.toDouble(-1);
        return value.isDouble() && std::isfinite(number) && std::floor(number) == number && number >= minimum && number < std::numeric_limits<int>::max();
    };
    const bool hasPage = target.contains(QStringLiteral("page"));
    const bool hasPageNumber = target.contains(QStringLiteral("pageNumber"));
    if (!document.isObject() || target.value(QStringLiteral("format")).toString().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0
        || (!hasPage && !hasPageNumber) || (hasPage && !pageValueValid(target.value(QStringLiteral("page")), 0))
        || (hasPageNumber && !pageValueValid(target.value(QStringLiteral("pageNumber")), 1))) {
        finding.add(QStringLiteral("invalid-pdf-location"), i18n("Invalid PDF target location: %1", location));
        return;
    }
    const int page = hasPage ? target.value(QStringLiteral("page")).toInt() : target.value(QStringLiteral("pageNumber")).toInt() - 1;
    finding.row.insert(QStringLiteral("targetPage"), page + 1);
    finding.row.insert(QStringLiteral("resolvedTarget"), i18n("Page %1", page + 1));
    if (hasPage && hasPageNumber && target.value(QStringLiteral("pageNumber")).toInt() != page + 1) {
        finding.add(QStringLiteral("inconsistent-pdf-page"), i18n("The PDF target contains conflicting page and pageNumber values."));
    }
    if (page >= book.pdf.pageCount) {
        finding.add(QStringLiteral("pdf-page-out-of-range"), i18n("Target page %1 does not exist. The PDF has %2 pages.", page + 1, book.pdf.pageCount));
    }
    const QString anchorId = target.value(QStringLiteral("anchorId")).toString();
    if (!anchorId.isEmpty()) {
        auto anchors = book.pdfSources.value(anchorId);
        if (anchors.isEmpty())
            anchors = book.pdfSources.value(anchorId.startsWith(QStringLiteral("uuid_")) ? anchorId.mid(5) : QStringLiteral("uuid_") + anchorId);
        if (anchors.size() != 1)
            finding.add(QStringLiteral("pdf-target-anchor-unresolved"), i18n("PDF target anchor is missing or ambiguous: %1", anchorId));
        else if (anchors.first().page != page)
            finding.add(QStringLiteral("pdf-target-anchor-page"), i18n("The PDF target anchor is on a different page: %1", anchorId));
    }
    if (target.contains(QStringLiteral("x")) || target.contains(QStringLiteral("y"))) {
        const auto xValue = target.value(QStringLiteral("x"));
        const auto yValue = target.value(QStringLiteral("y"));
        const double x = xValue.toDouble(-1);
        const double y = yValue.toDouble(-1);
        const QString position = target.value(QStringLiteral("position")).toString();
        if (!xValue.isDouble() || !yValue.isDouble() || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) {
            finding.add(QStringLiteral("invalid-pdf-position"), i18n("The PDF target position is invalid. Both coordinates must be between 0 and 1."));
        } else if (!position.isEmpty() && position != QStringLiteral("center") && position != QStringLiteral("topLeft")) {
            finding.add(QStringLiteral("invalid-pdf-position"), i18n("Unknown PDF target alignment: %1", position));
        } else {
            finding.row.insert(QStringLiteral("resolvedTarget"), i18n("Page %1, position (%2, %3)", page + 1, x, y));
        }
    }
}
}

namespace
{
QStringList anchorIdentity(const QDomElement &element, const QString &id)
{
    QStringList ids{id, element.attribute(QStringLiteral("id")), element.attribute(QStringLiteral("xml:id")), element.attribute(QStringLiteral("name"))};
    ids.removeAll(QString());
    ids.removeDuplicates();
    return ids;
}

bool nativeAnchorUse(BookIndex &book, const QStringList &ids)
{
    auto usesId = [&](const QDomDocument &document) {
        for (const auto &element : documentElements(document)) {
            const auto attributes = element.attributes();
            for (int i = 0; i < attributes.size(); ++i) {
                const auto attribute = attributes.item(i);
                const QString name = attribute.nodeName().section(QLatin1Char(':'), -1);
                const QString value = attribute.nodeValue();
                if (name == QStringLiteral("href") || name == QStringLiteral("src")) {
                    const QUrl url(value);
                    if (url.isRelative() && (ids.contains(url.fragment(QUrl::FullyDecoded)) || value.startsWith(QStringLiteral("epubcfi("))))
                        return true;
                } else if (name == QStringLiteral("for") || name == QStringLiteral("headers") || name.startsWith(QStringLiteral("aria-"))) {
                    for (const auto &token : value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
                        if (ids.contains(token))
                            return true;
                    }
                } else if (name == QStringLiteral("style")) {
                    for (const auto &id : ids)
                        if (value.contains(QLatin1Char('#') + id))
                            return true;
                }
            }
            if (elementName(element) == QStringLiteral("style")) {
                for (const auto &id : ids)
                    if (element.text().contains(QLatin1Char('#') + id))
                        return true;
            }
        }
        return false;
    };
    for (const auto &document : book.documents)
        if (usesId(document.dom))
            return true;
    for (const auto &item : book.container.manifestItems()) {
        if (item.mimetype == QByteArrayLiteral("application/x-dtbncx+xml")) {
            QDomDocument ncx;
            if (!ncx.setContent(book.container.readData(item.path)) || usesId(ncx))
                return true;
        } else if (item.mimetype == QByteArrayLiteral("text/css")) {
            const auto css = book.container.readData(item.path);
            for (const auto &id : ids) {
                const QByteArray selector = '#' + id.toUtf8();
                if (css.contains(selector))
                    return true;
            }
        }
    }
    return false;
}
}

QVariantMap BookReferenceCheck::targetAnchorRepair(const QString &bookId, const QString &fileName, const QString &documentFile, const QString &anchorId)
{
    if (bookId.isEmpty() || fileName.isEmpty() || documentFile.isEmpty() || anchorId.isEmpty()) return {};
    return {{QStringLiteral("kind"), QStringLiteral("target")}, {QStringLiteral("bookId"), bookId},
            {QStringLiteral("fileName"), fileName}, {QStringLiteral("documentFile"), documentFile}, {QStringLiteral("anchorId"), anchorId},
            {QStringLiteral("htmlSource"), QStringLiteral("<a id=\"%1\" data-role=\"anchor\" data-anchor-type=\"target\"></a>").arg(anchorId.toHtmlEscaped())}};
}

BookReferenceCheck::BookReferenceCheck(QObject *parent)
    : QObject(parent)
{
}

BookReferenceCheck::~BookReferenceCheck()
{
    if (m_thread) {
        m_thread->requestInterruption();
        m_thread->wait();
    }
}

bool BookReferenceCheck::running() const
{
    return m_thread != nullptr;
}

QVariantMap BookReferenceCheck::report() const
{
    return m_report;
}

void BookReferenceCheck::start(const QString &fileName)
{
    startCheck(fileName, QString());
}

void BookReferenceCheck::startWorkingCopy(const QString &bookId, const QString &fileName)
{
    if (!bookId.trimmed().isEmpty()) {
        startCheck(fileName, bookId);
    }
}

void BookReferenceCheck::startCheck(const QString &fileName, const QString &workingCopyBookId)
{
    if (running()) {
        return;
    }
    m_referenceRemovalPlan.clear();
    bool loaded = false;
    const auto references = BookDatabase::self().loadAllReferences(&loaded);
    bool booksLoaded = false;
    auto books = BookDatabase::self().loadEntries(&booksLoaded);
    m_workingCopyBookId = workingCopyBookId;
    m_report.clear();
    if (!loaded || !booksLoaded) {
        m_report.insert(QStringLiteral("message"), i18n("Unable to read the reference database. The check could not be completed."));
        Q_EMIT reportChanged();
        return;
    }
    const auto originalBooks = books;
    QVariantList annotations;
    QByteArray registeredHash;
    if (!workingCopyBookId.isEmpty()) {
        const auto matches = matchingBooks(books, workingCopyBookId);
        if (matches.size() == 1) {
            const auto &book = books.at(matches.first());
            QStringList keys{book.uniqueIdentifier, book.identifier, book.filename};
            keys += book.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts);
            const auto initialKeys = keys;
            for (const auto &key : initialKeys) {
                const auto normalized = normalizedId(key);
                keys.append(normalized);
                if (!QUuid(normalized).isNull()) {
                    keys.append(QStringLiteral("urn:uuid:") + normalized);
                    keys.append(QLatin1Char('{') + normalized + QLatin1Char('}'));
                }
            }
            keys.removeAll(QString());
            keys.removeDuplicates();
            for (const auto &key : keys) {
                bool notesLoaded = false;
                const auto notes = BookDatabase::self().loadAnnotations(key, &notesLoaded);
                if (!notesLoaded) {
                    m_report.insert(QStringLiteral("message"), i18n("Unable to read the remarks. The import check could not be completed."));
                    Q_EMIT reportChanged();
                    return;
                }
                for (const auto &value : notes) {
                    auto note = value.toMap();
                    note.insert(QStringLiteral("annotationBookId"), key);
                    note.insert(QStringLiteral("bookId"), book.uniqueIdentifier);
                    annotations.append(note);
                }
            }
            if (const auto state = BookDatabase::self().currentBookState(book.uniqueIdentifier))
                registeredHash = state->epubFileHash;
        }
    }
    if (!useWorkingCopy(books, workingCopyBookId, fileName)) {
        m_report.insert(QStringLiteral("message"), i18n("The EPUB working copy cannot be assigned to a unique book in the library."));
        Q_EMIT reportChanged();
        return;
    }
    Q_EMIT reportChanged();
    const auto result = std::make_shared<QVariantMap>();
    m_thread = QThread::create([fileName, books, originalBooks, references, annotations, registeredHash, workingCopyBookId, result]() {
        *result = workingCopyBookId.isEmpty() ? inspect(fileName, books, references)
                                              : inspectImport(workingCopyBookId, fileName, originalBooks, references, annotations, registeredHash);
    });
    connect(m_thread, &QThread::finished, this, [this, result]() {
        m_thread = nullptr;
        m_report = *result;
        Q_EMIT runningChanged();
        Q_EMIT reportChanged();
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    Q_EMIT runningChanged();
    m_thread->start();
}

QVariantMap BookReferenceCheck::restoreAnnotationAnchor(const QVariantMap &repair)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    bool offered = false;
    for (const auto &value : m_report.value(QStringLiteral("results")).toList())
        offered |= !repair.isEmpty() && value.toMap().value(QStringLiteral("annotationRepair")).toMap() == repair;
    if (running() || m_workingCopyBookId.isEmpty() || !offered)
        return failure(i18n("Only missing remark anchors from the current import report can be restored."));
    auto filesLock = MaintenanceFiles::lock();
    if (!filesLock)
        return failure(i18n("Editor files are currently being checked or changed. Please try again."));
    const QString fileName = repair.value(QStringLiteral("fileName")).toString();
    const QString expectedHash = repair.value(QStringLiteral("fileHash")).toString();
    const QString bookId = repair.value(QStringLiteral("bookId")).toString();
    bool loaded = false;
    const auto books = BookDatabase::self().loadEntries(&loaded);
    if (!loaded || matchingBooks(books, bookId).size() != 1 || fileName != localPath(m_report.value(QStringLiteral("fileName")).toString()))
        return failure(i18n("Only missing remark anchors from the current import report can be restored."));
    // This action repairs an import candidate. Authoritative editions remain
    // untouched until the normal, fully validated import succeeds.
    for (const auto &book : books) {
        if (QFileInfo(book.filename).canonicalFilePath() == QFileInfo(fileName).canonicalFilePath())
            return failure(i18n("Restore the remark anchor in an editor working copy, then import it."));
    }
    if (MaintenanceFiles::workingCopyInUse(fileName, false))
        return failure(i18n("Close the EPUB editor before restoring a remark anchor."));
    auto annotationUnchanged = [&]() {
        bool notesLoaded = false;
        const auto notes = BookDatabase::self().loadAnnotations(repair.value(QStringLiteral("annotationBookId")).toString(), &notesLoaded);
        int matches = 0;
        for (const auto &value : notes) {
            const auto note = value.toMap();
            bool same = true;
            for (const auto &key : {"annotationId", "anchorId", "cfiRange", "text"})
                same &= note.value(QString::fromLatin1(key)) == repair.value(QString::fromLatin1(key));
            matches += same;
        }
        return notesLoaded && matches == 1;
    };
    if (expectedHash.isEmpty() || fileDigest(fileName) != expectedHash || !annotationUnchanged())
        return failure(i18n("The working copy or remark has changed. Run the import check again."));
    const auto anchorId = repair.value(QStringLiteral("anchorId")).toString();
    const auto elementId = anchorId.startsWith(QStringLiteral("uuid_")) ? anchorId : QStringLiteral("uuid_") + anchorId;
    EPubContainer container(nullptr);
    const auto bytes = container.openFile(fileName)
        ? container.createAnnotationAnchoredEpub(repair.value(QStringLiteral("cfiRange")).toString(), elementId,
                                                 repair.value(QStringLiteral("text")).toString())
        : QByteArray();
    if (bytes.isEmpty())
        return failure(i18n("The saved position no longer matches the remark text. No anchor was restored. Check the passage in the EPUB editor."));
    QTemporaryDir directory;
    const auto candidatePath = directory.filePath(QStringLiteral("candidate.epub"));
    QFile candidate(candidatePath);
    if (!directory.isValid() || !candidate.open(QIODevice::WriteOnly) || candidate.write(bytes) != bytes.size() || !candidate.flush())
        return failure(i18n("Unable to prepare the restored remark anchor."));
    candidate.close();
    const auto validationError = BookTruthStore().validateAnchorChanges(bookId, fileName, candidatePath);
    if (!validationError.isEmpty())
        return failure(validationError);
    auto candidateBooks = books;
    if (!useWorkingCopy(candidateBooks, bookId, candidatePath))
        return failure(i18n("Unable to prepare the restored remark anchor."));
    const auto rows = inspectLibraryAnnotations(candidateBooks, {repair});
    if (rows.size() != 1 || rows.first().toMap().value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
        return failure(i18n("Unable to prepare the restored remark anchor."));
    if (fileDigest(fileName) != expectedHash || !annotationUnchanged())
        return failure(i18n("The working copy or remark has changed. Run the import check again."));
    const QString backupPath = fileName + QStringLiteral(".arianna-remark-anchor-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".bak");
    if (!QFile::copy(fileName, backupPath) || fileDigest(backupPath) != expectedHash || fileDigest(fileName) != expectedHash
        || !MaintenanceFiles::registerBackup(fileName, backupPath))
        return failure(i18n("Unable to create a current backup. The remark anchor was not restored."));
    if (!replaceFileContents(fileName, candidatePath))
        return failure(i18n("Unable to save the restored remark anchor. Backup: %1", backupPath));
    MaintenanceFiles::completeBackup(backupPath);
    startCheck(fileName, m_workingCopyBookId);
    return {{QStringLiteral("success"), true}, {QStringLiteral("backupPath"), backupPath},
            {QStringLiteral("message"), QString(i18n("Remark anchor restored in the working copy. Check the report, then retry the import. Backup: %1", backupPath))}};
}

QVariantList BookReferenceCheck::referenceTargetBooks(const QVariantMap &reference) const
{
    if (!containsEditableReference(m_report, reference)) {
        return {};
    }
    auto books = BookDatabase::self().loadEntries();
    std::sort(books.begin(), books.end(), [](const auto &left, const auto &right) {
        return QString::localeAwareCompare(left.title, right.title) < 0;
    });
    const auto sourceMatches = matchingBooks(books, reference.value(QStringLiteral("sourceBookId")).toString());
    const auto currentMatches = matchingBooks(books, reference.value(QStringLiteral("targetBookId")).toString());
    QHash<QString, QList<int>> uniqueIds;
    QHash<QString, QList<int>> aliases;
    for (int index = 0; index < books.size(); ++index) {
        const auto &book = books.at(index);
        if (!normalizedId(book.uniqueIdentifier).isEmpty()) {
            uniqueIds[normalizedId(book.uniqueIdentifier)].append(index);
        }
        QSet<QString> seen;
        for (const auto &alias : book.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString id = normalizedId(alias);
            if (!id.isEmpty() && !seen.contains(id)) {
                aliases[id].append(index);
                seen.insert(id);
            }
        }
    }
    QVariantList targets;
    for (int index = 0; index < books.size(); ++index) {
        const auto &book = books.at(index);
        const QString bookId = book.uniqueIdentifier.isEmpty() ? book.identifier.section(QLatin1Char(','), 0, 0).trimmed() : book.uniqueIdentifier;
        const bool sourceFile = std::any_of(sourceMatches.cbegin(), sourceMatches.cend(), [&](int source) {
            return localPath(books.at(source).filename) == localPath(book.filename);
        });
        const QString id = normalizedId(bookId);
        const auto candidates = uniqueIds.contains(id) ? uniqueIds.value(id) : aliases.value(id);
        if (bookId.isEmpty() || sourceFile || candidates.size() != 1 || candidates.first() != index) {
            continue;
        }
        const QString displayName = book.title + QStringLiteral(" — ") + QFileInfo(book.filename).fileName();
        targets.append(QVariantMap{{QStringLiteral("bookId"), bookId},
                                   {QStringLiteral("title"), book.title},
                                   {QStringLiteral("displayName"), displayName},
                                   {QStringLiteral("fileName"), book.filename},
                                   {QStringLiteral("selected"), currentMatches.size() == 1 && currentMatches.first() == index}});
    }
    return targets;
}

QVariantMap BookReferenceCheck::validateReferenceTarget(const QVariantMap &reference, const QString &targetBookId, const QString &targetLocation) const
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || !containsEditableReference(m_report, reference)) {
        return failure(i18n("Only stored cross-references from the current report can be edited."));
    }
    const auto current = BookDatabase::self().loadReferenceBySource(reference.value(QStringLiteral("sourceBookId")).toString(),
                                                                    reference.value(QStringLiteral("sourceAnchorId")).toString());
    if (current.isEmpty() || editableReference(current) != reference) {
        return failure(i18n("The cross-reference has changed or was deleted. Run the reference check again."));
    }
    const QString destination = targetLocation.trimmed();
    if (destination.isEmpty()) {
        return failure(i18n("Enter a target location."));
    }
    bool loaded = false;
    const auto books = BookDatabase::self().loadEntries(&loaded);
    if (!loaded) {
        return failure(i18n("Unable to read the reference database. The check could not be completed."));
    }
    const QString targetId = targetBookId.trimmed();
    const auto targets = matchingBooks(books, targetId);
    if (targets.size() != 1) {
        return failure(i18n("Target book is missing or ambiguous in the library: %1", targetId));
    }
    const auto &entry = books.at(targets.first());
    const QString targetPath = localPath(entry.filename);
    const auto sources = matchingBooks(books, reference.value(QStringLiteral("sourceBookId")).toString());
    if (normalizedId(reference.value(QStringLiteral("sourceBookId")).toString()) == normalizedId(targetId)
        || std::any_of(sources.cbegin(), sources.cend(), [&](int source) {
               return localPath(books.at(source).filename) == targetPath;
           })) {
        return failure(i18n("Select a target in another book for this cross-reference."));
    }
    // Reference edits take effect immediately, including in a reimport report.
    // Validate the authoritative target, not an unimported editor working copy.
    const QString hash = fileDigest(targetPath);
    BookIndex target(targetPath);
    if (!target.error.isEmpty()) {
        return failure(target.error);
    }
    Finding finding;
    finding.row.insert(QStringLiteral("targetBookFile"), targetPath);
    finding.row.insert(QStringLiteral("targetEditorBookId"), entry.uniqueIdentifier.isEmpty() ? targetId : entry.uniqueIdentifier);
    if (target.isPdf) {
        checkPdfTarget(target, destination, finding);
    } else {
        checkTarget(target, resolveLocation(target, destination, finding), finding);
    }
    if (finding.severity != 0) {
        auto result = failure(finding.messages.join(QLatin1Char('\n')));
        if (finding.row.contains(QStringLiteral("targetAnchorRepair")))
            result.insert(QStringLiteral("targetAnchorRepair"), finding.row.value(QStringLiteral("targetAnchorRepair")));
        return result;
    }
    if (hash.isEmpty() || fileDigest(targetPath) != hash) {
        return failure(i18n("The target book changed during validation. Check the target again."));
    }
    return {{QStringLiteral("success"), true},
            {QStringLiteral("message"), i18n("Target found: %1", finding.row.value(QStringLiteral("resolvedTarget")).toString())},
            {QStringLiteral("resolvedTarget"), finding.row.value(QStringLiteral("resolvedTarget"))},
            {QStringLiteral("targetBookId"), targetId},
            {QStringLiteral("targetLocation"), destination}};
}

QVariantMap BookReferenceCheck::saveReferenceTarget(const QVariantMap &reference, const QString &targetBookId, const QString &targetLocation)
{
    // Revalidate even after a successful preview; the target may have changed meanwhile.
    const auto checked = validateReferenceTarget(reference, targetBookId, targetLocation);
    if (!checked.value(QStringLiteral("success")).toBool()) {
        return checked;
    }
    bool conflict = false;
    if (!BookDatabase::self().updateReferenceTarget(reference,
                                                    checked.value(QStringLiteral("targetBookId")).toString(),
                                                    checked.value(QStringLiteral("targetLocation")).toString(),
                                                    &conflict)) {
        return {{QStringLiteral("success"), false},
                {QStringLiteral("message"),
                 conflict ? i18n("The cross-reference has changed or was deleted. Run the reference check again.")
                          : i18n("Unable to save the cross-reference. The target was not changed.")}};
    }
    startCheck(m_report.value(QStringLiteral("fileName")).toString(), m_workingCopyBookId);
    return {{QStringLiteral("success"), true}, {QStringLiteral("message"), i18n("Cross-reference target updated. Checking references again…")}};
}

QVariantMap BookReferenceCheck::saveReferenceTitle(const QVariantMap &edit, const QString &title)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || !containsTitleEdit(m_report, edit)) {
        return failure(i18n("Only unambiguous EPUB source anchors from the current report can be edited."));
    }
    const auto expected = edit.value(QStringLiteral("reference")).toMap();
    auto referenceUnchanged = [&]() {
        const auto current = BookDatabase::self().loadReferenceBySource(expected.value(QStringLiteral("sourceBookId")).toString(),
                                                                        expected.value(QStringLiteral("sourceAnchorId")).toString());
        return !current.isEmpty() && titleReference(current) == expected;
    };
    if (!referenceUnchanged()) {
        return failure(i18n("The cross-reference has changed or was deleted. Run the reference check again."));
    }
    bool loaded = false;
    auto books = BookDatabase::self().loadEntries(&loaded);
    const QString checkedFile = m_report.value(QStringLiteral("fileName")).toString();
    if (!loaded || !useWorkingCopy(books, m_workingCopyBookId, checkedFile)) {
        return failure(i18n("Unable to read the reference database. The check could not be completed."));
    }
    const QString fileName = edit.value(QStringLiteral("fileName")).toString();
    const QString expectedHash = edit.value(QStringLiteral("fileHash")).toString();
    const auto sources = matchingBooks(books, expected.value(QStringLiteral("sourceBookId")).toString());
    if (sources.size() != 1 || localPath(books.at(sources.first()).filename) != fileName || expectedHash.isEmpty() || fileDigest(fileName) != expectedHash) {
        return failure(i18n("The source book has changed. Run the reference check again before editing its title."));
    }

    QTemporaryDir directory;
    const QString candidatePath = directory.filePath(QStringLiteral("candidate.epub"));
    if (!directory.isValid() || !QFile::copy(fileName, candidatePath) || fileDigest(candidatePath) != expectedHash) {
        return failure(i18n("Unable to prepare an EPUB working copy for the title change."));
    }
    const QString newTitle = title.trimmed();
    EPubContainer candidate(nullptr);
    if (!candidate.openFile(candidatePath)
        || !candidate.updateReferenceAnchorTitle(edit.value(QStringLiteral("documentFile")).toString(),
                                                 expected.value(QStringLiteral("sourceAnchorId")).toString(),
                                                 newTitle)) {
        return failure(i18n("Unable to update the EPUB anchor title. The original book was not changed."));
    }
    const QString candidateHash = fileDigest(candidatePath);
    if (candidateHash.isEmpty()) {
        return failure(i18n("Unable to prepare an EPUB working copy for the title change."));
    }
    const bool fileChanged = candidateHash != expectedHash;
    const bool workingCopy = !m_workingCopyBookId.isEmpty() && fileName == localPath(checkedFile);
    const QString bookId = edit.value(QStringLiteral("bookId")).toString();
    if (fileChanged && workingCopy) {
        const QString error = BookTruthStore().validateAnchorChanges(bookId, fileName, candidatePath);
        if (!error.isEmpty()) {
            return failure(error);
        }
    }
    auto commit = [&](const QString &path) {
        return workingCopy ? replaceFileContents(fileName, path) : BookTruthStore().commitCandidateBookFile(bookId, path, false, true).success;
    };
    if (fileDigest(fileName) != expectedHash || !referenceUnchanged()) {
        return failure(i18n("The source book or cross-reference has changed. Run the reference check again."));
    }
    QString backupPath;
    if (fileChanged) {
        backupPath = fileName + QStringLiteral(".arianna-anchor-title-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".bak");
        if (!QFile::copy(fileName, backupPath) || fileDigest(backupPath) != expectedHash || fileDigest(fileName) != expectedHash) {
            return failure(i18n("Unable to create a current backup. The anchor title was not changed."));
        }
        MaintenanceFiles::registerBackup(fileName, backupPath);
        if (!commit(candidatePath)) {
            return failure(i18n("Unable to save the EPUB anchor title. Backup: %1", backupPath));
        }
    }
    if (!BookDatabase::self().updateReferenceTitle(expected, newTitle)) {
        // A failed database update must not leave a different title in the EPUB.
        const bool restored = !fileChanged || (fileDigest(fileName) == candidateHash && commit(backupPath));
        startCheck(checkedFile, m_workingCopyBookId);
        return failure(restored ? i18n("Unable to save the cross-reference title. The EPUB title was not changed.")
                                : i18n("Unable to save the cross-reference title or restore the EPUB. Backup: %1", backupPath));
    }
    MaintenanceFiles::completeBackup(backupPath);
    startCheck(checkedFile, m_workingCopyBookId);
    return {{QStringLiteral("success"), true},
            {QStringLiteral("backupPath"), backupPath},
            {QStringLiteral("message"), backupPath.isEmpty() ? i18n("Anchor title updated.") : i18n("Anchor title updated. Backup: %1", backupPath)}};
}

static QVariantMap planReferenceRemoval(const QVariantMap &current, const QList<BookEntry> &books, const QVariantList &references)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    QVariantMap plan{{QStringLiteral("reference"), current}};
    for (const bool source : {true, false}) {
        const QString role = source ? QStringLiteral("source") : QStringLiteral("target");
        const auto matches = matchingBooks(books, current.value(role + QStringLiteral("BookId")).toString());
        QVariantMap side{{QStringLiteral("action"), QStringLiteral("keep")}};
        auto keep = [&](const QString &reason) {
            side.insert(QStringLiteral("reason"), reason);
            plan.insert(role, side);
        };
        if (matches.isEmpty()) {
            keep(i18n("The book is missing from the library; no file will be changed."));
            continue;
        }
        if (matches.size() != 1)
            return failure(i18n("The book assignment is ambiguous. Run the reference check again."));
        const auto &entry = books.at(matches.first());
        const QString fileName = localPath(entry.filename);
        const QString bookId = entry.uniqueIdentifier.isEmpty() ? entry.identifier.section(QLatin1Char(','), 0, 0).trimmed() : entry.uniqueIdentifier;
        const QString hash = fileDigest(fileName);
        side.insert(QStringLiteral("fileName"), fileName);
        side.insert(QStringLiteral("fileHash"), hash);
        side.insert(QStringLiteral("bookId"), bookId);
        side.insert(QStringLiteral("bookTitle"), entry.title);
        if (!QFileInfo::exists(fileName)) {
            keep(i18n("The book file is missing; no anchor can be removed from it."));
            continue;
        }
        BookIndex index(fileName);
        if (hash.isEmpty() || !index.error.isEmpty() || !index.complete()) {
            return failure(i18n("The book cannot be fully read. No cross-reference was deleted: %1", fileName));
        }
        side.insert(QStringLiteral("format"), index.isPdf ? QStringLiteral("pdf") : QStringLiteral("epub"));
        QString id = source ? current.value(QStringLiteral("sourceAnchorId")).toString() : QString();
        QStringList ids;
        QDomElement element;
        if (index.isPdf) {
            if (!source)
                id = QJsonDocument::fromJson(current.value(QStringLiteral("targetLocation")).toString().toUtf8())
                         .object()
                         .value(QStringLiteral("anchorId"))
                         .toString();
            if (id.isEmpty()) {
                keep(i18n("This target has no separate anchor. The document or page will be kept."));
                continue;
            }
            auto anchors = index.pdfSources.value(id);
            if (anchors.isEmpty() && !id.startsWith(QStringLiteral("uuid_"))) {
                anchors = index.pdfSources.value(QStringLiteral("uuid_") + id);
                if (!anchors.isEmpty())
                    id.prepend(QStringLiteral("uuid_"));
            } else if (anchors.isEmpty() && id.startsWith(QStringLiteral("uuid_"))) {
                anchors = index.pdfSources.value(id.mid(5));
                if (!anchors.isEmpty())
                    id.remove(0, 5);
            }
            if (anchors.isEmpty()) {
                keep(i18n("The anchor is already missing; no file change is needed."));
                continue;
            }
            if (anchors.size() != 1)
                return failure(i18n("The anchor is ambiguous and cannot be safely deleted: %1", id));
            side.insert(QStringLiteral("page"), anchors.first().page);
            side.insert(QStringLiteral("annotation"), anchors.first().annotation);
            ids = {id};
        } else {
            QString documentFile;
            if (source) {
                const auto anchors = index.sources.value(id);
                if (anchors.isEmpty()) {
                    keep(i18n("The source cross-reference marker is missing; no file change is needed."));
                    continue;
                }
                if (anchors.size() != 1 || index.ids.value(id).size() > 1)
                    return failure(i18n("The anchor is ambiguous and cannot be safely deleted: %1", id));
                element = anchors.first().element;
                documentFile = anchors.first().file;
                if (!crossReferenceElement(element)) {
                    keep(i18n("This element is not a cross-reference marker and will be kept."));
                    continue;
                }
            } else {
                Finding finding;
                const auto url = resolveLocation(index, current.value(QStringLiteral("targetLocation")).toString(), finding);
                if (url.isEmpty()) {
                    keep(i18n("The target position cannot be resolved; its file will be kept."));
                    continue;
                }
                id = url.fragment(QUrl::FullyDecoded);
                if (id.isEmpty()) {
                    keep(i18n("This target has no separate anchor. The document or page will be kept."));
                    continue;
                }
                documentFile = url.path().mid(1);
                const auto anchors = index.documents.value(documentFile).ids.value(id);
                if (anchors.isEmpty()) {
                    keep(i18n("The anchor is already missing; no file change is needed."));
                    continue;
                }
                if (anchors.size() != 1)
                    return failure(i18n("The anchor is ambiguous and cannot be safely deleted: %1", id));
                element = anchors.first();
                const QString type = element.attribute(QStringLiteral("data-anchor-type"));
                if ((elementName(element) != QStringLiteral("a") && elementName(element) != QStringLiteral("span"))
                    || element.attribute(QStringLiteral("data-role")) != QStringLiteral("anchor") || (!type.isEmpty() && type != QStringLiteral("target"))) {
                    keep(i18n("The target belongs to the book's content or structure; it will be kept."));
                    continue;
                }
            }
            side.insert(QStringLiteral("documentFile"), documentFile);
            side.insert(QStringLiteral("elementIndex"), documentElements(index.documents.value(documentFile).dom).indexOf(element));
            ids = anchorIdentity(element, id);
        }
        side.insert(QStringLiteral("anchorId"), id);
        bool shared = false, otherSource = false;
        auto refersToBook = [&](const QString &otherId) {
            const auto candidates = matchingBooks(books, otherId);
            return std::any_of(candidates.cbegin(), candidates.cend(), [&](int i) {
                return localPath(books.at(i).filename) == fileName;
            });
        };
        QStringList bookIds{bookId, current.value(role + QStringLiteral("BookId")).toString()};
        bookIds.append(entry.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts));
        for (const auto &value : references) {
            const auto other = value.toMap();
            if (other.value(QStringLiteral("sourceBookId")) == current.value(QStringLiteral("sourceBookId"))
                && other.value(QStringLiteral("sourceAnchorId")) == current.value(QStringLiteral("sourceAnchorId")))
                continue;
            if (refersToBook(other.value(QStringLiteral("sourceBookId")).toString())) {
                bookIds.append(other.value(QStringLiteral("sourceBookId")).toString());
                otherSource |= ids.contains(other.value(QStringLiteral("sourceAnchorId")).toString());
            }
            if (refersToBook(other.value(QStringLiteral("targetBookId")).toString())) {
                bookIds.append(other.value(QStringLiteral("targetBookId")).toString());
                const QString location = other.value(QStringLiteral("targetLocation")).toString();
                if (index.isPdf) {
                    QString targetId = QJsonDocument::fromJson(location.toUtf8()).object().value(QStringLiteral("anchorId")).toString();
                    shared |= !targetId.isEmpty()
                        && (ids.contains(targetId) || ids.contains(QStringLiteral("uuid_") + targetId)
                            || (targetId.startsWith(QStringLiteral("uuid_")) && ids.contains(targetId.mid(5))));
                } else {
                    Finding finding;
                    const auto url = resolveLocation(index, location, finding);
                    shared |= ids.contains(url.fragment(QUrl::FullyDecoded)) || (url.isEmpty() && location.startsWith(QStringLiteral("epubcfi(")));
                }
            }
        }
        bookIds.removeDuplicates();
        auto normalizedAnchor = [](QString value) {
            if (value.startsWith(QStringLiteral("uuid_")))
                value.remove(0, 5);
            return value;
        };
        for (const auto &annotationBookId : bookIds) {
            bool loaded = false;
            const auto annotations = BookDatabase::self().loadAnnotations(annotationBookId, &loaded);
            if (!loaded)
                return failure(i18n("Unable to read annotations. No cross-reference was deleted."));
            for (const auto &value : annotations) {
                const auto annotation = value.toMap();
                const QStringList usedIds{annotation.value(QStringLiteral("anchorId")).toString(),
                                          annotation.value(QStringLiteral("annotationId")).toString(),
                                          annotation.value(QStringLiteral("location")).toMap().value(QStringLiteral("anchorId")).toString()};
                for (const auto &usedId : usedIds)
                    for (const auto &identity : ids) {
                        shared |= !usedId.isEmpty() && normalizedAnchor(usedId) == normalizedAnchor(identity);
                    }
            }
        }
        shared |= otherSource || (!index.isPdf && nativeAnchorUse(index, ids));
        if (shared && (!source || index.isPdf || otherSource)) {
            keep(i18n("The anchor is used by another reference, annotation or book link and will be kept."));
        } else {
            side.insert(QStringLiteral("action"), shared ? QStringLiteral("detach") : QStringLiteral("remove"));
            side.insert(QStringLiteral("reason"),
                        shared ? i18n("The cross-reference marker will be removed; the anchor ID stays available for other uses.")
                               : i18n("The anchor will be removed. Its text and book content will be preserved."));
            plan.insert(role, side);
        }
        if (fileDigest(fileName) != hash)
            return failure(i18n("The book changed while preparing deletion. Run the reference check again."));
    }
    const QString sourcePath = plan.value(QStringLiteral("source")).toMap().value(QStringLiteral("fileName")).toString();
    const QString targetPath = plan.value(QStringLiteral("target")).toMap().value(QStringLiteral("fileName")).toString();
    if (!sourcePath.isEmpty() && !targetPath.isEmpty() && !QFileInfo(sourcePath).canonicalFilePath().isEmpty()
        && QFileInfo(sourcePath).canonicalFilePath() == QFileInfo(targetPath).canonicalFilePath()) {
        return failure(i18n("Source and target refer to the same file. Only cross-references between different books can be deleted here."));
    }
    return {{QStringLiteral("success"), true}, {QStringLiteral("plan"), plan}};
}

QVariantMap BookReferenceCheck::referenceRemovalPlan(const QVariantMap &reference) const
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || !containsEditableReference(m_report, reference)) {
        return failure(i18n("Only stored cross-references from the current report can be edited."));
    }
    const auto current = BookDatabase::self().loadReferenceBySource(reference.value(QStringLiteral("sourceBookId")).toString(),
                                                                    reference.value(QStringLiteral("sourceAnchorId")).toString());
    if (current.isEmpty() || editableReference(current) != reference) {
        return failure(i18n("The cross-reference has changed or was deleted. Run the reference check again."));
    }
    bool booksLoaded = false, refsLoaded = false;
    auto books = BookDatabase::self().loadEntries(&booksLoaded);
    const auto references = BookDatabase::self().loadAllReferences(&refsLoaded);
    if (!booksLoaded || !refsLoaded || !useWorkingCopy(books, m_workingCopyBookId, m_report.value(QStringLiteral("fileName")).toString())) {
        return failure(i18n("Unable to read the reference database. The check could not be completed."));
    }
    return planReferenceRemoval(current, books, references);
}

QVariantMap BookReferenceCheck::prepareReferenceRemoval(const QVariantMap &reference)
{
    const auto result = referenceRemovalPlan(reference);
    m_referenceRemovalPlan = result.value(QStringLiteral("plan")).toMap();
    return result;
}

QVariantMap BookReferenceCheck::removeReference(const QVariantMap &plan)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    if (running() || plan.isEmpty() || plan != m_referenceRemovalPlan) {
        return failure(i18n("Prepare and confirm the cross-reference deletion first."));
    }
    const auto reference = plan.value(QStringLiteral("reference")).toMap();
    const auto prepared = referenceRemovalPlan(editableReference(reference));
    if (!prepared.value(QStringLiteral("success")).toBool() || prepared.value(QStringLiteral("plan")).toMap() != plan) {
        m_referenceRemovalPlan.clear();
        return failure(i18n("The books or their references have changed. Review and confirm the deletion again."));
    }
    bool booksLoaded = false, refsLoaded = false;
    auto books = BookDatabase::self().loadEntries(&booksLoaded);
    auto references = BookDatabase::self().loadAllReferences(&refsLoaded);
    const QString checkedFile = m_report.value(QStringLiteral("fileName")).toString();
    if (!booksLoaded || !refsLoaded || !useWorkingCopy(books, m_workingCopyBookId, checkedFile)) {
        return failure(i18n("Unable to read the reference database. The check could not be completed."));
    }
    for (auto it = references.begin(); it != references.end();) {
        const auto row = it->toMap();
        if (row.value(QStringLiteral("sourceBookId")) == reference.value(QStringLiteral("sourceBookId"))
            && row.value(QStringLiteral("sourceAnchorId")) == reference.value(QStringLiteral("sourceAnchorId")))
            it = references.erase(it);
        else
            ++it;
    }
    struct Change {
        QVariantMap side;
        QString candidate;
        QString hash;
        QString backup;
    };
    QList<Change> changes;
    QList<BookState> states;
    QTemporaryDir directory;
    if (!directory.isValid())
        return failure(i18n("Unable to prepare working copies for cross-reference deletion."));
    for (const bool source : {true, false}) {
        const auto side = plan.value(source ? QStringLiteral("source") : QStringLiteral("target")).toMap();
        const QString action = side.value(QStringLiteral("action")).toString();
        if (action == QStringLiteral("keep"))
            continue;
        const bool pdf = side.value(QStringLiteral("format")).toString() == QStringLiteral("pdf");
        const QString fileName = side.value(QStringLiteral("fileName")).toString();
        const QString candidatePath = directory.filePath(QString::number(changes.size()) + (pdf ? QStringLiteral(".pdf") : QStringLiteral(".epub")));
        if (!QFile::copy(fileName, candidatePath) || fileDigest(candidatePath) != side.value(QStringLiteral("fileHash")).toString()) {
            return failure(i18n("The book changed while preparing deletion. Run the reference check again."));
        }
        if (pdf) {
            const QString error = removePdfReferenceAnchor(candidatePath,
                                                           side.value(QStringLiteral("page")).toInt(),
                                                           side.value(QStringLiteral("annotation")).toInt(),
                                                           side.value(QStringLiteral("anchorId")).toString());
            if (!error.isEmpty())
                return failure(error);
        } else {
            EPubContainer candidate(nullptr);
            if (!candidate.openFile(candidatePath)
                || !candidate.removeReferenceMarker(side.value(QStringLiteral("documentFile")).toString(),
                                                    side.value(QStringLiteral("elementIndex")).toInt(),
                                                    side.value(QStringLiteral("anchorId")).toString(),
                                                    source,
                                                    action == QStringLiteral("detach"))) {
                return failure(i18n("Unable to remove the EPUB reference marker. No book was changed."));
            }
            if (m_workingCopyBookId.isEmpty() || fileName != localPath(checkedFile)) {
                const auto state = BookTruthStore().stateForFile(side.value(QStringLiteral("bookId")).toString(), candidatePath);
                if (!state)
                    return failure(i18n("Unable to prepare the new EPUB state. No book was changed."));
                states.append(*state);
            }
        }
        const QString hash = fileDigest(candidatePath);
        BookIndex verification(candidatePath);
        if (hash.isEmpty() || !verification.error.isEmpty() || !verification.complete()) {
            return failure(i18n("The modified book could not be verified. No cross-reference was deleted."));
        }
        changes.append({side, candidatePath, hash, {}});
    }
    auto candidateBooks = books;
    for (auto &book : candidateBooks) {
        for (const auto &change : changes)
            if (localPath(book.filename) == change.side.value(QStringLiteral("fileName")).toString())
                book.filename = change.candidate;
    }
    for (const auto &change : changes) {
        const auto before = inspect(change.side.value(QStringLiteral("fileName")).toString(), books, references);
        const auto after = inspect(change.candidate, candidateBooks, references);
        for (const auto &afterValue : after.value(QStringLiteral("results")).toList()) {
            const auto afterRow = afterValue.toMap();
            if (!afterRow.contains(QStringLiteral("sourceBookId")))
                continue;
            for (const auto &beforeValue : before.value(QStringLiteral("results")).toList()) {
                const auto beforeRow = beforeValue.toMap();
                if (editableReference(beforeRow) != editableReference(afterRow))
                    continue;
                const auto oldCodes = beforeRow.value(QStringLiteral("codes")).toStringList();
                for (const auto &code : afterRow.value(QStringLiteral("codes")).toStringList()) {
                    if (!oldCodes.contains(code))
                        return failure(i18n("Deleting this anchor would damage another cross-reference. No book was changed."));
                }
            }
        }
    }
    if (referenceRemovalPlan(editableReference(reference)).value(QStringLiteral("plan")).toMap() != plan) {
        m_referenceRemovalPlan.clear();
        return failure(i18n("The books or their references have changed. Review and confirm the deletion again."));
    }
    QStringList backups;
    for (auto &change : changes) {
        const QString fileName = change.side.value(QStringLiteral("fileName")).toString();
        const QString expectedHash = change.side.value(QStringLiteral("fileHash")).toString();
        change.backup = fileName + QStringLiteral(".arianna-reference-deletion-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".bak");
        if (fileDigest(fileName) != expectedHash || !QFile::copy(fileName, change.backup) || fileDigest(change.backup) != expectedHash) {
            return failure(i18n("Unable to create current backups. No cross-reference was deleted."));
        }
        MaintenanceFiles::registerBackup(fileName, change.backup);
        backups.append(change.backup);
    }
    int applied = 0;
    QString error;
    for (const auto &change : changes) {
        const QString fileName = change.side.value(QStringLiteral("fileName")).toString();
        if (fileDigest(fileName) != change.side.value(QStringLiteral("fileHash")).toString() || !replaceFileContents(fileName, change.candidate)) {
            error = i18n("Unable to save a book while deleting the cross-reference.");
            break;
        }
        ++applied;
    }
    if (error.isEmpty() && !BookDatabase::self().removeReferenceAndSaveStates(reference, states)) {
        error = i18n("The cross-reference could not be removed from the database.");
    }
    if (!error.isEmpty()) {
        bool restored = true;
        for (int i = applied - 1; i >= 0; --i) {
            const auto &change = changes.at(i);
            const QString fileName = change.side.value(QStringLiteral("fileName")).toString();
            restored = (fileDigest(fileName) == change.hash && replaceFileContents(fileName, change.backup)) && restored;
        }
        startCheck(checkedFile, m_workingCopyBookId);
        return failure(
            error + QLatin1Char('\n')
            + (restored ? i18n("All file changes were rolled back.") : i18n("Some files could not be restored. Backups: %1", backups.join(QLatin1Char('\n')))));
    }
    for (const auto &backup : backups)
        MaintenanceFiles::completeBackup(backup);
    startCheck(checkedFile, m_workingCopyBookId);
    return {{QStringLiteral("success"), true},
            {QStringLiteral("backupPaths"), backups},
            {QStringLiteral("message"),
             backups.isEmpty() ? i18n("Cross-reference deleted.") : i18n("Cross-reference deleted. Backups: %1", backups.join(QLatin1Char('\n')))}};
}

QVariantMap BookReferenceCheck::removeBook(const BookEntry &entry)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    auto &database = BookDatabase::self();
    bool booksLoaded = false, refsLoaded = false;
    const auto books = database.loadEntries(&booksLoaded);
    const auto references = database.loadAllReferences(&refsLoaded);
    if (!booksLoaded || !refsLoaded) {
        return failure(i18n("Unable to read the reference database. The check could not be completed."));
    }
    const auto removedIds = BookDatabase::identifiersForRemoval(entry.filename, books);
    QVariantList removedReferences, retainedReferences;
    for (const auto &value : references) {
        const auto reference = value.toMap();
        if (removedIds.contains(normalizedId(reference.value(QStringLiteral("sourceBookId")).toString()))
            || removedIds.contains(normalizedId(reference.value(QStringLiteral("targetBookId")).toString()))) {
            removedReferences.append(value);
        } else {
            retainedReferences.append(value);
        }
    }
    QList<BookEntry> retainedBooks;
    for (const auto &book : books) {
        if (book.filename != entry.filename) {
            retainedBooks.append(book);
        }
    }
    if (retainedBooks.size() == books.size()) {
        return failure(i18n("The book is no longer in the library."));
    }

    struct Change {
        BookEntry book;
        QString candidate;
        QString originalHash;
        QString candidateHash;
        QString backup;
        bool changed = false;
    };
    QList<Change> changes;
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return failure(i18n("Unable to prepare working copies for cross-reference deletion."));
    }
    auto candidateBooks = retainedBooks;
    // Stage each affected remaining book once. Re-plan against these evolving
    // copies after every removal: DOM and PDF annotation indices may shift.
    for (const auto &value : removedReferences) {
        const auto reference = value.toMap();
        for (const auto &role : {QStringLiteral("sourceBookId"), QStringLiteral("targetBookId")}) {
            const auto matches = matchingBooks(retainedBooks, reference.value(role).toString());
            if (matches.size() > 1) {
                return failure(i18n("The book assignment is ambiguous. Run the reference check again."));
            }
            if (matches.isEmpty()) {
                continue;
            }
            const int index = matches.first();
            const auto &book = retainedBooks.at(index);
            if (candidateBooks.at(index).filename != book.filename || !QFileInfo::exists(localPath(book.filename))) {
                continue;
            }
            const QString original = localPath(book.filename);
            const QString hash = fileDigest(original);
            const QString candidate = directory.filePath(QString::number(changes.size()) + QLatin1Char('.') + QFileInfo(original).suffix());
            if (hash.isEmpty() || !QFile::copy(original, candidate) || fileDigest(candidate) != hash) {
                return failure(i18n("Unable to prepare working copies for cross-reference deletion."));
            }
            changes.append({book, candidate, hash, {}, {}, false});
            candidateBooks[index].filename = candidate;
        }
    }
    for (const auto &value : removedReferences) {
        const auto planned = planReferenceRemoval(value.toMap(), candidateBooks, retainedReferences);
        if (!planned.value(QStringLiteral("success")).toBool()) {
            return planned;
        }
        const auto plan = planned.value(QStringLiteral("plan")).toMap();
        for (const bool source : {true, false}) {
            const auto side = plan.value(source ? QStringLiteral("source") : QStringLiteral("target")).toMap();
            const QString action = side.value(QStringLiteral("action")).toString();
            if (action == QStringLiteral("keep")) {
                continue;
            }
            const QString candidate = side.value(QStringLiteral("fileName")).toString();
            if (side.value(QStringLiteral("format")) == QStringLiteral("pdf")) {
                const QString error = removePdfReferenceAnchor(candidate,
                                                               side.value(QStringLiteral("page")).toInt(),
                                                               side.value(QStringLiteral("annotation")).toInt(),
                                                               side.value(QStringLiteral("anchorId")).toString());
                if (!error.isEmpty()) {
                    return failure(error);
                }
            } else {
                EPubContainer epub(nullptr);
                if (!epub.openFile(candidate)
                    || !epub.removeReferenceMarker(side.value(QStringLiteral("documentFile")).toString(),
                                                   side.value(QStringLiteral("elementIndex")).toInt(),
                                                   side.value(QStringLiteral("anchorId")).toString(),
                                                   source,
                                                   action == QStringLiteral("detach"))) {
                    return failure(i18n("Unable to remove the EPUB reference marker. No book was changed."));
                }
            }
            for (auto &change : changes) {
                if (change.candidate == candidate) {
                    change.changed = true;
                }
            }
        }
    }
    QList<BookState> states;
    for (auto &change : changes) {
        if (!change.changed) {
            continue;
        }
        change.candidateHash = fileDigest(change.candidate);
        BookIndex verification(change.candidate);
        if (change.candidateHash.isEmpty() || !verification.error.isEmpty() || !verification.complete()) {
            return failure(i18n("The modified book could not be verified. No cross-reference was deleted."));
        }
        const auto before = inspect(change.book.filename, retainedBooks, retainedReferences);
        const auto after = inspect(change.candidate, candidateBooks, retainedReferences);
        for (const auto &afterValue : after.value(QStringLiteral("results")).toList()) {
            const auto afterRow = afterValue.toMap();
            if (!afterRow.contains(QStringLiteral("sourceBookId"))) {
                continue;
            }
            for (const auto &beforeValue : before.value(QStringLiteral("results")).toList()) {
                const auto beforeRow = beforeValue.toMap();
                if (editableReference(beforeRow) == editableReference(afterRow)) {
                    const auto oldCodes = beforeRow.value(QStringLiteral("codes")).toStringList();
                    for (const auto &code : afterRow.value(QStringLiteral("codes")).toStringList()) {
                        if (!oldCodes.contains(code)) {
                            return failure(i18n("Deleting this anchor would damage another cross-reference. No book was changed."));
                        }
                    }
                }
            }
        }
        if (!verification.isPdf) {
            const QString bookId =
                change.book.uniqueIdentifier.isEmpty() ? change.book.identifier.section(QLatin1Char(','), 0, 0).trimmed() : change.book.uniqueIdentifier;
            const auto state = BookTruthStore().stateForFile(bookId, change.candidate);
            if (!state) {
                return failure(i18n("Unable to prepare the new EPUB state. No book was changed."));
            }
            states.append(*state);
        }
    }
    QStringList backups;
    for (auto &change : changes) {
        if (!change.changed) {
            continue;
        }
        const QString original = localPath(change.book.filename);
        change.backup = original + QStringLiteral(".arianna-book-deletion-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".bak");
        if (fileDigest(original) != change.originalHash || !QFile::copy(original, change.backup) || fileDigest(change.backup) != change.originalHash) {
            return failure(i18n("Unable to create current backups. No cross-reference was deleted."));
        }
        MaintenanceFiles::registerBackup(original, change.backup);
        backups.append(change.backup);
    }
    QList<qsizetype> applied;
    QString error;
    for (qsizetype i = 0; i < changes.size(); ++i) {
        const auto &change = changes.at(i);
        if (!change.changed) {
            continue;
        }
        const QString original = localPath(change.book.filename);
        if (fileDigest(original) != change.originalHash || !replaceFileContents(original, change.candidate)) {
            error = i18n("Unable to save a book while deleting the cross-reference.");
            break;
        }
        applied.append(i);
    }
    if (error.isEmpty() && !database.removeEntry(entry, states, &references)) {
        error = i18n("Unable to remove the book and its related data.");
    }
    if (!error.isEmpty()) {
        bool restored = true;
        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            const auto &change = changes.at(*it);
            const QString original = localPath(change.book.filename);
            restored = (fileDigest(original) == change.candidateHash && replaceFileContents(original, change.backup)) && restored;
        }
        return failure(
            error + QLatin1Char('\n')
            + (restored ? i18n("All file changes were rolled back.") : i18n("Some files could not be restored. Backups: %1", backups.join(QLatin1Char('\n')))));
    }
    for (const auto &backup : backups)
        MaintenanceFiles::completeBackup(backup);
    return {{QStringLiteral("success"), true}, {QStringLiteral("backupPaths"), backups}};
}

QVariantMap BookReferenceCheck::removeOrphanAnchor(const QVariantMap &removal)
{
    auto failure = [](const QString &message) {
        return QVariantMap{{QStringLiteral("success"), false}, {QStringLiteral("message"), message}};
    };
    const QString fileName = m_report.value(QStringLiteral("fileName")).toString();
    const QString expectedHash = m_report.value(QStringLiteral("fileHash")).toString();
    if (running() || !containsRemoval(m_report, removal) || removal.value(QStringLiteral("fileName")).toString() != fileName) {
        return failure(i18n("Only orphaned anchors from the current report can be removed."));
    }
    if (expectedHash.isEmpty() || fileDigest(fileName) != expectedHash) {
        return failure(i18n("The book has changed. Run the reference check again before removing the anchor."));
    }
    bool booksLoaded = false;
    bool referencesLoaded = false;
    auto books = BookDatabase::self().loadEntries(&booksLoaded);
    const auto references = BookDatabase::self().loadAllReferences(&referencesLoaded);
    if (!booksLoaded || !referencesLoaded) {
        return failure(i18n("Unable to read the reference database. The check could not be completed."));
    }
    if (!useWorkingCopy(books, m_workingCopyBookId, fileName)) {
        return failure(i18n("The EPUB working copy cannot be assigned to a unique book in the library."));
    }
    // A report can outlive a new reference assignment. Always verify against the live database before changing a file.
    const auto currentReport = inspect(fileName, books, references);
    if (currentReport.value(QStringLiteral("fileHash")).toString() != expectedHash || !containsRemoval(currentReport, removal)) {
        return failure(i18n("The anchor is no longer orphaned or has changed. Run the reference check again."));
    }

    QTemporaryDir directory;
    const bool isPdf = removal.value(QStringLiteral("format")).toString() == QStringLiteral("pdf");
    const QString candidatePath = directory.filePath(isPdf ? QStringLiteral("candidate.pdf") : QStringLiteral("candidate.epub"));
    if (!directory.isValid() || !QFile::copy(fileName, candidatePath) || fileDigest(candidatePath) != expectedHash) {
        return failure(i18n("Unable to prepare a working copy for anchor removal."));
    }
    if (isPdf) {
        const QString error = removePdfReferenceAnchor(candidatePath,
                                                       removal.value(QStringLiteral("page")).toInt(),
                                                       removal.value(QStringLiteral("annotation")).toInt(),
                                                       removal.value(QStringLiteral("anchorId")).toString());
        if (!error.isEmpty()) {
            return failure(error);
        }
    } else {
        EPubContainer candidate(nullptr);
        if (!candidate.openFile(candidatePath)
            || !candidate.deleteOrphanReferenceAnchor(removal.value(QStringLiteral("documentFile")).toString(),
                                                      removal.value(QStringLiteral("elementIndex")).toInt(),
                                                      removal.value(QStringLiteral("anchorId")).toString())) {
            return failure(i18n("Unable to remove the EPUB anchor. The original book was not changed."));
        }
    }

    auto candidateBooks = books;
    for (auto &book : candidateBooks) {
        if (localPath(book.filename) == fileName) {
            book.filename = candidatePath;
        }
    }
    const auto candidateReport = inspect(candidatePath, candidateBooks, references);
    if (!isPdf && !m_workingCopyBookId.isEmpty()) {
        const QString error = BookTruthStore().validateAnchorChanges(removal.value(QStringLiteral("bookId")).toString(), fileName, candidatePath);
        if (!error.isEmpty()) {
            return failure(error);
        }
    }
    // Include identifier aliases when checking incoming references; the truth store also validates annotations.
    for (const auto &candidateValue : candidateReport.value(QStringLiteral("results")).toList()) {
        const auto candidateRow = candidateValue.toMap();
        if (!candidateRow.contains(QStringLiteral("sourceBookId"))) {
            continue;
        }
        for (const auto &previousValue : currentReport.value(QStringLiteral("results")).toList()) {
            const auto previousRow = previousValue.toMap();
            if (candidateRow.value(QStringLiteral("sourceBookId")) != previousRow.value(QStringLiteral("sourceBookId"))
                || candidateRow.value(QStringLiteral("sourceAnchorId")) != previousRow.value(QStringLiteral("sourceAnchorId"))) {
                continue;
            }
            const auto oldCodes = previousRow.value(QStringLiteral("codes")).toStringList();
            for (const auto &code : candidateRow.value(QStringLiteral("codes")).toStringList()) {
                if (!oldCodes.contains(code)) {
                    return failure(i18n("This anchor is still used by another reference and cannot be removed."));
                }
            }
        }
    }

    const QString backupPath =
        fileName + QStringLiteral(".arianna-anchor-removal-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".bak");
    if (fileDigest(fileName) != expectedHash) {
        return failure(i18n("The book has changed. Run the reference check again before removing the anchor."));
    }
    if (!QFile::copy(fileName, backupPath)) {
        return failure(i18n("Unable to create a backup. The anchor was not removed."));
    }
    if (fileDigest(backupPath) != expectedHash || fileDigest(fileName) != expectedHash) {
        return failure(i18n("The book has changed. Run the reference check again before removing the anchor."));
    }
    MaintenanceFiles::registerBackup(fileName, backupPath);
    QString error;
    if (isPdf || !m_workingCopyBookId.isEmpty()) {
        QFile candidate(candidatePath);
        QSaveFile output(fileName);
        bool copied = candidate.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly);
        while (copied && !candidate.atEnd()) {
            const auto data = candidate.read(65536);
            copied = candidate.error() == QFileDevice::NoError && output.write(data) == data.size();
        }
        if (!copied || !output.commit()) {
            error = i18n("Unable to save the book after removing the anchor.");
        }
    } else {
        // Existing broken references must not prevent cleaning up an unrelated orphan;
        // any newly broken annotation or reference still rejects the candidate.
        const auto committed = BookTruthStore().commitCandidateBookFile(removal.value(QStringLiteral("bookId")).toString(), candidatePath, false, true);
        if (!committed.success) {
            error = committed.errorMessage;
        }
    }
    if (!error.isEmpty()) {
        return failure(i18n("Anchor removal failed: %1\nBackup: %2", error, backupPath));
    }
    MaintenanceFiles::completeBackup(backupPath);
    startCheck(fileName, m_workingCopyBookId);
    return {{QStringLiteral("success"), true},
            {QStringLiteral("backupPath"), backupPath},
            {QStringLiteral("message"), i18n("Orphaned anchor removed. Backup: %1", backupPath)}};
}

static void validateStoredReference(Finding &finding,
                                    const QList<BookEntry> &books,
                                    const QList<int> &sources,
                                    const QList<int> &targets,
                                    const std::function<BookIndex &(const QString &)> &indexFor,
                                    QHash<QString, QString> *sourceFileHashes = nullptr)
{
    const auto reference = finding.row;
    const QString sourceBookId = reference.value(QStringLiteral("sourceBookId")).toString();
    const QString targetBookId = reference.value(QStringLiteral("targetBookId")).toString();
    const QString anchorId = reference.value(QStringLiteral("sourceAnchorId")).toString();
    const QString targetLocation = reference.value(QStringLiteral("targetLocation")).toString().trimmed();
    auto resolveBook = [&](const QList<int> &candidates, const QString &id, bool source) -> BookIndex * {
        const QString prefix = source ? QStringLiteral("source") : QStringLiteral("target");
        if (candidates.size() != 1) {
            finding.add(prefix + QStringLiteral("-book-unresolved"),
                        source ? i18n("Source book is missing or ambiguous in the library: %1", id)
                               : i18n("Target book is missing or ambiguous in the library: %1", id));
            return nullptr;
        }
        const auto &entry = books.at(candidates.first());
        finding.row.insert(prefix + QStringLiteral("BookTitle"), entry.title);
        finding.row.insert(prefix + QStringLiteral("BookFile"), entry.filename);
        finding.row.insert(prefix + QStringLiteral("EditorBookId"),
                           entry.uniqueIdentifier.isEmpty() ? entry.identifier.section(QLatin1Char(','), 0, 0).trimmed() : entry.uniqueIdentifier);
        if (sourceFileHashes && source && !sourceFileHashes->contains(localPath(entry.filename))) {
            sourceFileHashes->insert(localPath(entry.filename), fileDigest(localPath(entry.filename)));
        }
        auto &index = indexFor(localPath(entry.filename));
        if (!index.error.isEmpty()) {
            finding.add(prefix + QStringLiteral("-book-unreadable"), index.error, index.unverified ? 1 : 2);
            return nullptr;
        }
        return &index;
    };
    auto *source = resolveBook(sources, sourceBookId, true);
    auto *target = resolveBook(targets, targetBookId, false);
    Anchor sourceAnchor;
    if (anchorId.isEmpty()) {
        finding.add(QStringLiteral("empty-source-anchor"), i18n("The reference has no source anchor ID."));
    } else if (source && source->isPdf) {
        const auto anchors = source->pdfSources.value(anchorId);
        if (anchors.isEmpty()) {
            finding.add(QStringLiteral("source-anchor-unresolved"), i18n("Source reference anchor is missing or ambiguous: %1", anchorId));
        } else if (anchors.size() > 1) {
            QStringList pages;
            for (const auto &anchor : anchors) {
                pages.append(QString::number(anchor.page + 1));
            }
            finding.add(QStringLiteral("source-anchor-unresolved"),
                        i18n("PDF source anchor %1 occurs more than once (pages: %2).", anchorId, pages.join(QStringLiteral(", "))));
        } else {
            finding.row.insert(QStringLiteral("sourcePage"), anchors.first().page + 1);
            finding.row.insert(QStringLiteral("sourceAnnotation"), anchors.first().annotation + 1);
        }
    } else if (source) {
        const auto anchors = source->sources.value(anchorId);
        if (anchors.size() != 1) {
            finding.add(QStringLiteral("source-anchor-unresolved"),
                        source->complete() ? i18n("Source reference anchor is missing or ambiguous: %1", anchorId)
                                           : i18n("Source anchor %1 could not be verified because a source document is unreadable.", anchorId));
            if (anchors.isEmpty() && source->ids.value(anchorId).isEmpty() && source->complete()) {
                addAnchorRepair(finding, true, anchorId);
            }
        } else {
            sourceAnchor = anchors.first();
            if (!source->complete()) {
                finding.add(QStringLiteral("incomplete-source-index"),
                            i18n("Some EPUB documents are unreadable; the book-wide anchor lookup is incomplete."),
                            1);
            }
            finding.row.insert(QStringLiteral("sourceFile"), sourceAnchor.file);
            finding.row.insert(QStringLiteral("line"), sourceAnchor.element.lineNumber());
            if (source->ids.value(anchorId).size() > 1) {
                finding.add(QStringLiteral("duplicate-source-id"), i18n("Source anchor ID occurs more than once: %1", anchorId));
            }
            if (!crossReferenceElement(sourceAnchor.element)) {
                finding.add(QStringLiteral("reference-type-mismatch"), i18n("The EPUB anchor type does not match the stored reference."));
            }
            if (sourceFileHashes && source->complete() && source->ids.value(anchorId).size() <= 1 && crossReferenceElement(sourceAnchor.element)) {
                const QString sourcePath = localPath(finding.row.value(QStringLiteral("sourceBookFile")).toString());
                finding.row.insert(QStringLiteral("sourceTitleEdit"),
                                   QVariantMap{{QStringLiteral("reference"), titleReference(reference)},
                                               {QStringLiteral("fileName"), sourcePath},
                                               {QStringLiteral("fileHash"), sourceFileHashes->value(sourcePath)},
                                               {QStringLiteral("bookId"), finding.row.value(QStringLiteral("sourceEditorBookId"))},
                                               {QStringLiteral("documentFile"), sourceAnchor.file},
                                               {QStringLiteral("htmlTitle"), sourceAnchor.element.attribute(QStringLiteral("title"))}});
            }
        }
        const QString editorAnchorId = sourceAnchor.element.isNull() ? anchorId : sourceAnchor.element.attribute(QStringLiteral("id"));
        const auto editorAnchors = source->ids.value(editorAnchorId);
        if (!editorAnchorId.isEmpty() && editorAnchors.size() == 1 && editorAnchors.first().element.attribute(QStringLiteral("id")) == editorAnchorId) {
            finding.row.insert(QStringLiteral("sourceEditor"),
                               QVariantMap{{QStringLiteral("fileName"), localPath(finding.row.value(QStringLiteral("sourceBookFile")).toString())},
                                           {QStringLiteral("bookId"), finding.row.value(QStringLiteral("sourceEditorBookId"))},
                                           {QStringLiteral("anchorId"), editorAnchorId},
                                           {QStringLiteral("documentFile"), editorAnchors.first().file}});
        }
    }
    if (targetLocation.isEmpty()) {
        finding.add(QStringLiteral("empty-target"), i18n("The reference has no target location."));
    } else if (target && target->isPdf) {
        checkPdfTarget(*target, targetLocation, finding);
    } else if (target) {
        const auto targetUrl = resolveLocation(*target, targetLocation, finding);
        checkTarget(*target, targetUrl, finding);
    }
}

static QVariantMap
inspectReferences(const QString &fileName, const QList<BookEntry> &books, const QVariantList &references, std::map<QString, std::unique_ptr<BookIndex>> &cache)
{
    const QString selectedPath = localPath(fileName);
    const QString selectedHash = fileDigest(selectedPath);
    QHash<QString, QString> sourceFileHashes{{selectedPath, selectedHash}};
    QHash<QString, QList<int>> uniqueIds;
    QHash<QString, QList<int>> aliases;
    int selected = -1;
    for (int i = 0; i < books.size(); ++i) {
        const auto &book = books.at(i);
        if (localPath(book.filename) == selectedPath) {
            selected = i;
        }
        if (!normalizedId(book.uniqueIdentifier).isEmpty()) {
            uniqueIds[normalizedId(book.uniqueIdentifier)].append(i);
        }
        auto bookAliases = book.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts);
        bookAliases.append(book.filename);
        bookAliases.append(book.identifier);
        for (const auto &alias : bookAliases) {
            auto &matches = aliases[normalizedId(alias)];
            if (!matches.contains(i)) {
                matches.append(i);
            }
        }
    }
    auto matches = [&](const QString &id) {
        const QString normalized = normalizedId(id);
        return uniqueIds.contains(normalized) ? uniqueIds.value(normalized) : aliases.value(normalized);
    };
    auto includesSelectedBook = [&](const QList<int> &candidates) {
        for (const int candidate : candidates) {
            if (localPath(books.at(candidate).filename) == selectedPath) {
                return true;
            }
        }
        return false;
    };
    auto indexFor = [&](const QString &path) -> BookIndex & {
        auto &index = cache[path];
        if (!index) {
            index = std::make_unique<BookIndex>(path);
        }
        return *index;
    };
    QVariantList rows;
    int errors = 0;
    int warnings = 0;
    int passed = 0;
    int incoming = 0;
    int outgoing = 0;
    auto append = [&](Finding finding) {
        const QString status = finding.severity == 2 ? QStringLiteral("error") : finding.severity == 1 ? QStringLiteral("warning") : QStringLiteral("ok");
        errors += finding.severity == 2;
        warnings += finding.severity == 1;
        passed += finding.severity == 0;
        if (finding.messages.isEmpty()) {
            finding.messages.append(i18n("Source and target were found and are unambiguous."));
        }
        finding.row.insert(QStringLiteral("status"), status);
        finding.row.insert(QStringLiteral("messages"), finding.messages);
        finding.row.insert(QStringLiteral("codes"), finding.codes);
        rows.append(finding.row);
    };
    auto &selectedBook = indexFor(selectedPath);
    if (!selectedBook.error.isEmpty()) {
        Finding finding;
        finding.add(QStringLiteral("book-unreadable"), selectedBook.error, selectedBook.unverified ? 1 : 2);
        append(finding);
    }
    if (selected < 0) {
        Finding finding;
        finding.add(QStringLiteral("book-not-in-library"),
                    i18n("This book is not in the library. Incoming and outgoing database references could not be checked."),
                    1);
        append(finding);
    }
    QSet<QString> storedSources;
    QSet<QString> storedPdfTargets;
    auto removalFor = [&](const QString &anchorId) {
        if (selected < 0) {
            return QVariantMap();
        }
        const auto &entry = books.at(selected);
        const QString bookId = entry.uniqueIdentifier.isEmpty() ? entry.identifier.section(QLatin1Char(','), 0, 0).trimmed() : entry.uniqueIdentifier;
        if (bookId.isEmpty() || matches(bookId).size() != 1) {
            return QVariantMap();
        }
        return QVariantMap{{QStringLiteral("fileName"), selectedPath},
                           {QStringLiteral("fileHash"), selectedHash},
                           {QStringLiteral("bookId"), bookId},
                           {QStringLiteral("anchorId"), anchorId},
                           {QStringLiteral("format"), selectedBook.isPdf ? QStringLiteral("pdf") : QStringLiteral("epub")}};
    };
    for (const auto &value : references) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return {};
        }
        const auto reference = value.toMap();
        const QString sourceBookId = reference.value(QStringLiteral("sourceBookId")).toString();
        const QString targetBookId = reference.value(QStringLiteral("targetBookId")).toString();
        const auto sources = matches(sourceBookId);
        const auto targets = matches(targetBookId);
        const bool fromSelected = includesSelectedBook(sources);
        const bool toSelected = includesSelectedBook(targets);
        if (!fromSelected && !toSelected) {
            continue;
        }
        const QString anchorId = reference.value(QStringLiteral("sourceAnchorId")).toString();
        if (fromSelected) {
            // An excluded internal reference still has a database entry; do not report its anchor as orphaned.
            storedSources.insert(anchorId);
        }
        if (toSelected && selectedBook.isPdf) {
            const QString targetId = QJsonDocument::fromJson(reference.value(QStringLiteral("targetLocation")).toString().toUtf8())
                                         .object()
                                         .value(QStringLiteral("anchorId"))
                                         .toString();
            if (!targetId.isEmpty()) {
                storedPdfTargets.insert(targetId);
                storedPdfTargets.insert(targetId.startsWith(QStringLiteral("uuid_")) ? targetId.mid(5) : QStringLiteral("uuid_") + targetId);
            }
        }
        const bool sameBook = normalizedId(sourceBookId) == normalizedId(targetBookId)
            || (sources.size() == 1 && targets.size() == 1 && localPath(books.at(sources.first()).filename) == localPath(books.at(targets.first()).filename));
        if (sameBook) {
            continue;
        }
        Finding finding;
        finding.row = reference;
        if (!sourceBookId.isEmpty() && !anchorId.isEmpty()) {
            finding.row.insert(QStringLiteral("referenceEdit"), editableReference(reference));
        }
        const QString direction = fromSelected ? QStringLiteral("outgoing") : QStringLiteral("incoming");
        finding.row.insert(QStringLiteral("direction"), direction);
        outgoing += direction == QStringLiteral("outgoing");
        incoming += direction == QStringLiteral("incoming");
        validateStoredReference(finding, books, sources, targets, indexFor, &sourceFileHashes);
        append(finding);
    }
    for (const auto &anchor : selectedBook.pdf.anchors) {
        if (!anchor.id.trimmed().isEmpty() && (storedSources.contains(anchor.id) || storedPdfTargets.contains(anchor.id))) {
            continue;
        }
        Finding finding;
        finding.row = {{QStringLiteral("direction"), QStringLiteral("outgoing")},
                       {QStringLiteral("sourceBookFile"), selectedPath},
                       {QStringLiteral("sourceAnchorId"), anchor.id},
                       {QStringLiteral("sourcePage"), anchor.page + 1},
                       {QStringLiteral("sourceAnnotation"), anchor.annotation + 1}};
        ++outgoing;
        if (anchor.id.trimmed().isEmpty()) {
            finding.add(QStringLiteral("empty-source-anchor"), i18n("The PDF reference anchor has no anchor ID."));
        } else {
            finding.add(QStringLiteral("orphan-source-anchor"), i18n("The PDF reference anchor has no matching entry in the reference database."));
        }
        auto removal = removalFor(anchor.id);
        if (!removal.isEmpty() && !storedSources.contains(anchor.id)) {
            removal.insert(QStringLiteral("page"), anchor.page);
            removal.insert(QStringLiteral("annotation"), anchor.annotation);
            finding.row.insert(QStringLiteral("removal"), removal);
        }
        append(finding);
    }
    for (auto it = selectedBook.documents.cbegin(); it != selectedBook.documents.cend(); ++it) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return {};
        }
        if (!it->error.isEmpty()) {
            Finding finding;
            finding.row.insert(QStringLiteral("sourceFile"), it.key());
            finding.add(QStringLiteral("incomplete-cross-reference-scan"),
                        i18n("Cross-reference anchors could not be fully checked in %1: %2", it.key(), it->error));
            append(finding);
            continue;
        }
        int elementIndex = -1;
        for (const auto &element : documentElements(it->dom)) {
            ++elementIndex;
            if (!crossReferenceElement(element) || storedSources.contains(sourceId(element))) {
                continue;
            }
            Finding finding;
            finding.row = {{QStringLiteral("direction"), QStringLiteral("outgoing")},
                           {QStringLiteral("sourceBookFile"), selectedPath},
                           {QStringLiteral("sourceFile"), it.key()},
                           {QStringLiteral("sourceAnchorId"), sourceId(element)},
                           {QStringLiteral("sourceAnchorTitle"), element.text().simplified().left(240)},
                           {QStringLiteral("line"), element.lineNumber()}};
            ++outgoing;
            finding.add(QStringLiteral("orphan-source-anchor"), i18n("The EPUB reference anchor has no matching entry in the reference database."));
            auto removal = removalFor(sourceId(element));
            if (!removal.isEmpty()) {
                removal.insert(QStringLiteral("documentFile"), it.key());
                removal.insert(QStringLiteral("elementIndex"), elementIndex);
                finding.row.insert(QStringLiteral("removal"), removal);
            }
            append(finding);
        }
    }
    const QString message = i18n("%1 results: %2 OK, %3 errors, %4 not fully verified.", rows.size(), passed, errors, warnings);
    return {{QStringLiteral("fileName"), selectedPath},
            {QStringLiteral("fileHash"), selectedHash == fileDigest(selectedPath) ? selectedHash : QString()},
            {QStringLiteral("checkedAt"), QDateTime::currentDateTime().toString(Qt::ISODate)},
            {QStringLiteral("message"), message},
            {QStringLiteral("success"), errors == 0 && warnings == 0},
            {QStringLiteral("results"), rows},
            {QStringLiteral("errors"), errors},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("passed"), passed},
            {QStringLiteral("incoming"), incoming},
            {QStringLiteral("outgoing"), outgoing}};
}

QVariantMap BookReferenceCheck::inspect(const QString &fileName, const QList<BookEntry> &books, const QVariantList &references)
{
    std::map<QString, std::unique_ptr<BookIndex>> cache;
    return inspectReferences(fileName, books, references, cache);
}

bool BookReferenceCheck::canReplaceEpubIdentifiers(const QString &fileName,
                                                   const QStringList &bookIds,
                                                   const QVariantList &annotations,
                                                   const QVariantList &references)
{
    QSet<QString> ids;
    for (const auto &id : bookIds)
        if (!normalizedId(id).isEmpty())
            ids.insert(normalizedId(id));
    if (ids.isEmpty())
        return false;
    const auto before = fileDigest(fileName);
    BookIndex book(fileName);
    if (before.isEmpty() || book.isPdf || book.unverified || !book.error.isEmpty() || !book.complete())
        return false;
    const auto *root = book.container.rootDirectory();
    const auto primary = normalizedId(book.container.metadata(QStringLiteral("unique-identifier")).value(0));
    if (!root || root->entry(QStringLiteral("META-INF/signatures.xml")) || (ids.contains(primary) && root->entry(QStringLiteral("META-INF/encryption.xml"))))
        return false;
    auto anchorPresent = [&](QString anchor) {
        anchor = QUrl::fromPercentEncoding(anchor.trimmed().toUtf8());
        if (anchor.isEmpty())
            return true; // Missing positions cannot establish that changing the identity is safe.
        const QString base = anchor.startsWith(QStringLiteral("uuid_")) ? anchor.mid(5) : anchor;
        for (const auto &name : QStringList{base, QStringLiteral("uuid_") + base}) {
            if (book.ids.contains(name) || book.sources.contains(name) || book.ids.contains(name + QStringLiteral("_begin"))
                || book.ids.contains(name + QStringLiteral("_end")))
                return true;
        }
        return false;
    };
    for (const auto &value : annotations) {
        const auto note = value.toMap();
        if (!ids.contains(normalizedId(note.value(QStringLiteral("bookId")).toString())) || note.value(QStringLiteral("format")) == QStringLiteral("pdf"))
            continue;
        // Legacy CFIs require the reader; absence of a matching DOM ID is not sufficient.
        if (!note.value(QStringLiteral("cfiRange")).toString().isEmpty() || anchorPresent(note.value(QStringLiteral("anchorId")).toString()))
            return false;
    }
    for (const auto &value : references) {
        const auto reference = value.toMap();
        if (ids.contains(normalizedId(reference.value(QStringLiteral("sourceBookId")).toString()))
            && anchorPresent(reference.value(QStringLiteral("sourceAnchorId")).toString()))
            return false;
        if (!ids.contains(normalizedId(reference.value(QStringLiteral("targetBookId")).toString())))
            continue;
        const auto location = reference.value(QStringLiteral("targetLocation")).toString();
        if (location.trimmed().isEmpty())
            return false;
        Finding finding;
        const auto target = resolveLocation(book, location, finding);
        const QUrl url(location);
        if (finding.codes.contains(QStringLiteral("ambiguous-target")) && url.path().isEmpty() && url.hasFragment()
            && !book.ids.contains(url.fragment(QUrl::FullyDecoded)))
            continue;
        if (!finding.codes.isEmpty())
            return false;
        checkTarget(book, target, finding);
        if (!finding.codes.contains(QStringLiteral("missing-target-file")) && !finding.codes.contains(QStringLiteral("missing-target-anchor")))
            return false;
    }
    return !QThread::currentThread()->isInterruptionRequested() && fileDigest(fileName) == before;
}

QVariantList BookReferenceCheck::inspectStoredReferences(const QList<BookEntry> &books, const QVariantList &references, const Progress &progress)
{
    // Build the same identifier/alias lookup as the per-book check once, without
    // opening unreferenced library files or inspecting orphaned EPUB anchors.
    QHash<QString, QList<int>> uniqueIds, aliases;
    for (int i = 0; i < books.size(); ++i) {
        const auto &book = books.at(i);
        const auto id = normalizedId(book.uniqueIdentifier);
        if (!id.isEmpty()) uniqueIds[id].append(i);
        auto names = book.identifier.split(QLatin1Char(','), Qt::SkipEmptyParts);
        names.append(book.filename);
        names.append(book.identifier);
        for (const auto &name : names) {
            const auto alias = normalizedId(name);
            if (!alias.isEmpty() && !aliases[alias].contains(i)) aliases[alias].append(i);
        }
    }
    const auto matches = [&](const QString &id) {
        const auto normalized = normalizedId(id);
        return uniqueIds.contains(normalized) ? uniqueIds.value(normalized) : aliases.value(normalized);
    };
    std::map<QString, std::unique_ptr<BookIndex>> cache;
    const auto indexFor = [&](const QString &path) -> BookIndex & {
        auto &index = cache[path];
        if (!index) index = std::make_unique<BookIndex>(path);
        return *index;
    };
    QVariantList rows;
    for (int i = 0; i < references.size() && !QThread::currentThread()->isInterruptionRequested(); ++i) {
        const auto reference = references.at(i).toMap();
        const QString sourceId = reference.value(QStringLiteral("sourceBookId")).toString();
        const QString targetId = reference.value(QStringLiteral("targetBookId")).toString();
        const auto sources = matches(sourceId);
        const auto targets = matches(targetId);
        const int displayBook = sources.size() == 1 ? sources.first() : targets.size() == 1 ? targets.first() : -1;
        if (progress) progress(i, references.size(), displayBook < 0 ? QString() : books.at(displayBook).filename);
        const bool sameBook = normalizedId(sourceId) == normalizedId(targetId)
            || (sources.size() == 1 && targets.size() == 1
                && localPath(books.at(sources.first()).filename) == localPath(books.at(targets.first()).filename));
        if (sameBook && (!sources.isEmpty() || !targets.isEmpty())) continue;
        Finding finding;
        finding.row = reference;
        if (sources.isEmpty() && targets.isEmpty()) {
            finding.add(QStringLiteral("unassigned-reference"), i18n("Neither book of this cross-reference is in the library."));
        } else {
            validateStoredReference(finding, books, sources, targets, indexFor);
        }
        if (displayBook >= 0) {
            const auto &book = books.at(displayBook);
            finding.row.insert(QStringLiteral("fileName"), book.filename);
            finding.row.insert(QStringLiteral("bookTitle"), book.title);
            finding.row.insert(QStringLiteral("bookId"), book.uniqueIdentifier.isEmpty()
                ? book.identifier.section(QLatin1Char(','), 0, 0).trimmed() : book.uniqueIdentifier);
        }
        finding.row.insert(QStringLiteral("status"), finding.severity == 2 ? QStringLiteral("error")
            : finding.severity == 1 ? QStringLiteral("warning") : QStringLiteral("ok"));
        if (finding.messages.isEmpty()) finding.messages.append(i18n("Source and target were found and are unambiguous."));
        finding.row.insert(QStringLiteral("messages"), finding.messages);
        finding.row.insert(QStringLiteral("codes"), finding.codes);
        // A maintenance report is a read-only snapshot, never a repair plan.
        for (const auto &key : {"removal", "repairs", "referenceEdit", "sourceTitleEdit"})
            finding.row.remove(QString::fromLatin1(key));
        rows.append(finding.row);
        if (cache.size() > 64) cache.clear();
    }
    if (progress && !QThread::currentThread()->isInterruptionRequested()) progress(references.size(), references.size(), QString());
    return rows;
}

QVariantList BookReferenceCheck::inspectLibraryReferences(const QList<BookEntry> &books,
                                                          const QVariantList &references,
                                                          const QVariantList &annotations,
                                                          const Progress &progress)
{
    std::map<QString, std::unique_ptr<BookIndex>> cache;
    QVariantList rows;
    QSet<QByteArray> seen;
    // References absent from both sides of the library never appear in a per-book report.
    for (const auto &value : references) {
        const auto ref = value.toMap();
        if (matchingBooks(books, ref.value(QStringLiteral("sourceBookId")).toString()).isEmpty()
            && matchingBooks(books, ref.value(QStringLiteral("targetBookId")).toString()).isEmpty()) {
            auto row = ref;
            row.insert(QStringLiteral("status"), QStringLiteral("error"));
            row.insert(QStringLiteral("codes"), QStringList{QStringLiteral("unassigned-reference")});
            row.insert(QStringLiteral("messages"), QStringList{i18n("Neither book of this cross-reference is in the library.")});
            rows.append(row);
        }
    }
    for (int i = 0; i < books.size() && !QThread::currentThread()->isInterruptionRequested(); ++i) {
        const auto &book = books.at(i);
        if (progress)
            progress(i, books.size(), book.filename);
        const auto report = inspectReferences(book.filename, books, references, cache);
        if (report.value(QStringLiteral("fileHash")).toString().isEmpty() && QFileInfo(book.filename).isFile()
            && !QThread::currentThread()->isInterruptionRequested()) {
            rows.append(QVariantMap{{QStringLiteral("fileName"), book.filename},
                                    {QStringLiteral("status"), QStringLiteral("warning")},
                                    {QStringLiteral("codes"), QStringList{QStringLiteral("file-unverified")}},
                                    {QStringLiteral("messages"), QStringList{i18n("The file could not be read completely or changed during the check.")}}});
        }
        for (const auto &value : report.value(QStringLiteral("results")).toList()) {
            auto row = value.toMap();
            if (row.contains(QStringLiteral("sourceBookId"))) {
                const auto key =
                    QJsonDocument::fromVariant(QVariantList{row.value(QStringLiteral("sourceBookId")), row.value(QStringLiteral("sourceAnchorId"))})
                        .toJson(QJsonDocument::Compact);
                if (seen.contains(key))
                    continue;
                seen.insert(key);
            } else if (row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("orphan-source-anchor"))
                       && row.value(QStringLiteral("removal")).toMap().value(QStringLiteral("format")) == QStringLiteral("pdf")) {
                bool usedByNote = false;
                const QString id = row.value(QStringLiteral("sourceAnchorId")).toString();
                for (const auto &noteValue : annotations) {
                    const auto note = noteValue.toMap();
                    if (!matchingBooks(books, note.value(QStringLiteral("bookId")).toString()).contains(i))
                        continue;
                    QVariant location = note.value(QStringLiteral("location"));
                    if (location.metaType().id() == QMetaType::QString)
                        location = QJsonDocument::fromJson(location.toString().toUtf8()).toVariant();
                    usedByNote |=
                        note.value(QStringLiteral("anchorId")).toString() == id || location.toMap().value(QStringLiteral("anchorId")).toString() == id;
                }
                if (usedByNote)
                    continue;
            }
            row.insert(QStringLiteral("fileName"), book.filename);
            row.insert(QStringLiteral("bookTitle"), book.title);
            row.insert(QStringLiteral("bookId"),
                       book.uniqueIdentifier.isEmpty() ? book.identifier.section(QLatin1Char(','), 0, 0).trimmed() : book.uniqueIdentifier);
            // Maintenance results are snapshots, never executable repair plans.
            for (const auto &key : {"removal", "repairs", "referenceEdit", "sourceTitleEdit"})
                row.remove(QString::fromLatin1(key));
            rows.append(row);
        }
        // Bound DOM retention for large libraries; recent linked books remain cached.
        if (cache.size() > 64)
            cache.clear();
    }
    return rows;
}

QVariantList BookReferenceCheck::inspectLibraryAnnotations(const QList<BookEntry> &books, const QVariantList &annotations, const Progress &progress)
{
    std::map<QString, std::unique_ptr<BookIndex>> cache;
    QHash<QString, QPair<qint64, QDateTime>> versions;
    QVariantList rows;
    for (int i = 0; i < annotations.size() && !QThread::currentThread()->isInterruptionRequested(); ++i) {
        const auto note = annotations.at(i).toMap();
        const QString bookId = note.value(QStringLiteral("bookId")).toString();
        const QString anchorId = note.value(QStringLiteral("anchorId")).toString().trimmed();
        const auto matches = matchingBooks(books, bookId);
        Finding finding;
        finding.row = {{QStringLiteral("bookId"), bookId},
                       {QStringLiteral("annotationId"), note.value(QStringLiteral("annotationId"))},
                       {QStringLiteral("annotationBookId"), note.value(QStringLiteral("annotationBookId"), bookId)},
                       {QStringLiteral("cfiRange"), note.value(QStringLiteral("cfiRange"))},
                       {QStringLiteral("anchorId"), anchorId},
                       {QStringLiteral("text"), note.value(QStringLiteral("text"))},
                       {QStringLiteral("note"), note.value(QStringLiteral("note"))}};
        QString fileName;
        if (matches.size() != 1) {
            finding.add(QStringLiteral("annotation-book-unresolved"),
                        i18n("The remark's book is missing or ambiguous in the library."),
                        matches.isEmpty() ? 2 : 1);
        } else {
            const auto &book = books.at(matches.first());
            fileName = localPath(book.filename);
            finding.row.insert(QStringLiteral("fileName"), fileName);
            finding.row.insert(QStringLiteral("bookTitle"), book.title);
            auto &index = cache[fileName];
            if (!index) {
                const QFileInfo info(fileName);
                versions.insert(fileName, {info.size(), info.lastModified()});
                index = std::make_unique<BookIndex>(fileName);
            }
            if (!index->error.isEmpty() || !index->complete()) {
                finding.add(QStringLiteral("annotation-book-unreadable"),
                            i18n("The remark could not be verified because the book is unavailable or unreadable."),
                            1);
            } else if (index->isPdf) {
                QVariant location = note.value(QStringLiteral("location"));
                if (location.metaType().id() == QMetaType::QString)
                    location = QJsonDocument::fromJson(location.toString().toUtf8()).toVariant();
                const auto geometry = location.toMap();
                bool number = false;
                const double page = geometry.value(QStringLiteral("page")).toDouble(&number);
                if (note.value(QStringLiteral("format")).toString() != QStringLiteral("pdf") || !number || !std::isfinite(page) || std::floor(page) != page
                    || page < 0 || page >= index->pdf.pageCount) {
                    finding.add(QStringLiteral("annotation-page-invalid"), i18n("The PDF remark has an invalid or missing page."));
                }
                const auto rects = geometry.value(QStringLiteral("rects")).toList();
                bool valid = !rects.isEmpty();
                for (const auto &value : rects) {
                    const auto rect = value.toMap();
                    for (const auto &key : {"x", "y", "width", "height"}) {
                        bool numeric = false;
                        const double coordinate = rect.value(QString::fromLatin1(key)).toDouble(&numeric);
                        valid &= numeric && std::isfinite(coordinate) && coordinate >= 0 && coordinate <= 1;
                    }
                    const double x = rect.value(QStringLiteral("x")).toDouble(), y = rect.value(QStringLiteral("y")).toDouble();
                    const double w = rect.value(QStringLiteral("width")).toDouble(), h = rect.value(QStringLiteral("height")).toDouble();
                    valid &= w > 0 && h > 0 && x + w <= 1.000001 && y + h <= 1.000001;
                }
                if (!valid)
                    finding.add(QStringLiteral("annotation-geometry-invalid"), i18n("The PDF remark has invalid or missing highlight rectangles."));
            } else if (note.value(QStringLiteral("format")).toString() == QStringLiteral("pdf")) {
                finding.add(QStringLiteral("annotation-format-mismatch"), i18n("The remark format does not match the book."));
            } else if (!anchorId.isEmpty()) {
                auto anchors = index->ids.value(anchorId);
                if (anchors.isEmpty())
                    anchors = index->ids.value(anchorId.startsWith(QStringLiteral("uuid_")) ? anchorId.mid(5) : QStringLiteral("uuid_") + anchorId);
                const auto base = anchorId.startsWith(QStringLiteral("uuid_")) ? anchorId : QStringLiteral("uuid_") + anchorId;
                const bool range = anchors.isEmpty() && index->ids.value(base + QStringLiteral("_begin")).size() == 1
                    && index->ids.value(base + QStringLiteral("_end")).size() == 1;
                if (anchors.size() != 1 && !range)
                    finding.add(QStringLiteral("annotation-anchor-unresolved"), i18n("The remark anchor is missing or ambiguous: %1", anchorId));
            } else if (!note.value(QStringLiteral("cfiRange")).toString().isEmpty()) {
                finding.add(QStringLiteral("annotation-cfi-unverified"),
                            i18n("This older remark uses a CFI range; its position requires checking in the reader."),
                            1);
            } else {
                finding.add(QStringLiteral("annotation-position-missing"), i18n("The remark has no saved position."));
            }
            if (!index->isPdf && index->error.isEmpty() && index->complete()) {
                const QString remarkText = note.value(QStringLiteral("text")).toString();
                if (!remarkText.simplified().isEmpty()) {
                    if (!index->textCounts.contains(remarkText))
                        index->textCounts.insert(remarkText, index->container.countAnnotationText(remarkText));
                    finding.row.insert(QStringLiteral("textMatchCount"), index->textCounts.value(remarkText));
                }
            }
            const QFileInfo after(fileName);
            const auto before = versions.value(fileName);
            if (before.first != after.size() || before.second != after.lastModified())
                finding.add(QStringLiteral("file-changed"), i18n("The book changed during the check. Check it again."), 1);
        }
        if (progress)
            progress(i + 1, annotations.size(), fileName);
        finding.row.insert(QStringLiteral("status"),
                           finding.severity == 2       ? QStringLiteral("error")
                               : finding.severity == 1 ? QStringLiteral("warning")
                                                       : QStringLiteral("ok"));
        finding.row.insert(QStringLiteral("codes"), finding.codes);
        finding.row.insert(QStringLiteral("messages"), finding.messages.isEmpty() ? QStringList{i18n("Remark position found.")} : finding.messages);
        rows.append(finding.row);
        if (cache.size() > 64)
            cache.clear();
    }
    return rows;
}

namespace
{
QString xmlSource(const QDomNode &node)
{
    QString text;
    QTextStream stream(&text);
    node.save(stream, 2);
    return text;
}

// A unified diff of the displayed XML. One hunk spans the changed region; bound
// LCS work for heavily rewritten chapters and use a complete replacement there.
QString sourceDiff(const QString &imported, const QString &existing, const QString &importPath, const QString &existingPath)
{
    if (imported == existing)
        return {};
    auto lines = [](const QString &text) {
        if (text.isEmpty())
            return QStringList{};
        auto result = text.split(QLatin1Char('\n'));
        if (result.last().isEmpty())
            result.removeLast();
        return result;
    };
    const auto a = lines(imported), b = lines(existing);
    int prefix = 0, suffix = 0;
    while (prefix < a.size() && prefix < b.size() && a.at(prefix) == b.at(prefix))
        ++prefix;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix && a.at(a.size() - suffix - 1) == b.at(b.size() - suffix - 1))
        ++suffix;
    const int n = a.size() - prefix - suffix, m = b.size() - prefix - suffix;
    const int before = qMin(3, prefix), after = qMin(3, suffix), start = prefix - before;
    QString diff = QStringLiteral("--- Import/%1\n+++ Existing/%2\n@@ -%3,%4 +%5,%6 @@\n")
                       .arg(importPath, existingPath)
                       .arg(a.isEmpty() ? 0 : start + 1)
                       .arg(before + n + after)
                       .arg(b.isEmpty() ? 0 : start + 1)
                       .arg(before + m + after);
    auto append = [&](QChar sign, const QString &line) {
        diff += sign + line + QLatin1Char('\n');
    };
    for (int i = start; i < prefix; ++i)
        append(QLatin1Char(' '), a.at(i));
    if (qint64(n + 1) * (m + 1) <= 1000000) {
        QVector<int> lcs((n + 1) * (m + 1));
        auto at = [&](int i, int j) -> int & {
            return lcs[i * (m + 1) + j];
        };
        for (int i = n - 1; i >= 0; --i) {
            if (QThread::currentThread()->isInterruptionRequested())
                return {};
            for (int j = m - 1; j >= 0; --j)
                at(i, j) = a.at(prefix + i) == b.at(prefix + j) ? 1 + at(i + 1, j + 1) : qMax(at(i + 1, j), at(i, j + 1));
        }
        int i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a.at(prefix + i) == b.at(prefix + j)) {
                append(QLatin1Char(' '), a.at(prefix + i));
                ++i;
                ++j;
            } else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) {
                append(QLatin1Char('-'), a.at(prefix + i++));
            } else
                append(QLatin1Char('+'), b.at(prefix + j++));
        }
    } else {
        for (int i = 0; i < n; ++i)
            append(QLatin1Char('-'), a.at(prefix + i));
        for (int j = 0; j < m; ++j)
            append(QLatin1Char('+'), b.at(prefix + j));
    }
    for (int i = 0; i < after; ++i)
        append(QLatin1Char(' '), a.at(a.size() - suffix + i));
    return diff;
}

QDomElement contextElement(QDomElement element)
{
    const auto original = element;
    static const QSet<QString> blocks{QStringLiteral("p"),
                                      QStringLiteral("li"),
                                      QStringLiteral("td"),
                                      QStringLiteral("th"),
                                      QStringLiteral("h1"),
                                      QStringLiteral("h2"),
                                      QStringLiteral("h3"),
                                      QStringLiteral("h4"),
                                      QStringLiteral("h5"),
                                      QStringLiteral("h6"),
                                      QStringLiteral("blockquote"),
                                      QStringLiteral("figcaption")};
    for (int depth = 0; !element.isNull() && depth < 8; ++depth, element = element.parentNode().toElement()) {
        if (blocks.contains(elementName(element)))
            return element;
        if (elementName(element) == QStringLiteral("body"))
            break;
    }
    return original.parentNode().isElement() ? original.parentNode().toElement() : original;
}

QList<Anchor> comparisonAnchors(const BookIndex &book, const QString &id, const QString &path = {})
{
    auto anchors = book.ids.value(id);
    if (anchors.isEmpty())
        anchors = book.sources.value(id);
    if (!path.isEmpty()) {
        anchors.erase(std::remove_if(anchors.begin(),
                                     anchors.end(),
                                     [&](const Anchor &anchor) {
                                         return anchor.file != path;
                                     }),
                      anchors.end());
    }
    return anchors;
}

QVariantMap anchorComparison(const BookIndex &imported, const BookIndex &existing, const QString &id, const QString &kind, const QString &existingPath = {})
{
    QVariantMap hint{{QStringLiteral("anchorId"), id}, {QStringLiteral("kind"), kind}, {QStringLiteral("label"), id}};
    const auto originals = comparisonAnchors(existing, id, existingPath);
    if (originals.size() != 1) {
        hint.insert(QStringLiteral("message"), i18n("The anchor is missing or ambiguous in the existing edition too. No original HTML can be offered."));
        return hint;
    }
    const auto original = originals.first();
    const auto originalContext = contextElement(original.element);
    const auto originalHtml = xmlSource(original.element);
    const auto right = xmlSource(originalContext);
    hint.insert(QStringLiteral("existingDocument"), original.file);
    hint.insert(QStringLiteral("existingLine"), original.element.lineNumber());
    hint.insert(QStringLiteral("existingSource"), right.left(12000));
    hint.insert(QStringLiteral("sourceTruncated"), right.size() > 12000);
    if (originalHtml.size() <= 12000)
        hint.insert(QStringLiteral("anchorHtml"), originalHtml);
    auto candidates = comparisonAnchors(imported, id);
    bool matchedId = !candidates.isEmpty();
    if (candidates.isEmpty()) {
        const auto text = originalContext.text().simplified();
        const auto anchorText = original.element.text().simplified();
        for (auto doc = imported.documents.cbegin(); doc != imported.documents.cend(); ++doc) {
            if (QThread::currentThread()->isInterruptionRequested())
                return {};
            if (!doc->error.isEmpty())
                continue;
            for (const auto &element : documentElements(doc->dom)) {
                const auto context = element;
                if (elementName(context) != elementName(originalContext))
                    continue;
                const auto candidateText = context.text().simplified();
                if (candidateText.isEmpty() || (candidateText != text && (anchorText.size() < 16 || !candidateText.contains(anchorText))))
                    continue;
                const bool seen = std::any_of(candidates.cbegin(), candidates.cend(), [&](const Anchor &a) {
                    return a.file == doc.key() && a.element == context;
                });
                if (!seen)
                    candidates.append({doc.key(), context});
                if (candidates.size() > 16)
                    break;
            }
            if (candidates.size() > 16)
                break;
        }
    }
    if (candidates.size() == 1) {
        const auto candidate = candidates.first();
        const auto left = xmlSource(matchedId ? contextElement(candidate.element) : candidate.element);
        hint.insert(QStringLiteral("importDocument"), candidate.file);
        hint.insert(QStringLiteral("importLine"), candidate.element.lineNumber());
        hint.insert(QStringLiteral("importSource"), left.left(12000));
        hint.insert(QStringLiteral("sourceTruncated"), left.size() > 12000 || right.size() > 12000);
        hint.insert(QStringLiteral("match"), matchedId ? QStringLiteral("id") : QStringLiteral("text"));
        hint.insert(QStringLiteral("message"),
                    matchedId ? i18n("The anchor ID occurs in both editions. Compare its markup and surrounding text.")
                              : i18n("Possible insertion point found by matching text. Verify the passage before copying the original anchor."));
        if (left.size() <= 12000 && right.size() <= 12000)
            hint.insert(QStringLiteral("diff"), sourceDiff(left, right, candidate.file, original.file));
    } else {
        hint.insert(QStringLiteral("match"), candidates.isEmpty() ? QStringLiteral("missing") : QStringLiteral("ambiguous"));
        hint.insert(QStringLiteral("message"),
                    candidates.isEmpty() ? i18n("No matching passage was found in the import. Use the document comparison to locate it manually.")
                                         : i18n("Several matching passages were found in the import. No insertion point was selected automatically."));
    }
    return hint;
}

QVariantList editionDocuments(BookIndex &imported, BookIndex &existing, bool &limited)
{
    QStringList paths = imported.documents.keys();
    paths += existing.documents.keys();
    paths.removeDuplicates();
    paths.sort();
    QVariantList rows;
    int budget = 2000000;
    for (const auto &path : paths) {
        if (QThread::currentThread()->isInterruptionRequested())
            break;
        if (rows.size() >= 120 || budget <= 0) {
            limited = true;
            break;
        }
        auto text = [&](BookIndex &book) {
            const auto doc = book.documents.constFind(path);
            if (doc == book.documents.cend())
                return QString();
            return doc->error.isEmpty() ? xmlSource(doc->dom) : QString::fromUtf8(book.container.readData(path));
        };
        const auto left = text(imported), right = text(existing);
        if (left == right)
            continue;
        const auto maxSide = qMin(120000, budget / 2);
        const auto shownLeft = left.left(maxSide), shownRight = right.left(maxSide);
        const bool truncated = shownLeft.size() != left.size() || shownRight.size() != right.size();
        budget -= shownLeft.size() + shownRight.size();
        QStringList messages;
        for (const auto *book : {&imported, &existing}) {
            const auto doc = book->documents.constFind(path);
            if (doc != book->documents.cend() && !doc->error.isEmpty())
                messages.append(doc->error);
        }
        rows.append(QVariantMap{{QStringLiteral("label"), path},
                                {QStringLiteral("importDocument"), imported.documents.contains(path) ? path : QString()},
                                {QStringLiteral("existingDocument"), existing.documents.contains(path) ? path : QString()},
                                {QStringLiteral("importSource"), shownLeft},
                                {QStringLiteral("existingSource"), shownRight},
                                {QStringLiteral("sourceTruncated"), truncated},
                                {QStringLiteral("message"), messages.join(QLatin1Char('\n'))},
                                {QStringLiteral("diff"), truncated ? QString() : sourceDiff(left, right, path, path)}});
        limited |= truncated;
    }
    return rows;
}
}

QVariantMap BookReferenceCheck::inspectImport(const QString &bookId,
                                              const QString &fileName,
                                              const QList<BookEntry> &books,
                                              const QVariantList &references,
                                              const QVariantList &annotations,
                                              const QByteArray &registeredHash)
{
    auto workingBooks = books;
    const auto matches = matchingBooks(books, bookId);
    if (matches.size() != 1 || !useWorkingCopy(workingBooks, bookId, fileName))
        return {{QStringLiteral("success"), false},
                {QStringLiteral("message"), i18n("The EPUB working copy cannot be assigned to a unique book in the library.")}};
    const auto &book = books.at(matches.first());
    const auto originalPath = localPath(book.filename), importedPath = localPath(fileName);
    const auto originalHash = fileDigest(originalPath), importedHash = fileDigest(importedPath);
    auto report = inspect(importedPath, workingBooks, references);
    auto rows = report.value(QStringLiteral("results")).toList();
    QVariantList selectedNotes;
    for (const auto &value : annotations) {
        auto note = value.toMap();
        const auto owners = matchingBooks(books, note.value(QStringLiteral("bookId")).toString());
        if (owners.size() == 1 && owners.first() == matches.first()) {
            note.insert(QStringLiteral("bookId"), book.uniqueIdentifier);
            selectedNotes.append(note);
        }
    }
    auto noteRows = inspectLibraryAnnotations(workingBooks, selectedNotes);
    for (auto &value : noteRows) {
        auto row = value.toMap();
        row.insert(QStringLiteral("direction"), QStringLiteral("annotation"));
        rows.append(row);
    }
    report.insert(QStringLiteral("annotations"), selectedNotes.size());
    const auto unaugmentedRows = rows;
    QVariantMap comparison{{QStringLiteral("importFile"), importedPath}, {QStringLiteral("existingFile"), originalPath}};
    if (originalPath == importedPath
        || (!originalHash.isEmpty() && QFileInfo(originalPath).canonicalFilePath() == QFileInfo(importedPath).canonicalFilePath())) {
        comparison.insert(QStringLiteral("message"),
                          i18n("The import and existing edition point to the same file. The previous source text is not available for comparison."));
    } else {
        BookIndex imported(importedPath), existing(originalPath);
        if (!imported.error.isEmpty() || !existing.error.isEmpty() || imported.isPdf || existing.isPdf) {
            comparison.insert(QStringLiteral("message"), i18n("Both editions must be readable EPUB files for a source comparison."));
        } else {
            comparison.insert(QStringLiteral("available"), true);
            comparison.insert(QStringLiteral("message"),
                              !registeredHash.isEmpty() && originalHash.toLatin1() != registeredHash
                                  ? i18n("The existing edition differs from its registered state. Check that it is the intended reference edition.")
                                  : i18n("Import on the left, existing edition on the right. The comparison does not change either file."));
            bool limited = false;
            comparison.insert(QStringLiteral("documents"), editionDocuments(imported, existing, limited));
            comparison.insert(QStringLiteral("limited"), limited);
            auto isSelected = [&](const QVariant &id) {
                const auto found = matchingBooks(books, id.toString());
                return found.size() == 1 && found.first() == matches.first();
            };
            for (auto &value : rows) {
                if (QThread::currentThread()->isInterruptionRequested())
                    return {};
                auto row = value.toMap();
                QVariantList hints;
                auto add = [&](const QString &id, const QString &kind, const QString &path = QString()) {
                    if (!id.isEmpty())
                        hints.append(anchorComparison(imported, existing, id, kind, path));
                };
                if (row.value(QStringLiteral("direction")) == QStringLiteral("annotation")) {
                    auto id = row.value(QStringLiteral("anchorId")).toString();
                    const auto base = id.startsWith(QStringLiteral("uuid_")) ? id : QStringLiteral("uuid_") + id;
                    // Offer recovery only for entirely missing anchors. Duplicates
                    // and partial marker pairs need inspection, not another marker.
                    if (!id.isEmpty() && !row.value(QStringLiteral("cfiRange")).toString().isEmpty()
                        && !row.value(QStringLiteral("text")).toString().simplified().isEmpty()
                        && imported.complete() && imported.ids.value(id).isEmpty() && imported.ids.value(base).isEmpty()
                        && imported.ids.value(base.mid(5)).isEmpty() && imported.ids.value(base + QStringLiteral("_begin")).isEmpty()
                        && imported.ids.value(base + QStringLiteral("_end")).isEmpty()) {
                        QVariantMap repair;
                        for (const auto &key : {"bookId", "annotationBookId", "annotationId", "anchorId", "cfiRange", "text"})
                            repair.insert(QString::fromLatin1(key), row.value(QString::fromLatin1(key)));
                        repair.insert(QStringLiteral("fileName"), importedPath);
                        repair.insert(QStringLiteral("fileHash"), importedHash);
                        row.insert(QStringLiteral("annotationRepair"), repair);
                    }
                    if (!id.isEmpty()) {
                        const auto alternate = id.startsWith(QStringLiteral("uuid_")) ? id.mid(5) : QStringLiteral("uuid_") + id;
                        if (existing.ids.value(id).isEmpty() && !existing.ids.value(alternate).isEmpty())
                            id = alternate;
                        const auto rangeBase = id.startsWith(QStringLiteral("uuid_")) ? id : QStringLiteral("uuid_") + id;
                        if (existing.ids.value(id).isEmpty() && existing.ids.contains(rangeBase + QStringLiteral("_begin"))
                            && existing.ids.contains(rangeBase + QStringLiteral("_end"))) {
                            add(rangeBase + QStringLiteral("_begin"), QStringLiteral("annotation"));
                            add(rangeBase + QStringLiteral("_end"), QStringLiteral("annotation"));
                        } else
                            add(id, QStringLiteral("annotation"));
                    }
                } else {
                    if (isSelected(row.value(QStringLiteral("sourceBookId"))))
                        add(row.value(QStringLiteral("sourceAnchorId")).toString(), QStringLiteral("source"));
                    if (isSelected(row.value(QStringLiteral("targetBookId")))) {
                        Finding unused;
                        const auto location = resolveLocation(existing, row.value(QStringLiteral("targetLocation")).toString(), unused);
                        add(location.fragment(QUrl::FullyDecoded), QStringLiteral("target"), location.path().mid(1));
                    }
                }
                auto repairs = row.value(QStringLiteral("repairs")).toList();
                for (const auto &hintValue : hints) {
                    const auto hint = hintValue.toMap();
                    if (hint.value(QStringLiteral("anchorHtml")).toString().isEmpty())
                        continue;
                    bool updated = false;
                    for (auto &repairValue : repairs) {
                        auto repair = repairValue.toMap();
                        if (repair.value(QStringLiteral("anchorId")) != hint.value(QStringLiteral("anchorId"))
                            || !isSelected(repair.value(QStringLiteral("bookId"))))
                            continue;
                        repair.insert(QStringLiteral("htmlSource"), hint.value(QStringLiteral("anchorHtml")));
                        repair.insert(QStringLiteral("fromExistingEdition"), true);
                        repair.insert(QStringLiteral("editionHint"), hint);
                        repairValue = repair;
                        updated = true;
                    }
                    if (!updated && row.value(QStringLiteral("direction")) == QStringLiteral("annotation")
                        && row.value(QStringLiteral("codes")).toStringList().contains(QStringLiteral("annotation-anchor-unresolved"))) {
                        repairs.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("annotation")},
                                                   {QStringLiteral("bookId"), book.uniqueIdentifier},
                                                   {QStringLiteral("fileName"), importedPath},
                                                   {QStringLiteral("documentFile"), hint.value(QStringLiteral("importDocument"))},
                                                   {QStringLiteral("anchorId"), hint.value(QStringLiteral("anchorId"))},
                                                   {QStringLiteral("htmlSource"), hint.value(QStringLiteral("anchorHtml"))},
                                                   {QStringLiteral("fromExistingEdition"), true}});
                    }
                }
                if (!repairs.isEmpty())
                    row.insert(QStringLiteral("repairs"), repairs);
                if (!hints.isEmpty())
                    row.insert(QStringLiteral("editionHints"), hints);
                value = row;
            }
        }
    }
    if (originalHash != fileDigest(originalPath) || importedHash != fileDigest(importedPath)) {
        rows = unaugmentedRows;
        comparison = {{QStringLiteral("available"), false},
                      {QStringLiteral("message"), i18n("An edition changed during the comparison. Run the import check again.")}};
    }
    int errors = 0, warnings = 0, passed = 0;
    for (const auto &value : rows) {
        const auto status = value.toMap().value(QStringLiteral("status")).toString();
        errors += status == QStringLiteral("error");
        warnings += status == QStringLiteral("warning");
        passed += status == QStringLiteral("ok");
    }
    report.insert(QStringLiteral("results"), rows);
    report.insert(QStringLiteral("errors"), errors);
    report.insert(QStringLiteral("warnings"), warnings);
    report.insert(QStringLiteral("passed"), passed);
    report.insert(QStringLiteral("success"), errors == 0 && warnings == 0);
    report.insert(QStringLiteral("message"), i18n("%1 results: %2 OK, %3 errors, %4 not fully verified.", rows.size(), passed, errors, warnings));
    report.insert(QStringLiteral("editionComparison"), comparison);
    return report;
}

#include "moc_bookreferencecheck.cpp"
