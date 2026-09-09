// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "ariannatrace.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>
#include <qtenvironmentvariables.h>

Q_LOGGING_CATEGORY(ARIANNA_TRACE_LOG, "org.kde.arianna.trace", QtInfoMsg)

namespace
{
constexpr auto FlightIdEnv = "ARIANNA_FLIGHT_ID";
constexpr auto FlightT0Env = "ARIANNA_FLIGHT_T0_MS";
constexpr auto FlightLogPathEnv = "ARIANNA_FLIGHT_LOG_PATH";

QString s_flightId;
QString s_role;
QString s_logFilePath;
qint64 s_timeZeroUtcMs = 0;
quint64 s_sequence = 0;
bool s_initialized = false;
bool s_landed = false;
bool s_enabled = false;

QString defaultLogFilePath()
{
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString logRoot =
        basePath.isEmpty() ? QDir::home().filePath(QStringLiteral(".local/share/arianna/log")) : QDir(basePath).filePath(QStringLiteral("log"));
    return QDir(logRoot).filePath(QStringLiteral("flight-history.jsonl"));
}

QString roleOrFallback()
{
    return s_role.isEmpty() ? QStringLiteral("unknown") : s_role;
}

QString utcTimestamp(const qint64 msecs)
{
    return QDateTime::fromMSecsSinceEpoch(msecs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString localTimestamp(const qint64 msecs, const QTimeZone &timeZone)
{
    return QDateTime::fromMSecsSinceEpoch(msecs, timeZone).toString(Qt::ISODateWithMs);
}

void ensureInitialized()
{
    if (s_initialized) {
        return;
    }

    s_flightId = qEnvironmentVariable(FlightIdEnv);
    if (s_flightId.isEmpty()) {
        s_flightId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        qputenv(FlightIdEnv, s_flightId.toUtf8());
    }

    bool ok = false;
    s_timeZeroUtcMs = qgetenv(FlightT0Env).toLongLong(&ok);
    if (!ok || s_timeZeroUtcMs <= 0) {
        s_timeZeroUtcMs = QDateTime::currentMSecsSinceEpoch();
        qputenv(FlightT0Env, QByteArray::number(s_timeZeroUtcMs));
    }

    s_logFilePath = qEnvironmentVariable(FlightLogPathEnv);
    if (s_logFilePath.isEmpty()) {
        s_logFilePath = defaultLogFilePath();
        qputenv(FlightLogPathEnv, s_logFilePath.toUtf8());
    }

    s_initialized = true;
}

QJsonObject fieldsObject(const QVariantMap &fields)
{
    return QJsonObject::fromVariantMap(fields);
}

bool appendLine(const QString &path, const QByteArray &line)
{
    const QFileInfo fileInfo(path);
    QDir directory(fileInfo.absolutePath());
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }

    QByteArray payload = line;
    payload.append('\n');
    return file.write(payload) == payload.size();
}
}

void AriannaTrace::initializeProcess(const QString &role, const QStringList &arguments, const bool enabled)
{
    s_role = role.trimmed().isEmpty() ? QStringLiteral("unknown") : role.trimmed();
    s_enabled = enabled;
    if (!s_enabled) {
        return;
    }

    ensureInitialized();

    QVariantMap fields;
    fields.insert(QStringLiteral("arguments"), arguments);
    fields.insert(QStringLiteral("application"), QCoreApplication::applicationName());
    fields.insert(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
    fields.insert(QStringLiteral("historyFile"), s_logFilePath);
    event(QStringLiteral("arianna.process.start"), fields);
}

void AriannaTrace::event(const QString &name, const QVariantMap &fields)
{
    if (!s_enabled) {
        return;
    }

    ensureInitialized();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QTimeZone localTimeZone = QTimeZone::systemTimeZone();
    const QDateTime nowLocal = QDateTime::fromMSecsSinceEpoch(now, localTimeZone);
    QJsonObject object;
    object.insert(QStringLiteral("schema"), 1);
    object.insert(QStringLiteral("flightId"), s_flightId);
    object.insert(QStringLiteral("sequence"), QString::number(++s_sequence));
    object.insert(QStringLiteral("event"), name);
    object.insert(QStringLiteral("role"), roleOrFallback());
    object.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    object.insert(QStringLiteral("utcMs"), QString::number(now));
    object.insert(QStringLiteral("local"), localTimestamp(now, localTimeZone));
    object.insert(QStringLiteral("timeZone"), QString::fromUtf8(localTimeZone.id()));
    object.insert(QStringLiteral("utcOffsetSeconds"), nowLocal.offsetFromUtc());
    object.insert(QStringLiteral("timeZeroUtcMs"), QString::number(s_timeZeroUtcMs));
    object.insert(QStringLiteral("timeZeroUtc"), utcTimestamp(s_timeZeroUtcMs));
    object.insert(QStringLiteral("timeZeroLocal"), localTimestamp(s_timeZeroUtcMs, localTimeZone));
    object.insert(QStringLiteral("utc"), utcTimestamp(now));
    object.insert(QStringLiteral("elapsedMs"), now - s_timeZeroUtcMs);
    object.insert(QStringLiteral("data"), fieldsObject(fields));

    const QByteArray jsonLine = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!appendLine(s_logFilePath, jsonLine)) {
        qCWarning(ARIANNA_TRACE_LOG).noquote() << QStringLiteral("Unable to append Arianna flight history: %1").arg(s_logFilePath);
    }

    qCInfo(ARIANNA_TRACE_LOG).noquote().nospace() << nowLocal.toString(Qt::ISODateWithMs)
                                                  << QStringLiteral(" [+%1ms] ").arg(now - s_timeZeroUtcMs, 6, 10, QLatin1Char('0')) << name
                                                  << QStringLiteral(" ")
                                                  << QString::fromUtf8(QJsonDocument(fieldsObject(fields)).toJson(QJsonDocument::Compact));
}

void AriannaTrace::land(const QString &reason)
{
    if (!s_enabled || s_landed) {
        return;
    }

    s_landed = true;
    QVariantMap fields;
    if (!reason.trimmed().isEmpty()) {
        fields.insert(QStringLiteral("reason"), reason.trimmed());
    }
    event(QStringLiteral("arianna.process.land"), fields);
}

bool AriannaTrace::isEnabled()
{
    return s_enabled;
}

QString AriannaTrace::flightId()
{
    if (!s_enabled) {
        return {};
    }

    ensureInitialized();
    return s_flightId;
}

QString AriannaTrace::logFilePath()
{
    if (!s_enabled) {
        return {};
    }

    ensureInitialized();
    return s_logFilePath;
}

QString AriannaTrace::shortId(const QString &value, const int visibleCharacters)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const int length = qMax(4, visibleCharacters);
    return trimmed.left(length);
}

qint64 AriannaTrace::timeZeroUtcMs()
{
    if (!s_enabled) {
        return 0;
    }

    ensureInitialized();
    return s_timeZeroUtcMs;
}
