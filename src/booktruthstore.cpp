// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "booktruthstore.h"

#include "bookdatabase.h"
#include "categoryentriesmodel.h"
#include "epubcontainer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTemporaryFile>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace
{
struct BookHashes {
    bool success = false;
    QString errorMessage;
    QByteArray textContentHash;
    QByteArray documentStateHash;
    QByteArray epubFileHash;
};

struct FileReplacementBackup {
    QString targetPath;
    QString backupPath;
    bool hadOriginal = false;
};

struct AnchorScanResult {
    QSet<QString> anchorIds;
    QSet<QString> elementIds;
    QStringList errors;
};

struct AnchorValidationResult {
    bool success = false;
    QString errorMessage;
    QList<BookAnchorValidationIssue> issues;
};

static QMutex bookMutationMutex;

static QString uuidToDatabaseString(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

static QString anchorElementId(const QUuid &uuid)
{
    return QStringLiteral("uuid_") + uuidToDatabaseString(uuid);
}

static bool isUuidAnchorId(const QString &id)
{
    static const QRegularExpression uuidAnchorPattern(
        QStringLiteral(R"(^uuid_[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}(_begin|_end)?$)"));
    return uuidAnchorPattern.match(id).hasMatch();
}

static QString normalizedAnchorElementId(const QString &anchorId)
{
    const QString trimmed = anchorId.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("uuid_"))) {
        return trimmed;
    }

    return QStringLiteral("uuid_") + trimmed;
}

static bool hasAnchorElement(const AnchorScanResult &scan, const QString &anchorId)
{
    const QString normalized = normalizedAnchorElementId(anchorId);
    if (normalized.isEmpty()) {
        return false;
    }

    return scan.anchorIds.contains(normalized) || scan.anchorIds.contains(anchorId)
        || (scan.anchorIds.contains(normalized + QStringLiteral("_begin")) && scan.anchorIds.contains(normalized + QStringLiteral("_end")));
}

static QString referenceIssueKey(const QVariantMap &reference)
{
    return reference.value(QStringLiteral("sourceBookId")).toString() + QLatin1Char('\t') + reference.value(QStringLiteral("sourceAnchorId")).toString()
        + QLatin1Char('\t') + reference.value(QStringLiteral("targetBookId")).toString() + QLatin1Char('\t')
        + reference.value(QStringLiteral("targetLocation")).toString();
}

static BookAnchorValidationIssue
makeAnchorIssue(const QString &type, const QString &bookId, const QString &anchorId, const QString &objectId, const QString &message)
{
    BookAnchorValidationIssue issue;
    issue.type = type;
    issue.bookId = bookId;
    issue.anchorId = anchorId;
    issue.objectId = objectId;
    issue.message = message;
    return issue;
}

static QByteArray readFileBytes(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to open EPUB candidate: %1").arg(file.errorString());
        }
        return {};
    }

    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to read EPUB candidate: %1").arg(file.errorString());
        }
        return {};
    }

    return data;
}

static QString writeTemporaryEpub(const QString &targetPath, const QByteArray &data, QString *errorMessage)
{
    const QFileInfo targetInfo(targetPath);
    QDir targetDir = targetInfo.dir();
    if (!targetDir.exists() && !targetDir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to create EPUB target directory: %1").arg(targetDir.absolutePath());
        }
        return {};
    }

    QTemporaryFile tempFile(targetDir.filePath(targetInfo.fileName() + QStringLiteral(".XXXXXX")));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to create temporary EPUB: %1").arg(tempFile.errorString());
        }
        return {};
    }

    const QString tempPath = tempFile.fileName();
    if (tempFile.write(data) != data.size()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to write temporary EPUB: %1").arg(tempFile.errorString());
        }
        tempFile.close();
        QFile::remove(tempPath);
        return {};
    }

    tempFile.close();
    if (tempFile.error() != QFileDevice::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to finalize temporary EPUB: %1").arg(tempFile.errorString());
        }
        QFile::remove(tempPath);
        return {};
    }

    return tempPath;
}

