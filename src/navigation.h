// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>

#include "categoryentriesmodel.h"

class Navigation : public QObject
{
    Q_OBJECT

public:
    Navigation(QObject *parent = nullptr);

Q_SIGNALS:
    void openBook(const QString &fileName, const QString &locations, const QString &currentLocation);

    void openLibrary(const QString &title, CategoryEntriesModel *model, bool replace);

    void openSettings();
};
