// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "categoryentriesmodel.h"
#include "contentlist/contentlist.h"
#include <QQmlParserStatus>

/**
 * \brief Main catalogue model class.
 *
 * BookListModel extends CategoryEntriesModel, and is the main model that
 * handles book entries and the different categories that books can be in.
 *
 * It also extends QQmlParseStatus to ensure that the loading the cache of
 * books is postponed until the application UI has been painted at least once.
 *
 * BookListModel keeps track of which books there are, how they can be sorted
 * and how far the reader is in reading a specific book.
 *
 * It caches its entries in the BookDataBase.
 *
 * ContentModel is the model used to enable searching the collection, it is
 * typically a ContentList.
 */
class BookListModel : public CategoryEntriesModel, public QQmlParserStatus
{
    Q_OBJECT
    /// \brief count holds how many entries there are in the catalogue.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

    /// \brief The content model is an abstract list model that holds data to search through.
    Q_PROPERTY(ContentList *contentModel READ contentModel WRITE setContentModel NOTIFY contentModelChanged)

    /// \brief The "newly added" category entries model manages the newly added entries.
    Q_PROPERTY(CategoryEntriesModel *newlyAddedCategoryModel READ newlyAddedCategoryModel NOTIFY newlyAddedCategoryModelChanged)

    /// \brief The "author" category entries model manages the sorting of entries by author.
    Q_PROPERTY(CategoryEntriesModel *authorCategoryModel READ authorCategoryModel NOTIFY authorCategoryModelChanged)

    /// \brief The "series" category entries model managed the sorting of entry by series.
    Q_PROPERTY(CategoryEntriesModel *seriesCategoryModel READ seriesCategoryModel NOTIFY seriesCategoryModelChanged)

    /// \brief The "publisher" category entries model managed the sorting of entry by publisher.
    Q_PROPERTY(QObject *publisherCategoryModel READ publisherCategoryModel NOTIFY publisherCategoryModelChanged)

    /// \brief The "keyword" category entries model managed the sorting of entry by keyword.
    Q_PROPERTY(QObject *keywordCategoryModel READ keywordCategoryModel NOTIFY keywordCategoryModelChanged)
    /**
     * \brief The "folder" category entries model managed the sorting of entry by file system folder.
     */
    Q_PROPERTY(QObject *folderCategoryModel READ folderCategoryModel NOTIFY folderCategoryModelChanged)

    /// \brief cacheLoaded holds whether the database cache has been loaded..
    Q_PROPERTY(bool cacheLoaded READ cacheLoaded NOTIFY cacheLoadedChanged)
    Q_INTERFACES(QQmlParserStatus)
public:
    explicit BookListModel(QObject *parent = nullptr);
    ~BookListModel() override;

    /**
     * Inherited from QmlParserStatus, not implemented.
     */
    void classBegin() override{};
    /**
     * \brief triggers the loading of the cache.
     * Inherited from QmlParserStatus
     */
    void componentComplete() override;

    /// \brief Enum holding the different categories implemented.
    enum Grouping { GroupByNone = 0, GroupByRecentlyAdded, GroupByRecentlyRead, GroupByTitle, GroupByAuthor, GroupByPublisher };
    Q_ENUMS(Grouping)

    /// @return the contentModel. Used for searching.
    ContentList *contentModel() const;

    /// \brief set the ContentModel.
    /// @param newModel The new content model.
    void setContentModel(ContentList *newModel);

    /// @returns how many entries there are in the catalogue.
    int count() const;

    /// @return The categoryEntriesModel that manages the recently added entries.
    CategoryEntriesModel *newlyAddedCategoryModel() const;

    /// @return The categoryEntriesModel that manages the sorting of entries by author.
    CategoryEntriesModel *authorCategoryModel() const;

    /// \return The categoryEntriesModel that manages the sorting of entries by series.
    CategoryEntriesModel *seriesCategoryModel() const;

    /// \brief Returns the leaf model representing the series the entry with the
    /// passed URL is a part of
    ///
    /// Base assumption: A book is only part of one series. This is not always true,
    /// but not sure how to sensibly represent that.
    ///
    /// \param fileName the File Name of the entry to get the series of.
    Q_INVOKABLE CategoryEntriesModel *seriesModelForEntry(const QString &fileName);

    /// \return The categoryEntriesModel that manages the sorting of entries
    /// by publisher.
    CategoryEntriesModel *publisherCategoryModel() const;

    /// \return The categoryEntriesModel that manages the sorting of entries
    /// by keywords, names and genres.
    CategoryEntriesModel *keywordCategoryModel() const;

    /// \return The categoryEntriesModel that manages the sorting of entries by folder.
    CategoryEntriesModel *folderCategoryModel() const;

    /// \returns whether the cache is loaded from the database.
    bool cacheLoaded() const;

    /// \brief Update the data of a book at runtime
    ///
    /// This is used in to update totalPages and currentPage.
    ///
    /// \param fileName The filename to update the page for.
    /// \param property The property to update, can be "currentPage" or "totalPages".
    /// \param value The value to set it to.
    Q_INVOKABLE void setBookData(const QString &fileName, const QString &property, const QString &value);

    /// Delete a book from the model, and optionally delete the entry from file storage.
    /// \param fileName The filename of the book to remove.
    /// \param deleteFile Whether to also delete the file from the disk.
    Q_INVOKABLE void removeBook(const QString &fileName, bool deleteFile = false);

    /// \brief A list of the files currently known by the applications
    /// \returns a QStringList with paths to known books.
    Q_INVOKABLE QStringList knownBookFiles() const;

Q_SIGNALS:
    /// \brief Fires when the seriesCategoryModel has changed or finished initializing.
    void seriesCategoryModelChanged();
    /// \brief Fires when the publisherCategoryModel has changed or finished initializing.
    void publisherCategoryModelChanged();
    /// \brief Fires when the folderCategoryModel has changed or finished initializing.
    void folderCategoryModelChanged();
    /// \brief Fires when the cache is done loading.
    void cacheLoadedChanged();
    /// \brief Fires when the newlyAddedCategoryModel has changed or finished
    /// initializing.
    void newlyAddedCategoryModelChanged();
    /// \brief Fires when the content model has changed.
    void contentModelChanged();
    /// \brief Fires when the keywordCategoryModel has changed or finished initializing.
    void keywordCategoryModelChanged();
    /// \brief Fires when the count has changed.
    void countChanged();
    /// \brief Fires when the authorCategoryModel has changed or finished initializing.
    void authorCategoryModelChanged();

private:
    class Private;
    std::unique_ptr<Private> d;

    Q_SLOT void contentModelItemsInserted(QModelIndex index, int first, int last);
};