static bool copyAnchorSafetyBackup(const QString &sourcePath, const QString &anchorId, QString *backupPath, QString *errorMessage)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to create anchor backup because EPUB does not exist: %1").arg(sourcePath);
        }
        return false;
    }

    QString safeAnchorId = anchorId.trimmed();
    safeAnchorId.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9._-])")), QStringLiteral("_"));
    if (safeAnchorId.isEmpty()) {
        safeAnchorId = QStringLiteral("anchor");
    }

    const QString suffix = sourceInfo.suffix().isEmpty() ? QStringLiteral("epub") : sourceInfo.suffix();
    const QString backupStem = sourceInfo.completeBaseName() + QStringLiteral(".before_anchoring_") + safeAnchorId;
    QDir directory = sourceInfo.dir();

    for (int attempt = 0; attempt < 1000; ++attempt) {
        const QString candidateName = backupStem + (attempt == 0 ? QString() : QStringLiteral("-%1").arg(attempt)) + QLatin1Char('.') + suffix;
        const QString candidatePath = directory.filePath(candidateName);
        if (QFileInfo::exists(candidatePath)) {
            continue;
        }

        if (!QFile::copy(sourceInfo.absoluteFilePath(), candidatePath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to create anchor backup: %1").arg(candidatePath);
            }
            return false;
        }

        if (backupPath) {
            *backupPath = candidatePath;
        }
        return true;
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("Unable to create unique anchor backup for EPUB: %1").arg(sourcePath);
    }
    return false;
}

static bool replaceFileWithBackup(const QString &replacementPath, const QString &targetPath, FileReplacementBackup &backup, QString *errorMessage)
{
    backup.targetPath = targetPath;
    backup.backupPath.clear();
    backup.hadOriginal = false;

    if (QFileInfo::exists(targetPath)) {
        backup.backupPath = targetPath + QStringLiteral(".bak-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(targetPath, backup.backupPath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to move existing EPUB aside: %1").arg(targetPath);
            }
            return false;
        }
        backup.hadOriginal = true;
    }

    if (!QFile::rename(replacementPath, targetPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to move EPUB replacement into place: %1").arg(targetPath);
        }
        if (backup.hadOriginal) {
            QFile::rename(backup.backupPath, targetPath);
        }
        QFile::remove(replacementPath);
        return false;
    }

    return true;
}

static bool rollbackFileReplacement(const FileReplacementBackup &backup, QString *errorMessage)
{
    if (backup.targetPath.isEmpty()) {
        return true;
    }

    bool rolledBack = true;
    QStringList errors;

    if (QFileInfo::exists(backup.targetPath) && !QFile::remove(backup.targetPath)) {
        rolledBack = false;
        errors.append(QStringLiteral("unable to remove new EPUB %1").arg(backup.targetPath));
    }

    if (backup.hadOriginal && !QFile::rename(backup.backupPath, backup.targetPath)) {
        rolledBack = false;
        errors.append(QStringLiteral("unable to restore previous EPUB %1").arg(backup.targetPath));
    }

    if (!rolledBack && errorMessage) {
        *errorMessage = errors.join(QStringLiteral("; "));
    }

    return rolledBack;
}

static void discardFileReplacementBackup(const FileReplacementBackup &backup)
{
    if (backup.hadOriginal && !backup.backupPath.isEmpty() && !QFile::remove(backup.backupPath)) {
        qWarning() << "Unable to remove EPUB replacement backup" << backup.backupPath;
    }
}

static bool isDocumentItem(const EpubItem &item)
{
    return item.mimetype == QByteArrayLiteral("application/xhtml+xml") || item.mimetype == QByteArrayLiteral("text/html")
        || item.mimetype == QByteArrayLiteral("application/x-dtbook+xml") || item.mimetype == QByteArrayLiteral("text/x-oeb1-document");
}

static QString elementLocalName(const QDomElement &element)
{
    const QString localName = element.localName().isEmpty() ? element.tagName() : element.localName();
    const qsizetype namespaceSeparator = localName.indexOf(QLatin1Char(':'));
    return (namespaceSeparator >= 0 ? localName.mid(namespaceSeparator + 1) : localName).toLower();
}

static bool isNonVisibleElement(const QDomElement &element)
{
    static const QSet<QString> names = {
        QStringLiteral("head"),
        QStringLiteral("meta"),
        QStringLiteral("link"),
        QStringLiteral("script"),
        QStringLiteral("style"),
        QStringLiteral("title"),
    };

    return names.contains(elementLocalName(element));
}

