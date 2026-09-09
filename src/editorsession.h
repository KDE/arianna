// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUuid>

struct EditorSession {
    QUuid sessionId;
    QString bookId;

    QString authoritativeFilePath;
    QString workingCopyPath;

    QByteArray lastImportedFileHash;

    QDateTime createdAt;
    QDateTime lastImportedAt;

    qint64 editorProcessId = -1;
    bool active = false;
};
