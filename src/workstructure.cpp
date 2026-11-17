// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "workstructure.h"
#include "epubcontainer.h"
#include <KLocalizedString>

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
const QString typeName = QStringLiteral("arianna.work-structure");
const QString legacyName = QStringLiteral("arianna:work-structure");
const QString recordId = QStringLiteral("arianna-work-structure-record");
constexpr int maxEvidence = 6000;
constexpr int maxRecordBytes = 4 * 1024 * 1024;
const QString opfNs = QStringLiteral("http://www.idpf.org/2007/opf");
QString local(const QDomNode &n)
{
    return n.localName().isEmpty() ? n.nodeName().section(QLatin1Char(':'), -1) : n.localName();
}
QString hash(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
QString now()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
}
QDomElement child(const QDomElement &e, const QString &name)
{
    for (auto n = e.firstChildElement(); !n.isNull(); n = n.nextSiblingElement())
        if (local(n) == name)
            return n;
    return {};
}
QString plain(const QDomNode &node)
{
    if (node.isText() || node.isCDATASection())
        return node.nodeValue();
    const auto tag = local(node).toLower();
    if (tag == QLatin1String("script") || tag == QLatin1String("style"))
        return {};
    QString text;
    for (auto n = node.firstChild(); !n.isNull(); n = n.nextSibling())
        text += plain(n);
    return text;
}
QDomDocument parse(const QByteArray &bytes)
{
    QDomDocument doc;
    if (bytes.size() > 64 * 1024 * 1024 || !doc.setContent(bytes, QDomDocument::ParseOption::UseNamespaceProcessing))
        return {};
    return doc;
}
QString location(const QString &base, const QString &href)
{
    const QUrl url(href);
    if (url.isEmpty() || !url.scheme().isEmpty() || !url.host().isEmpty() || href.startsWith(QLatin1Char('/')) || href.contains(QLatin1Char('\\')))
        return {};
    const QString path = url.path(QUrl::FullyDecoded);
    const QString result = path.isEmpty() ? base : QDir::cleanPath(QFileInfo(base).path() + QLatin1Char('/') + path);
    if (result.startsWith(QStringLiteral("../")) || result == QLatin1String(".."))
        return {};
    return result + (url.hasFragment() ? QLatin1Char('#') + url.fragment(QUrl::FullyDecoded) : QString());
}
QString folded(QString text)
{
    text = text.normalized(QString::NormalizationForm_D).toLower();
    text.remove(QRegularExpression(QStringLiteral("[\\p{M}]")));
    return text;
}
QString detectTerm(const QString &text)
{
    const QString ascii = folded(text);
    static const QList<QPair<QString, QString>> patterns = {
        {QStringLiteral("asvasa"), QStringLiteral("(?:^|[^a-z])(?:asvasa|ashvasa|aasvaasa)(?:h|m|s)?(?:$|[^a-z])|आश्वास")},
        {QStringLiteral("adhyaya"), QStringLiteral("(?:^|[^a-z])(?:adhyaya|adhyaaya)(?:h|m|s)?(?:$|[^a-z])|अध्याय")},
        {QStringLiteral("skandha"), QStringLiteral("(?:^|[^a-z])skandha(?:h|m|s)?(?:$|[^a-z])|स्कन्ध|स्कंध")},
        {QStringLiteral("kanda"), QStringLiteral("(?:^|[^a-z])(?:kanda|kaanda)(?:h|m|s)?(?:$|[^a-z])|काण्ड|कांड")},
        {QStringLiteral("sloka"), QStringLiteral("(?:^|[^a-z])(?:sloka|shloka)(?:h|m|s)?(?:$|[^a-z])|श्लोक")},
        {QStringLiteral("pada"), QStringLiteral("(?:^|[^a-z])paada(?:h|m|s)?(?:$|[^a-z])|(?:^|[^a-z])pada(?:h|m|s)?(?:$|[^a-z])|पाद")}};
    QString result;
    for (const auto &p : patterns) {
        // Devanagari combining marks must remain intact for the literal branch.
        if (QRegularExpression(p.second).match(ascii).hasMatch() || QRegularExpression(p.second).match(text).hasMatch()) {
            if (!result.isEmpty())
                return {}; // Competing terms require review.
            result = p.first;
        }
    }
    return result;
}
QJsonArray numbers(const QString &label)
{
    const QString normalized = WorkStructure::digits(label);
    const QRegularExpression expression(QStringLiteral("(?<![\\p{L}\\p{N}])([0-9]{1,9}(?:[.:-][0-9]{1,9})*)(?![\\p{L}\\p{N}])"));
    auto matches = expression.globalMatch(normalized);
    QJsonArray result;
    if (!matches.hasNext())
        return result;
    const auto first = matches.next();
    if (matches.hasNext())
        return {}; // Never choose between several numbers.
    for (const QString &part : first.captured(1).split(QRegularExpression(QStringLiteral("[.:-]"))))
        result.append(part.toInt());
    return result;
}
QJsonObject failure(const QString &validity, const QString &message, const QJsonObject &record = {})
{
    return {{QStringLiteral("validity"), validity}, {QStringLiteral("message"), message}, {QStringLiteral("record"), record}};
}
QString coordinateKey(const QJsonArray &coordinate)
{
    QStringList parts;
    for (const auto &n : coordinate)
        parts.append(QString::number(n.toInt()));
    return parts.join(QLatin1Char('.'));
}
QHash<QString, QJsonObject> evidenceIndex(const QJsonObject &snap)
{
    QHash<QString, QJsonObject> index;
    for (const auto &value : snap.value(QStringLiteral("evidence")).toArray()) {
        const auto e = value.toObject();
        index.insert(e.value(QStringLiteral("id")).toString(), e);
    }
    return index;
}
int arrayIndex(const QJsonArray &array, const QJsonValue &value)
{
    for (int i = 0; i < array.size(); ++i)
        if (array[i] == value)
            return i;
    return -1;
}
QJsonObject termById(const QString &id)
{
    for (const auto &value : WorkStructure::terms())
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
            return value.toObject();
    return {};
}
QJsonObject makeDraft(const QJsonObject &snap, const QJsonArray &levels, const QJsonArray &assignments)
{
    QJsonArray separators{QStringLiteral(".")};
    for (const auto &value : snap.value(QStringLiteral("evidence")).toArray()) {
        const QString marker = value.toObject().value(QStringLiteral("marker")).toString();
        for (const auto &separator : {QStringLiteral("-"), QStringLiteral(":")})
            if (marker.contains(separator) && !separators.contains(separator))
                separators.append(separator);
    }
    return {{QStringLiteral("snapshotId"), snap.value(QStringLiteral("snapshotId"))},
            {QStringLiteral("levels"), levels},
            {QStringLiteral("assignments"), assignments},
            {QStringLiteral("separators"), separators}};
}
void archivePaths(const KArchiveDirectory *dir, const QString &prefix, QStringList &paths)
{
    if (!dir)
        return;
    for (const auto &name : dir->entries()) {
        const auto *entry = dir->entry(name);
        const QString path = prefix + name;
        if (entry && entry->isDirectory())
            archivePaths(dynamic_cast<const KArchiveDirectory *>(entry), path + QLatin1Char('/'), paths);
        else if (entry && entry->isFile())
            paths.append(path);
    }
}
// Strip only Arianna's own record reference and modification date. Other OPF
// metadata and package/spine structure must survive a candidate unchanged.
QByteArray comparableOpf(QDomDocument doc)
{
    auto metadata = child(doc.documentElement(), QStringLiteral("metadata"));
    for (auto n = metadata.firstChildElement(); !n.isNull();) {
        auto next = n.nextSiblingElement();
        if ((local(n) == QLatin1String("meta")
             && (n.attribute(QStringLiteral("name")) == legacyName
                 || (n.attribute(QStringLiteral("property")) == QLatin1String("dcterms:modified") && !n.hasAttribute(QStringLiteral("refines")))))
            || (local(n) == QLatin1String("link") && n.attribute(QStringLiteral("id")) == recordId))
            metadata.removeChild(n);
        n = next;
    }
    // Canonical comparison of element structure, attributes and meaningful text
    // avoids namespace-prefix/indentation artifacts of QDom serialization.
    QJsonArray entries;
    std::function<void(const QDomNode &)> visit = [&](const QDomNode &n) {
        if (n.isElement()) {
            const auto e = n.toElement();
            QJsonObject attrs;
            const auto map = e.attributes();
            for (int i = 0; i < map.size(); ++i) {
                const auto attr = map.item(i);
                if (attr.nodeName().startsWith(QStringLiteral("xmlns")))
                    continue;
                attrs.insert(attr.namespaceURI() + QLatin1Char(':') + local(attr), attr.nodeValue());
            }
            entries.append(
                QJsonObject{{QStringLiteral("name"), QString(e.namespaceURI() + QLatin1Char(':') + local(e))}, {QStringLiteral("attributes"), attrs}});
            for (auto c = n.firstChild(); !c.isNull(); c = c.nextSibling())
                visit(c);
            entries.append(QStringLiteral("end"));
        } else if (n.isText() && !n.nodeValue().trimmed().isEmpty())
            entries.append(n.nodeValue());
    };
    visit(doc.documentElement());
    return QJsonDocument(entries).toJson(QJsonDocument::Compact);
}
struct StoredRecord {
    QJsonObject record;
    QString path;
    QString error;
    bool present = false;
};
StoredRecord stored(EPubContainer &book)
{
    StoredRecord result;
    const auto doc = parse(book.readData(book.packagePath()));
    const auto metadata = child(doc.documentElement(), QStringLiteral("metadata"));
    int count = 0;
    for (auto n = metadata.firstChildElement(); !n.isNull(); n = n.nextSiblingElement()) {
        QByteArray bytes;
        QString path;
        bool owned = false;
        if (local(n) == QLatin1String("meta") && n.attribute(QStringLiteral("name")) == legacyName) {
            owned = true;
            bytes = n.attribute(QStringLiteral("content")).toUtf8();
        } else if (local(n) == QLatin1String("link") && n.attribute(QStringLiteral("rel")).split(QLatin1Char(' ')).contains(QStringLiteral("record"))) {
            owned =
                n.attribute(QStringLiteral("id")) == recordId || n.attribute(QStringLiteral("href")).endsWith(QStringLiteral("arianna-work-structure.json"));
            if (n.attribute(QStringLiteral("media-type")) != QLatin1String("application/json") && !owned)
                continue;
            path = location(book.packagePath(), n.attribute(QStringLiteral("href")));
            if (path.isEmpty() || path.contains(QLatin1Char('#'))) {
                if (owned) {
                    result.present = true;
                    result.error = i18n("Invalid structure record path.");
                }
                continue;
            }
            bytes = book.readData(path);
        } else
            continue;
        QJsonParseError error;
        const auto data = bytes.size() <= maxRecordBytes ? QJsonDocument::fromJson(bytes, &error) : QJsonDocument{};
        const auto record = data.object();
        if (record.value(QStringLiteral("type")).toString() != typeName && !owned)
            continue;
        result.present = true;
        ++count;
        if (!data.isObject() || record.value(QStringLiteral("type")).toString() != typeName) {
            result.error = i18n("Malformed or oversized structure record.");
            continue;
        }
        result.record = record;
        result.path = path;
    }
    if (count > 1)
        result.error = i18n("Several competing structure records exist.");
    return result;
}
}