static void appendVisibleText(const QDomNode &node, QString &text)
{
    if (node.isText() || node.isCDATASection()) {
        text.append(node.nodeValue());
        text.append(QLatin1Char(' '));
        return;
    }

    const QDomElement element = node.toElement();
    if (!element.isNull() && isNonVisibleElement(element)) {
        return;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        appendVisibleText(child, text);
        child = child.nextSibling();
    }
}

static bool setDomContentWithNamespaces(QDomDocument &doc, const QByteArray &data)
{
    return static_cast<bool>(doc.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing));
}

static void scanAnchorElements(const QDomNode &node, AnchorScanResult &result, QHash<QString, QString> &firstLocation, const QString &documentPath)
{
    const QDomElement element = node.toElement();
    if (!element.isNull()) {
        const QString elementId = element.attribute(QStringLiteral("id")).trimmed();
        if (!elementId.isEmpty()) {
            result.elementIds.insert(elementId);
        }

        const QString dataRole = element.attribute(QStringLiteral("data-role")).trimmed();
        if (dataRole == QStringLiteral("anchor")) {
            const QString id = elementId;
            if (id.isEmpty()) {
                result.errors.append(QStringLiteral("Anchor without id in %1").arg(documentPath));
            } else {
                if (result.anchorIds.contains(id)) {
                    result.errors.append(QStringLiteral("Duplicate anchor id %1 in %2 and %3").arg(id, firstLocation.value(id), documentPath));
                } else {
                    firstLocation.insert(id, documentPath);
                    result.anchorIds.insert(id);
                }

                if (id.startsWith(QStringLiteral("uuid_")) && !isUuidAnchorId(id)) {
                    result.errors.append(QStringLiteral("Malformed UUID anchor id %1 in %2").arg(id, documentPath));
                }
            }
        }
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        scanAnchorElements(child, result, firstLocation, documentPath);
        child = child.nextSibling();
    }
}

static AnchorScanResult scanPersistentAnchors(EPubContainer &container)
{
    AnchorScanResult result;
    QHash<QString, QString> firstLocationById;
    const QHash<QString, EpubItem> manifestItems = container.manifestItems();
    for (auto it = manifestItems.cbegin(); it != manifestItems.cend(); ++it) {
        if (!isDocumentItem(it.value())) {
            continue;
        }

        const QByteArray data = container.readData(it.value().path);
        if (data.isEmpty()) {
            result.errors.append(QStringLiteral("Unable to read EPUB document %1").arg(it.value().path));
            continue;
        }

        QDomDocument doc;
        if (!setDomContentWithNamespaces(doc, data)) {
            result.errors.append(QStringLiteral("Unable to parse EPUB document %1").arg(it.value().path));
            continue;
        }

        scanAnchorElements(doc.documentElement(), result, firstLocationById, it.value().path);
    }

    return result;
}

static QString fragmentFromLocation(const QString &location)
{
    const int fragmentIndex = location.indexOf(QLatin1Char('#'));
    if (fragmentIndex < 0) {
        return {};
    }

    return QUrl::fromPercentEncoding(location.mid(fragmentIndex + 1).toUtf8()).trimmed();
}

static QStringList targetAnchorCandidateIds(const QVariantMap &reference)
{
    QStringList candidates;
    auto append = [&candidates](QString value) {
        value = value.trimmed();
        if (!value.isEmpty() && !candidates.contains(value)) {
            candidates.append(value);
        }
    };

    const QString targetLocation = reference.value(QStringLiteral("targetLocation")).toString().trimmed();
    if (!targetLocation.contains(QLatin1Char('/')) && !targetLocation.contains(QLatin1Char('#')) && !targetLocation.startsWith(QStringLiteral("epubcfi("))) {
        append(targetLocation);
    }
    append(fragmentFromLocation(targetLocation));
    return candidates;
}

static bool hasElementOrAnchor(const AnchorScanResult &scan, const QString &id)
{
    return scan.elementIds.contains(id) || hasAnchorElement(scan, id);
}

