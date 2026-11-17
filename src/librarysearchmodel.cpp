// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "librarysearchmodel.h"
#include "bookdatabase.h"
#include "categoryentriesmodel.h"
#include "epubcontainer.h"
#include "pdfsearchprocess.h"

#include <KLocalizedString>
#include <QCollator>
#include <QFileInfo>
#include <QHash>
#include <QThread>
#include <QUrl>
#include <algorithm>

namespace
{
bool interrupted()
{
    return QThread::currentThread()->isInterruptionRequested();
}

QVariantMap hit(const QString &location, const QString &section, const QString &before, const QString &text, const QString &after)
{
    return {{QStringLiteral("location"), location},
            {QStringLiteral("section"), section},
            {QStringLiteral("excerpt"), QString(before + text + after)},
            {QStringLiteral("markup"),
             QString(before.toHtmlEscaped() + QStringLiteral("<b>") + text.toHtmlEscaped() + QStringLiteral("</b>") + after.toHtmlEscaped())}};
}

QVariantList searchBook(const BookEntry &book, const QString &query, QString &error)
{
    QVariantList results;
    const QString suffix = QFileInfo(book.filename).suffix().toLower();
    if (suffix == QStringLiteral("epub")) {
        EPubContainer container(nullptr);
        QObject::connect(&container, &EPubContainer::errorOccured, &container, [&error](const QString &message) {
            error = message;
        });
        if (!container.openFile(book.filename)) {
            if (error.isEmpty())
                error = i18n("Unable to open this EPUB.");
            return {};
        }
        const auto matches = container.searchText(query, interrupted, &error);
        for (const auto &match : matches) {
            if (interrupted())
                break;
            results.append(hit(match.location, match.section, match.before, match.text, match.after));
        }
    } else if (suffix == QStringLiteral("pdf")) {
        const auto result = searchPdfInProcess(book.filename, query, interrupted);
        results = result.hits;
        error = result.error;
    } else {
        error = i18n("Full-text search is available for EPUB and PDF books.");
    }
    return results;
}
}

LibrarySearchModel::LibrarySearchModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

LibrarySearchModel::~LibrarySearchModel()
{
    if (m_thread) {
        m_thread->requestInterruption();
        m_thread->wait();
        delete m_thread;
    }
}

int LibrarySearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant LibrarySearchModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size() || role != Qt::UserRole)
        return {};
    return m_rows.at(index.row());
}

QHash<int, QByteArray> LibrarySearchModel::roleNames() const
{
    return {{Qt::UserRole, "result"}};
}

void LibrarySearchModel::start(const QString &query)
{
    if (running())
        return;
    beginResetModel();
    m_rows.clear();
    m_bookHits.clear();
    m_query = query.simplified();
    m_error.clear();
    m_cancelled = false;
    m_completed = m_total = m_bookCount = m_matchCount = m_failedCount = m_resultBooks = m_normalResultBooks = m_firstErrorRow = 0;
    endResetModel();
    if (m_query.isEmpty()) {
        Q_EMIT changed();
        return;
    }
    bool loaded = false;
    // Snapshot on the database's owning thread; the worker owns all document readers.
    auto books = BookDatabase::self().loadEntries(&loaded);
    if (!loaded) {
        m_error = i18n("Unable to load the library for full-text search.");
        Q_EMIT changed();
        return;
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::sort(books.begin(), books.end(), [&collator](const BookEntry &a, const BookEntry &b) {
        const int order = collator.compare(a.title, b.title);
        return order == 0 ? a.filename < b.filename : order < 0;
    });
    m_total = books.size();
    const QString needle = m_query;
    m_thread = QThread::create([this, books, needle] {
        for (const auto &book : books) {
            if (interrupted())
                break;
            QString error;
            const auto hits = searchBook(book, needle, error);
            const bool complete = !interrupted();
            QMetaObject::invokeMethod(
                this,
                [this, book, hits, error, complete] {
                    appendBook(book, hits, error);
                    if (complete)
                        ++m_completed;
                    Q_EMIT changed();
                },
                Qt::QueuedConnection);
            if (!complete)
                break;
        }
    });
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;
        Q_EMIT changed();
    });
    m_thread->start();
    Q_EMIT changed();
}

void LibrarySearchModel::cancel()
{
    if (!m_thread || m_cancelled)
        return;
    m_cancelled = true;
    m_thread->requestInterruption();
    Q_EMIT changed();
}

