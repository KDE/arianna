// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Deliberately a small QCoreApplication, without the GUI, book server or KCrash.
#include <KLocalizedString>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <podofo/main/PdfCommon.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

namespace
{
constexpr int maximumMatches = 10000;

QJsonObject search(const QString &fileName, const QString &query)
{
    QJsonArray hits;
    QString error;
    try {
        PoDoFo::PdfMemDocument document;
        document.Load(fileName.toStdString());
        bool hasText = false;
        bool truncated = false;
        for (unsigned page = 0; page < document.GetPages().GetCount() && !truncated; ++page) {
            std::vector<PoDoFo::PdfTextEntry> entries;
            document.GetPages().GetPageAt(page).ExtractTextTo(entries, PoDoFo::PdfTextExtractParams{});
            QString text;
            for (const auto &entry : entries) {
                if (!text.isEmpty())
                    text += QLatin1Char(' ');
                text += QString::fromUtf8(entry.Text.data(), entry.Text.size());
            }
            text = text.simplified();
            hasText |= !text.isEmpty();
            const QString location =
                QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("page"), int(page)}})
                                      .toJson(QJsonDocument::Compact));
            qsizetype position = 0;
            while ((position = text.indexOf(query, position, Qt::CaseInsensitive)) >= 0) {
                if (hits.size() == maximumMatches) {
                    truncated = true;
                    error = i18n("Only the first %1 matches in this PDF are shown. Try a more specific search text.", maximumMatches);
                    break;
                }
                const qsizetype start = qMax<qsizetype>(0, position - 65);
                const QString before = (start ? QStringLiteral("…") : QString()) + text.mid(start, position - start);
                const QString match = text.mid(position, query.size());
                const QString after = text.mid(position + query.size(), 65) + (position + query.size() + 65 < text.size() ? QStringLiteral("…") : QString());
                hits.append(QJsonObject{
                    {QStringLiteral("location"), location},
                    {QStringLiteral("section"), i18n("Page %1", page + 1)},
                    {QStringLiteral("excerpt"), QString(before + match + after)},
                    {QStringLiteral("markup"),
                     QString(before.toHtmlEscaped() + QStringLiteral("<b>") + match.toHtmlEscaped() + QStringLiteral("</b>") + after.toHtmlEscaped())}});
                position += query.size();
            }
        }
        if (!hasText)
            error = i18n("This PDF has no searchable text.");
    } catch (const std::exception &exception) {
        error = i18n("Unable to search this PDF: %1", QString::fromUtf8(exception.what()).left(2000));
    } catch (...) {
        error = i18n("Unable to search this PDF.");
    }
    return {{QStringLiteral("version"), 1}, {QStringLiteral("hits"), hits}, {QStringLiteral("error"), error}};
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    PoDoFo::PdfCommon::SetMaxLoggingSeverity(PoDoFo::PdfLogSeverity::Error);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 1;
    const auto request = QJsonDocument::fromJson(input.read(1024 * 1024)).object();
    const QString fileName = request.value(QStringLiteral("fileName")).toString();
    const QString query = request.value(QStringLiteral("query")).toString().simplified();
    if (fileName.isEmpty() || query.isEmpty())
        return 1;
    const QByteArray response = QJsonDocument(search(fileName, query)).toJson(QJsonDocument::Compact);
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly) || output.write(response) != response.size() || !output.flush())
        return 1;
    return 0;
}
