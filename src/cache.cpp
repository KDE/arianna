// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-LicenseIdentifer: LGPL-2.1-or-later

#include "cache.h"
#include <QDebug>
#include <QStandardPaths>
#include <QJsonDocument>
#include <KSharedConfig>
#include <KConfigGroup>

Cache::Cache(QObject *parent)
    : QObject(parent)
{}

Cache::~Cache() = default;

void Cache::saveLocations(const QVariantMap &locations)
{
    auto config = KSharedConfig::openConfig(QStringLiteral("arianna"), KConfig::FullConfig, QStandardPaths::CacheLocation);
    auto epubCache = config->group("epubCache");
    epubCache.writeEntry("entries", QJsonDocument::fromVariant(locations).toJson());
    epubCache.sync();
}

QJsonObject Cache::loadLocations() const
{
    const auto config = KSharedConfig::openConfig(QStringLiteral("arianna"), KConfig::FullConfig, QStandardPaths::CacheLocation);
    const auto epubCache = config->group("epubCache");
    const auto json = QJsonDocument::fromJson(epubCache.readEntry("entries", QByteArrayLiteral("{}")));
    return json.object();
}
