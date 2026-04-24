// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <qqmlintegration.h>

#include "categoryentriesmodel.h"

class Navigation : public QObject
{
    Q_OBJECT
    QML_SINGLETON
    QML_ELEMENT

    Q_PROPERTY(QString bookServerToken READ bookServerToken CONSTANT)

public:
    explicit Navigation(QObject *parent = nullptr);

    QString bookServerToken() const;

Q_SIGNALS:
    void openBook(const QString &fileName, const QString &locations, const QString &currentLocation, const BookEntry &entry);

    void openLibrary(const QString &title, CategoryEntriesModel *model, bool replace);

    void openSettings();

private:
    QString m_bookServerToken;
};