static AnchorValidationResult validateKnownAnchorsInBook(const QString &bookId, const QString &candidatePath)
{
    EPubContainer candidate(nullptr);
    if (!candidate.openFile(candidatePath)) {
        return {false, QStringLiteral("Unable to open EPUB for anchor validation: %1").arg(candidatePath), {}};
    }

    const AnchorScanResult candidateAnchors = scanPersistentAnchors(candidate);

    QList<BookAnchorValidationIssue> issues;
    for (const QString &error : candidateAnchors.errors) {
        issues.append(makeAnchorIssue(QStringLiteral("candidate-error"), bookId, QString(), QString(), error));
    }

    const QVariantList annotations = BookDatabase::self().loadAnnotations(bookId);
    for (const QVariant &annotationValue : annotations) {
        const QVariantMap annotation = annotationValue.toMap();
        const QString annotationId = annotation.value(QStringLiteral("annotationId")).toString().trimmed();
        const QString anchorId = annotation.value(QStringLiteral("anchorId")).toString().trimmed();
        if (!anchorId.isEmpty() && !hasAnchorElement(candidateAnchors, anchorId)) {
            issues.append(makeAnchorIssue(QStringLiteral("annotation"),
                                          bookId,
                                          anchorId,
                                          annotationId,
                                          QStringLiteral("Annotation anchor no longer exists in EPUB: %1").arg(anchorId)));
        }
    }

    const QVariantList references = BookDatabase::self().loadReferences(bookId);
    for (const QVariant &referenceValue : references) {
        const QVariantMap reference = referenceValue.toMap();
        const QString sourceAnchorId = reference.value(QStringLiteral("sourceAnchorId")).toString().trimmed();
        if (!sourceAnchorId.isEmpty() && !hasAnchorElement(candidateAnchors, sourceAnchorId)) {
            issues.append(makeAnchorIssue(QStringLiteral("reference-source"),
                                          bookId,
                                          sourceAnchorId,
                                          referenceIssueKey(reference),
                                          QStringLiteral("Reference source anchor no longer exists in EPUB: %1").arg(sourceAnchorId)));
        }
    }

    const QVariantList targetReferences = BookDatabase::self().loadReferencesTargeting(bookId);
    for (const QVariant &referenceValue : targetReferences) {
        const QVariantMap reference = referenceValue.toMap();
        const QString targetLocation = reference.value(QStringLiteral("targetLocation")).toString().trimmed();
        const QStringList targetCandidates = targetAnchorCandidateIds(reference);
        const bool hasTargetAnchor = std::any_of(targetCandidates.cbegin(), targetCandidates.cend(), [&candidateAnchors](const QString &candidate) {
            return hasElementOrAnchor(candidateAnchors, candidate);
        });
        if (!targetCandidates.isEmpty() && !hasTargetAnchor) {
            issues.append(makeAnchorIssue(QStringLiteral("reference-target"),
                                          bookId,
                                          targetCandidates.constFirst(),
                                          referenceIssueKey(reference),
                                          QStringLiteral("Reference target anchor no longer exists in EPUB: %1").arg(targetCandidates.constFirst())));
            continue;
        }

        const QString fragment = fragmentFromLocation(targetLocation);
        if (!fragment.isEmpty() && !hasElementOrAnchor(candidateAnchors, fragment)) {
            issues.append(makeAnchorIssue(QStringLiteral("reference-target-location"),
                                          bookId,
                                          fragment,
                                          referenceIssueKey(reference),
                                          QStringLiteral("Reference target location no longer exists in EPUB: %1").arg(targetLocation)));
        }
    }

    if (!issues.isEmpty()) {
        QStringList messages;
        for (const BookAnchorValidationIssue &issue : std::as_const(issues)) {
            messages.append(issue.message);
        }
        qWarning() << "Known anchors no longer match EPUB content for book" << bookId << messages;
        return {false, messages.join(QStringLiteral("; ")), issues};
    }

    return {true, {}, {}};
}

static QByteArray visibleTextDigestInput(const QByteArray &data)
{
    QDomDocument doc;
    if (!setDomContentWithNamespaces(doc, data)) {
        return data;
    }

    QString visibleText;
    appendVisibleText(doc.documentElement(), visibleText);
    visibleText.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    visibleText = visibleText.trimmed();
    return visibleText.toUtf8();
}

