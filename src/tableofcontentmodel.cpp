// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "tableofcontentmodel.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

TreeItem::TreeItem(const QString &title, const QString &href, const QString &id, const QJsonArray &childs, TreeItem *parent)
    : m_parentItem(parent)
    , m_title(title)
    , m_href(href)
    , m_id(id)
{
    for (const auto &jsonEntry : childs) {
        const auto value = jsonEntry.toObject();
        auto child = std::make_unique<TreeItem>(value[QStringLiteral("label")].toString(),
                                                value[QStringLiteral("href")].toString(),
                                                value[QStringLiteral("id")].toString(),
                                                value[QStringLiteral("subitems")].toArray(),
                                                this);
        appendChild(std::move(child));
    }
}

void TreeItem::clear()
{
    m_childItems.clear();
}

void TreeItem::appendChild(std::unique_ptr<TreeItem> &&item)
{
    m_childItems.push_back(std::move(item));
}

TreeItem *TreeItem::child(int row)
{
    if (row < 0 || row >= m_childItems.size()) {
        return nullptr;
    }
    return m_childItems.at(row).get();
}

int TreeItem::childCount() const
{
    return m_childItems.size();
}

int TreeItem::row() const
{
    if (m_parentItem) {
        const auto it = std::find_if(m_parentItem->m_childItems.cbegin(), m_parentItem->m_childItems.cend(), [this](const std::unique_ptr<TreeItem> &treeItem) {
            return treeItem.get() == const_cast<TreeItem *>(this);
        });

        if (it != m_parentItem->m_childItems.cend()) {
            return std::distance(m_parentItem->m_childItems.cbegin(), it);
        }
        Q_ASSERT(false); // should not happen
        return -1;
    }

    return 0;
}

QVariant TreeItem::data(int role) const
{
    switch (role) {
    case Qt::DisplayRole:
    case TableOfContentModel::TitleRole:
        return m_title;
    case TableOfContentModel::HrefRole:
        return m_href;
    case TableOfContentModel::IdRole:
        return m_id;
    default:
        return {};
    }
};

TreeItem *TreeItem::parentItem()
{
    return m_parentItem;
}

TableOfContentModel::TableOfContentModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_rootItem(std::make_unique<TreeItem>(QString{}, QString{}, QString{}, QJsonArray{}))
{
}

QHash<int, QByteArray> TableOfContentModel::roleNames() const
{
    return {
        {TitleRole, "title"},
        {HrefRole, "href"},
        {IdRole, "id"},
    };
}

QModelIndex TableOfContentModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return {};
    }

    TreeItem *parentItem;

    if (!parent.isValid()) {
        parentItem = m_rootItem.get();
    } else {
        parentItem = static_cast<TreeItem *>(parent.internalPointer());
    }

    auto childItem = parentItem->child(row);
    if (childItem) {
        return createIndex(row, column, childItem);
    }
    return {};
}

QModelIndex TableOfContentModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return {};
    }

    auto childItem = static_cast<TreeItem *>(index.internalPointer());
    auto parentItem = childItem->parentItem();

    if (parentItem == m_rootItem.get()) {
        return {};
    }

    return createIndex(parentItem->row(), 0, parentItem);
}

int TableOfContentModel::rowCount(const QModelIndex &parent) const
{
    TreeItem *parentItem;
    if (parent.column() > 0) {
        return 0;
    }
    if (!parent.isValid()) {
        parentItem = m_rootItem.get();
    } else {
        parentItem = static_cast<TreeItem *>(parent.internalPointer());
    }

    return parentItem->childCount();
}

int TableOfContentModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return 1;
}

QVariant TableOfContentModel::data(const QModelIndex &index, int role) const
{
    Q_ASSERT(checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid));

    auto item = static_cast<TreeItem *>(index.internalPointer());

    return item->data(role);
}

void TableOfContentModel::importFromJson(const QByteArray &json)
{
    auto doc = QJsonDocument::fromJson(json);
    Q_ASSERT(doc.isArray());

    const auto jsonEntries = doc.array();

    beginResetModel();
    m_rootItem->clear();
    for (const auto &jsonEntry : jsonEntries) {
        const auto value = jsonEntry.toObject();
        m_rootItem->appendChild(std::make_unique<TreeItem>(value[QStringLiteral("label")].toString(),
                                                           value[QStringLiteral("href")].toString(),
                                                           value[QStringLiteral("id")].toString(),
                                                           value[QStringLiteral("subitems")].toArray(),
                                                           m_rootItem.get()));
    }
    endResetModel();
}
