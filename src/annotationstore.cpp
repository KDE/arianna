// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "annotationstore.h"

#include "bookdatabase.h"
#include "booktruthstore.h"
#include "epubcontainer.h"
#include "noterenderer.h"
#include <KLocalizedString>

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

static bool hasUniqueAnnotationAnchor(EPubContainer &container, const QString &id)
{
    if (id.isEmpty())
        return false;
    const QString root = id.startsWith(QLatin1String("uuid_")) ? id.mid(5) : id;
    const QStringList roots{root, QStringLiteral("uuid_") + root};
    const auto anchors = container.targetAnchors();
    const TargetAnchorInfo *single = nullptr, *begin = nullptr, *end = nullptr;
    int matches = 0;
    qsizetype beginPosition = -1, endPosition = -1;
    for (qsizetype i = 0; i < anchors.size(); ++i) {
        const auto &anchor = anchors.at(i);
        for (const auto &candidate : roots) {
            if (anchor.ref == candidate) {
                ++matches;
                if (anchor.type == QLatin1String("annotation"))
                    single = &anchor;
            } else if (anchor.ref == candidate + QLatin1String("_begin")) {
                ++matches;
                if (anchor.type == QLatin1String("annotation-begin")) {
                    begin = &anchor;
                    beginPosition = i;
                }
            } else if (anchor.ref == candidate + QLatin1String("_end")) {
                ++matches;
                if (anchor.type == QLatin1String("annotation-end")) {
                    end = &anchor;
                    endPosition = i;
                }
            }
        }
    }
    return (matches == 1 && single)
        || (matches == 2 && begin && end && begin->file == end->file && beginPosition < endPosition && begin->ref.chopped(6) == end->ref.chopped(4));
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
    QVariantMap copy = annotation;
    const QString cfiRange = copy.value(QStringLiteral("cfiRange")).toString().isEmpty() ? copy.value(QStringLiteral("value")).toString()
                                                                                         : copy.value(QStringLiteral("cfiRange")).toString();
    if (bookId.isEmpty() || cfiRange.isEmpty()) {
        return annotationError(copy, QStringLiteral("Unable to create anchored annotation without book id and CFI range"));
    }

    BookTruthStore store;
    if (!expectedStateId.isEmpty() && uuidFromString(expectedStateId).isNull()) {
        return annotationError(copy, i18n("The book has changed. Select the passage again before saving the remark."));
    }
    const BookCommitResult commit = store.createAnchor(bookId, cfiRange, copy.value(QStringLiteral("text")).toString(), uuidFromString(expectedStateId));
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

    if (!BookDatabase::self().saveAnnotation(bookId, copy)) {
        copy.insert(QStringLiteral("expectedStateId"), uuidToString(commit.newStateId));
        return annotationError(copy, i18n("The EPUB anchor was created, but the remark could not be saved. Your draft is still available."));
    }
    Q_EMIT annotationsChanged(bookId);
    return copy;
}

QVariantMap AnnotationStore::renderNote(const QString &markdown) const
{
    return NoteRenderer::render(markdown);
}

QVariantMap AnnotationStore::saveAiAnnotation(const QString &bookId, const QVariantMap &annotation, const QString &markdown)
{
    const auto rendered = NoteRenderer::render(markdown);
    if (rendered.isEmpty())
        return annotationError(annotation, i18n("Enter a remark of at most 256 Ki characters."));
    return saveRenderedAiAnnotation(bookId, annotation, rendered);
}

QVariantMap AnnotationStore::renderDiscussion(const QVariantList &turns) const
{
    return NoteRenderer::renderDiscussion(turns);
}

QStringList AnnotationStore::discussionQuestions(const QVariantMap &annotation) const
{
    return NoteRenderer::discussionQuestions(annotation.value(QStringLiteral("noteMetadata")).toMap());
}

