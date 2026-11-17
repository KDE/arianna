// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "categoryentriesmodel.h"
#include <QLockFile>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace MaintenanceFiles
{
QString sessionsRoot();
QString normalizedId(QString id);
QByteArray digest(const QString &file);
QVariantMap readJson(const QString &file);
bool writeJson(const QString &file, const QVariantMap &data);
QString processToken(qint64 pid);
bool processAlive(qint64 pid, const QString &token);
bool sessionActive(const QVariantMap &session);
bool workingCopyInUse(const QString &path, bool includeReviews = true);
QStringList sessionCopies();
QStringList conflictingCopies(const BookEntry &book);
QString anchorEditBlockReason(const BookEntry &book);
std::unique_ptr<QLockFile> lock();
QString manifestPath(const QString &workingCopy);
bool registerBackup(const QString &original, const QString &backup);
void completeBackup(const QString &backup);
QVariantList inspect(const QList<BookEntry> &books,
                     const QVariantList &states,
                     const QVariantList &history,
                     const std::function<void(int, int, const QString &)> &progress = {});
bool removeFile(const QVariantMap &plan);
}
