// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "epubmath.h"

#include <QDomDocument>
#include <QRegularExpression>
#include <QUrl>

namespace
{
bool parse(QDomDocument &doc, const QByteArray &data)
{
    return bool(doc.setContent(data, QDomDocument::ParseOptions(QDomDocument::ParseOption::UseNamespaceProcessing)
                                       | QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
}

bool hasMath(const QDomNode &node)
{
    const auto element = node.toElement();
    const QString name = element.localName().toLower();
    if (name == QStringLiteral("math")) {
        return true;
    }
    if (name == QStringLiteral("script")) {
        return element.attribute(QStringLiteral("type")).trimmed().compare(QStringLiteral("text/x-mathjax-config"), Qt::CaseInsensitive) == 0
            || element.attribute(QStringLiteral("type")).startsWith(QStringLiteral("math/tex"));
    }
    if (name == QStringLiteral("style") || name == QStringLiteral("pre") || name == QStringLiteral("code")
        || name == QStringLiteral("textarea") || name == QStringLiteral("mjx-container")) {
        return false;
    }
    if (node.isText() || node.isCDATASection()) {
        static const QRegularExpression delimiters(QStringLiteral(R"((\\\([\s\S]+?\\\)|\\\[[\s\S]+?\\\]|\$\$[\s\S]+?\$\$))"));
        if (delimiters.match(node.nodeValue()).hasMatch()) return true;
        static const QRegularExpression dollars(QStringLiteral(R"((?<![\\$])\$(?!\s)([^$\n]+?)(?<!\s)\$(?!\$))"));
        static const QRegularExpression mathematical(QStringLiteral(R"([A-Za-z\\^_=+*/{}])"));
        auto matches = dollars.globalMatch(node.nodeValue());
        while (matches.hasNext()) {
            if (mathematical.match(matches.next().captured(1)).hasMatch()) return true;
        }
        return false;
    }
    for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
        if (hasMath(child)) {
            return true;
        }
    }
    return false;
}

QList<QDomElement> elementsNamed(const QDomNode &node, const QString &name)
{
    QList<QDomElement> result;
    if (node.toElement().localName() == name) result.append(node.toElement());
    for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling())
        result.append(elementsNamed(child, name));
    return result;
}
}

bool EpubMath::needsMathJax(const QByteArray &xhtml)
{
    QDomDocument doc;
    return parse(doc, xhtml) && doc.documentElement().localName() == QStringLiteral("html") && hasMath(doc.documentElement());
}

QByteArray EpubMath::injectMathJax(const QByteArray &xhtml, const QString &bootstrapUrl)
{
    if (bootstrapUrl.isEmpty() || !needsMathJax(xhtml)) {
        return xhtml;
    }
    QDomDocument doc;
    if (!parse(doc, xhtml)) {
        return xhtml;
    }
    auto root = doc.documentElement();
    QDomElement head;
    const auto elements = elementsNamed(root, QStringLiteral("head"));
    if (!elements.isEmpty()) {
        head = elements.at(0);
    }
    const auto scripts = elementsNamed(root, QStringLiteral("script"));
    for (int i = 0; i < scripts.size(); ++i) {
        auto existing = scripts.at(i);
        if (existing.hasAttribute(QStringLiteral("data-arianna-mathjax"))) {
            return xhtml;
        }
        static const QRegularExpression loader(QStringLiteral("(?:mathjax|tex-(?:mml-)?(?:svg|chtml)|mml-(?:svg|chtml))"),
                                              QRegularExpression::CaseInsensitiveOption);
        if (existing.hasAttribute(QStringLiteral("src")) && loader.match(existing.attribute(QStringLiteral("src"))).hasMatch()) {
            if (QUrl(existing.attribute(QStringLiteral("src"))).path().endsWith(QStringLiteral("/MathJax.js"))
                || existing.attribute(QStringLiteral("src")) == QStringLiteral("MathJax.js")) {
                existing.setAttribute(QStringLiteral("data-arianna-legacy-mathjax-src"), existing.attribute(QStringLiteral("src")));
                existing.removeAttribute(QStringLiteral("src"));
                existing.setAttribute(QStringLiteral("type"), QStringLiteral("application/x-arianna-disabled-mathjax"));
            } else {
                existing.setAttribute(QStringLiteral("data-arianna-mathjax-author-loader"), QStringLiteral("true"));
            }
        }
    }
    auto script = doc.createElementNS(root.namespaceURI(), QStringLiteral("script"));
    script.setAttribute(QStringLiteral("src"), bootstrapUrl);
    script.setAttribute(QStringLiteral("data-arianna-mathjax"), QStringLiteral("4"));
    script.setAttribute(QStringLiteral("defer"), QStringLiteral("defer"));
    script.appendChild(doc.createTextNode(QStringLiteral(" ")));
    // Do not add a missing head before body: that changes body CFIs.
    (head.isNull() ? root : head).appendChild(script);
    // Pretty printing would add text nodes and change EPUB CFI offsets.
    return doc.toByteArray(-1);
}