void LibrarySearchModel::toggleBook(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const auto heading = m_rows.at(row);
    if (!heading.value(QStringLiteral("heading")).toBool() || heading.value(QStringLiteral("count")).toInt() < 3)
        return;
    const auto &hits = m_bookHits.at(heading.value(QStringLiteral("bookIndex")).toInt());
    const bool collapsed = heading.value(QStringLiteral("collapsed")).toBool();
    const bool normalBook = row < m_firstErrorRow;
    if (collapsed) {
        beginInsertRows({}, row + 1, row + hits.size());
        m_rows.insert(row + 1, hits.size(), {});
        std::copy(hits.cbegin(), hits.cend(), m_rows.begin() + row + 1);
        m_rows[row].insert(QStringLiteral("collapsed"), false);
        if (normalBook)
            m_firstErrorRow += hits.size();
        endInsertRows();
    } else {
        beginRemoveRows({}, row + 1, row + hits.size());
        m_rows.remove(row + 1, hits.size());
        m_rows[row].insert(QStringLiteral("collapsed"), true);
        if (normalBook)
            m_firstErrorRow -= hits.size();
        endRemoveRows();
    }
    Q_EMIT dataChanged(index(row), index(row), {Qt::UserRole});
}

void LibrarySearchModel::appendBook(const BookEntry &book, const QVariantList &hits, const QString &error)
{
    if (hits.isEmpty() && error.isEmpty())
        return;
    QList<QVariantMap> rows;
    rows.reserve(hits.size());
    int hitNumber = 0;
    QHash<QString, int> occurrencesByLocation;
    for (const auto &value : hits) {
        auto row = value.toMap();
        row.insert(QStringLiteral("heading"), false);
        row.insert(QStringLiteral("filename"), book.filename);
        row.insert(QStringLiteral("number"), ++hitNumber);
        // PDF locations identify pages; distinguish multiple matches on the same page.
        row.insert(QStringLiteral("indexAtLocation"), occurrencesByLocation[row.value(QStringLiteral("location")).toString()]++);
        rows.append(row);
    }
    const bool failed = !error.isEmpty();
    const bool collapsed = hits.size() >= 3;
    const int first = failed ? m_rows.size() : m_firstErrorRow;
    const int visibleRows = 1 + (collapsed ? 0 : hits.size());
    const int separator = failed ? (m_failedCount ? 1 : (m_normalResultBooks ? 2 : 0)) : (m_normalResultBooks ? 1 : 0);
    QString thumbnail = book.thumbnail;
    if (QFileInfo(thumbnail).isAbsolute())
        thumbnail = QUrl::fromLocalFile(thumbnail).toString();
    QVariantMap heading{{QStringLiteral("heading"), true},
                        {QStringLiteral("bookIndex"), m_bookHits.size()},
                        {QStringLiteral("number"), failed ? m_resultBooks + 1 : m_normalResultBooks + 1},
                        {QStringLiteral("title"), book.title.isEmpty() ? QFileInfo(book.filename).completeBaseName() : book.title},
                        {QStringLiteral("author"), book.author.join(QStringLiteral(", "))},
                        {QStringLiteral("thumbnail"), thumbnail},
                        {QStringLiteral("count"), hits.size()},
                        {QStringLiteral("collapsed"), collapsed},
                        {QStringLiteral("separator"), separator},
                        {QStringLiteral("error"), error}};
    beginInsertRows({}, first, first + visibleRows - 1);
    m_bookHits.append(rows);
    m_rows.insert(first, visibleRows, {});
    m_rows[first] = heading;
    if (!collapsed)
        std::copy(rows.cbegin(), rows.cend(), m_rows.begin() + first + 1);
    if (failed)
        ++m_failedCount;
    else {
        ++m_normalResultBooks;
        m_firstErrorRow += visibleRows;
    }
    ++m_resultBooks;
    if (!hits.isEmpty())
        ++m_bookCount;
    m_matchCount += hits.size();
    endInsertRows();

    if (!failed && m_failedCount) {
        // New matches precede already visible errors. Refresh their display numbers and boundary rule.
        int bookNumber = m_normalResultBooks;
        for (int row = m_firstErrorRow; row < m_rows.size(); ++row) {
            if (!m_rows.at(row).value(QStringLiteral("heading")).toBool())
                continue;
            m_rows[row].insert(QStringLiteral("number"), ++bookNumber);
            if (row == m_firstErrorRow)
                m_rows[row].insert(QStringLiteral("separator"), 2);
            Q_EMIT dataChanged(index(row), index(row), {Qt::UserRole});
        }
    }
}