static void addHashField(QCryptographicHash &hash, const QByteArray &field)
{
    hash.addData(QByteArray::number(field.size()));
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(field);
    hash.addData(QByteArrayLiteral("\n"));
}

static QByteArray calculateFileHash(const QString &filename, QString *errorMessage = nullptr)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to open EPUB for file hashing: %1").arg(filename);
        }
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer;
    buffer.resize(64 * 1024);
    while (true) {
        const qint64 bytesRead = file.read(buffer.data(), buffer.size());
        if (bytesRead > 0) {
            hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(bytesRead)));
            continue;
        }

        if (bytesRead < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to read EPUB for file hashing: %1").arg(filename);
            }
            return {};
        }

        break;
    }

    return hash.result().toHex();
}

static QStringList documentItemIdsInHashOrder(const EPubContainer &container)
{
    QStringList itemIds = container.items();
    QStringList unorderedDocumentItemIds;
    const QHash<QString, EpubItem> manifestItems = container.manifestItems();
    for (auto it = manifestItems.cbegin(); it != manifestItems.cend(); ++it) {
        if (!itemIds.contains(it.key()) && isDocumentItem(it.value())) {
            unorderedDocumentItemIds.append(it.key());
        }
    }

    std::sort(unorderedDocumentItemIds.begin(), unorderedDocumentItemIds.end(), [&manifestItems](const QString &left, const QString &right) {
        return QString::localeAwareCompare(manifestItems.value(left).path, manifestItems.value(right).path) < 0;
    });
    itemIds.append(unorderedDocumentItemIds);

    return itemIds;
}

static BookHashes calculateHashes(const QString &filename)
{
    QString fileHashError;
    const QByteArray epubFileHash = calculateFileHash(filename, &fileHashError);
    if (epubFileHash.isEmpty()) {
        BookHashes hashes;
        hashes.errorMessage = fileHashError;
        return hashes;
    }

    EPubContainer container(nullptr);
    if (!container.openFile(filename)) {
        BookHashes hashes;
        hashes.errorMessage = QStringLiteral("Unable to open EPUB for state hashing: %1").arg(filename);
        return hashes;
    }

    QCryptographicHash textHash(QCryptographicHash::Sha256);
    textHash.addData(QByteArrayLiteral("arianna-epub-visible-text-sha256-v1\n"));

    const QHash<QString, EpubItem> manifestItems = container.manifestItems();
    for (const QString &itemId : documentItemIdsInHashOrder(container)) {
        const EpubItem item = manifestItems.value(itemId);
        if (!isDocumentItem(item)) {
            continue;
        }

        const QByteArray data = container.readData(item.path);
        if (data.isEmpty()) {
            BookHashes hashes;
            hashes.errorMessage = QStringLiteral("Unable to read EPUB document for state hashing: %1").arg(item.path);
            return hashes;
        }

        addHashField(textHash, item.path.toUtf8());
        addHashField(textHash, visibleTextDigestInput(data));
    }

    const QString documentStateHash = container.getContentHash();
    if (documentStateHash.isEmpty()) {
        BookHashes hashes;
        hashes.errorMessage = QStringLiteral("Unable to calculate EPUB document state hash: %1").arg(filename);
        return hashes;
    }

    BookHashes hashes;
    hashes.success = true;
    hashes.textContentHash = textHash.result().toHex();
    hashes.documentStateHash = documentStateHash.toLatin1();
    hashes.epubFileHash = epubFileHash;
    return hashes;
}

static BookState makeBookState(const QString &bookId, const BookHashes &hashes)
{
    BookState state;
    state.stateId = QUuid::createUuidV7();
    state.bookId = bookId;
    state.textContentHash = hashes.textContentHash;
    state.documentStateHash = hashes.documentStateHash;
    state.epubFileHash = hashes.epubFileHash;
    return state;
}
}

