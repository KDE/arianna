// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QVariantMap>
#include <qqmlintegration.h>

class QThread;
struct BookEntry;

/// A flattened list of book headings and hits, so QML can virtualize long result lists.
class LibrarySearchModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int bookCount READ bookCount NOTIFY changed)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY changed)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY changed)
    Q_PROPERTY(QString query READ query NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit LibrarySearchModel(QObject *parent = nullptr);
    ~LibrarySearchModel() override;
    bool running() const
    {
        return m_thread != nullptr;
    }
    bool cancelled() const
    {
        return m_cancelled;
    }
    int completed() const
    {
        return m_completed;
    }
    int total() const
    {
        return m_total;
    }
    int bookCount() const
    {
        return m_bookCount;
    }
    int matchCount() const
    {
        return m_matchCount;
    }
    int failedCount() const
    {
        return m_failedCount;
    }
    QString query() const
    {
        return m_query;
    }
    QString error() const
    {
        return m_error;
    }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void start(const QString &query);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void toggleBook(int row);

Q_SIGNALS:
    void changed();

private:
    void appendBook(const BookEntry &book, const QVariantList &hits, const QString &error);
    QThread *m_thread = nullptr;
    QList<QVariantMap> m_rows;
    QList<QList<QVariantMap>> m_bookHits;
    QString m_query;
    QString m_error;
    bool m_cancelled = false;
    int m_completed = 0;
    int m_total = 0;
    int m_bookCount = 0;
    int m_matchCount = 0;
    int m_failedCount = 0;
    int m_resultBooks = 0;
    int m_normalResultBooks = 0;
    int m_firstErrorRow = 0;
};
