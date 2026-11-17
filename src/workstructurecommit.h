// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <functional>
class QObject;
namespace WorkStructureCommit
{
// Asynchronous EPUBCheck; publication always runs on the owning server thread.
void start(QObject *owner,
           const QString &bookId,
           const QString &path,
           const QJsonObject &snapshot,
           const QJsonObject &draft,
           const QString &expectedRevision,
           const QString &validator,
           std::function<void(const QJsonObject &)> completed);
}