QVariantMap AnnotationStore::saveAiDiscussionAnnotation(const QString &bookId, const QVariantMap &annotation, const QVariantList &turns)
{
    for (const auto &value : turns) {
        const auto target = value.toMap().value(QStringLiteral("annotationTarget")).toMap();
        if (!target.value(QStringLiteral("bookId")).toString().isEmpty() && target.value(QStringLiteral("bookId")).toString() != bookId)
            return annotationError(annotation, i18n("The discussion belongs to another book. Your discussion has been kept."));
    }
    const auto rendered = NoteRenderer::renderDiscussion(turns);
    if (rendered.isEmpty())
        return annotationError(annotation, i18n("The discussion is incomplete or exceeds the size limit. Your discussion has been kept."));
    return saveRenderedAiAnnotation(bookId, annotation, rendered);
}

QVariantMap AnnotationStore::saveRenderedAiAnnotation(const QString &bookId, const QVariantMap &annotation, const QVariantMap &rendered)
{
    QVariantMap copy = annotation;
    bool existing = false;
    for (const auto &value : loadAnnotations(bookId)) {
        const auto saved = value.toMap();
        if (!copy.value(QStringLiteral("annotationId")).toString().isEmpty()
            && saved.value(QStringLiteral("annotationId")) == copy.value(QStringLiteral("annotationId"))) {
            copy = saved;
            if (annotation.contains(QStringLiteral("color")))
                copy.insert(QStringLiteral("color"), annotation.value(QStringLiteral("color")));
            existing = true;
            break;
        }
    }
    for (auto it = rendered.begin(); it != rendered.end(); ++it)
        copy.insert(it.key(), it.value());
    copy.insert(QStringLiteral("format"), QStringLiteral("epub"));
    copy.insert(QStringLiteral("modified"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    copy.remove(QStringLiteral("error"));
    copy.remove(QStringLiteral("message"));
    if (!existing && copy.value(QStringLiteral("anchorId")).toString().isEmpty()) {
        const auto expected = copy.value(QStringLiteral("expectedStateId")).toString();
        if (expected.isEmpty())
            return annotationError(copy, i18n("The original book selection is unavailable. Select the passage again."));
        return createAnchoredAnnotation(bookId, copy, expected);
    }
    // Existing remarks and retries after a database failure reuse their anchor.
    // Verify it still exists rather than creating a second anchor or using a stale CFI.
    BookTruthStore store;
    const auto snapshot = store.openBook(bookId);
    EPubContainer container(nullptr);
    if (!snapshot.success || !container.openFile(snapshot.filename))
        return annotationError(copy, i18n("The remark's EPUB is unavailable."));
    if (!existing && (!snapshot.state || snapshot.state->stateId != uuidFromString(copy.value(QStringLiteral("expectedStateId")).toString())))
        return annotationError(copy, i18n("The book has changed. Select the passage again before saving the remark."));
    container.extractTargetAnchors();
    const auto id = copy.value(QStringLiteral("anchorId")).toString();
    if (!hasUniqueAnnotationAnchor(container, id))
        return annotationError(copy, i18n("The remark's EPUB anchor is missing."));
    if (!existing && copy.value(QStringLiteral("annotationId")).toString().trimmed().isEmpty())
        copy.insert(QStringLiteral("annotationId"), uuidToString(QUuid::createUuidV7()));
    if (copy.value(QStringLiteral("created")).toString().isEmpty())
        copy.insert(QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!BookDatabase::self().saveAnnotation(bookId, copy))
        return annotationError(copy, i18n("The remark could not be saved. Your draft is still available."));
    Q_EMIT annotationsChanged(bookId);
    return copy;
}

QVariantMap AnnotationStore::createUserAnnotation(const QString &bookId, const QVariantMap &annotation)
{
    QVariantMap copy = annotation;
    if (bookId.trimmed().isEmpty()) {
        return annotationError(copy, QStringLiteral("Unable to create a user annotation without a book id"));
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
