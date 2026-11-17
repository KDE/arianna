// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "bookstate.h"

#include <QList>
#include <QString>

#include <optional>

struct BookAnchorValidationIssue {
    QString type;
    QString bookId;
    QString anchorId;
    QString objectId;
    QString message;
};

struct BookSnapshot {
    bool success = false;
    QString errorMessage;

    QString bookId;
    QString filename;

    QByteArray textContentHash;
    QByteArray documentStateHash;
    QByteArray epubFileHash;

    std::optional<BookState> state;
};

struct BookCommitResult {
    bool success = false;
    bool invalidCandidate = false;
    bool validationFailed = false;

    QUuid oldStateId;
    QUuid newStateId;

    QByteArray textContentHash;
    QByteArray documentStateHash;
    QByteArray epubFileHash;

    std::optional<QUuid> createdAnchorId;
    bool unchanged = false;
    QList<BookAnchorValidationIssue> anchorValidationIssues;
    QString errorMessage;
};

class BookTruthStore
{
    BookCommitResult createAnchorInternal(const QString &bookId,
                                          const QString &cfiRange,
                                          const QString &expectedText,
                                          const QString &documentPath,
                                          int occurrence,
                                          const QByteArray &expectedFileHash,
                                          const QUuid &anchorUuid,
                                          const QUuid &expectedStateId = {});

public:
    BookSnapshot openBook(const QString &bookId) const;

    BookCommitResult createAnchor(const QString &bookId, const QString &cfiRange, const QString &expectedText = {}, const QUuid &expectedStateId = {});
    BookCommitResult createAnchorAtText(const QString &bookId, const QString &documentPath, int occurrence,
                                        const QString &expectedText, const QByteArray &expectedFileHash, const QUuid &anchorUuid);
    // allowExistingAnchorIssues permits only issues already present in the authoritative EPUB.
    BookCommitResult
    commitCandidateBookFile(const QString &bookId, const QString &candidateEpubPath, bool allowInvalidAnchors = false, bool allowExistingAnchorIssues = false,
                            const QByteArray &expectedFileHash = {}, bool metadataOnly = false);
    BookCommitResult commitActiveFileChangeIfNeeded(const QString &bookId, bool allowInvalidAnchors = false);
    // Read-only validation for corrections to an EPUB that has not been reimported yet.
    QString validateAnchorChanges(const QString &bookId, const QString &previousPath, const QString &candidatePath) const;
    // Read-only state preparation for coordinated file/database changes.
    std::optional<BookState> stateForFile(const QString &bookId, const QString &fileName) const;

    std::optional<BookState> currentState(const QString &bookId) const;
};
