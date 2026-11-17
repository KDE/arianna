// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "readercontextbuilder.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace
{
QStringList values(const QVariant &value)
{
    QStringList result;
    if (value.metaType().id() == QMetaType::QVariantList || value.metaType().id() == QMetaType::QStringList) {
        for (const auto &item : value.toList())
            result.append(values(item));
    } else if (value.metaType().id() == QMetaType::QVariantMap) {
        const auto map = value.toMap();
        for (const auto &key : {QStringLiteral("name"), QStringLiteral("value"), QStringLiteral("label"), QStringLiteral("text")}) {
            if (map.contains(key))
                return values(map.value(key));
        }
        // Foliate language maps (e.g. {en: "Title", sa: "..."}).
        for (const auto &item : map)
            result.append(values(item));
    } else if (!value.toString().trimmed().isEmpty()) {
        result.append(value.toString().trimmed());
    }
    result.removeDuplicates();
    return result;
}
void put(QVariantMap &target, const QString &key, const QVariant &value, bool list = false)
{
    const auto strings = values(value);
    if (!strings.isEmpty())
        target.insert(key, list ? QVariant(strings) : QVariant(strings.join(QStringLiteral(", "))));
}
}

QVariantMap ReaderContextBuilder::build(const QVariantMap &metadata, const QVariantMap &selection, const QVariantMap &position) const
{
    // A selection from an earlier document must never acquire a new book's metadata.
    const auto bookId = metadata.value(QStringLiteral("bookId")).toString();
    const auto selectionBookId = selection.value(QStringLiteral("bookId")).toString();
    if (!selectionBookId.isEmpty() && !bookId.isEmpty() && selectionBookId != bookId)
        return {};
    const auto text = selection.contains(QStringLiteral("content")) ? selection.value(QStringLiteral("content")).toString()
                                                                    : selection.value(QStringLiteral("text")).toString();
    if (text.trimmed().isEmpty())
        return {};
    QVariantMap bibliography;
    for (const auto &key : {QStringLiteral("bookId"), QStringLiteral("identifier"), QStringLiteral("title"), QStringLiteral("edition")})
        put(bibliography, key, metadata.value(key));
    if (!bibliography.contains(QStringLiteral("bookId")))
        put(bibliography, QStringLiteral("bookId"), selectionBookId);
    put(bibliography,
        QStringLiteral("authors"),
        values(metadata.value(QStringLiteral("author"))).isEmpty() ? metadata.value(QStringLiteral("creator")) : metadata.value(QStringLiteral("author")),
        true);
    put(bibliography, QStringLiteral("translators"), metadata.value(QStringLiteral("translator")), true);
    put(bibliography, QStringLiteral("editors"), metadata.value(QStringLiteral("editor")), true);
    put(bibliography, QStringLiteral("languages"), metadata.value(QStringLiteral("language")), true);

    const auto dom = selection.value(QStringLiteral("readerContext")).toMap();
    QVariantMap location;
    auto toc = selection.value(QStringLiteral("tocItem")).toMap();
    if (values(toc.value(QStringLiteral("label"))).isEmpty())
        toc = position.value(QStringLiteral("tocItem")).toMap();
    put(location, QStringLiteral("chapter"), toc.value(QStringLiteral("label")));
    put(location, QStringLiteral("chapterHref"), toc.value(QStringLiteral("href")));
    put(location, QStringLiteral("heading"), dom.value(QStringLiteral("heading")));
    // Only the DOM extractor's explicit, unambiguous data-verse attributes qualify.
    put(location, QStringLiteral("verse"), dom.value(QStringLiteral("verse")));
    put(location, QStringLiteral("selectionCfi"), selection.value(QStringLiteral("value")));
    put(location, QStringLiteral("readerCfi"), position.value(QStringLiteral("cfi")));
    if (selection.contains(QStringLiteral("sectionIndex")))
        location.insert(QStringLiteral("sectionIndex"), selection.value(QStringLiteral("sectionIndex")));

    QVariantMap selected{{QStringLiteral("text"), text}};
    if (dom.contains(QStringLiteral("language")))
        selected.insert(QStringLiteral("language"), dom.value(QStringLiteral("language")));
    else if (selection.contains(QStringLiteral("lang")))
        selected.insert(QStringLiteral("language"), selection.value(QStringLiteral("lang")));
    if (dom.contains(QStringLiteral("languageSegments")))
        selected.insert(QStringLiteral("languageSegments"), dom.value(QStringLiteral("languageSegments")));
    QVariantMap surrounding;
    for (const auto &key : {QStringLiteral("sentence"), QStringLiteral("paragraph"), QStringLiteral("section")}) {
        // Preserve source whitespace and Unicode; never use speech/translation cleanup.
        if (!dom.value(key).toString().isEmpty()) {
            surrounding.insert(key, dom.value(key));
            if (dom.value(key + QStringLiteral("Truncated")).toBool())
                surrounding.insert(key + QStringLiteral("Truncated"), true);
        }
    }
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("snapshotId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("capturedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("bibliography"), bibliography},
            {QStringLiteral("location"), location},
            {QStringLiteral("selection"), selected},
            {QStringLiteral("surrounding"), surrounding}};
}

QVariantMap ReaderContextBuilder::withOptions(const QVariantMap &snapshot, const QStringList &excludedFields, const QString &editedText) const
{
    auto result = snapshot;
    if (result.isEmpty())
        return result;
    const QStringList optional{QStringLiteral("bibliography.authors"),
                               QStringLiteral("bibliography.translators"),
                               QStringLiteral("bibliography.editors"),
                               QStringLiteral("bibliography.languages"),
                               QStringLiteral("bibliography.edition"),
                               QStringLiteral("location.chapter"),
                               QStringLiteral("location.chapterHref"),
                               QStringLiteral("location.heading"),
                               QStringLiteral("location.verse"),
                               QStringLiteral("location.selectionCfi"),
                               QStringLiteral("location.readerCfi"),
                               QStringLiteral("location.sectionIndex"),
                               QStringLiteral("selection.language"),
                               QStringLiteral("selection.languageSegments"),
                               QStringLiteral("surrounding.sentence"),
                               QStringLiteral("surrounding.paragraph"),
                               QStringLiteral("surrounding.section")};
    QStringList omitted;
    for (const auto &path : excludedFields) {
        if (!optional.contains(path))
            continue;
        const auto keys = path.split(QLatin1Char('.'));
        auto group = result.value(keys.first()).toMap();
        if (group.remove(keys.last()))
            omitted.append(path);
        group.remove(keys.last() + QStringLiteral("Truncated"));
        result.insert(keys.first(), group);
    }
    auto selection = result.value(QStringLiteral("selection")).toMap();
    if (editedText != selection.value(QStringLiteral("text")).toString())
        selection.insert(QStringLiteral("userEditedText"), editedText);
    result.insert(QStringLiteral("selection"), selection);
    if (!omitted.isEmpty())
        result.insert(QStringLiteral("omittedFields"), omitted);
    return result;
}

QString ReaderContextBuilder::toJson(const QVariantMap &context) const
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(context)).toJson(QJsonDocument::Indented));
}
