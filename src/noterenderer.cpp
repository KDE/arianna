// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "noterenderer.h"
#include <KLocalizedString>
#include <QJsonDocument>
#include <QVariantList>
#include <QVariantMap>

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QTextTable>
#include <QUrl>
#include <QUuid>

namespace
{
// Markdown images must never cause resource reads, even while parsing.
class MarkdownDocument : public QTextDocument
{
protected:
    QVariant loadResource(int, const QUrl &) override
    {
        return {};
    }
};

struct MathSource {
    QString source;
    bool complete;
};

// Protect TeX before Markdown consumes its escapes, underscores, stars and
// table separators. Code spans/fences and indented code remain literal.
QString protectMath(const QString &text, const QString &id, QList<MathSource> &math)
{
    QString result;
    const QString prefix = QStringLiteral("ARIANNATEX") + QString(id).remove(QLatin1Char('-'));
    static const QRegularExpression fence(QStringLiteral("^ {0,3}(?:> ?)*(`{3,}|~{3,})(.*)$"));
    QChar fenceChar;
    int fenceLength = 0;
    bool lineStart = true;
    for (qsizetype i = 0; i < text.size();) {
        if (lineStart) {
            const qsizetype end = text.indexOf(QLatin1Char('\n'), i);
            const qsizetype length = (end < 0 ? text.size() : end) - i;
            const QString line = text.mid(i, length);
            const auto match = fence.match(line);
            if (match.hasMatch()) {
                const QString marker = match.captured(1);
                if (fenceLength == 0) {
                    fenceChar = marker.front();
                    fenceLength = marker.size();
                } else if (marker.front() == fenceChar && marker.size() >= fenceLength && match.captured(2).trimmed().isEmpty()) {
                    fenceLength = 0;
                }
                result += text.mid(i, length + (end >= 0 ? 1 : 0));
                i += length + (end >= 0 ? 1 : 0);
                continue;
            }
            if (fenceLength || line.startsWith(QStringLiteral("    ")) || line.startsWith(QLatin1Char('\t'))) {
                result += text.mid(i, length + (end >= 0 ? 1 : 0));
                i += length + (end >= 0 ? 1 : 0);
                continue;
            }
        }
        if (text[i] == QLatin1Char('`')) {
            qsizetype count = 1;
            while (i + count < text.size() && text[i + count] == QLatin1Char('`'))
                ++count;
            const QString ticks(count, QLatin1Char('`'));
            qsizetype end = i + count;
            while ((end = text.indexOf(ticks, end)) >= 0) {
                if ((end == 0 || text[end - 1] != QLatin1Char('`')) && (end + count == text.size() || text[end + count] != QLatin1Char('`')))
                    break;
                end += count;
            }
            if (end >= 0) {
                result += text.mid(i, end + count - i);
                i = end + count;
                lineStart = false;
                continue;
            }
        }
        const bool canonical = text.mid(i, 2) == QLatin1String("\\(") || text.mid(i, 2) == QLatin1String("\\[");
        const bool dollars = text.mid(i, 2) == QLatin1String("$$");
        if (canonical || dollars) {
            const QString close = dollars ? QStringLiteral("$$") : text[i + 1] == QLatin1Char('(') ? QStringLiteral("\\)") : QStringLiteral("\\]");
            qsizetype end = i + 2;
            while ((end = text.indexOf(close, end)) >= 0) {
                qsizetype slashes = 0;
                for (qsizetype p = end; p > 0 && text[p - 1] == QLatin1Char('\\'); --p)
                    ++slashes;
                if (slashes % 2 == 0)
                    break;
                end += close.size();
            }
            // Incomplete streamed formulas stay readable until their closing
            // delimiter arrives. They are excluded from MathJax for now.
            const QString source = text.mid(i, end < 0 ? text.size() - i : end + close.size() - i);
            const QString token = prefix + QString::number(math.size()) + QStringLiteral("END");
            math.append({source, end >= 0});
            result += token;
            i += source.size();
            lineStart = false;
            continue;
        }
        // A Markdown-escaped backslash must not open a formula or a code span.
        if (text.mid(i, 2) == QLatin1String("\\\\") || text.mid(i, 2) == QLatin1String("\\`")) {
            result += text.mid(i, 2);
            i += 2;
            lineStart = false;
            continue;
        }
        lineStart = text[i] == QLatin1Char('\n');
        result += text[i++];
    }
    return result;
}

QString inlineHtml(const QTextBlock &block)
{
    QString html;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        const auto format = fragment.charFormat();
        if (format.isImageFormat()) {
            // Images are not part of the response contract. Keep their label.
            html += format.toImageFormat().stringProperty(QTextFormat::ImageAltText).toHtmlEscaped();
            continue;
        }
        QString part = fragment.text().toHtmlEscaped();
        part.replace(QChar::LineSeparator, QStringLiteral("<br>"));
        if (format.fontFixedPitch())
            part = QStringLiteral("<code>") + part + QStringLiteral("</code>");
        if (format.fontWeight() >= QFont::DemiBold)
            part = QStringLiteral("<strong>") + part + QStringLiteral("</strong>");
        if (format.fontItalic())
            part = QStringLiteral("<em>") + part + QStringLiteral("</em>");
        if (format.fontStrikeOut())
            part = QStringLiteral("<del>") + part + QStringLiteral("</del>");
        if (format.isAnchor()) {
            const QUrl url(format.anchorHref());
            // No file:, javascript:, data:, custom schemes or IPC links.
            if ((url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")) && !url.host().isEmpty())
                part = QStringLiteral("<a href=\"") + url.toString(QUrl::FullyEncoded).toHtmlEscaped() + QStringLiteral("\" rel=\"noreferrer\">") + part
                    + QStringLiteral("</a>");
        }
        html += part;
    }
    return html;
}

QString paragraphHtml(const QTextBlock &block)
{
    const auto format = block.blockFormat();
    if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
        return QStringLiteral("<hr>");
    const int heading = format.headingLevel();
    const QString tag = heading > 0 ? QStringLiteral("h") + QString::number(qMin(heading, 6)) : QStringLiteral("p");
    return QLatin1Char('<') + tag + QLatin1Char('>') + inlineHtml(block) + QStringLiteral("</") + tag + QLatin1Char('>');
}

using Iterator = QTextFrame::iterator;

QString nextHtml(Iterator &it, const Iterator &end);

QString tableHtml(const QTextTable *table)
{
    QString html = QStringLiteral("<table>");
    for (int row = 0; row < table->rows(); ++row) {
        html += QStringLiteral("<tr>");
        for (int column = 0; column < table->columns(); ++column) {
            const auto cell = table->cellAt(row, column);
            if (cell.row() != row || cell.column() != column)
                continue;
            const QString tag = row < qMax(1, table->format().headerRowCount()) ? QStringLiteral("th") : QStringLiteral("td");
            html += QLatin1Char('<') + tag + QStringLiteral(" rowspan=\"") + QString::number(cell.rowSpan()) + QStringLiteral("\" colspan=\"")
                + QString::number(cell.columnSpan()) + QStringLiteral("\">");
            auto cellIt = cell.begin();
            while (cellIt != cell.end())
                html += nextHtml(cellIt, cell.end());
            html += QStringLiteral("</") + tag + QLatin1Char('>');
        }
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

QString listHtml(Iterator &it, const Iterator &end)
{
    const auto *list = it.currentBlock().textList();
    const int indent = list->format().indent();
    const bool ordered = list->format().style() <= QTextListFormat::ListDecimal;
    QString html = ordered ? QStringLiteral("<ol start=\"") + QString::number(list->format().start()) + QStringLiteral("\">") : QStringLiteral("<ul>");
    while (it != end && !it.currentFrame()) {
        const auto block = it.currentBlock();
        if (block.textList() != list)
            break;
        html += QStringLiteral("<li>") + inlineHtml(block);
        ++it;
        while (it != end && !it.currentFrame() && it.currentBlock().textList() && it.currentBlock().textList()->format().indent() > indent)
            html += listHtml(it, end);
        html += QStringLiteral("</li>");
    }
    return html + (ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>"));
}

QString nextHtml(Iterator &it, const Iterator &end)
{
    if (const auto *frame = it.currentFrame()) {
        ++it;
        if (const auto *table = qobject_cast<const QTextTable *>(frame))
            return tableHtml(table);
        return {};
    }
    const auto block = it.currentBlock();
    if (!block.isValid()) {
        ++it;
        return {};
    }
    const int quote = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
    QString html;
    if (block.blockFormat().nonBreakableLines()) {
        const QString language = block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
        html = QStringLiteral("<pre><code data-language=\"") + language.toHtmlEscaped() + QStringLiteral("\">");
        bool first = true;
        while (it != end && !it.currentFrame() && it.currentBlock().blockFormat().nonBreakableLines()
               && it.currentBlock().blockFormat().stringProperty(QTextFormat::BlockCodeLanguage) == language) {
            if (!first)
                html += QLatin1Char('\n');
            html += it.currentBlock().text().toHtmlEscaped();
            first = false;
            ++it;
        }
        html += QStringLiteral("</code></pre>");
    } else if (block.textList()) {
        html = listHtml(it, end);
    } else {
        ++it;
        if (!block.text().isEmpty() || block.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
            html = paragraphHtml(block);
    }
    for (int i = 0; i < quote && !html.isEmpty(); ++i)
        html = QStringLiteral("<blockquote>") + html + QStringLiteral("</blockquote>");
    return html;
}

QVariantList render(const QString &markdown, const QString &id)
{
    QList<MathSource> math;
    MarkdownDocument document;
    document.setMarkdown(protectMath(markdown, id, math),
                         QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    QVariantList blocks;
    const QRegularExpression mathToken(QStringLiteral("ARIANNATEX") + QString(id).remove(QLatin1Char('-')) + QStringLiteral("(\\d+)END"));
    auto it = document.rootFrame()->begin();
    while (it != document.rootFrame()->end()) {
        QString html = nextHtml(it, document.rootFrame()->end());
        if (html.isEmpty())
            continue;
        // Match each placeholder once rather than scanning every block for
        // every formula in a long answer.
        if (!math.isEmpty()) {
            QString expanded;
            qsizetype copied = 0;
            auto matches = mathToken.globalMatch(html);
            while (matches.hasNext()) {
                const auto match = matches.next();
                expanded += html.mid(copied, match.capturedStart() - copied);
                bool valid = false;
                const int index = match.captured(1).toInt(&valid);
                if (valid && index >= 0 && index < math.size()) {
                    const auto &formula = math.at(index);
                    expanded += QStringLiteral("<span class=\"") + (formula.complete ? QStringLiteral("math-source") : QStringLiteral("math-pending"))
                        + QStringLiteral("\">") + formula.source.toHtmlEscaped() + QStringLiteral("</span>");
                } else
                    expanded += match.captured();
                copied = match.capturedEnd();
            }
            expanded += html.mid(copied);
            html = expanded;
        }
        blocks.append(QVariantMap{{QStringLiteral("id"), QString(QStringLiteral("answer-") + id + QStringLiteral("-block-") + QString::number(blocks.size()))},
                                  {QStringLiteral("html"), html}});
    }
    return blocks;
}
}

QVariantMap NoteRenderer::render(const QString &markdown)
{
    if (markdown.trimmed().isEmpty() || markdown.size() > 256 * 1024)
        return {};
    const auto blocks = ::render(markdown, QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString html;
    for (const auto &block : blocks)
        html += block.toMap().value(QStringLiteral("html")).toString();
    MarkdownDocument plain;
    plain.setHtml(html);
    return {{QStringLiteral("noteSource"), markdown}, {QStringLiteral("noteHtml"), html}, {QStringLiteral("note"), plain.toPlainText()}};
}

QVariantMap NoteRenderer::renderDiscussion(const QVariantList &turns)
{
    if (turns.isEmpty() || turns.size() > 128)
        return {};
    QVariantList canonical;
    qsizetype characters = 0;
    bool hasAnswer = false;
    for (const auto &value : turns) {
        const auto turn = value.toMap();
        const QString question = turn.value(QStringLiteral("question")).toString();
        const QString answer = turn.value(QStringLiteral("answer")).toString();
        const QString status = turn.value(QStringLiteral("status")).toString();
        if (question.trimmed().isEmpty()
            || !QStringList{QStringLiteral("completed"), QStringLiteral("failed"), QStringLiteral("closed"),
                            QStringLiteral("interrupted"), QStringLiteral("cancelled"), QStringLiteral("canceled")}.contains(status))
            return {};
        characters += question.size() + answer.size();
        if (characters > 256 * 1024)
            return {};
        hasAnswer |= !answer.trimmed().isEmpty();
        QVariantMap saved{{QStringLiteral("question"), question}, {QStringLiteral("answer"), answer}, {QStringLiteral("status"), status}};
        for (const auto &key : {QStringLiteral("threadId"), QStringLiteral("turnId"), QStringLiteral("submittedAt")}) {
            const QString id = turn.value(key).toString();
            if (id.size() > 512)
                return {};
            saved.insert(key, id);
        }
        const auto target = turn.value(QStringLiteral("annotationTarget")).toMap();
        const auto context = turn.value(QStringLiteral("context")).toMap();
        for (const auto &key : {QStringLiteral("selectionCfi"), QStringLiteral("selectionStateId"), QStringLiteral("contextSnapshotId")}) {
            const QString id = turn.contains(key) ? turn.value(key).toString()
                : key == QLatin1String("selectionCfi") ? target.value(QStringLiteral("cfiRange")).toString()
                : key == QLatin1String("selectionStateId") ? target.value(QStringLiteral("expectedStateId")).toString()
                : context.value(QStringLiteral("snapshotId")).toString();
            if (id.size() > 16 * 1024)
                return {};
            saved.insert(key, id);
        }
        canonical.append(saved);
    }
    if (!hasAnswer || QJsonDocument::fromVariant(canonical).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
        return {};

    QString html = QStringLiteral("<div class=\"chat-conversation\">");
    QString source;
    for (const auto &value : canonical) {
        const auto turn = value.toMap();
        const QString question = turn.value(QStringLiteral("question")).toString();
        const QString answer = turn.value(QStringLiteral("answer")).toString();
        const QString status = turn.value(QStringLiteral("status")).toString();
        html += QStringLiteral("<div class=\"chat-message chat-user\"><div class=\"chat-role\">")
            + i18n("User").toHtmlEscaped() + QStringLiteral("</div>")
            + NoteRenderer::render(question).value(QStringLiteral("noteHtml")).toString() + QStringLiteral("</div>");
        html += QStringLiteral("<div class=\"chat-message chat-assistant\"><div class=\"chat-role\">")
            + i18n("AI").toHtmlEscaped() + QStringLiteral("</div>");
        if (status != QLatin1String("completed")) {
            html += QStringLiteral("<p class=\"chat-status\">")
                + (status == QLatin1String("failed") ? i18n("Response failed") : i18n("Response interrupted")).toHtmlEscaped()
                + QStringLiteral("</p>");
        }
        html += answer.trimmed().isEmpty() ? QStringLiteral("<p>") + i18n("No response was saved.").toHtmlEscaped() + QStringLiteral("</p>")
            : NoteRenderer::render(answer).value(QStringLiteral("noteHtml")).toString();
        html += QStringLiteral("</div>");
        source += i18n("User") + QStringLiteral(":\n") + question + QStringLiteral("\n\n")
            + i18n("AI") + QStringLiteral(":\n") + answer + QStringLiteral("\n\n");
    }
    html += QStringLiteral("</div>");
    MarkdownDocument plain;
    plain.setHtml(html);
    return {{QStringLiteral("noteSource"), source}, {QStringLiteral("noteHtml"), html}, {QStringLiteral("note"), plain.toPlainText()},
            {QStringLiteral("noteMetadata"), QVariantMap{{QStringLiteral("schemaVersion"), 2},
                {QStringLiteral("origin"), QStringLiteral("ai-discussion")}, {QStringLiteral("kind"), QStringLiteral("conversation")},
                {QStringLiteral("turns"), canonical}}}};
}

QStringList NoteRenderer::discussionQuestions(const QVariantMap &metadata)
{
    QStringList questions;
    if (metadata.value(QStringLiteral("origin")) != QLatin1String("ai-discussion"))
        return questions;
    for (const auto &value : metadata.value(QStringLiteral("turns")).toList()) {
        const QString question = value.toMap().value(QStringLiteral("question")).toString();
        if (!question.trimmed().isEmpty())
            questions.append(question);
    }
    if (questions.isEmpty() && !metadata.value(QStringLiteral("question")).toString().trimmed().isEmpty())
        questions.append(metadata.value(QStringLiteral("question")).toString());
    return questions;
}
