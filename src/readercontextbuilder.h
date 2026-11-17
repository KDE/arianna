// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QVariantMap>
#include <qqmlintegration.h>

// Stateless, JSON-compatible snapshots. No reader, transport or document ownership.
class ReaderContextBuilder : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    using QObject::QObject;
    Q_INVOKABLE QVariantMap build(const QVariantMap &metadata, const QVariantMap &selection, const QVariantMap &position) const;
    Q_INVOKABLE QVariantMap withOptions(const QVariantMap &snapshot, const QStringList &excludedFields, const QString &editedText) const;
    Q_INVOKABLE QString toJson(const QVariantMap &context) const;
};
