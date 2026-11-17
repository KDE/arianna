// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
namespace NoteRenderer
{
QVariantMap render(const QString &markdown);
QVariantMap renderDiscussion(const QVariantList &turns);
QStringList discussionQuestions(const QVariantMap &metadata);
}
