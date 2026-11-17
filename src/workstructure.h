// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class EPubContainer;

// The EPUB is authoritative. Snapshots/proposals are transient, and only an
// explicit reviewed commit creates a record. No analysis database is owned here.
namespace WorkStructure
{
QJsonArray terms();
// Presence check only: reads package metadata/record, without scanning source
// evidence. A present record still needs read()/snapshot() validation before use.
bool hasStoredRecord(EPubContainer &book);
QJsonObject snapshot(EPubContainer &book, const QString &bookId);
QJsonObject suggest(const QJsonObject &snapshot);
QJsonObject modelSchema(const QJsonObject &snapshot);
QJsonObject applyModel(const QJsonObject &snapshot, const QJsonObject &answer, QString *error);
QJsonObject confirmedRecord(const QJsonObject &snapshot, const QJsonObject &draft, QString *error);
QJsonObject read(EPubContainer &book, const QJsonObject &snapshot);
QJsonObject node(const QJsonObject &record, const QString &nodeId);
QJsonObject atLocation(const QJsonObject &record, const QString &hrefOrCfi);
QJsonObject resolve(const QJsonObject &record, const QString &text, const QString &role);
QByteArray candidate(EPubContainer &book, const QJsonObject &record, QString *error);
bool preservesSource(EPubContainer &before, EPubContainer &after, QString *error);
QString digits(QString text);
}
