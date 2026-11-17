// SPDX-FileCopyrightText: 2017 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "bookstate.h"

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>

struct BookEntry;
/**
 * \brief A Class to hold a cache of known books to reduce the amount of time spent indexing.
 *
 * BookDatabase handles holding the conversion between SQL entry and
 * BookEntry structs.
 *
 * The BookEntry struct is defined in CategoryEntriesModel.
 */
class BookDatabase : public QObject
{
    Q_OBJECT
public:
    static BookDatabase &self()
    {
        static BookDatabase instance;
        return instance;
    }
    // ********************************************
    // *             Book Management              *
    // ********************************************

    /// @return a list of all known books in the database.
    QList<BookEntry> loadEntries(bool *success = nullptr);

    /// Location for maintenance's independent read-only database connection.
    QString databaseFileName() const;
    /// Persist a reviewed grouping without altering file identities or user data.
    bool combineBookFormats(const QVariantMap &plan);
    QVariantMap assignNewUniqueIdentifier(const QString &fileName, const QString &expectedIdentifier);

    /// @return an entry matching the file name if it exists.
    std::optional<BookEntry> loadEntry(const QString &fileName);
    std::optional<BookEntry> loadEntryByIdentifier(const QString &identifier);
    std::optional<BookEntry> loadEntryByUniqueIdentifier(const QString &uniqueIdentifier);

    /// Save reader history atomically, without importing or changing the book file.
    bool recordReaderClosed(const QString &fileName, const QDateTime &opened, const QDateTime &closed);

    /// \brief Add a new book to the cache.
    /// \param entry The entry to add.
    void addEntry(const BookEntry &entry);

    /// Import a new file only if none of its normalized book IDs is already registered.
    /// The identity check and insertion share one write transaction.
    QVariantMap importEntry(const BookEntry &entry, const QStringList &replacedBookIds = {});
    bool canChangeBookIdentifiers(const QStringList &bookIds, const QString &fileName = {});
    QVariantMap deleteOrphanedBookStateHistory(const QString &bookId, const QString &stateId);
    QVariantMap deleteOrphanedBookCurrentState(const QString &bookId, const QString &stateId);
    QVariantMap deleteOrphanedBookStateHistoryEntries(const QVariantList &entries);
    QVariantMap deleteOrphanedBookStateEntries(const QVariantList &entries);

    /// \brief Update all stored fields for an existing book.
    /// \param entry The entry to update.
    void updateEntry(const BookEntry &entry);

    /// Atomically remove a book, its annotations, incoming/outgoing references
    /// and current state. Identities shared with another library copy are kept.
    /// \param entry the entry to remove.
    /// \return whether removal succeeded; on failure no data is removed.
    bool removeEntry(const BookEntry &entry, const QList<BookState> &states = {}, const QVariantList *expectedReferences = nullptr);

    /// Normalized IDs belonging only to the removed library entry.
    static QSet<QString> identifiersForRemoval(const QString &fileName, const QList<BookEntry> &entries);

    /// \brief updateEntry update an entry by filename.
    /// \param fileName the filename of the entry to update.
    /// \param property the property/fieldname you wish to update.
    /// \param value a QVariant with the value.
    void updateEntry(const QString &fileName, const QString &property, const QVariant &value);

    // ********************************************
    // *   Annotation and Reference Management    *
    // ********************************************

    /// @return saved annotations for a book file.
    QVariantList loadAnnotations(const QString &bookId, bool *success = nullptr);

    /// Add or update an annotation for a book file.
    bool saveAnnotation(const QString &bookId, const QVariantMap &annotation);
    bool replaceAnnotationAnchor(const QString &bookId, const QString &annotationId, const QString &expectedAnchorId,
                                 const QString &expectedText, const QString &expectedCfiRange, const QString &newAnchorId);

    /// Remove an annotation for a book file.
    void removeAnnotation(const QString &bookId, const QString &annotationKey);

    /// @return saved references for a book file.
    QVariantList loadReferences(const QString &bookId);

    /// @return reference for given by source book and anchor.
    QVariantMap loadReferenceBySource(const QString &bookId, const QString &sourceAnchorId);

    /// Add or update an reference between two books
    void saveReference(const QVariantMap &reference);
    // Update only the destination, provided it still matches the inspected reference.
    bool updateReferenceTarget(const QVariantMap &expected, const QString &targetBookId, const QString &targetLocation, bool *conflict = nullptr);
    // Update only the title if the reference still matches the inspected source, destination and title.
    bool updateReferenceTitle(const QVariantMap &expected, const QString &title);

    void removeReference(const QString &sourceBookId, const QString &sourceAnchorId);
    // Atomically remove the inspected reference and publish the states of changed EPUB files.
    bool removeReferenceAndSaveStates(const QVariantMap &expected, const QList<BookState> &states);

    QVariantList loadReferencesTargeting(const QString &bookId);

    /// Snapshot for library-wide reference validation; reports database errors separately from an empty result.
    QVariantList loadAllReferences(bool *success = nullptr);

    /// @return the last known current state for a book if it has been initialized.
    std::optional<BookState> currentBookState(const QString &bookId);

    /// Store the current single authoritative state for a book.
    bool saveBookState(const BookState &state);

Q_SIGNALS:
    /// \brief Fires when the library database file was changed by this or another process.
    void databaseChanged();

private:
    explicit BookDatabase(QObject *parent = nullptr);
    ~BookDatabase() override;

    class Private;
    std::unique_ptr<Private> d;
};
