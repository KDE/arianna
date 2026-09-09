// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "navigation.h"

#include "bookserverconfig.h"

#include <QByteArray>
#include <QUrl>
#include <QUuid>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

#include <mutex>

namespace
{
QString &webEngineSessionTokenStorage()
{
    static QString sessionToken;
    return sessionToken;
}

class AriannaWebRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit AriannaWebRequestInterceptor(const QString &sessionToken, QObject *parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent)
        , m_sessionToken(sessionToken)
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const QString scheme = url.scheme();

        if (scheme == QStringLiteral("qrc") || scheme == QStringLiteral("data") || scheme == QStringLiteral("blob") || scheme == QStringLiteral("about")
            || scheme == QStringLiteral("epub")) {
            return;
        }

        if (BookServerConfig::matchesBookServer(url)) {
            info.setHttpHeader(QByteArrayLiteral("X-Arianna-Session-Token"), m_sessionToken.toUtf8());
            return;
        }

        if (scheme == QStringLiteral("https") && url.host() == QStringLiteral("cdn.jsdelivr.net")) {
            return;
        }

        info.block(true);
    }

private:
    QString m_sessionToken;
};
}

Navigation::Navigation(QObject *parent)
    : QObject(parent)
    , m_bookServerToken(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

QString Navigation::bookServerToken() const
{
    return m_bookServerToken;
}

QString Navigation::webEngineSessionToken() const
{
    return webEngineSessionTokenStorage();
}

void Navigation::setWebEngineSessionToken(const QString &sessionToken)
{
    if (m_webEngineSessionToken == sessionToken && webEngineSessionTokenStorage() == sessionToken) {
        return;
    }

    m_webEngineSessionToken = sessionToken;
    webEngineSessionTokenStorage() = sessionToken;
    Q_EMIT webEngineSessionTokenChanged();
}

void Navigation::ensureWebEngineInitialized()
{
    static std::once_flag initializedFlag;
    std::call_once(initializedFlag, [] {
        auto *webProfile = QWebEngineProfile::defaultProfile();
        webProfile->setHttpCacheType(QWebEngineProfile::NoCache);
        webProfile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);

        const QString sessionToken = webEngineSessionTokenStorage();
        if (!sessionToken.isEmpty()) {
            webProfile->setUrlRequestInterceptor(new AriannaWebRequestInterceptor(sessionToken, webProfile));
        }
    });
}

#include "moc_navigation.cpp"
