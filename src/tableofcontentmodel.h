// SPDX-FileCopyrightText: 2023 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QAbstractListModel>

class TableOfContentModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit TableOfContentModel(QObject *parent = nullptr);

    enum ExtraRole {
        TitleRole = Qt::UserRole + 1,
        HrefRole,
        IdRole,
    };

    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    Q_INVOKABLE void importFromJson(const QByteArray &json);

private:
    struct Item {
        explicit Item(const QString &title, const QString &href, const QString &id);
        QString title;
        QString href;
        QString id;
    };

    std::vector<Item> m_items;
};