// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "bookstateimport.h"

#include "booktruthstore.h"
#include "epubcontainer.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace
{
static bool addDeviceDataToHash(QIODevice &device, QCryptographicHash &hash)
{
    QByteArray buffer;
    buffer.resize(64 * 1024);
    while (true) {
        const qint64 bytesRead = device.read(buffer.data(), buffer.size());
        if (bytesRead > 0) {
            hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(bytesRead)));
            continue;
        }

        if (bytesRead < 0) {
            return false;
        }

        return device.atEnd();
    }
}

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!addDeviceDataToHash(file, hash)) {
        return {};
    }

    return hash.result().toHex();
}

static BookStateImportResult resultForStatus(const BookStateImportStatus status, const QString &errorMessage = QString())
{
    BookStateImportResult result;
    result.status = status;
    result.errorMessage = errorMessage;
    return result;
}

static QVariantMap validationIssueToMap(const BookAnchorValidationIssue &issue)
{
    QVariantMap map;
    map.insert(QStringLiteral("type"), issue.type);
    map.insert(QStringLiteral("bookId"), issue.bookId);
    map.insert(QStringLiteral("anchorId"), issue.anchorId);
    map.insert(QStringLiteral("objectId"), issue.objectId);
    map.insert(QStringLiteral("message"), issue.message);
    return map;
}

static QVariantList validationIssuesToList(const QList<BookAnchorValidationIssue> &issues)
{
    QVariantList list;
    for (const BookAnchorValidationIssue &issue : issues) {
        list.append(validationIssueToMap(issue));
    }
    return list;
}
}

QString bookStateImportStatusName(const BookStateImportStatus status)
{
    switch (status) {
    case BookStateImportStatus::Imported:
        return QStringLiteral("imported");
    case BookStateImportStatus::Unchanged:
        return QStringLiteral("unchanged");
    case BookStateImportStatus::InvalidEpub:
        return QStringLiteral("invalid-epub");
    case BookStateImportStatus::MissingWorkingCopy:
        return QStringLiteral("missing-working-copy");
    case BookStateImportStatus::AnchorValidationFailed:
        return QStringLiteral("anchor-validation-failed");
    case BookStateImportStatus::CommitFailed:
        return QStringLiteral("commit-failed");
    }

    return QStringLiteral("commit-failed");
}

BookStateImportResult BookStateImport::importEditorWorkingCopy(EditorSession &session, const bool allowInvalidAnchors) const
{
    const QFileInfo workingCopyInfo(session.workingCopyPath);
    if (session.bookId.isEmpty() || session.workingCopyPath.isEmpty() || !workingCopyInfo.exists() || !workingCopyInfo.isFile()) {
        return resultForStatus(BookStateImportStatus::MissingWorkingCopy, QStringLiteral("Editor working copy is missing"));
    }

    const QByteArray importedFileHash = fileHash(workingCopyInfo.absoluteFilePath());
    if (importedFileHash.isEmpty()) {
        return resultForStatus(BookStateImportStatus::MissingWorkingCopy, QStringLiteral("Unable to read editor working copy"));
    }

    BookStateImportResult result;
    result.importedFileHash = importedFileHash;

    if (!session.lastImportedFileHash.isEmpty() && session.lastImportedFileHash == importedFileHash) {
        result.status = BookStateImportStatus::Unchanged;
        return result;
    }

    EPubContainer candidate(nullptr);
    if (!candidate.openFile(workingCopyInfo.absoluteFilePath())) {
        result.status = BookStateImportStatus::InvalidEpub;
        result.errorMessage = QStringLiteral("Edited working copy is not a readable EPUB");
        return result;
    }

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(session.bookId);
    if (!snapshot.success || !snapshot.state) {
        result.status = BookStateImportStatus::CommitFailed;
        result.errorMessage = snapshot.errorMessage;
        return result;
    }

    result.currentStateId = snapshot.state->stateId;
    result.previousStateId = snapshot.state->stateId;

    const BookCommitResult commit = store.commitCandidateBookFile(session.bookId, workingCopyInfo.absoluteFilePath(), allowInvalidAnchors);
    result.textContentHash = commit.textContentHash;
    result.documentStateHash = commit.documentStateHash;
    result.epubFileHash = commit.epubFileHash;
    result.previousStateId = commit.oldStateId;
    result.newStateId = commit.newStateId;
    result.currentStateId = commit.newStateId.isNull() ? commit.oldStateId : commit.newStateId;
    result.anchorValidationIssues = validationIssuesToList(commit.anchorValidationIssues);

    if (!commit.success) {
        result.errorMessage = commit.errorMessage;
        if (commit.invalidCandidate) {
            result.status = BookStateImportStatus::InvalidEpub;
        } else if (commit.validationFailed) {
            result.status = BookStateImportStatus::AnchorValidationFailed;
        } else {
            result.status = BookStateImportStatus::CommitFailed;
        }
        return result;
    }

    session.lastImportedFileHash = importedFileHash;
    session.lastImportedAt = QDateTime::currentDateTimeUtc();

    result.status = commit.unchanged ? BookStateImportStatus::Unchanged : BookStateImportStatus::Imported;
    result.currentStateId = result.newStateId;
    return result;
}
