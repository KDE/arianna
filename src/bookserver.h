// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

#pragma once

#include <QHttpServer>
#include <QHttpServerResponse>
#include <QString>

class BookServer
{
public:
    explicit BookServer(const QString &token);

private:
    QHttpServer server;
};