BookSnapshot BookTruthStore::openBook(const QString &bookId) const
{
    BookSnapshot snapshot;
    snapshot.bookId = bookId;

    const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    if (!entry) {
        snapshot.errorMessage = QStringLiteral("No book entry found for identifier: %1").arg(bookId);
        return snapshot;
    }

    snapshot.filename = QFileInfo(entry->filename).absoluteFilePath();
    const QFileInfo fileInfo(snapshot.filename);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        snapshot.errorMessage = QStringLiteral("EPUB file does not exist: %1").arg(snapshot.filename);
        return snapshot;
    }

    const BookHashes hashes = calculateHashes(fileInfo.absoluteFilePath());
    if (!hashes.success) {
        snapshot.errorMessage = hashes.errorMessage;
        return snapshot;
    }

    snapshot.textContentHash = hashes.textContentHash;
    snapshot.documentStateHash = hashes.documentStateHash;
    snapshot.epubFileHash = hashes.epubFileHash;

    std::optional<BookState> state = currentState(bookId);
    if (!state) {
        BookState initialState = makeBookState(bookId, hashes);
        if (!BookDatabase::self().saveBookState(initialState)) {
            snapshot.errorMessage = QStringLiteral("Unable to persist initial book state for: %1").arg(bookId);
            return snapshot;
        }

        state = initialState;
    }

    snapshot.state = state;
    snapshot.success = true;
    return snapshot;
}

BookCommitResult BookTruthStore::createAnchor(const QString &bookId, const QString &cfiRange)
{
    BookCommitResult result;

    if (bookId.isEmpty() || cfiRange.isEmpty()) {
        result.errorMessage = QStringLiteral("Unable to create annotation anchor without book id and CFI range");
        return result;
    }

    const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    if (!entry) {
        result.errorMessage = QStringLiteral("No book entry found for identifier: %1").arg(bookId);
        return result;
    }

    QMutexLocker locker(&bookMutationMutex);

    const BookSnapshot snapshot = openBook(bookId);
    if (!snapshot.success || !snapshot.state) {
        result.errorMessage = snapshot.errorMessage.isEmpty() ? QStringLiteral("No current state found for book: %1").arg(bookId) : snapshot.errorMessage;
        return result;
    }

    const BookState currentState = *snapshot.state;
    result.oldStateId = currentState.stateId;
    result.textContentHash = snapshot.textContentHash;
    result.documentStateHash = snapshot.documentStateHash;
    result.epubFileHash = snapshot.epubFileHash;

    const QString outputPath = QFileInfo(entry->filename).absoluteFilePath();
    const QString inputPath = outputPath;
    const QUuid anchorUuid = QUuid::createUuidV7();
    const QString elementId = anchorElementId(anchorUuid);

    QByteArray anchoredEpub;
    {
        EPubContainer container(nullptr);
        if (!container.openFile(inputPath)) {
            result.errorMessage = QStringLiteral("Unable to open EPUB for annotation anchor creation: %1").arg(inputPath);
            return result;
        }

        anchoredEpub = container.createAnnotationAnchoredEpub(cfiRange, elementId);
    }

    if (anchoredEpub.isEmpty()) {
        result.errorMessage = QStringLiteral("Unable to create annotation anchor for selected CFI range");
        return result;
    }

    QString temporaryError;
    const QString temporaryPath = writeTemporaryEpub(outputPath, anchoredEpub, &temporaryError);
    if (temporaryPath.isEmpty()) {
        result.errorMessage = temporaryError;
        return result;
    }

    const BookHashes newHashes = calculateHashes(temporaryPath);
    if (!newHashes.success) {
        QFile::remove(temporaryPath);
        result.errorMessage = newHashes.errorMessage;
        return result;
    }

    BookState newState = makeBookState(bookId, newHashes);

    QString safetyBackupPath;
    QString safetyBackupError;
    if (!copyAnchorSafetyBackup(outputPath, elementId, &safetyBackupPath, &safetyBackupError)) {
        QFile::remove(temporaryPath);
        result.errorMessage = safetyBackupError;
        return result;
    }
    qDebug() << "Created pre-anchor EPUB backup:" << safetyBackupPath;

    FileReplacementBackup backup;
    QString replacementError;
    if (!replaceFileWithBackup(temporaryPath, outputPath, backup, &replacementError)) {
        result.errorMessage = replacementError;
        return result;
    }

    if (!BookDatabase::self().saveBookState(newState)) {
        QString rollbackError;
        const bool rolledBack = rollbackFileReplacement(backup, &rollbackError);
        result.errorMessage = rolledBack ? QStringLiteral("Unable to persist annotation anchor state")
                                         : QStringLiteral("Unable to persist annotation anchor state; rollback failed: %1").arg(rollbackError);
        return result;
    }

    discardFileReplacementBackup(backup);

    result.success = true;
    result.newStateId = newState.stateId;
    result.textContentHash = newState.textContentHash;
    result.documentStateHash = newState.documentStateHash;
    result.epubFileHash = newState.epubFileHash;
    result.createdAnchorId = anchorUuid;
    return result;
}