QString WorkStructure::digits(QString text)
{
    for (auto &c : text)
        if (c >= QChar(u'०') && c <= QChar(u'९'))
            c = QChar(u'0' + c.digitValue());
    return text;
}
QJsonArray WorkStructure::terms()
{
    return {QJsonObject{{QStringLiteral("id"), QStringLiteral("asvasa")},
                        {QStringLiteral("iast"), QStringLiteral("āśvāsa")},
                        {QStringLiteral("devanagari"), QStringLiteral("आश्वास")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("adhyaya")},
                        {QStringLiteral("iast"), QStringLiteral("adhyāya")},
                        {QStringLiteral("devanagari"), QStringLiteral("अध्याय")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("skandha")},
                        {QStringLiteral("iast"), QStringLiteral("skandha")},
                        {QStringLiteral("devanagari"), QStringLiteral("स्कन्ध")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("kanda")},
                        {QStringLiteral("iast"), QStringLiteral("kāṇḍa")},
                        {QStringLiteral("devanagari"), QStringLiteral("काण्ड")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("sloka")},
                        {QStringLiteral("iast"), QStringLiteral("śloka")},
                        {QStringLiteral("devanagari"), QStringLiteral("श्लोक")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("pada")},
                        {QStringLiteral("iast"), QStringLiteral("pāda")},
                        {QStringLiteral("devanagari"), QStringLiteral("पाद")}}};
}

bool WorkStructure::hasStoredRecord(EPubContainer &book)
{
    return stored(book).present;
}

