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
    Q_PROPERTY(QString webEngineSessionToken READ webEngineSessionToken WRITE setWebEngineSessionToken NOTIFY webEngineSessionTokenChanged)

public:
    explicit Navigation(QObject *parent = nullptr);

    QString bookServerToken() const;
    QString webEngineSessionToken() const;
    void setWebEngineSessionToken(const QString &sessionToken);
    Q_INVOKABLE void ensureWebEngineInitialized();

Q_SIGNALS:
    void webEngineSessionTokenChanged();
    void openBook(const QString &fileName, const QString &locations, const QString &currentLocation, const BookEntry &entry, bool readOnly, bool noLibraryImport = false);

    void openLibrary(const QString &title, CategoryEntriesModel *model, bool replace);

    void openSettings();

private:
    QString m_bookServerToken;
    QString m_webEngineSessionToken;
};
