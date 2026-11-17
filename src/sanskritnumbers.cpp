// SPDX-License-Identifier: GPL-2.0-or-later

#include "sanskritnumbers.h"

#include <array>
#include <QVariantMap>
#include <QStringList>

namespace
{
// Cardinal counting forms, without contextual declension or external sandhi.
// 1..4 use the neuter forms. One accepted variant is fixed for each number.
// Sources and scope: autotests/README.speech.md (Sanskrit number words).
constexpr std::array<QStringView, 101> numberWords{
    u"शून्य",      u"एकम्",        u"द्वे",          u"त्रीणि",       u"चत्वारि",      u"पञ्च",         u"षट्",         u"सप्त",         u"अष्ट",         u"नव",
    u"दश",       u"एकादश",      u"द्वादश",       u"त्रयोदश",      u"चतुर्दश",       u"पञ्चदश",       u"षोडश",       u"सप्तदश",       u"अष्टादश",      u"नवदश",
    u"विंशतिः",   u"एकविंशतिः",   u"द्वाविंशतिः",   u"त्रयोविंशतिः",  u"चतुर्विंशतिः",   u"पञ्चविंशतिः",   u"षड्विंशतिः",   u"सप्तविंशतिः",   u"अष्टाविंशतिः",  u"नवविंशतिः",
    u"त्रिंशत्",    u"एकत्रिंशत्",    u"द्वात्रिंशत्",    u"त्रयस्त्रिंशत्",   u"चतुस्त्रिंशत्",    u"पञ्चत्रिंशत्",    u"षट्त्रिंशत्",    u"सप्तत्रिंशत्",    u"अष्टात्रिंशत्",   u"नवत्रिंशत्",
    u"चत्वारिंशत्", u"एकचत्वारिंशत्", u"द्विचत्वारिंशत्", u"त्रिचत्वारिंशत्", u"चतुश्चत्वारिंशत्", u"पञ्चचत्वारिंशत्", u"षट्चत्वारिंशत्", u"सप्तचत्वारिंशत्", u"अष्टचत्वारिंशत्", u"नवचत्वारिंशत्",
    u"पञ्चाशत्",   u"एकपञ्चाशत्",   u"द्विपञ्चाशत्",   u"त्रिपञ्चाशत्",   u"चतुःपञ्चाशत्",   u"पञ्चपञ्चाशत्",   u"षट्पञ्चाशत्",   u"सप्तपञ्चाशत्",   u"अष्टपञ्चाशत्",   u"नवपञ्चाशत्",
    u"षष्टिः",    u"एकषष्टिः",    u"द्विषष्टिः",    u"त्रिषष्टिः",    u"चतुःषष्टिः",    u"पञ्चषष्टिः",    u"षट्षष्टिः",    u"सप्तषष्टिः",    u"अष्टषष्टिः",    u"नवषष्टिः",
    u"सप्ततिः",   u"एकसप्ततिः",   u"द्विसप्ततिः",   u"त्रिसप्ततिः",   u"चतुःसप्ततिः",   u"पञ्चसप्ततिः",   u"षट्सप्ततिः",   u"सप्तसप्ततिः",   u"अष्टसप्ततिः",   u"नवसप्ततिः",
    u"अशीतिः",   u"एकाशीतिः",   u"द्व्यशीतिः",    u"त्र्यशीतिः",    u"चतुरशीतिः",    u"पञ्चाशीतिः",   u"षडशीतिः",    u"सप्ताशीतिः",   u"अष्टाशीतिः",   u"नवाशीतिः",
    u"नवतिः",    u"एकनवतिः",    u"द्विनवतिः",    u"त्रिनवतिः",    u"चतुर्नवतिः",    u"पञ्चनवतिः",    u"षण्णवतिः",    u"सप्तनवतिः",    u"अष्टनवतिः",    u"नवनवतिः",
    u"शतम्"};

bool supportedDigit(QChar character)
{
    return (character >= QLatin1Char('0') && character <= QLatin1Char('9')) || (character >= QChar(u'०') && character <= QChar(u'९'));
}

bool wordCharacter(QChar character)
{
    return character.isLetterOrNumber() || character.isMark() || character == QLatin1Char('_') || character == QChar(0x200c) || character == QChar(0x200d);
}

bool numberSeparator(QChar character)
{
    return character == QLatin1Char('.') || character == QLatin1Char(',') || character == QLatin1Char(':') || character == QLatin1Char('/')
        || character == QLatin1Char('-') || character == QChar(0x2013) || character == QChar(0x2014) || character == QChar(0x066b)
        || character == QChar(0x066c);
}

bool standalone(QStringView text, qsizetype start, qsizetype end)
{
    if (start > 0) {
        const QChar before = text.at(start - 1);
        if (wordCharacter(before) || before == QLatin1Char('+') || before == QLatin1Char('-') || before == QChar(0x2212))
            return false;
        if (start > 1 && numberSeparator(before) && text.at(start - 2).isNumber())
            return false;
    }
    if (end < text.size()) {
        const QChar after = text.at(end);
        if (wordCharacter(after) || after == QLatin1Char('%') || after == QChar(0x066a))
            return false;
        if (end + 1 < text.size() && numberSeparator(after) && text.at(end + 1).isNumber())
            return false;
    }
    return true;
}
}

QString SanskritNumbers::expandForSpeech(QStringView text, qsizetype start, qsizetype length)
{
    start = qBound(qsizetype(0), start, text.size());
    const qsizetype end = length < 0 ? text.size() : start + qMin(length, text.size() - start);
    QString result;
    qsizetype copied = start;
    for (qsizetype position = start; position < end;) {
        if (!supportedDigit(text.at(position))) {
            ++position;
            continue;
        }
        const qsizetype begin = position;
        const bool devanagari = text.at(begin).unicode() >= u'०';
        bool sameScript = true;
        int value = 0;
        while (position < text.size() && supportedDigit(text.at(position))) {
            sameScript &= (text.at(position).unicode() >= u'०') == devanagari;
            // Bounded arithmetic even for arbitrarily long digit sequences.
            if (value <= 100)
                value = value * 10 + text.at(position).digitValue();
            ++position;
        }
        if (position > end || !sameScript || value > 100 || !standalone(text, begin, position) || (position - begin > 1 && text.at(begin).digitValue() == 0))
            continue;
        result += text.mid(copied, begin - copied);
        result += numberWords.at(value);
        copied = position;
    }
    result += text.mid(copied, end - copied);
    return result;
}

QString SanskritNumbers::coordinateForSpeech(const QVariantList &semanticPath)
{
    QStringList parts;
    for (const auto &value : semanticPath) {
        const auto part = value.toMap();
        const int number = part.value(QStringLiteral("number")).toInt();
        const QString label = part.value(QStringLiteral("devanagari")).toString();
        if (number < 1 || number > 100 || label.isEmpty()) return {};
        parts.append(label + QLatin1Char(' ') + numberWords.at(number).toString());
    }
    return parts.join(QStringLiteral(", "));
}