QJsonObject WorkStructure::snapshot(EPubContainer &book, const QString &bookId)
{
    QJsonObject snap{{QStringLiteral("snapshotId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {QStringLiteral("bookId"), bookId},
                     {QStringLiteral("sourceFileHash"), book.getFileHash()},
                     {QStringLiteral("algorithmVersion"), QStringLiteral("sanskrit-structure-1")},
                     {QStringLiteral("capturedAt"), now()}};
    const auto opf = parse(book.readData(book.packagePath()));
    if (opf.isNull() || snap.value(QStringLiteral("sourceFileHash")).toString().isEmpty())
        return failure(QStringLiteral("invalid"), i18n("Unable to read EPUB source."));
    QJsonObject bibliography;
    QJsonArray authors, editors;
    const auto metadata = child(opf.documentElement(), QStringLiteral("metadata"));
    for (auto n = metadata.firstChildElement(); !n.isNull(); n = n.nextSiblingElement()) {
        const QString name = local(n);
        const QString value = plain(n).trimmed();
        if (name == QLatin1String("title") && !bibliography.contains(QStringLiteral("title")))
            bibliography.insert(QStringLiteral("title"), value);
        if (name == QLatin1String("creator") || name == QLatin1String("contributor")) {
            QString role = n.attributeNS(opfNs, QStringLiteral("role"), n.attribute(QStringLiteral("role")));
            const QString ref = QLatin1Char('#') + n.attribute(QStringLiteral("id"));
            if (ref.size() > 1)
                for (auto m = metadata.firstChildElement(); !m.isNull(); m = m.nextSiblingElement())
                    if (m.attribute(QStringLiteral("refines")) == ref && m.attribute(QStringLiteral("property")) == QLatin1String("role"))
                        role = plain(m).trimmed();
            if (role == QLatin1String("edt"))
                editors.append(value);
            else if (name == QLatin1String("creator") && (role.isEmpty() || role == QLatin1String("aut")))
                authors.append(value);
        }
        if (name == QLatin1String("publisher"))
            bibliography.insert(QStringLiteral("publisher"), value);
        const auto property = n.attribute(QStringLiteral("property"));
        const auto legacy = n.attribute(QStringLiteral("name"));
        if (property == QLatin1String("schema:bookEdition") || property == QLatin1String("https://schema.org/bookEdition")
            || property == QLatin1String("http://schema.org/bookEdition"))
            bibliography.insert(QStringLiteral("edition"), value);
        else if (legacy == QLatin1String("edition") || legacy == QLatin1String("calibre:edition"))
            bibliography.insert(QStringLiteral("edition"), n.attribute(QStringLiteral("content")));
    }
    if (!authors.isEmpty())
        bibliography.insert(QStringLiteral("authors"), authors);
    if (!editors.isEmpty())
        bibliography.insert(QStringLiteral("editors"), editors);
    snap.insert(QStringLiteral("bibliography"), bibliography);
    QJsonArray evidence, sources, spine, issues;
    const auto manifest = book.manifestItems();
    QStringList ids = book.items();
    for (const auto &id : ids)
        spine.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("path"), manifest.value(id).path}});
    QStringList extra;
    for (auto it = manifest.cbegin(); it != manifest.cend(); ++it)
        if ((it->properties.contains(QStringLiteral("nav")) || it->mimetype == QByteArrayLiteral("application/x-dtbncx+xml")) && !ids.contains(it.key()))
            extra.append(it.key());
    extra.sort();
    ids.append(extra);
    auto add = [&](const QString &id,
                   const QString &kind,
                   const QString &label,
                   const QString &href,
                   const QString &cfi,
                   const QString &path,
                   const QString &parent,
                   const QString &term,
                   const QJsonArray &nums,
                   const QString &marker) {
        if (evidence.size() >= maxEvidence)
            return;
        evidence.append(QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("kind"), kind},
                                    {QStringLiteral("label"), label.left(320)},
                                    {QStringLiteral("href"), href},
                                    {QStringLiteral("cfi"), cfi},
                                    {QStringLiteral("documentPath"), path},
                                    {QStringLiteral("parentEvidenceId"), parent},
                                    {QStringLiteral("term"), term},
                                    {QStringLiteral("numbers"), nums},
                                    {QStringLiteral("marker"), marker}});
    };
    for (const auto &id : ids) {
        const auto item = manifest.value(id);
        if (item.mimetype != QByteArrayLiteral("application/xhtml+xml") && item.mimetype != QByteArrayLiteral("text/html")
            && item.mimetype != QByteArrayLiteral("application/x-dtbncx+xml"))
            continue;
        const auto bytes = book.readData(item.path);
        const auto doc = parse(bytes);
        if (doc.isNull()) {
            issues.append(QString(i18n("Unreadable source document: ") + item.path));
            continue;
        }
        sources.append(QJsonObject{{QStringLiteral("path"), item.path}, {QStringLiteral("hash"), hash(bytes)}});
        const bool isNav = item.properties.contains(QStringLiteral("nav"));
        const bool isNcx = item.mimetype == QByteArrayLiteral("application/x-dtbncx+xml");
        int serial = 0;
        if (isNav || isNcx) {
            std::function<void(const QDomElement &, const QString &, bool)> visitNav = [&](const QDomElement &e, const QString &parent, bool inToc) {
                const QString name = local(e).toLower();
                if (name == QLatin1String("nav"))
                    inToc = e.attributeNS(QStringLiteral("http://www.idpf.org/2007/ops"), QStringLiteral("type"), e.attribute(QStringLiteral("epub:type")))
                                .split(QLatin1Char(' '))
                                .contains(QStringLiteral("toc"));
                if (name == QLatin1String("navmap"))
                    inToc = true;
                QString nextParent = parent;
                if (inToc && (name == QLatin1String("li") || name == QLatin1String("navpoint"))) {
                    const QString evidenceId = id + QStringLiteral(":toc:") + QString::number(++serial);
                    const auto labelNode = isNcx ? child(child(e, QStringLiteral("navLabel")), QStringLiteral("text"))
                                                 : (!child(e, QStringLiteral("a")).isNull() ? child(e, QStringLiteral("a")) : child(e, QStringLiteral("span")));
                    const QString label = plain(labelNode).simplified();
                    const QString target =
                        isNcx ? child(e, QStringLiteral("content")).attribute(QStringLiteral("src")) : labelNode.attribute(QStringLiteral("href"));
                    add(evidenceId,
                        isNcx ? QStringLiteral("ncx") : QStringLiteral("toc"),
                        label,
                        location(item.path, target),
                        {},
                        item.path,
                        parent,
                        detectTerm(label),
                        numbers(label),
                        {});
                    nextParent = evidenceId;
                }
                for (auto c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                    visitNav(c, nextParent, inToc);
            };
            visitNav(doc.documentElement(), {}, false);
        } else {
            QVector<QPair<int, QString>> headings;
            std::function<void(const QDomElement &, const QString &)> visit = [&](const QDomElement &e, const QString &semanticParent) {
                QString parentForChildren = semanticParent;
                const QString name = local(e).toLower();
                ++serial;
                if (name == QLatin1String("script") || name == QLatin1String("style"))
                    return;
                const bool heading = QRegularExpression(QStringLiteral("^h[1-6]$")).match(name).hasMatch();
                QString verse = e.attribute(QStringLiteral("data-verse-number"), e.attribute(QStringLiteral("data-verse")));
                QString marker;
                QString term = detectTerm(e.attribute(QStringLiteral("data-structure-type")) + QLatin1Char(' ')
                                          + e.attributeNS(QStringLiteral("http://www.idpf.org/2007/ops"), QStringLiteral("type")) + QLatin1Char(' ')
                                          + (heading ? plain(e) : QString()));
                if (!heading && verse.isEmpty() && (name == QLatin1String("p") || name == QLatin1String("div"))) {
                    // Only a terminal double-danda marker in a leaf verse block
                    // qualifies; ordinary numeric prose is never verse evidence.
                    bool hasBlockChild = false;
                    for (auto c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                        if (local(c) == QLatin1String("p") || local(c) == QLatin1String("div"))
                            hasBlockChild = true;
                    if (!hasBlockChild) {
                        const auto match = QRegularExpression(QStringLiteral("॥\\s*([0-9०-९]{1,9}(?:[.:-][0-9०-९]{1,9})*)\\s*॥\\s*$")).match(plain(e));
                        if (match.hasMatch()) {
                            marker = match.captured(1);
                            verse = marker;
                        }
                    }
                }
                if (heading || !verse.isEmpty() || !e.attribute(QStringLiteral("data-pada")).isEmpty()) {
                    const QString evidenceId = id + QStringLiteral(":element:") + QString::number(serial);
                    if (heading) {
                        const int depth = name.mid(1).toInt();
                        while (!headings.isEmpty() && headings.last().first >= depth)
                            headings.removeLast();
                    }
                    if (!verse.isEmpty() && term.isEmpty())
                        term = QStringLiteral("sloka");
                    if (!e.attribute(QStringLiteral("data-pada")).isEmpty()) {
                        term = QStringLiteral("pada");
                        verse = e.attribute(QStringLiteral("data-pada"));
                    }
                    const QString fragment = e.attribute(QStringLiteral("id"));
                    const QString href = item.path + (fragment.isEmpty() ? QString() : QLatin1Char('#') + fragment);
                    add(evidenceId,
                        heading ? QStringLiteral("heading") : QStringLiteral("verse"),
                        plain(e).simplified(),
                        href,
                        book.cfiForNode(id, e),
                        item.path,
                        semanticParent.isEmpty() ? (headings.isEmpty() ? QString() : headings.last().second) : semanticParent,
                        term,
                        numbers(verse.isEmpty() ? plain(e) : verse),
                        marker);
                    parentForChildren = evidenceId;
                    if (heading)
                        headings.append({name.mid(1).toInt(), evidenceId});
                }
                for (auto c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                    visit(c, parentForChildren);
            };
            visit(doc.documentElement(), {});
        }
    }
    if (evidence.size() >= maxEvidence)
        issues.append(i18n("The evidence limit was reached. Review only the supplied subset."));
    QJsonObject basis{{QStringLiteral("bibliography"), bibliography}, {QStringLiteral("spine"), spine}, {QStringLiteral("sources"), sources}};
    snap.insert(QStringLiteral("sourceBasisHash"), hash(QJsonDocument(basis).toJson(QJsonDocument::Compact)));
    // A label-only TOC group has a real source identity but no anchor. Its
    // first linked descendant is a deterministic navigation start, explicitly
    // flagged for review rather than supplied by the model.
    QHash<QString, QJsonObject> sourceNodes;
    for (const auto &v : evidence)
        sourceNodes.insert(v.toObject().value(QStringLiteral("id")).toString(), v.toObject());
    for (int pass = 0; pass < 16; ++pass) {
        bool changed = false;
        for (const auto &v : evidence) {
            const auto e = sourceNodes.value(v.toObject().value(QStringLiteral("id")).toString());
            const auto parentId = e.value(QStringLiteral("parentEvidenceId")).toString();
            auto parent = sourceNodes.value(parentId);
            if (parent.isEmpty() || !parent.value(QStringLiteral("href")).toString().isEmpty()
                || (parent.value(QStringLiteral("kind")) != QJsonValue(QStringLiteral("toc")) && parent.value(QStringLiteral("kind")) != QJsonValue(QStringLiteral("ncx"))))
                continue;
            if (!e.value(QStringLiteral("href")).toString().isEmpty()) {
                parent.insert(QStringLiteral("href"), e.value(QStringLiteral("href")));
                parent.insert(QStringLiteral("derivedTarget"), true);
                sourceNodes.insert(parentId, parent);
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    QHash<QString, QJsonObject> linkedToc;
    for (const auto &v : evidence) {
        const auto e = sourceNodes.value(v.toObject().value(QStringLiteral("id")).toString());
        if (e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("toc")) && !e.value(QStringLiteral("derivedTarget")).toBool())
            linkedToc.insert(e.value(QStringLiteral("href")).toString(), e);
    }
    for (int i = 0; i < evidence.size(); ++i) {
        auto e = sourceNodes.value(evidence[i].toObject().value(QStringLiteral("id")).toString());
        const auto link = linkedToc.value(e.value(QStringLiteral("href")).toString());
        if (e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("heading")) && e.value(QStringLiteral("parentEvidenceId")).toString().isEmpty()
            && !link.isEmpty())
            e.insert(QStringLiteral("parentEvidenceId"), link.value(QStringLiteral("parentEvidenceId")));
        evidence[i] = e;
    }
    QHash<QString, QString> sourceHashes;
    for (const auto &source : sources)
        sourceHashes.insert(source.toObject().value(QStringLiteral("path")).toString(), source.toObject().value(QStringLiteral("hash")).toString());
    QHash<QString, QJsonObject> navByTarget;
    for (int i = 0; i < evidence.size(); ++i) {
        auto e = evidence[i].toObject();
        e.insert(QStringLiteral("documentHash"), sourceHashes.value(e.value(QStringLiteral("documentPath")).toString()));
        evidence[i] = e;
        const QString target = e.value(QStringLiteral("href")).toString();
        if (e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("toc")) && !target.isEmpty())
            navByTarget.insert(target, e);
    }
    for (const auto &value : evidence) {
        const auto e = value.toObject();
        const auto other = navByTarget.value(e.value(QStringLiteral("href")).toString());
        if (e.value(QStringLiteral("kind")) != QJsonValue(QStringLiteral("ncx")) || other.isEmpty())
            continue;
        if ((!e.value(QStringLiteral("term")).toString().isEmpty() && !other.value(QStringLiteral("term")).toString().isEmpty()
             && e.value(QStringLiteral("term")) != other.value(QStringLiteral("term")))
            || (!e.value(QStringLiteral("numbers")).toArray().isEmpty() && !other.value(QStringLiteral("numbers")).toArray().isEmpty()
                && e.value(QStringLiteral("numbers")) != other.value(QStringLiteral("numbers"))))
            issues.append(i18n("NAV and NCX disagree about a target. Confirm its mapping explicitly."));
    }
    snap.insert(QStringLiteral("sources"), sources);
    snap.insert(QStringLiteral("evidence"), evidence);
    snap.insert(QStringLiteral("issues"), issues);
    snap.insert(QStringLiteral("existing"), read(book, snap));
    return snap;
}

QJsonObject WorkStructure::suggest(const QJsonObject &snap)
{
    QJsonArray levels, assignments;
    const auto evidence = snap.value(QStringLiteral("evidence")).toArray();
    QHash<QString, QJsonObject> byId;
    QHash<QString, QString> documentTerms;
    for (const auto &v : evidence) {
        const auto e = v.toObject();
        byId.insert(e.value(QStringLiteral("id")).toString(), e);
        if (e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("heading")) && !e.value(QStringLiteral("term")).toString().isEmpty())
            documentTerms.insert(e.value(QStringLiteral("href")).toString(), e.value(QStringLiteral("term")).toString());
    }
    for (bool tocFirst : {true, false})
        for (const auto &v : evidence) {
            const auto e = v.toObject();
            const auto term = e.value(QStringLiteral("term")).toString();
            const bool navigation =
                e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("toc")) || e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("ncx"));
            if (navigation != tocFirst || term.isEmpty() || levels.contains(term))
                continue;
            levels.append(term);
        }
    QHash<QString, QJsonArray> coordinates;
    QSet<QString> assignedKeys;
    for (int pass = 0; pass <= levels.size(); ++pass)
        for (const auto &v : evidence) {
            const auto e = v.toObject();
            const QString id = e.value(QStringLiteral("id")).toString();
            const QString term = e.value(QStringLiteral("term")).toString();
            if (coordinates.contains(id))
                continue;
            if ((e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("toc")) || e.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("ncx")))
                && !e.value(QStringLiteral("derivedTarget")).toBool() && documentTerms.value(e.value(QStringLiteral("href")).toString()) == term)
                continue;
            auto coordinate = e.value(QStringLiteral("numbers")).toArray();
            const int depth = arrayIndex(levels, term);
            if (depth < 0 || coordinate.isEmpty()
                || (e.value(QStringLiteral("href")).toString().isEmpty() && e.value(QStringLiteral("cfi")).toString().isEmpty()))
                continue;
            if (coordinate.size() == 1 && depth > 0) {
                QString parentId = e.value(QStringLiteral("parentEvidenceId")).toString();
                QSet<QString> visited;
                while (!parentId.isEmpty() && !coordinates.contains(parentId) && !visited.contains(parentId)) {
                    visited.insert(parentId);
                    parentId = byId.value(parentId).value(QStringLiteral("parentEvidenceId")).toString();
                }
                auto parent = coordinates.value(parentId);
                if (parent.size() != depth)
                    continue;
                parent.append(coordinate.at(0));
                coordinate = parent;
            }
            if (coordinate.size() != depth + 1 || assignedKeys.contains(coordinateKey(coordinate)))
                continue;
            coordinates.insert(id, coordinate);
            assignedKeys.insert(coordinateKey(coordinate));
            QJsonArray uncertainties{i18n("Verify edition-specific numbering and semantic hierarchy.")};
            if (e.value(QStringLiteral("derivedTarget")).toBool())
                uncertainties.append(i18n("This TOC group has no link. Its first linked child is used as the navigation start."));
            assignments.append(QJsonObject{{QStringLiteral("evidenceId"), id},
                                           {QStringLiteral("term"), term},
                                           {QStringLiteral("coordinate"), coordinate},
                                           {QStringLiteral("origin"), QStringLiteral("local-rule")},
                                           {QStringLiteral("reason"), i18n("Term and numbering found in EPUB source.")},
                                           {QStringLiteral("uncertainties"), uncertainties},
                                           {QStringLiteral("confirmed"), false}});
        }
    return makeDraft(snap, levels, assignments);
}

