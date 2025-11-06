// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <memory>
#include <qqmlintegration.h>

class TreeItem
{
public:
    explicit TreeItem(const QString &title, const QString &href, const QString &id, const QJsonArray &childs, TreeItem *parentItem = nullptr);

    void appendChild(std::unique_ptr<TreeItem> &&child);

    void clear();
    TreeItem *child(int row);
    int childCount() const;
    QVariant data(int role) const;
    int row() const;
    TreeItem *parentItem();

private:
    // Position in treee
    std::vector<std::unique_ptr<TreeItem>> m_childItems;
    TreeItem *m_parentItem;

    // Content
    const QString m_title;
    const QString m_href;
    const QString m_id;
};

class TableOfContentModel : public QAbstractItemModel
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit TableOfContentModel(QObject *parent = nullptr);

    enum ExtraRole {
        TitleRole = Qt::UserRole + 1,
        HrefRole,
        IdRole,
    };

    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;

    Q_INVOKABLE void importFromJson(const QByteArray &json);

private:
    std::unique_ptr<TreeItem> m_rootItem;
};