BookCommitResult BookTruthStore::commitCandidateBookFile(const QString &bookId, const QString &candidateEpubPath, const bool allowInvalidAnchors)
{
    BookCommitResult result;

    if (bookId.isEmpty() || candidateEpubPath.isEmpty()) {
        result.invalidCandidate = true;
        result.errorMessage = QStringLiteral("Unable to commit EPUB candidate without book id and candidate path");
        return result;
    }

    const QFileInfo candidateInfo(candidateEpubPath);
    if (!candidateInfo.exists() || !candidateInfo.isFile()) {
        result.invalidCandidate = true;
        result.errorMessage = QStringLiteral("EPUB candidate does not exist: %1").arg(candidateEpubPath);
        return result;
    }

    QMutexLocker locker(&bookMutationMutex);

    const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    if (!entry) {
        result.errorMessage = QStringLiteral("No book entry found for identifier: %1").arg(bookId);
        return result;
    }

    const BookSnapshot snapshot = openBook(bookId);
    if (!snapshot.success || !snapshot.state) {
        result.errorMessage = snapshot.errorMessage.isEmpty() ? QStringLiteral("No current state found for book: %1").arg(bookId) : snapshot.errorMessage;
        return result;
    }

    const BookState currentState = *snapshot.state;
    result.oldStateId = currentState.stateId;
    result.textContentHash = snapshot.textContentHash;
    result.documentStateHash = snapshot.documentStateHash;
    result.epubFileHash = snapshot.epubFileHash;

    EPubContainer candidate(nullptr);
    if (!candidate.openFile(candidateInfo.absoluteFilePath())) {
        result.invalidCandidate = true;
        result.errorMessage = QStringLiteral("Unable to open EPUB candidate: %1").arg(candidateInfo.absoluteFilePath());
        return result;
    }

    const BookHashes newHashes = calculateHashes(candidateInfo.absoluteFilePath());
    if (!newHashes.success) {
        result.invalidCandidate = true;
        result.errorMessage = newHashes.errorMessage;
        return result;
    }

    if (currentState.epubFileHash == newHashes.epubFileHash) {
        result.success = true;
        result.unchanged = true;
        result.newStateId = currentState.stateId;
        return result;
    }

    AnchorValidationResult anchorValidation{true, {}, {}};
    if (currentState.documentStateHash != newHashes.documentStateHash) {
        anchorValidation = validateKnownAnchorsInBook(bookId, candidateInfo.absoluteFilePath());
        if (!anchorValidation.success && !allowInvalidAnchors) {
            result.validationFailed = true;
            result.errorMessage = anchorValidation.errorMessage;
            result.anchorValidationIssues = anchorValidation.issues;
            return result;
        }
    }

    QString readError;
    const QByteArray candidateData = readFileBytes(candidateInfo.absoluteFilePath(), &readError);
    if (candidateData.isEmpty()) {
        result.invalidCandidate = true;
        result.errorMessage = readError;
        return result;
    }

    const QString outputPath = QFileInfo(entry->filename).absoluteFilePath();
    QString temporaryError;
    const QString temporaryPath = writeTemporaryEpub(outputPath, candidateData, &temporaryError);
    if (temporaryPath.isEmpty()) {
        result.errorMessage = temporaryError;
        return result;
    }

    BookState newState = makeBookState(bookId, newHashes);

    FileReplacementBackup backup;
    QString replacementError;
    if (!replaceFileWithBackup(temporaryPath, outputPath, backup, &replacementError)) {
        result.errorMessage = replacementError;
        return result;
    }

    if (!BookDatabase::self().saveBookState(newState)) {
        QString rollbackError;
        const bool rolledBack = rollbackFileReplacement(backup, &rollbackError);
        result.errorMessage = rolledBack ? QStringLiteral("Unable to persist candidate EPUB state")
                                         : QStringLiteral("Unable to persist candidate EPUB state; rollback failed: %1").arg(rollbackError);
        return result;
    }

    discardFileReplacementBackup(backup);

    result.success = true;
    result.newStateId = newState.stateId;
    result.textContentHash = newState.textContentHash;
    result.documentStateHash = newState.documentStateHash;
    result.epubFileHash = newState.epubFileHash;
    result.anchorValidationIssues = anchorValidation.issues;
    return result;
}