QJsonObject WorkStructure::modelSchema(const QJsonObject &snap)
{
    QJsonArray termIds;
    for (const auto &t : terms())
        termIds.append(t.toObject().value(QStringLiteral("id")));
    QJsonArray evidenceIds;
    for (const auto &e : snap.value(QStringLiteral("evidence")).toArray())
        evidenceIds.append(e.toObject().value(QStringLiteral("id")));
    const QJsonObject string{{QStringLiteral("type"), QStringLiteral("string")}};
    const QJsonObject strings{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), string}};
    QJsonObject item{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("required"),
         QJsonArray{QStringLiteral("evidenceId"),
                    QStringLiteral("term"),
                    QStringLiteral("coordinate"),
                    QStringLiteral("reason"),
                    QStringLiteral("uncertainties")}},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("evidenceId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), evidenceIds}}},
             {QStringLiteral("term"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), termIds}}},
             {QStringLiteral("coordinate"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("array")},
                  {QStringLiteral("items"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 999999999}}}}},
             {QStringLiteral("reason"), string},
             {QStringLiteral("uncertainties"), strings}}}};
    return {{QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("additionalProperties"), false},
            {QStringLiteral("required"),
             QJsonArray{QStringLiteral("levels"), QStringLiteral("assignments"), QStringLiteral("uncertainties"), QStringLiteral("sources")}},
            {QStringLiteral("properties"),
             QJsonObject{
                 {QStringLiteral("levels"),
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                              {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), termIds}}}}},
                 {QStringLiteral("assignments"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), item}}},
                 {QStringLiteral("uncertainties"), strings},
                 {QStringLiteral("sources"), strings}}}};
}

