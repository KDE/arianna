// SPDX-FileCopyrightText: 2026 Markus Hessler
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "bookserverconfig.h"

#include "config.h"

namespace
{
QString normalizedAddress(QString address)
{
    address = address.trimmed();
    if (address.startsWith(QLatin1Char('[')) && address.endsWith(QLatin1Char(']'))) {
        address = address.mid(1, address.size() - 2);
    }

    return address.isEmpty() ? Config::defaultBookServerAddressValue() : address;
}

QString hostForUrl(const QString &address)
{
    if (address.contains(QLatin1Char(':')) && !address.startsWith(QLatin1Char('[')) && !address.endsWith(QLatin1Char(']'))) {
        return QStringLiteral("[%1]").arg(address);
    }

    return address;
}
}

QString BookServerConfig::address()
{
    return normalizedAddress(Config::bookServerAddress());
}

quint16 BookServerConfig::port()
{
    const int configuredPort = Config::bookServerPort();
    if (configuredPort <= 0 || configuredPort > 65535) {
        return static_cast<quint16>(Config::defaultBookServerPortValue());
    }

    return static_cast<quint16>(configuredPort);
}

QString BookServerConfig::baseUrl()
{
    return baseUrl(port());
}

QString BookServerConfig::baseUrl(const quint16 actualPort)
{
    return QStringLiteral("http://%1:%2").arg(hostForUrl(address())).arg(actualPort);
}

QHostAddress BookServerConfig::listenAddress()
{
    const QString configuredAddress = address();
    if (configuredAddress.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return QHostAddress::LocalHost;
    }

    QHostAddress hostAddress;
    if (hostAddress.setAddress(configuredAddress)) {
        return hostAddress;
    }

    return QHostAddress::LocalHost;
}

bool BookServerConfig::matchesBookServer(const QUrl &url)
{
    const QString scheme = url.scheme();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return false;
    }

    return url.host().compare(address(), Qt::CaseInsensitive) == 0 && url.port() == port();
}
