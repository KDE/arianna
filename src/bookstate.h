// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

struct BookState {
    QUuid stateId;
    QString bookId;

    QByteArray textContentHash;
    QByteArray documentStateHash;
    QByteArray epubFileHash;
};
