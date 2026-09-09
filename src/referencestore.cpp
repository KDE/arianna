#include "referencestore.h"

#include "bookdatabase.h"

ReferenceStore::ReferenceStore(QObject *parent)
    : QObject(parent)
{
    connect(&BookDatabase::self(), &BookDatabase::databaseChanged, this, [this]() {
        Q_EMIT referencesChanged(QString());
    });
}

QVariantList ReferenceStore::loadReferences(const QString &bookId) const
{
    return BookDatabase::self().loadReferences(bookId);
}

QVariantMap ReferenceStore::loadReferenceBySource(const QString &sourceBookId, const QString &sourceAnchorId) const
{
    return BookDatabase::self().loadReferenceBySource(sourceBookId, sourceAnchorId);
}

void ReferenceStore::saveReference(const QVariantMap &reference)
{
    BookDatabase::self().saveReference(reference);
    Q_EMIT referencesChanged(reference.value(QStringLiteral("sourceBookId")).toString());
}

void ReferenceStore::removeReference(const QString &sourceBookId, const QString &sourceAnchorId)
{
    BookDatabase::self().removeReference(sourceBookId, sourceAnchorId);
    Q_EMIT referencesChanged(sourceBookId);
}
