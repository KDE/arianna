// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-LicenseIdentifer: LGPL-2.1-or-later

#include <QObject>
#include <QVariant>
#include <QJsonObject>

class Cache : public QObject
{
    Q_OBJECT

public:
    explicit Cache(QObject *parent = nullptr);
    ~Cache();

    Q_INVOKABLE void saveLocations(const QVariantMap &locations);
    Q_INVOKABLE QJsonObject loadLocations() const;
};