QJsonObject WorkStructure::applyModel(const QJsonObject &snap, const QJsonObject &answer, QString *error)
{
    auto reject = [&](const QString &message) {
        if (error)
            *error = message;
        return QJsonObject{};
    };
    if (!answer.value(QStringLiteral("levels")).isArray() || !answer.value(QStringLiteral("assignments")).isArray()
        || !answer.value(QStringLiteral("sources")).isArray() || !answer.value(QStringLiteral("uncertainties")).isArray())
        return reject(i18n("Incomplete structured model response."));
    const auto index = evidenceIndex(snap);
    const auto levels = answer.value(QStringLiteral("levels")).toArray();
    QSet<QString> unique;
    for (const auto &level : levels) {
        if (termById(level.toString()).isEmpty() || unique.contains(level.toString()))
            return reject(i18n("Unknown or duplicate structure term."));
        unique.insert(level.toString());
    }
    const auto stringsOnly = [](const QJsonValue &value) {
        if (!value.isArray())
            return false;
        for (const auto &entry : value.toArray())
            if (!entry.isString() || entry.toString().size() > 4096)
                return false;
        return true;
    };
    if (!stringsOnly(answer.value(QStringLiteral("sources"))) || !stringsOnly(answer.value(QStringLiteral("uncertainties"))))
        return reject(i18n("Invalid uncertainty or source hints."));
    QHash<QString, QJsonObject> proposed;
    for (const auto &v : answer.value(QStringLiteral("assignments")).toArray())
        proposed.insert(v.toObject().value(QStringLiteral("evidenceId")).toString(), v.toObject());
    QJsonArray assignments;
    for (const auto &value : answer.value(QStringLiteral("assignments")).toArray()) {
        auto a = value.toObject();
        const auto e = index.value(a.value(QStringLiteral("evidenceId")).toString());
        if (e.isEmpty() || !levels.contains(a.value(QStringLiteral("term"))) || !a.value(QStringLiteral("coordinate")).isArray()
            || !a.value(QStringLiteral("reason")).isString() || !a.value(QStringLiteral("uncertainties")).isArray())
            return reject(i18n("Model mapping has no valid EPUB evidence."));
        const auto coordinate = a.value(QStringLiteral("coordinate")).toArray();
        const auto original = e.value(QStringLiteral("numbers")).toArray();
        if (coordinate.isEmpty())
            continue; // Explicit abstention.
        if (coordinate.size() != arrayIndex(levels, a.value(QStringLiteral("term"))) + 1 || !stringsOnly(a.value(QStringLiteral("uncertainties"))))
            return reject(i18n("Model mapping depth conflicts with the proposed hierarchy."));
        for (const auto &n : coordinate)
            if (!n.isDouble() || n.toDouble() < 1 || n.toDouble() > 999999999 || std::floor(n.toDouble()) != n.toDouble())
                return reject(i18n("Model coordinates require positive whole numbers."));
        QString ancestor = e.value(QStringLiteral("parentEvidenceId")).toString();
        QSet<QString> visited;
        while (!ancestor.isEmpty() && !visited.contains(ancestor)) {
            visited.insert(ancestor);
            const auto parent = index.value(ancestor);
            const auto assignedParent = proposed.value(ancestor);
            const auto parentCoordinate = assignedParent.value(QStringLiteral("coordinate")).toArray();
            if (!parentCoordinate.isEmpty()) {
                if (parentCoordinate.size() >= coordinate.size())
                    return reject(i18n("Model parent conflicts with EPUB hierarchy."));
                for (int i = 0; i < parentCoordinate.size(); ++i)
                    if (coordinate[i] != parentCoordinate[i])
                        return reject(i18n("Model parent numbering conflicts with EPUB hierarchy."));
            }
            ancestor = parent.value(QStringLiteral("parentEvidenceId")).toString();
        }
        if (original.isEmpty() || coordinate.size() < original.size())
            return reject(i18n("Model invented numbering absent from the EPUB evidence."));
        for (int i = 0; i < original.size(); ++i)
            if (coordinate.at(coordinate.size() - original.size() + i) != original.at(i))
                return reject(i18n("Model numbering conflicts with EPUB source."));
        a.insert(QStringLiteral("origin"), QStringLiteral("model"));
        a.insert(QStringLiteral("confirmed"), false);
        a.insert(QStringLiteral("sources"), answer.value(QStringLiteral("sources")));
        assignments.append(a);
    }
    auto draft = makeDraft(snap, levels, assignments);
    draft.insert(QStringLiteral("uncertainties"), answer.value(QStringLiteral("uncertainties")));
    draft.insert(QStringLiteral("sources"), answer.value(QStringLiteral("sources")));
    return draft;
}

