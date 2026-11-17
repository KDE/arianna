// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "categoryentriesmodel.h"

#include <QHash>
#include <QObject>
#include <qqmlintegration.h>

/// Persistent user history of actual reader lifetimes, independent of visibility.
class ReaderHistory : public QObject
{
    Q_OBJECT
    QML_ELEMENT
public:
    explicit ReaderHistory(QObject *parent = nullptr);
    ~ReaderHistory() override;

    // Call only for library-managed readers; this never imports a file.
    Q_INVOKABLE void trackReader(QObject *reader, const QString &fileName);

    /// Select by closing time, then restore the selected sessions' opening order.
    static QList<BookEntry> lastClosedBooks(int count);

private:
    struct Session {
        QString fileName;
        QDateTime opened;
    };
    void closeReader(QObject *reader);
    void closeReaders();
    void saveClosed(const Session &session);

    QHash<QObject *, Session> m_readers;
    QDateTime m_lastOpened;
    QDateTime m_lastClosed;
};
