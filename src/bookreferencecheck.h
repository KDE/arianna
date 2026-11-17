// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "categoryentriesmodel.h"

#include <QObject>
#include <QVariantMap>
#include <functional>
#include <qqmlintegration.h>

class QThread;

class BookReferenceCheck : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QVariantMap report READ report NOTIFY reportChanged)

public:
    explicit BookReferenceCheck(QObject *parent = nullptr);
    ~BookReferenceCheck() override;
    bool running() const;
    QVariantMap report() const;
    Q_INVOKABLE void start(const QString &fileName);
    Q_INVOKABLE void startWorkingCopy(const QString &bookId, const QString &fileName);
    Q_INVOKABLE QVariantMap removeOrphanAnchor(const QVariantMap &removal);
    Q_INVOKABLE QVariantMap restoreAnnotationAnchor(const QVariantMap &repair);
    Q_INVOKABLE QVariantList referenceTargetBooks(const QVariantMap &reference) const;
    Q_INVOKABLE QVariantMap validateReferenceTarget(const QVariantMap &reference, const QString &targetBookId, const QString &targetLocation) const;
    Q_INVOKABLE QVariantMap saveReferenceTarget(const QVariantMap &reference, const QString &targetBookId, const QString &targetLocation);
    Q_INVOKABLE QVariantMap saveReferenceTitle(const QVariantMap &edit, const QString &title);
    Q_INVOKABLE QVariantMap prepareReferenceRemoval(const QVariantMap &reference);
    Q_INVOKABLE QVariantMap removeReference(const QVariantMap &plan);

    // Purely read-only inspection of a snapshot; no access to the live database on the worker thread.
    static QVariantMap inspect(const QString &fileName, const QList<BookEntry> &books, const QVariantList &references);

    // Display-only hint after a caller has verified a missing EPUB target ID.
    static QVariantMap targetAnchorRepair(const QString &bookId, const QString &fileName, const QString &documentFile, const QString &anchorId);

    static QVariantMap inspectImport(const QString &bookId,
                                     const QString &fileName,
                                     const QList<BookEntry> &books,
                                     const QVariantList &references,
                                     const QVariantList &annotations,
                                     const QByteArray &registeredHash = {});

    using Progress = std::function<void(int, int, const QString &)>;
    /// Validate each stored cross-reference once, opening only its source and target books.
    static QVariantList inspectStoredReferences(const QList<BookEntry> &books, const QVariantList &references, const Progress &progress = {});
    static QVariantList
    inspectLibraryReferences(const QList<BookEntry> &books, const QVariantList &references, const QVariantList &annotations, const Progress &progress = {});
    static QVariantList inspectLibraryAnnotations(const QList<BookEntry> &books, const QVariantList &annotations, const Progress &progress = {});

    /// True only when no stored annotation or reference under these IDs is anchored in this EPUB.
    static bool canReplaceEpubIdentifiers(const QString &fileName,
                                         const QStringList &bookIds,
                                         const QVariantList &annotations,
                                         const QVariantList &references);

    /// Remove the library entry and its data, cleaning unused cross-reference
    /// markers in the remaining books with backups and rollback on failure.
    static QVariantMap removeBook(const BookEntry &entry);

Q_SIGNALS:
    void runningChanged();
    void reportChanged();

private:
    void startCheck(const QString &fileName, const QString &workingCopyBookId);
    QVariantMap referenceRemovalPlan(const QVariantMap &reference) const;
    QVariantMap m_referenceRemovalPlan;
    QString m_workingCopyBookId;
    QThread *m_thread = nullptr;
    QVariantMap m_report;
};