QJsonObject WorkStructure::confirmedRecord(const QJsonObject &snap, const QJsonObject &draft, QString *error)
{
    auto reject = [&](const QString &message) {
        if (error)
            *error = message;
        return QJsonObject{};
    };
    if (snap.value(QStringLiteral("snapshotId")) != draft.value(QStringLiteral("snapshotId"))
        || snap.value(QStringLiteral("sourceBasisHash")).toString().isEmpty())
        return reject(i18n("The review snapshot is stale or missing."));
    const auto levelIds = draft.value(QStringLiteral("levels")).toArray();
    if (levelIds.isEmpty() || levelIds.size() > 6)
        return reject(i18n("Choose at least one structure level."));
    QJsonArray levels;
    QSet<QString> uniqueTerms;
    for (const auto &id : levelIds) {
        auto term = termById(id.toString());
        if (term.isEmpty() || uniqueTerms.contains(id.toString()))
            return reject(i18n("Unknown or duplicate structure level."));
        uniqueTerms.insert(id.toString());
        term.insert(QStringLiteral("term"), id);
        levels.append(term);
    }
    const auto index = evidenceIndex(snap);
    QJsonArray nodes, usedEvidence;
    QHash<QString, QString> nodeAtCoordinate;
    QSet<QString> usedIds;
    QHash<QString, QJsonArray> usedLocators;
    for (const auto &value : draft.value(QStringLiteral("assignments")).toArray()) {
        const auto a = value.toObject();
        if (!a.value(QStringLiteral("confirmed")).toBool())
            continue;
        const auto e = index.value(a.value(QStringLiteral("evidenceId")).toString());
        const auto coordinate = a.value(QStringLiteral("coordinate")).toArray();
        const int depth = arrayIndex(levelIds, a.value(QStringLiteral("term")));
        if (e.isEmpty() || depth < 0 || coordinate.size() != depth + 1 || usedIds.contains(e.value(QStringLiteral("id")).toString()))
            return reject(i18n("A confirmed mapping has invalid evidence or depth."));
        for (const auto &n : coordinate) {
            const double num = n.toDouble();
            if (!n.isDouble() || !std::isfinite(num) || num < 1 || num > 999999999 || std::floor(num) != num)
                return reject(i18n("Coordinates require positive whole numbers."));
        }
        const QString key = coordinateKey(coordinate);
        const QString cfi = e.value(QStringLiteral("cfi")).toString();
        const QString href = e.value(QStringLiteral("href")).toString();
        const QString locator = cfi.isEmpty() ? href : cfi;
        if (locator.isEmpty() || nodeAtCoordinate.contains(key))
            return reject(i18n("Two mappings share a coordinate or source target."));
        const QString id = e.value(QStringLiteral("id")).toString();
        usedIds.insert(id);
        if (usedLocators.contains(locator)) {
            const auto previous = usedLocators.value(locator);
            for (int i = 0; i < qMin(previous.size(), coordinate.size()); ++i)
                if (previous[i] != coordinate[i])
                    return reject(i18n("Two mappings share a coordinate or source target."));
        }
        usedLocators.insert(locator, coordinate);
        nodeAtCoordinate.insert(key, id);
        QJsonObject node{{QStringLiteral("id"), id},
                         {QStringLiteral("level"), a.value(QStringLiteral("term"))},
                         {QStringLiteral("number"), coordinate.last()},
                         {QStringLiteral("coordinate"), coordinate},
                         {QStringLiteral("locators"),
                          QJsonArray{QJsonObject{{QStringLiteral("href"), href},
                                                 {QStringLiteral("cfi"), cfi},
                                                 {QStringLiteral("documentPath"), e.value(QStringLiteral("documentPath"))}}}},
                         {QStringLiteral("evidenceIds"), QJsonArray{id}},
                         {QStringLiteral("origin"), a.value(QStringLiteral("origin"))},
                         {QStringLiteral("reason"), a.value(QStringLiteral("reason"))},
                         {QStringLiteral("uncertainties"), a.value(QStringLiteral("uncertainties"))},
                         {QStringLiteral("marker"), e.value(QStringLiteral("marker"))},
                         {QStringLiteral("validation"), QStringLiteral("user-confirmed")}};
        if (a.contains(QStringLiteral("sources"))) {
            node.insert(QStringLiteral("sources"), a.value(QStringLiteral("sources")));
            node.insert(QStringLiteral("sourcesVerification"), QStringLiteral("unverified"));
        }
        nodes.append(node);
        usedEvidence.append(e);
    }
    if (nodes.isEmpty())
        return reject(i18n("Confirm at least one mapping before writing the EPUB."));
    for (int i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i].toObject();
        auto coordinate = node.value(QStringLiteral("coordinate")).toArray();
        coordinate.removeLast();
        if (coordinate.isEmpty())
            node.insert(QStringLiteral("parent"), QJsonValue(QJsonValue::Null));
        else {
            const auto parent = nodeAtCoordinate.value(coordinateKey(coordinate));
            if (parent.isEmpty())
                return reject(i18n("A confirmed child needs its confirmed parent mapping."));
            node.insert(QStringLiteral("parent"), parent);
        }
        nodes[i] = node;
    }
    QJsonArray formats;
    const auto separators = draft.value(QStringLiteral("separators")).toArray();
    for (const auto &separator : separators)
        if (!QJsonArray{QStringLiteral("-"), QStringLiteral("."), QStringLiteral(":")}.contains(separator))
            return reject(i18n("Unsupported coordinate separator."));
    for (int depth = 1; depth <= levelIds.size(); ++depth) {
        QJsonArray subset;
        for (int i = 0; i < depth; ++i)
            subset.append(levelIds[i]);
        formats.append(QJsonObject{{QStringLiteral("levels"), subset},
                                   {QStringLiteral("separators"), separators},
                                   {QStringLiteral("digitSets"), QJsonArray{QStringLiteral("ascii"), QStringLiteral("devanagari")}}});
    }
    return {{QStringLiteral("type"), typeName},
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("revision"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("bookIdentifier"), snap.value(QStringLiteral("bookId"))},
            {QStringLiteral("analysis"),
             QJsonObject{{QStringLiteral("algorithmVersion"), snap.value(QStringLiteral("algorithmVersion"))},
                         {QStringLiteral("sourceBasisVersion"), 1},
                         {QStringLiteral("sourceBasisHash"), snap.value(QStringLiteral("sourceBasisHash"))},
                         {QStringLiteral("sources"), snap.value(QStringLiteral("sources"))},
                         {QStringLiteral("calculatedAt"), snap.value(QStringLiteral("capturedAt"))},
                         {QStringLiteral("model"), draft.value(QStringLiteral("model"))},
                         {QStringLiteral("promptVersion"), QStringLiteral("sanskrit-structure-1")}}},
            {QStringLiteral("review"), QJsonObject{{QStringLiteral("status"), QStringLiteral("confirmed")}, {QStringLiteral("confirmedAt"), now()}}},
            {QStringLiteral("levels"), levels},
            {QStringLiteral("nodes"), nodes},
            {QStringLiteral("evidence"), usedEvidence},
            {QStringLiteral("coordinateFormats"), formats},
            {QStringLiteral("scope"), QJsonObject{{QStringLiteral("partial"), nodes.size() < snap.value(QStringLiteral("evidence")).toArray().size()}}}};
}

