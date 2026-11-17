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
    /// Stores a user-owned annotation without modifying its document. PDF notes
    /// use this path; EPUB range annotations retain their anchored workflow.
    Q_INVOKABLE QVariantMap createUserAnnotation(const QString &bookId, const QVariantMap &annotation);
    Q_INVOKABLE QVariantMap renderNote(const QString &markdown) const;
    Q_INVOKABLE QVariantMap renderDiscussion(const QVariantList &turns) const;
    Q_INVOKABLE QStringList discussionQuestions(const QVariantMap &annotation) const;
    Q_INVOKABLE QVariantMap saveAiDiscussionAnnotation(const QString &bookId, const QVariantMap &annotation, const QVariantList &turns);
    Q_INVOKABLE QVariantMap saveAiAnnotation(const QString &bookId, const QVariantMap &annotation, const QString &markdown);
    Q_INVOKABLE void saveAnnotation(const QString &bookId, const QVariantMap &annotation);
    Q_INVOKABLE void removeAnnotation(const QString &bookId, const QString &annotationKey);

private:
    QVariantMap saveRenderedAiAnnotation(const QString &bookId, const QVariantMap &annotation, const QVariantMap &rendered);

Q_SIGNALS:
    void annotationsChanged(const QString &bookId);
};
