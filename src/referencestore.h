// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

class ReferenceStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit ReferenceStore(QObject *parent = nullptr);

    Q_INVOKABLE QVariantList loadReferences(const QString &bookId) const;
    Q_INVOKABLE QVariantMap loadReferenceBySource(const QString &sourceBookId, const QString &sourceAnchorId) const;
    Q_INVOKABLE void saveReference(const QVariantMap &reference);
    Q_INVOKABLE void removeReference(const QString &sourceBookId, const QString &sourceAnchorId);

Q_SIGNALS:
    void referencesChanged(const QString &bookId);
};
