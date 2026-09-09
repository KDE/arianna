// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

class AnnotationStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit AnnotationStore(QObject *parent = nullptr);

    Q_INVOKABLE QVariantList loadAnnotations(const QString &bookId) const;
    Q_INVOKABLE QString currentStateId(const QString &bookId) const;
    Q_INVOKABLE QVariantMap createAnchoredAnnotation(const QString &bookId, const QVariantMap &annotation, const QString &expectedStateId);
    Q_INVOKABLE void saveAnnotation(const QString &bookId, const QVariantMap &annotation);
    Q_INVOKABLE void removeAnnotation(const QString &bookId, const QString &annotationKey);

Q_SIGNALS:
    void annotationsChanged(const QString &bookId);
};
