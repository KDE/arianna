// SPDX-License-Identifier: GPL-2.0-or-later
#include "transliterator.h"

#include <iast/transliteration.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

bool Transliterator::containsDevanagari(const QString &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](QChar character) {
        return character.script() == QChar::Script_Devanagari;
    });
}

std::optional<QString> Transliterator::devanagariToIast(const QString &text)
{
    if (!containsDevanagari(text)) {
        return text;
    }
    // libiast accepts NUL-terminated strings; reject embedded NULs rather than
    // silently returning a truncated transcription.
    if (text.contains(QChar::Null)) {
        return std::nullopt;
    }

    const QByteArray input = text.toUtf8();
    char *output = nullptr;
    const int result = transliterate_devanagari_to_latin(input.constData(), &output);
    const std::unique_ptr<char, decltype(&std::free)> buffer(output, &std::free);
    if (result != 0 || !buffer) {
        return std::nullopt;
    }
    return QString::fromUtf8(buffer.get());
}

std::optional<QString> Transliterator::iastToDevanagari(const QString &text)
{
    if (text.isEmpty()) {
        return text;
    }
    if (text.contains(QChar::Null)) {
        return std::nullopt;
    }
    // libiast expects lowercase, precomposed IAST; preserve the original input.
    const QByteArray input = text.toLower().normalized(QString::NormalizationForm_C).toUtf8();
    char *output = nullptr;
    const int result = transliterate_latin_to_devanagari(input.constData(), &output);
    const std::unique_ptr<char, decltype(&std::free)> buffer(output, &std::free);
    if (result != 0 || !buffer) {
        return std::nullopt;
    }
    return QString::fromUtf8(buffer.get());
}
