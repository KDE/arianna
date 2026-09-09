// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace AriannaTrace
{
void initializeProcess(const QString &role, const QStringList &arguments = {}, bool enabled = false);
void event(const QString &name, const QVariantMap &fields = {});
void land(const QString &reason = {});

bool isEnabled();
QString flightId();
QString logFilePath();
QString shortId(const QString &value, int visibleCharacters = 8);
qint64 timeZeroUtcMs();
}
