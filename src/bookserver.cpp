// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "bookserver.h"

using namespace Qt::StringLiterals;

#include <QFileInfo>

BookServer::BookServer()
{
    server.route(u"/book/"_s, [](const QHttpServerRequest &request) {
        const auto fileName = QUrl::fromPercentEncoding(request.query().queryItemValue(u"url"_s).toUtf8()).replace(QLatin1Char('+'), QLatin1Char(' '));
        QFileInfo fileInfo(fileName.mid(7));
        return QHttpServerResponse::fromFile(fileName.mid(7));
    });

    server.afterRequest([](QHttpServerResponse &&resp) {
        resp.setHeader("Access-Control-Allow-Origin", "*");
        return std::move(resp);
    });

    const auto port = server.listen(QHostAddress::Any, 45961);
    if (!port) {
        qWarning() << "Server failed to listen on a port.";
        return;
    }

    qWarning() << u"Running on http://127.0.0.1:%1/ (Press CTRL+C to quit)"_s.arg(port);
}
