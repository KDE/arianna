// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "tableofcontentmodel.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

TableOfContentModel::Item::Item(const QString &_title, const QString &_href, const QString &_id)
    : title(_title)
    , href(_href)
    , id(_id)
{
}

TableOfContentModel::TableOfContentModel(QObject *parent)
    : QAbstractListModel(parent)
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

QVariant TableOfContentModel::data(const QModelIndex &index, int role) const
{
    Q_ASSERT(checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid));

    const auto &item = m_items[index.row()];

    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return item.title;
    case HrefRole:
        return item.href;
    case IdRole:
        return item.id;
    default:
        Q_UNREACHABLE();
    }
}

int TableOfContentModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return m_items.size();
}

void TableOfContentModel::importFromJson(const QByteArray &json)
{
    auto doc = QJsonDocument::fromJson(json);
    Q_ASSERT(doc.isArray());

    const auto jsonEntries = doc.array();

    beginResetModel();
    m_items.clear();
    for (const auto &jsonEntry : jsonEntries) {
        const auto value = jsonEntry.toObject();
        m_items.emplace_back(value[QStringLiteral("label")].toString(), value[QStringLiteral("href")].toString(), value[QStringLiteral("id")].toString());
    }
    endResetModel();
}