QJsonObject WorkStructure::read(EPubContainer &book, const QJsonObject &snap)
{
    const auto storedRecord = stored(book);
    if (!storedRecord.error.isEmpty())
        return failure(QStringLiteral("invalid"), storedRecord.error, storedRecord.record);
    if (!storedRecord.present)
        return failure(QStringLiteral("absent"), {});
    const auto record = storedRecord.record;
    if (record.value(QStringLiteral("schemaVersion")) != QJsonValue(1))
        return failure(QStringLiteral("unsupported"), i18n("Unsupported structure schema version."), record);
    if (record.value(QStringLiteral("bookIdentifier")) != snap.value(QStringLiteral("bookId")))
        return failure(QStringLiteral("invalid"), i18n("Structure belongs to another book."), record);
    if (record.value(QStringLiteral("review")).toObject().value(QStringLiteral("status")) != QJsonValue(QStringLiteral("confirmed")))
        return failure(QStringLiteral("unconfirmed"), i18n("Structure is not confirmed."), record);
    if (record.value(QStringLiteral("analysis")).toObject().value(QStringLiteral("sourceBasisHash")) != snap.value(QStringLiteral("sourceBasisHash")))
        return failure(QStringLiteral("stale"), i18n("EPUB source dependencies have changed. Review the preserved structure."), record);
    QJsonArray ids, mappings;
    const auto index = evidenceIndex(snap);
    for (const auto &value : record.value(QStringLiteral("levels")).toArray()) {
        const auto level = value.toObject();
        const auto canonical = termById(level.value(QStringLiteral("term")).toString());
        if (canonical.isEmpty() || level.value(QStringLiteral("iast")) != canonical.value(QStringLiteral("iast"))
            || level.value(QStringLiteral("devanagari")) != canonical.value(QStringLiteral("devanagari")))
            return failure(QStringLiteral("invalid"), i18n("Stored structure term is not in the verified vocabulary."), record);
        ids.append(level.value(QStringLiteral("term")));
    }
    for (const auto &value : record.value(QStringLiteral("nodes")).toArray()) {
        const auto n = value.toObject();
        const auto e = index.value(n.value(QStringLiteral("id")).toString());
        if (e.isEmpty())
            return failure(QStringLiteral("invalid"), i18n("Structure target has no current source evidence."), record);
        if (n.value(QStringLiteral("marker")) != e.value(QStringLiteral("marker")) || n.value(QStringLiteral("coordinate")).toArray().isEmpty()
            || n.value(QStringLiteral("number")) != n.value(QStringLiteral("coordinate")).toArray().last())
            return failure(QStringLiteral("invalid"), i18n("Stored verse marker or numbering conflicts with source."), record);
        const auto locator = n.value(QStringLiteral("locators")).toArray();
        if (locator.size() != 1 || locator[0].toObject().value(QStringLiteral("href")) != e.value(QStringLiteral("href"))
            || locator[0].toObject().value(QStringLiteral("cfi")) != e.value(QStringLiteral("cfi")))
            return failure(QStringLiteral("invalid"), i18n("Structure target differs from EPUB evidence."), record);
        mappings.append(QJsonObject{{QStringLiteral("evidenceId"), n.value(QStringLiteral("id"))},
                                    {QStringLiteral("term"), n.value(QStringLiteral("level"))},
                                    {QStringLiteral("coordinate"), n.value(QStringLiteral("coordinate"))},
                                    {QStringLiteral("confirmed"), true}});
    }
    auto draft = makeDraft(snap, ids, mappings);
    draft.insert(QStringLiteral("separators"), QJsonArray{});
    QString error;
    const auto rebuilt = confirmedRecord(snap, draft, &error);
    if (rebuilt.isEmpty())
        return failure(QStringLiteral("invalid"), error, record);
    for (int i = 0; i < record.value(QStringLiteral("nodes")).toArray().size(); ++i)
        if (record.value(QStringLiteral("nodes")).toArray()[i].toObject().value(QStringLiteral("parent"))
            != rebuilt.value(QStringLiteral("nodes")).toArray()[i].toObject().value(QStringLiteral("parent")))
            return failure(QStringLiteral("invalid"), i18n("Structure parent relationships conflict."), record);
    for (const auto &f : record.value(QStringLiteral("coordinateFormats")).toArray()) {
        const auto format = f.toObject();
        const auto terms = format.value(QStringLiteral("levels")).toArray();
        if (terms.isEmpty() || terms.size() > ids.size())
            return failure(QStringLiteral("invalid"), i18n("Invalid coordinate format."), record);
        for (int i = 0; i < terms.size(); ++i)
            if (terms[i] != ids[i])
                return failure(QStringLiteral("invalid"), i18n("Coordinate levels conflict."), record);
        for (const auto &s : format.value(QStringLiteral("separators")).toArray())
            if (!QJsonArray{QStringLiteral("-"), QStringLiteral("."), QStringLiteral(":")}.contains(s))
                return failure(QStringLiteral("invalid"), i18n("Unknown coordinate separator."), record);
    }
    return {{QStringLiteral("validity"), QStringLiteral("valid")},
            {QStringLiteral("record"), record},
            {QStringLiteral("revision"), record.value(QStringLiteral("revision"))}};
}

