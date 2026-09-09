// SPDX-FileCopyrightText: 2026 Markus Hessler
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QHostAddress>
#include <QString>
#include <QUrl>

namespace BookServerConfig
{
QString address();
quint16 port();
QString baseUrl();
QString baseUrl(quint16 actualPort);
QHostAddress listenAddress();
bool matchesBookServer(const QUrl &url);
}
