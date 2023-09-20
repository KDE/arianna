// SPDX-FileCopyrightText: 2017 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <memory>
#include <optional>
#include <qtypes.h>

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

    /// @return a list of all known books in the database.
    QList<BookEntry> loadEntries();

    /// @return an entry matching the file name if it exists.
    std::optional<BookEntry> loadEntry(const QString &fileName);

    /// \brief Add a new book to the cache.
    /// \param entry The entry to add.
    void addEntry(const BookEntry &entry);

    /// \brief remove an entry by filename from the cache.
    /// \param entry the entry to remove.
    void removeEntry(const BookEntry &entry);

    /// \brief updateEntry update an entry by filename.
    /// \param fileName the filename of the entry to update.
    /// \param property the property/fieldname you wish to update.
    /// \param value a QVariant with the value.
    void updateEntry(const QString &fileName, const QString &property, const QVariant &value);

private:
    explicit BookDatabase(QObject *parent = nullptr);
    ~BookDatabase() override;

    class Private;
    std::unique_ptr<Private> d;
};
