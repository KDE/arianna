// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QVariantMap>
#include <functional>
#include <qqmlintegration.h>

class QThread;
struct BookEntry;

class LibraryMaintenance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString category READ category NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY changed)
    Q_PROPERTY(QVariantMap report READ report NOTIFY changed)

public:
    explicit LibraryMaintenance(QObject *parent = nullptr);
    ~LibraryMaintenance() override;
    bool running() const
    {
        return m_thread != nullptr;
    }
    QString category() const
    {
        return m_category;
    }
    int completed() const
    {
        return m_completed;
    }
    int total() const
    {
        return m_total;
    }
    QString currentFile() const
    {
        return m_currentFile;
    }
    QVariantMap report() const
    {
        return m_report;
    }
    Q_INVOKABLE void start(const QString &category);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void cleanCache(const QStringList &selectedFiles, bool reviewed = false);
    Q_INVOKABLE void discardWorkingCopy(const QString &fileName);
    Q_INVOKABLE bool saveReport(const QUrl &destination) const;
    Q_INVOKABLE QVariantMap findRemarkText(const QVariantMap &remark) const;
    Q_INVOKABLE QVariantMap anchorRemarkAtText(const QVariantMap &remark, const QVariantMap &match);
    Q_INVOKABLE QVariantMap combineFormats(const QVariantMap &plan);
    Q_INVOKABLE QVariantMap assignNewBookId(const QString &fileName, const QString &expectedIdentifier);
    Q_INVOKABLE QVariantMap deleteOrphanedBookStateHistory(const QString &bookId, const QString &stateId);
    Q_INVOKABLE QVariantMap deleteOrphanedBookCurrentState(const QString &bookId, const QString &stateId);
    Q_INVOKABLE QVariantMap deleteOrphanedBookStateEntry(const QString &tableName, const QString &bookId, const QString &stateId);
    Q_INVOKABLE QVariantMap deleteAllOrphanedBookStateHistory();
    Q_INVOKABLE QVariantMap deleteAllOrphanedBookStates();
    Q_INVOKABLE QVariantMap deleteDuplicate(const QString &fileName);
    Q_INVOKABLE QVariantMap deleteSharedBookIdFile(const QString &fileName, const QString &bookId, bool deleteFile);

    using Progress = std::function<void(int, int, const QString &)>;
    // Opens its own read-only SQLite connection; safe to call on a worker.
    static QVariantMap inspect(const QString &databaseFile,
                               const QString &category,
                               const QString &epubCommand = {},
                               const QString &pdfCommand = {},
                               const Progress &progress = {});

Q_SIGNALS:
    void changed();

private:
    void removeCacheFiles(const QStringList &selectedFiles, bool reviewed, bool discardWorkingCopy);
    QVariantMap removeBook(const BookEntry &book, bool deleteFile);
    QThread *m_thread = nullptr;
    QString m_category;
    int m_completed = 0;
    int m_total = 0;
    QString m_currentFile;
    QVariantMap m_report;
};
