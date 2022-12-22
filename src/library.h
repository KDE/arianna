// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include <QAbstractListModel>
#include <QSqlTableModel>
#include <qobject.h>

class LibraryModel : public QSqlTableModel
{
    Q_OBJECT

public:
    enum ColorRoles {
        IdRole = Qt::UserRole + 1,
        AuthorRole,
        FileNameRole,
        TitleRole,
    };
    LibraryModel(QObject *parent = nullptr);

    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role) const override;

    Q_INVOKABLE bool addBook(const QString &fileName);
};