BookCommitResult BookTruthStore::commitActiveFileChangeIfNeeded(const QString &bookId, const bool allowInvalidAnchors)
{
    BookCommitResult result;

    if (bookId.isEmpty()) {
        result.errorMessage = QStringLiteral("Unable to import active EPUB file without book id");
        return result;
    }

    QMutexLocker locker(&bookMutationMutex);

    const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    if (!entry) {
        result.errorMessage = QStringLiteral("No book entry found for identifier: %1").arg(bookId);
        return result;
    }

    const QString activePath = QFileInfo(entry->filename).absoluteFilePath();
    const QFileInfo activeInfo(activePath);
    if (!activeInfo.exists() || !activeInfo.isFile()) {
        result.invalidCandidate = true;
        result.errorMessage = QStringLiteral("EPUB file does not exist: %1").arg(activePath);
        return result;
    }

    QString fileHashError;
    const QByteArray activeFileHash = calculateFileHash(activePath, &fileHashError);
    if (activeFileHash.isEmpty()) {
        result.invalidCandidate = true;
        result.errorMessage = fileHashError;
        return result;
    }

    std::optional<BookState> currentState = BookDatabase::self().currentBookState(bookId);
    if (currentState && currentState->epubFileHash == activeFileHash) {
        result.success = true;
        result.unchanged = true;
        result.oldStateId = currentState->stateId;
        result.newStateId = currentState->stateId;
        result.textContentHash = currentState->textContentHash;
        result.documentStateHash = currentState->documentStateHash;
        result.epubFileHash = currentState->epubFileHash;
        return result;
    }

    EPubContainer candidate(nullptr);
    if (!candidate.openFile(activePath)) {
        result.invalidCandidate = true;
        result.errorMessage = QStringLiteral("Unable to open active EPUB file: %1").arg(activePath);
        return result;
    }

    const BookHashes newHashes = calculateHashes(activePath);
    if (!newHashes.success) {
        result.invalidCandidate = true;
        result.errorMessage = newHashes.errorMessage;
        return result;
    }

    AnchorValidationResult anchorValidation{true, {}, {}};
    if (!currentState || currentState->documentStateHash != newHashes.documentStateHash) {
        anchorValidation = validateKnownAnchorsInBook(bookId, activePath);
        if (!anchorValidation.success && !allowInvalidAnchors) {
            result.validationFailed = true;
            result.errorMessage = anchorValidation.errorMessage;
            result.anchorValidationIssues = anchorValidation.issues;
            if (currentState) {
                result.oldStateId = currentState->stateId;
                result.textContentHash = currentState->textContentHash;
                result.documentStateHash = currentState->documentStateHash;
                result.epubFileHash = currentState->epubFileHash;
            }
            return result;
        }
    }

    BookState newState = makeBookState(bookId, newHashes);
    if (currentState) {
        result.oldStateId = currentState->stateId;
    }

    if (!BookDatabase::self().saveBookState(newState)) {
        result.errorMessage = QStringLiteral("Unable to persist active EPUB file state");
        return result;
    }

    result.success = true;
    result.newStateId = newState.stateId;
    result.textContentHash = newState.textContentHash;
    result.documentStateHash = newState.documentStateHash;
    result.epubFileHash = newState.epubFileHash;
    result.anchorValidationIssues = anchorValidation.issues;
    return result;
}

std::optional<BookState> BookTruthStore::currentState(const QString &bookId) const
{
    return BookDatabase::self().currentBookState(bookId);
}
