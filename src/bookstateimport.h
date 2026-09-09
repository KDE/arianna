// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "editorsession.h"

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <QVariantList>

enum class BookStateImportStatus {
    Imported,
    Unchanged,
    InvalidEpub,
    MissingWorkingCopy,
    AnchorValidationFailed,
    CommitFailed,
};

struct BookStateImportResult {
    BookStateImportStatus status = BookStateImportStatus::CommitFailed;

    QUuid previousStateId;
    QUuid newStateId;
    QUuid currentStateId;

    QByteArray textContentHash;
    QByteArray documentStateHash;
    QByteArray epubFileHash;
    QByteArray importedFileHash;

    QVariantList anchorValidationIssues;
    QString errorMessage;
};

QString bookStateImportStatusName(BookStateImportStatus status);

class BookStateImport
{
public:
    BookStateImportResult importEditorWorkingCopy(EditorSession &session, bool allowInvalidAnchors = false) const;
};
