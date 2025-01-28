// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "bookserver.h"

using namespace Qt::StringLiterals;

#include <QFileInfo>
#include <QTcpServer>

BookServer::BookServer()
{
    server.route(u"/book"_s, [](const QHttpServerRequest &request) {
        // + is an standing for %20
        // fromPercentEncoded doesn't handle it but it needs to come first
        // otherwise we end up with %2B -> + -> ' ' which won't be the correct path
        const QByteArray spaceReplaced = request.query().queryItemValue(u"url"_s).toUtf8().replace('+', ' ');
        const auto fileName = QUrl::fromPercentEncoding(spaceReplaced);
        if (!fileName.endsWith(u".epub"_s)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }
        return QHttpServerResponse::fromFile(fileName.mid(7));
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    server.addAfterRequestHandler(&server, [](const QHttpServerRequest &, QHttpServerResponse &resp) {
        auto headers = resp.headers();
        headers.append("Access-Control-Allow-Origin", "*");
        resp.setHeaders(headers);
    });
#else
    server.afterRequest([](QHttpServerResponse &&resp) {
        resp.setHeader("Access-Control-Allow-Origin", "*");
        return std::move(resp);
    });
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto tcpserver = std::make_unique<QTcpServer>();
    if (!tcpserver->listen(QHostAddress::Any, 45961) || !server.bind(tcpserver.get())) {
        qWarning() << "Server failed to listen on a port.";
        return;
    }
    quint16 port = tcpserver->serverPort();
    auto s = tcpserver.release();
    Q_UNUSED(s);
#else
    const auto port = server.listen(QHostAddress::Any, 45961);
    if (!port) {
        qWarning() << "Server failed to listen on a port.";
        return;
    }
#endif

    qWarning() << u"Running on http://127.0.0.1:%1/ (Press CTRL+C to quit)"_s.arg(port);
}
