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
public:
    BookSnapshot openBook(const QString &bookId) const;

    BookCommitResult createAnchor(const QString &bookId, const QString &cfiRange);
    BookCommitResult commitCandidateBookFile(const QString &bookId, const QString &candidateEpubPath, bool allowInvalidAnchors = false);
    BookCommitResult commitActiveFileChangeIfNeeded(const QString &bookId, bool allowInvalidAnchors = false);

    std::optional<BookState> currentState(const QString &bookId) const;
};