QJsonObject WorkStructure::node(const QJsonObject &record, const QString &nodeId)
{
    QJsonObject result;
    for (const auto &v : record.value(QStringLiteral("nodes")).toArray())
        if (v.toObject().value(QStringLiteral("id")).toString() == nodeId)
            result = v.toObject();
    if (result.isEmpty())
        return {};
    QJsonArray path;
    const auto coordinate = result.value(QStringLiteral("coordinate")).toArray();
    const auto levels = record.value(QStringLiteral("levels")).toArray();
    for (int i = 0; i < coordinate.size() && i < levels.size(); ++i) {
        auto part = termById(levels[i].toObject().value(QStringLiteral("term")).toString());
        part.insert(QStringLiteral("number"), coordinate[i]);
        path.append(part);
    }
    result.insert(QStringLiteral("semanticPath"), path);
    result.insert(QStringLiteral("structureRevision"), record.value(QStringLiteral("revision")));
    return result;
}
QJsonObject WorkStructure::atLocation(const QJsonObject &record, const QString &loc)
{
    QJsonObject match;
    for (const auto &value : record.value(QStringLiteral("nodes")).toArray()) {
        const auto n = value.toObject();
        for (const auto &v : n.value(QStringLiteral("locators")).toArray()) {
            const auto locator = v.toObject();
            const QString href = locator.value(QStringLiteral("href")).toString();
            const QString cfi = locator.value(QStringLiteral("cfi")).toString();
            if (loc.isEmpty() || (loc != cfi && (loc != href || (!href.contains(QLatin1Char('#')) && !cfi.isEmpty()))))
                continue;
            if (!match.isEmpty() && match.value(QStringLiteral("id")) != n.value(QStringLiteral("id"))) {
                const auto a = match.value(QStringLiteral("coordinate")).toArray();
                const auto b = n.value(QStringLiteral("coordinate")).toArray();
                for (int i = 0; i < qMin(a.size(), b.size()); ++i)
                    if (a[i] != b[i])
                        return {};
                if (a.size() >= b.size())
                    continue;
            }
            match = n;
        }
    }
    return match.isEmpty() ? QJsonObject{} : node(record, match.value(QStringLiteral("id")).toString());
}
QJsonObject WorkStructure::resolve(const QJsonObject &record, const QString &text, const QString &role)
{
    if (!QStringList{QStringLiteral("navigation"), QStringLiteral("verse-marker"), QStringLiteral("cross-reference")}.contains(role))
        return {};
    const QString normalized = digits(text.trimmed());
    if (!QRegularExpression(QStringLiteral("^[0-9]{1,9}(?:[.:-][0-9]{1,9})*$")).match(normalized).hasMatch())
        return {};
    QJsonArray coordinate;
    for (const auto &part : normalized.split(QRegularExpression(QStringLiteral("[.:-]")))) {
        if (part.startsWith(QLatin1Char('0')))
            return {};
        coordinate.append(part.toInt());
    }
    bool accepted = false;
    for (const auto &value : record.value(QStringLiteral("coordinateFormats")).toArray()) {
        const auto f = value.toObject();
        if (f.value(QStringLiteral("levels")).toArray().size() != coordinate.size())
            continue;
        bool all = true;
        for (const auto &c : normalized)
            if (!c.isDigit() && !f.value(QStringLiteral("separators")).toArray().contains(QString(c)))
                all = false;
        accepted |= all;
    }
    if (!accepted)
        return {};
    QJsonObject result;
    for (const auto &value : record.value(QStringLiteral("nodes")).toArray()) {
        const auto n = value.toObject();
        if (n.value(QStringLiteral("coordinate")).toArray() != coordinate)
            continue;
        if (!result.isEmpty())
            return {};
        result = node(record, n.value(QStringLiteral("id")).toString());
    }
    return result;
}

QByteArray WorkStructure::candidate(EPubContainer &book, const QJsonObject &record, QString *error)
{
    auto reject = [&](const QString &message) {
        if (error)
            *error = message;
        return QByteArray{};
    };
    const auto old = stored(book);
    if (!old.error.isEmpty() || (old.present && old.record.value(QStringLiteral("schemaVersion")) != QJsonValue(1)))
        return reject(i18n("Existing structure cannot be safely replaced."));
    auto doc = parse(book.readData(book.packagePath()));
    auto metadata = child(doc.documentElement(), QStringLiteral("metadata"));
    if (metadata.isNull())
        return reject(i18n("No package metadata element."));
    const QByteArray json = QJsonDocument(record).toJson(QJsonDocument::Indented);
    if (json.size() > maxRecordBytes)
        return reject(i18n("Structure record is too large."));
    QHash<QString, QByteArray> replacements, additions;
    if (doc.documentElement().attribute(QStringLiteral("version")).startsWith(QLatin1Char('2'))) {
        QDomElement element;
        for (auto n = metadata.firstChildElement(); !n.isNull(); n = n.nextSiblingElement())
            if (local(n) == QLatin1String("meta") && n.attribute(QStringLiteral("name")) == legacyName)
                element = n;
        if (element.isNull()) {
            element = doc.createElementNS(opfNs, QStringLiteral("meta"));
            metadata.appendChild(element);
        }
        element.setAttribute(QStringLiteral("name"), legacyName);
        element.setAttribute(QStringLiteral("content"), QString::fromUtf8(json));
    } else {
        QString path = old.path;
        if (path.isEmpty())
            path = QDir::cleanPath(QFileInfo(book.packagePath()).path() + QStringLiteral("/metadata/arianna-work-structure.json"));
        if (old.path.isEmpty() && book.rootDirectory()->entry(path))
            return reject(i18n("Structure resource path already exists."));
        QDomElement link;
        for (auto n = metadata.firstChildElement(); !n.isNull(); n = n.nextSiblingElement()) {
            if (local(n) == QLatin1String("link") && (!old.path.isEmpty() && location(book.packagePath(), n.attribute(QStringLiteral("href"))) == old.path))
                link = n;
            if (n.attribute(QStringLiteral("id")) == recordId && local(n) != QLatin1String("link"))
                return reject(i18n("Structure metadata ID already exists."));
        }
        if (link.isNull()) {
            link = doc.createElementNS(opfNs, QStringLiteral("link"));
            metadata.appendChild(link);
        }
        link.setAttribute(QStringLiteral("id"), recordId);
        link.setAttribute(QStringLiteral("rel"), QStringLiteral("record"));
        link.setAttribute(QStringLiteral("media-type"), QStringLiteral("application/json"));
        link.setAttribute(QStringLiteral("href"), QDir(QLatin1Char('/') + QFileInfo(book.packagePath()).path()).relativeFilePath(QLatin1Char('/') + path));
        (old.path.isEmpty() ? additions : replacements).insert(path, json);
        QDomElement modified;
        for (auto n = metadata.firstChildElement(); !n.isNull(); n = n.nextSiblingElement())
            if (local(n) == QLatin1String("meta") && n.attribute(QStringLiteral("property")) == QLatin1String("dcterms:modified")
                && !n.hasAttribute(QStringLiteral("refines")))
                modified = n;
        if (modified.isNull()) {
            modified = doc.createElementNS(opfNs, QStringLiteral("meta"));
            modified.setAttribute(QStringLiteral("property"), QStringLiteral("dcterms:modified"));
            metadata.appendChild(modified);
        }
        while (modified.hasChildNodes())
            modified.removeChild(modified.firstChild());
        modified.appendChild(doc.createTextNode(now()));
    }
    replacements.insert(book.packagePath(), doc.toByteArray());
    const auto bytes = book.createMetadataCandidate(replacements, additions);
    if (bytes.isEmpty())
        return reject(i18n("Could not copy every EPUB resource into the candidate."));
    return bytes;
}

bool WorkStructure::preservesSource(EPubContainer &before, EPubContainer &after, QString *error)
{
    auto reject = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto old = stored(before);
    const auto next = stored(after);
    if (!next.error.isEmpty() || !next.present || after.packagePath() != before.packagePath())
        return reject(i18n("Candidate metadata record is invalid."));
    if (comparableOpf(parse(before.readData(before.packagePath()))) != comparableOpf(parse(after.readData(after.packagePath()))))
        return reject(i18n("Candidate changed unrelated package metadata or spine."));
    QStringList oldPaths, newPaths;
    archivePaths(before.rootDirectory(), {}, oldPaths);
    archivePaths(after.rootDirectory(), {}, newPaths);
    for (const auto &path : oldPaths) {
        if (path == before.packagePath() || (!old.path.isEmpty() && path == old.path))
            continue;
        if (!newPaths.contains(path) || before.readData(path) != after.readData(path))
            return reject(i18n("Candidate changed a source resource: ") + path);
    }
    for (const auto &path : newPaths)
        if (!oldPaths.contains(path) && path != next.path)
            return reject(i18n("Candidate added an unrelated resource."));
    return true;
}
