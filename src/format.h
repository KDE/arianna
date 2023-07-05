// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.0-or-later

#pragma once

#include <QObject>
#include <KFormat>

class Format : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE QString formatDuration(quint64 msecs) const;
};
