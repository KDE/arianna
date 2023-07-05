// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "library.h"

LibraryModel::LibraryModel(QObject *parent)
{
}

QHash<int, QByteArray> LibraryModel::roleNames() const
{
    return {};
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const
{
    return {};
}

bool LibraryModel::addBook(const QString &fileName)
{
    return true;
}

#include "moc_library.cpp"
