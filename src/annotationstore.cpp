// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "annotationstore.h"

#include "bookdatabase.h"
#include "booktruthstore.h"

#include <QDateTime>
#include <QDebug>
#include <QUuid>

namespace
{
static QString uuidToString(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

static QUuid uuidFromString(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        return {};
    }

    if (!value.startsWith(QLatin1Char('{'))) {
        value = QStringLiteral("{") + value + QStringLiteral("}");
    }

    return QUuid(value);
}

static QVariantMap annotationError(QVariantMap annotation, const QString &message)
{
    annotation.insert(QStringLiteral("error"), true);
    annotation.insert(QStringLiteral("message"), message);
    return annotation;
}
}

AnnotationStore::AnnotationStore(QObject *parent)
    : QObject(parent)
{
    connect(&BookDatabase::self(), &BookDatabase::databaseChanged, this, [this]() {
        Q_EMIT annotationsChanged(QString());
    });
}

QVariantList AnnotationStore::loadAnnotations(const QString &bookId) const
{
    return BookDatabase::self().loadAnnotations(bookId);
}

QString AnnotationStore::currentStateId(const QString &bookId) const
{
    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    if (!snapshot.success || !snapshot.state) {
        qWarning() << "Unable to get current annotation state" << bookId << snapshot.errorMessage;
        return {};
    }

    return uuidToString(snapshot.state->stateId);
}

QVariantMap AnnotationStore::createAnchoredAnnotation(const QString &bookId, const QVariantMap &annotation, const QString &expectedStateId)
{
    Q_UNUSED(expectedStateId)
    QVariantMap copy = annotation;
    const QString cfiRange = copy.value(QStringLiteral("cfiRange")).toString().isEmpty() ? copy.value(QStringLiteral("value")).toString()
                                                                                         : copy.value(QStringLiteral("cfiRange")).toString();
    if (bookId.isEmpty() || cfiRange.isEmpty()) {
        return annotationError(copy, QStringLiteral("Unable to create anchored annotation without book id and CFI range"));
    }

    BookTruthStore store;
    const BookCommitResult commit = store.createAnchor(bookId, cfiRange);
    if (!commit.success) {
        return annotationError(copy, commit.errorMessage);
    }

    if (!commit.createdAnchorId) {
        return annotationError(copy, QStringLiteral("Annotation anchor creation did not return an anchor id"));
    }

    QUuid annotationUuid = uuidFromString(copy.value(QStringLiteral("annotationId")).toString());
    if (annotationUuid.isNull()) {
        annotationUuid = QUuid::createUuidV7();
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (copy.value(QStringLiteral("created")).toString().isEmpty()) {
        copy.insert(QStringLiteral("created"), now);
    }

    copy.insert(QStringLiteral("annotationId"), uuidToString(annotationUuid));
    copy.insert(QStringLiteral("cfiRange"), cfiRange);
    copy.insert(QStringLiteral("value"), cfiRange);
    copy.insert(QStringLiteral("anchorId"), uuidToString(*commit.createdAnchorId));

    BookDatabase::self().saveAnnotation(bookId, copy);
    Q_EMIT annotationsChanged(bookId);
    return copy;
}

void AnnotationStore::saveAnnotation(const QString &bookId, const QVariantMap &annotation)
{
    BookDatabase::self().saveAnnotation(bookId, annotation);
    Q_EMIT annotationsChanged(bookId);
}

void AnnotationStore::removeAnnotation(const QString &bookId, const QString &annotationKey)
{
    BookDatabase::self().removeAnnotation(bookId, annotationKey);
    Q_EMIT annotationsChanged(bookId);
}

#include "moc_annotationstore.cpp"
