// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookformats.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>

QString BookFormats::type(const BookEntry &book)
{
    const auto suffix = QFileInfo(book.filename).suffix().toLower();
    return suffix == QStringLiteral("epub") ? QStringLiteral("EPUB") : suffix == QStringLiteral("pdf") ? QStringLiteral("PDF") : QString();
}

QString BookFormats::identity(const BookEntry &book)
{
    const QString title = book.title.normalized(QString::NormalizationForm_C).simplified().toCaseFolded();
    QStringList authors;
    for (const auto &author : book.author) {
        const auto name = author.normalized(QString::NormalizationForm_C).simplified().toCaseFolded();
        if (!name.isEmpty())
            authors.append(name);
    }
    authors.removeDuplicates();
    authors.sort();
    if (title.isEmpty() || authors.isEmpty() || type(book).isEmpty())
        return {};
    return QString::fromUtf8(QJsonDocument::fromVariant(QVariantList{title, authors}).toJson(QJsonDocument::Compact));
}

QStringList BookFormats::types(const BookEntry &book)
{
    QStringList result;
    for (const auto &member : members(book)) {
        const auto format = type(member);
        if (!format.isEmpty() && !result.contains(format))
            result.append(format);
    }
    result.sort();
    return result;
}

QString BookFormats::displayKey(const BookEntry &book)
{
    if (book.formatGroup.isEmpty())
        return {};
    const auto key = identity(book);
    return key.isEmpty() ? QString() : book.formatGroup + QLatin1Char(':') + key;
}

QVariantList BookFormats::candidates(const QList<BookEntry> &books)
{
    QMap<QString, QList<BookEntry>> matches;
    for (const auto &book : books) {
        const auto key = identity(book);
        if (!key.isEmpty())
            matches[key].append(book);
    }
    QVariantList result;
    for (auto it = matches.cbegin(); it != matches.cend(); ++it) {
        QSet<QString> types, groups;
        QStringList files;
        for (const auto &book : it.value()) {
            types.insert(type(book));
            groups.insert(book.formatGroup);
            files.append(book.filename);
        }
        if (types.size() < 2 || (groups.size() == 1 && !groups.contains(QString())))
            continue;
        files.sort();
        result.append(QVariantMap{{QStringLiteral("key"), it.key()},
                                  {QStringLiteral("files"), files},
                                  {QStringLiteral("title"), it->first().title},
                                  {QStringLiteral("authors"), it->first().author}});
    }
    return result;
}

QList<BookEntry> BookFormats::members(const BookEntry &book)
{
    if (book.formatVariants.isEmpty())
        return {book};
    QList<BookEntry> result;
    for (const auto &value : book.formatVariants)
        result.append(value.toMap().value(QStringLiteral("entry")).value<BookEntry>());
    return result;
}

BookEntry BookFormats::combined(const QList<BookEntry> &books)
{
    QList<BookEntry> variants;
    QSet<QString> seen;
    for (const auto &book : books)
        for (auto member : members(book)) {
            if (seen.contains(member.filename))
                continue;
            seen.insert(member.filename);
            member.formatVariants.clear();
            variants.append(member);
        }
    if (variants.isEmpty())
        return BookEntry();
    std::sort(variants.begin(), variants.end(), [](const BookEntry &a, const BookEntry &b) {
        return type(a) == type(b) ? a.filename < b.filename : type(a) < type(b);
    });
    BookEntry result = variants.first();
    for (const auto &book : variants)
        if (book.lastOpenedTime.isValid() && (!result.lastOpenedTime.isValid() || book.lastOpenedTime > result.lastOpenedTime))
            result = book;
    if (variants.size() == 1)
        return result;
    // Keep the cover stable even when the preferred reader format changes.
    for (const auto &book : variants)
        if (!book.thumbnail.isEmpty()) {
            result.thumbnail = book.thumbnail;
            break;
        }
    for (const auto &book : variants)
        result.formatVariants.append(QVariantMap{{QStringLiteral("format"), type(book)},
                                                 {QStringLiteral("filename"), book.filename},
                                                 {QStringLiteral("entry"), QVariant::fromValue(book)}});
    return result;
}
