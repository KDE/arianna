// SPDX-FileCopyrightText: 2018 Martin T. H. Sandsmark <martin.sandsmark@kde.org>
// SPDX-FileCopyrightText: 2021 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: BSD-3-Clause

#include "epubcontainer.h"

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KLocalizedString>

#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

#include <KZip>
#include <QBuffer>
#include <QCryptographicHash>
#include <QMimeDatabase>

#include <algorithm>
#include <functional>
#include <qpropertyprivate.h>
#include <utility>

#define METADATA_FOLDER QStringLiteral("META-INF")
#define MIMETYPE_FILE QStringLiteral("mimetype")
#define CONTAINER_FILE QStringLiteral("META-INF/container.xml")

static QString escapeXmlAttribute(const QString &value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
    escaped.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
    escaped.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
    return escaped;
}

static QString escapeXmlText(const QString &value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
    escaped.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
    escaped.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
    return escaped;
}

static QStringList normalizedMetadataList(const QStringList &values)
{
    QStringList result;
    QSet<QString> seen;

    for (QString value : values) {
        value.replace(QLatin1Char(';'), QLatin1Char(','));
        const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString item : items) {
            item = item.trimmed();
            if (item.isEmpty()) {
                continue;
            }

            const QString key = item.toCaseFolded();
            if (seen.contains(key)) {
                continue;
            }

            seen.insert(key);
            result.append(item);
        }
    }

    return result;
}

static QString normalizedSubjectPath(QString subject)
{
    subject.replace(QLatin1Char(';'), QLatin1Char(','));

    QStringList segments;
    const QStringList rawSegments = subject.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (QString segment : rawSegments) {
        segment = segment.trimmed().simplified();
        if (!segment.isEmpty()) {
            segments.append(segment);
        }
    }

    if (!segments.isEmpty()) {
        return segments.join(QLatin1Char('/'));
    }

    return subject.trimmed().simplified();
}

static QStringList normalizedSubjectPathList(const QStringList &values)
{
    QStringList result;
    QSet<QString> seen;

    for (const QString &value : normalizedMetadataList(values)) {
        const QString path = normalizedSubjectPath(value);
        if (path.isEmpty()) {
            continue;
        }

        const QString key = path.toCaseFolded();
        if (seen.contains(key)) {
            continue;
        }

        seen.insert(key);
        result.append(path);
    }

    return result;
}

static QStringList subjectPathsWithAncestors(const QStringList &subjects)
{
    QStringList result;
    QSet<QString> seen;

    for (const QString &subject : normalizedSubjectPathList(subjects)) {
        QStringList path;
        const QStringList segments = subject.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &segment : segments) {
            path.append(segment);
            const QString ancestor = path.join(QLatin1Char('/'));
            const QString key = ancestor.toCaseFolded();
            if (seen.contains(key)) {
                continue;
            }

            seen.insert(key);
            result.append(ancestor);
        }
    }

    return result;
}

static QString subjectLabelForPath(const QString &path)
{
    return path.section(QLatin1Char('/'), -1).trimmed();
}

static QString subjectParentPath(const QString &path)
{
    const qsizetype separator = path.lastIndexOf(QLatin1Char('/'));
    if (separator < 0) {
        return {};
    }

    return path.left(separator);
}

static int subjectLevelForPath(const QString &path)
{
    if (path.isEmpty()) {
        return 1;
    }

    return path.count(QLatin1Char('/')) + 1;
}

static constexpr QLatin1StringView AriannaSubjectAuthority("Arianna");
static constexpr QLatin1StringView AriannaPackageVocabulary("https://kde.org/ns/arianna/epub/vocab/#");
static constexpr QLatin1StringView AriannaSubjectLevelProperty("arianna:subject-level");
static constexpr QLatin1StringView AriannaParentSubjectProperty("arianna:parent-subject");

static QString localElementName(const QDomElement &element)
{
    QString localName = element.localName().isEmpty() ? element.tagName() : element.localName();
    const qsizetype namespaceSeparator = localName.indexOf(QLatin1Char(':'));
    if (namespaceSeparator >= 0) {
        localName = localName.mid(namespaceSeparator + 1);
    }
    return localName.toLower();
}

static bool isDublinCoreSubjectElement(const QDomElement &element)
{
    static const QString dublinCoreNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");
    if (element.isNull() || localElementName(element) != QStringLiteral("subject")) {
        return false;
    }

    return element.namespaceURI() == dublinCoreNamespace || element.prefix() == QStringLiteral("dc") || element.tagName() == QStringLiteral("dc:subject");
}

static bool isDublinCoreCreatorElement(const QDomElement &element)
{
    static const QString dublinCoreNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");
    if (element.isNull() || localElementName(element) != QStringLiteral("creator")) {
        return false;
    }

    return element.namespaceURI() == dublinCoreNamespace || element.prefix() == QStringLiteral("dc") || element.tagName() == QStringLiteral("dc:creator");
}

static QString refinedSubjectPath(const QDomElement &subjectElement, const QDomNodeList &nodeList)
{
    const QString label = normalizedSubjectPath(subjectElement.text());
    const QString id = subjectElement.attribute(QStringLiteral("id")).trimmed();
    if (id.isEmpty()) {
        return label;
    }

    const QString refines = QStringLiteral("#") + id;
    QString authority;
    QString term;
    bool hasAriannaHierarchy = false;

    for (int i = 0; i < nodeList.size(); ++i) {
        const QDomElement element = nodeList.at(i).toElement();
        if (element.tagName() != QStringLiteral("meta") || element.attribute(QStringLiteral("refines")) != refines) {
            continue;
        }

        const QString property = element.attribute(QStringLiteral("property")).trimmed();
        if (property == QStringLiteral("authority")) {
            authority = element.text().trimmed();
        } else if (property == QStringLiteral("term")) {
            term = normalizedSubjectPath(element.text());
        } else if (property == QString(AriannaSubjectLevelProperty) || property == QString(AriannaParentSubjectProperty)) {
            hasAriannaHierarchy = true;
        }
    }

    if (!term.isEmpty() && (authority.compare(QString(AriannaSubjectAuthority), Qt::CaseInsensitive) == 0 || hasAriannaHierarchy)) {
        return term;
    }

    return label;
}

static QString appendProperty(const QString &properties, const QString &property)
{
    QStringList parts = properties.split(u' ', Qt::SkipEmptyParts);

    if (!parts.contains(property)) {
        parts.append(property);
    }

    return parts.join(u' ');
}

static QString xmlLocalName(const QDomElement &element)
{
    const QString name = element.localName().isEmpty() ? element.tagName() : element.localName();
    const qsizetype separator = name.indexOf(QLatin1Char(':'));
    return (separator >= 0 ? name.mid(separator + 1) : name).toLower();
}

static QDomElement firstChildElementByLocalName(const QDomElement &parent, const QString &localName)
{
    for (QDomNode child = parent.firstChild(); !child.isNull(); child = child.nextSibling()) {
        const QDomElement element = child.toElement();
        if (!element.isNull() && xmlLocalName(element) == localName) {
            return element;
        }
    }

    return {};
}

static bool nodeHasElementByLocalName(const QDomNode &node, const QString &localName)
{
    for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
        const QDomElement element = child.toElement();
        if (!element.isNull() && xmlLocalName(element) == localName) {
            return true;
        }
        if (nodeHasElementByLocalName(child, localName)) {
            return true;
        }
    }

    return false;
}

static bool documentHasElementByLocalName(const QDomDocument &doc, const QString &localName)
{
    const QDomElement root = doc.documentElement();
    return (!root.isNull() && xmlLocalName(root) == localName) || nodeHasElementByLocalName(root, localName);
}

static bool isXmlLikeFile(const QString &path)
{
    const QString lower = path.toLower();

    return lower.endsWith(QStringLiteral(".xhtml")) || lower.endsWith(QStringLiteral(".html")) || lower.endsWith(QStringLiteral(".htm"))
        || lower.endsWith(QStringLiteral(".svg")) || lower.endsWith(QStringLiteral(".smil"));
}

static bool isRemoteResourceHostMime(const QString &mediaType)
{
    return mediaType == QStringLiteral("application/xhtml+xml") || mediaType == QStringLiteral("image/svg+xml")
        || mediaType == QStringLiteral("application/smil+xml");
}

static bool isCssFile(const QString &path)
{
    return path.toLower().endsWith(QStringLiteral(".css"));
}

static QString archivePathFromUri(QString path)
{
    const qsizetype fragmentIndex = path.indexOf(QLatin1Char('#'));
    if (fragmentIndex >= 0) {
        path = path.left(fragmentIndex);
    }

    path = QUrl::fromPercentEncoding(path.toUtf8());
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }

    return QDir::cleanPath(path);
}

static QString pathWithoutFragment(QString path);
static bool setDomContentPreservingMarkup(QDomDocument &doc, const QByteArray &data);
static bool setDomContentWithNamespaces(QDomDocument &doc, const QByteArray &data);

static QString resolveRelativePath(const QString &basePath, const QString &relativePath)
{
    if (relativePath.startsWith(QStringLiteral("http://")) || relativePath.startsWith(QStringLiteral("https://"))
        || relativePath.startsWith(QStringLiteral("data:")) || relativePath.startsWith(QStringLiteral("blob:"))
        || relativePath.startsWith(QStringLiteral("#"))) {
        return relativePath;
    }

    const QString cleanRelative = archivePathFromUri(relativePath);
    const QString baseDir = QFileInfo(basePath).path();

    return QDir::cleanPath(baseDir + QStringLiteral("/") + cleanRelative);
}

static QString relativePathFrom(const QString &basePath, const QString &targetPath)
{
    const QString baseDir = QFileInfo(basePath).path();
    if (baseDir.isEmpty() || baseDir == QStringLiteral(".")) {
        return targetPath;
    }

    const QString prefix = baseDir + QLatin1Char('/');
    if (targetPath.startsWith(prefix)) {
        return targetPath.mid(prefix.size());
    }

    return QDir(baseDir).relativeFilePath(targetPath);
}

static QString resolveReferenceTarget(QString basePath, QString href)
{
    href = href.trimmed();
    if (href.isEmpty()) {
        return {};
    }

    if (href.startsWith(QStringLiteral("epubcfi("))) {
        return href;
    }

    const QString lower = href.toLower();
    if (lower.startsWith(QStringLiteral("http://")) || lower.startsWith(QStringLiteral("https://")) || lower.startsWith(QStringLiteral("data:"))
        || lower.startsWith(QStringLiteral("blob:")) || lower.startsWith(QStringLiteral("javascript:")) || lower.startsWith(QStringLiteral("mailto:"))) {
        return {};
    }

    basePath = pathWithoutFragment(basePath);

    QString fragment;
    const qsizetype fragmentIndex = href.indexOf(QLatin1Char('#'));
    if (fragmentIndex >= 0) {
        fragment = QStringLiteral("#") + QUrl::fromPercentEncoding(href.mid(fragmentIndex + 1).toUtf8());
        href = href.left(fragmentIndex);
    }

    QString targetPath;
    if (href.isEmpty()) {
        targetPath = basePath;
    } else {
        const QString cleanHref = archivePathFromUri(href);
        const QString baseDir = QFileInfo(basePath).path();
        targetPath = QDir::cleanPath(baseDir + QStringLiteral("/") + cleanHref);
    }

    if (targetPath == QStringLiteral(".")) {
        targetPath.clear();
    }

    return targetPath + fragment;
}

static QString pathWithoutFragment(QString path)
{
    const qsizetype fragmentIndex = path.indexOf(QLatin1Char('#'));
    if (fragmentIndex >= 0) {
        path = path.left(fragmentIndex);
    }
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }

    return QDir::cleanPath(path);
}

static bool isSupportedImageItem(const EpubItem &item)
{
    return QImageReader::supportedMimeTypes().contains(item.mimetype);
}

static bool isDocumentItem(const EpubItem &item)
{
    return item.mimetype == QByteArrayLiteral("application/xhtml+xml") || item.mimetype == QByteArrayLiteral("text/html")
        || item.mimetype == QByteArrayLiteral("application/x-dtbook+xml") || item.mimetype == QByteArrayLiteral("text/x-oeb1-document");
}

static bool isNavigationDocumentItem(const EpubItem &item)
{
    return item.properties.contains(QStringLiteral("nav")) || item.mimetype == QByteArrayLiteral("application/x-dtbncx+xml");
}

static QString imageReference(const QDomElement &element)
{
    const QString localName = element.localName().isEmpty() ? element.tagName() : element.localName();
    if (localName == QStringLiteral("img")) {
        return element.attribute(QStringLiteral("src"));
    }
    if (localName == QStringLiteral("image")) {
        QString href = element.attribute(QStringLiteral("href"));
        if (href.isEmpty()) {
            href = element.attribute(QStringLiteral("xlink:href"));
        }
        return href;
    }

    return {};
}

static void collectImageReferences(const QDomNode &node, QStringList &references)
{
    const QDomElement element = node.toElement();
    if (!element.isNull()) {
        const QString reference = imageReference(element);
        if (!reference.isEmpty()) {
            references.append(reference);
        }
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        collectImageReferences(child, references);
        child = child.nextSibling();
    }
}

static QByteArray rewriteCssResourceLinks(const QByteArray &data, const QString &currentPath, const ResourceMap &resourceMap)
{
    QString text = QString::fromUtf8(data);

    static const QRegularExpression urlRegex(QStringLiteral(R"###(url\(\s*['"]?([^'")]+)['"]?\s*\))###"));

    QRegularExpressionMatchIterator it = urlRegex.globalMatch(text);

    QList<QPair<QString, QString>> replacements;

    while (it.hasNext()) {
        const auto match = it.next();

        const QString original = match.captured(0);

        const QString value = match.captured(1);

        if (value.startsWith(QStringLiteral("http://")) || value.startsWith(QStringLiteral("https://")) || value.startsWith(QStringLiteral("data:"))
            || value.startsWith(QStringLiteral("blob:")) || value.startsWith(QStringLiteral("#"))) {
            continue;
        }

        const QString resolved = resolveRelativePath(currentPath, value);

        if (!resourceMap.contains(resolved)) {
            continue;
        }

        const QString rewritten = QStringLiteral("url(\"") + resourceMap.value(resolved) + QStringLiteral("\")");

        replacements.append({original, rewritten});

        qDebug() << "Rewriting CSS resource:" << value << "->" << resourceMap.value(resolved);
    }

    for (const auto &[from, to] : replacements) {
        text.replace(from, to);
    }

    return text.toUtf8();
}

static QByteArray rewriteXmlResourceLinks(const QByteArray &data, const QString &currentPath, const ResourceMap &resourceMap)
{
    QString text = QString::fromUtf8(data);

    static const QRegularExpression attrRegex(QStringLiteral(R"###((src|href|poster|data-sync)=["']([^"']+)["'])###"));

    QRegularExpressionMatchIterator it = attrRegex.globalMatch(text);

    QList<QPair<QString, QString>> replacements;

    while (it.hasNext()) {
        const auto match = it.next();

        const QString original = match.captured(0);

        const QString attr = match.captured(1);

        const QString value = match.captured(2);

        if (value.startsWith(QStringLiteral("http://")) || value.startsWith(QStringLiteral("https://")) || value.startsWith(QStringLiteral("data:"))
            || value.startsWith(QStringLiteral("blob:")) || value.startsWith(QStringLiteral("#"))) {
            continue;
        }

        const QString resolved = resolveRelativePath(currentPath, value);

        if (!resourceMap.contains(resolved)) {
            continue;
        }

        const QString rewritten = attr + QStringLiteral("=\"") + escapeXmlAttribute(resourceMap.value(resolved)) + QStringLiteral("\"");

        replacements.append({original, rewritten});

        qDebug() << "Rewriting XML resource:" << value << "->" << resourceMap.value(resolved);
    }

    for (const auto &[from, to] : replacements) {
        text.replace(from, to);
    }

    return text.toUtf8();
}

struct ServerReadyCopyContext {
    ResourceMap resourceMap;
    bool includeReferences = false;
    QString opfPath;
    QString referencesPath;
    QVector<EpubReference> references;
    QHash<QString, EpubReference> referenceBySourceAnchor;
};

static QString uniqueManifestItemId(const QDomElement &manifest, const QString &baseId)
{
    QSet<QString> existingIds;
    const QDomNodeList itemNodes = manifest.elementsByTagName(QStringLiteral("item"));
    for (int i = 0; i < itemNodes.count(); ++i) {
        const QString id = itemNodes.at(i).toElement().attribute(QStringLiteral("id"));
        if (!id.isEmpty()) {
            existingIds.insert(id);
        }
    }

    QString id = baseId;
    int suffix = 2;
    while (existingIds.contains(id)) {
        id = baseId + QString::number(suffix++);
    }

    return id;
}

static void addReferencesDocumentToOpf(QDomDocument &doc, const QString &opfPath, const QString &referencesPath)
{
    const QDomNodeList manifestNodes = doc.elementsByTagName(QStringLiteral("manifest"));
    if (manifestNodes.isEmpty()) {
        return;
    }

    QDomElement manifest = manifestNodes.at(0).toElement();
    const QString href = relativePathFrom(opfPath, referencesPath);
    const QDomNodeList itemNodes = manifest.elementsByTagName(QStringLiteral("item"));
    QString referencesItemId;
    for (int i = 0; i < itemNodes.count(); ++i) {
        const QDomElement item = itemNodes.at(i).toElement();
        if (item.attribute(QStringLiteral("href")) == href) {
            referencesItemId = item.attribute(QStringLiteral("id"));
            break;
        }
    }

    const QString namespaceUri = manifest.namespaceURI();
    if (referencesItemId.isEmpty()) {
        referencesItemId = uniqueManifestItemId(manifest, QStringLiteral("arianna-references"));
        QDomElement item = namespaceUri.isEmpty() ? doc.createElement(QStringLiteral("item")) : doc.createElementNS(namespaceUri, QStringLiteral("item"));
        item.setAttribute(QStringLiteral("id"), referencesItemId);
        item.setAttribute(QStringLiteral("href"), href);
        item.setAttribute(QStringLiteral("media-type"), QStringLiteral("application/xhtml+xml"));
        manifest.appendChild(item);
    }

    const QDomNodeList spineNodes = doc.elementsByTagName(QStringLiteral("spine"));
    if (spineNodes.isEmpty()) {
        return;
    }

    QDomElement spine = spineNodes.at(0).toElement();
    const QDomNodeList itemRefNodes = spine.elementsByTagName(QStringLiteral("itemref"));
    for (int i = 0; i < itemRefNodes.count(); ++i) {
        if (itemRefNodes.at(i).toElement().attribute(QStringLiteral("idref")) == referencesItemId) {
            return;
        }
    }

    const QString spineNamespaceUri = spine.namespaceURI();
    QDomElement itemRef =
        spineNamespaceUri.isEmpty() ? doc.createElement(QStringLiteral("itemref")) : doc.createElementNS(spineNamespaceUri, QStringLiteral("itemref"));
    itemRef.setAttribute(QStringLiteral("idref"), referencesItemId);
    itemRef.setAttribute(QStringLiteral("linear"), QStringLiteral("no"));
    spine.appendChild(itemRef);
}

static QByteArray rewriteReferenceAnchorLinks(const QByteArray &data, const QString &currentPath, const ServerReadyCopyContext &context)
{
    if (!context.includeReferences || context.referenceBySourceAnchor.isEmpty()) {
        return data;
    }

    QDomDocument doc;
    if (!setDomContentWithNamespaces(doc, data)) {
        return data;
    }

    bool changed = false;
    const QString referenceHrefPrefix = relativePathFrom(currentPath, context.referencesPath) + QLatin1Char('#');
    const QDomNodeList anchors = doc.elementsByTagName(QStringLiteral("a"));
    for (int i = 0; i < anchors.count(); ++i) {
        QDomElement anchor = anchors.at(i).toElement();
        if (anchor.attribute(QStringLiteral("data-role")) != QStringLiteral("anchor")
            || anchor.attribute(QStringLiteral("data-anchor-type")) != QStringLiteral("crossref")) {
            continue;
        }

        const QString sourceAnchorId = anchor.attribute(QStringLiteral("id"));
        if (sourceAnchorId.isEmpty() || !context.referenceBySourceAnchor.contains(sourceAnchorId)) {
            continue;
        }

        anchor.setAttribute(QStringLiteral("href"), referenceHrefPrefix + sourceAnchorId);
        changed = true;
    }

    return changed ? doc.toByteArray() : data;
}

static QByteArray createReferencesDocument(const ServerReadyCopyContext &context)
{
    QString html;
    html += QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    html += QStringLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\">\n");
    html += QStringLiteral("<head><title>Arianna References</title></head>\n");
    html += QStringLiteral("<body>\n");
    html += QStringLiteral("<h1>References</h1>\n");

    if (context.references.isEmpty()) {
        html += QStringLiteral("<p>No references.</p>\n");
    }

    for (const EpubReference &reference : std::as_const(context.references)) {
        if (reference.sourceAnchorId.isEmpty()) {
            continue;
        }

        html += QStringLiteral("<section id=\"") + escapeXmlAttribute(reference.sourceAnchorId) + QStringLiteral("\">\n");
        html += QStringLiteral("<h2>") + escapeXmlText(reference.sourceAnchorTitle.isEmpty() ? QStringLiteral("Referenz") : reference.sourceAnchorTitle)
            + QStringLiteral("</h2>\n");
        html += QStringLiteral("<dl>\n");
        html += QStringLiteral("<dt>Source anchor</dt><dd>") + escapeXmlText(reference.sourceAnchorId) + QStringLiteral("</dd>\n");
        if (!reference.sourceAnchorTitle.isEmpty()) {
            html += QStringLiteral("<dt>Source title</dt><dd>") + escapeXmlText(reference.sourceAnchorTitle) + QStringLiteral("</dd>\n");
        }
        html += QStringLiteral("<dt>Target book</dt><dd>") + escapeXmlText(reference.targetBookId) + QStringLiteral("</dd>\n");
        html += QStringLiteral("<dt>Target location</dt><dd>") + escapeXmlText(reference.targetLocation) + QStringLiteral("</dd>\n");
        html += QStringLiteral("</dl>\n");
        if (!reference.targetPreviewHtml.isEmpty()) {
            html += QStringLiteral("<blockquote>") + escapeXmlText(reference.targetPreviewHtml) + QStringLiteral("</blockquote>\n");
        }
        html += QStringLiteral("</section>\n");
    }

    html += QStringLiteral("</body>\n</html>\n");
    return html.toUtf8();
}

static QByteArray rewriteOpfManifestLinks(const QByteArray &data, const QString &opfPath, const ServerReadyCopyContext &context)
{
    QDomDocument doc;

    if (!doc.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing)) {
        return data;
    }

    QDomNodeList manifestNodes = doc.elementsByTagName(QStringLiteral("manifest"));
    if (manifestNodes.isEmpty()) {
        return data;
    }

    QDomElement manifest = manifestNodes.at(0).toElement();
    QDomNodeList itemNodes = manifest.elementsByTagName(QStringLiteral("item"));

    for (int i = 0; i < itemNodes.count(); ++i) {
        QDomElement item = itemNodes.at(i).toElement();

        const QString href = item.attribute(QStringLiteral("href"));
        const QString mediaType = item.attribute(QStringLiteral("media-type"));

        if (href.isEmpty()) {
            continue;
        }

        const QString resolved = QDir::cleanPath(resolveRelativePath(opfPath, href));

        // 1. Manifest-Ressource selbst auf Server-URL umbiegen
        if (context.resourceMap.contains(resolved)) {
            item.setAttribute(QStringLiteral("href"), context.resourceMap.value(resolved));

            qDebug() << "Rewriting OPF manifest resource:" << resolved << "->" << context.resourceMap.value(resolved);
        }

        // 2. Content-Dokumente als Host von Remote Resources markieren
        if (!context.resourceMap.isEmpty() && isRemoteResourceHostMime(mediaType)) {
            const QString oldProperties = item.attribute(QStringLiteral("properties"));

            item.setAttribute(QStringLiteral("properties"), appendProperty(oldProperties, QStringLiteral("remote-resources")));
        }
    }

    if (context.includeReferences && !context.referencesPath.isEmpty()) {
        addReferencesDocumentToOpf(doc, opfPath, context.referencesPath);
    }

    return doc.toByteArray();
}

static void copyDirectoryServerReady(KZip &outZip, const KArchiveDirectory *dir, const QString &prefix, const ServerReadyCopyContext &context)
{
    if (!dir) {
        return;
    }

    const QStringList entries = dir->entries();

    for (const QString &entryName : entries) {
        const KArchiveEntry *entry = dir->entry(entryName);
        if (!entry) {
            continue;
        }

        const QString fullPath = prefix.isEmpty() ? entryName : prefix + QStringLiteral("/") + entryName;

        const QString cleanPath = QDir::cleanPath(fullPath);

        if (cleanPath == QStringLiteral("mimetype")) {
            continue;
        }

        if (entry->isDirectory()) {
            const auto *subdir = dynamic_cast<const KArchiveDirectory *>(entry);

            if (subdir) {
                copyDirectoryServerReady(outZip, subdir, cleanPath, context);
            }

            continue;
        }

        const auto *file = dynamic_cast<const KArchiveFile *>(entry);

        if (!file) {
            continue;
        }

        // Diese Dateien werden jetzt vom BookServer geliefert.
        if (context.resourceMap.contains(cleanPath)) {
            continue;
        }

        if (context.includeReferences && cleanPath == context.referencesPath) {
            continue;
        }

        QScopedPointer<QIODevice> dev(file->createDevice());
        if (!dev) {
            continue;
        }

        QByteArray data = dev->readAll();

        const QString lower = cleanPath.toLower();

        if (lower.endsWith(QStringLiteral(".opf"))) {
            data = rewriteOpfManifestLinks(data, cleanPath, context);
        } else if (isXmlLikeFile(cleanPath)) {
            data = rewriteReferenceAnchorLinks(data, cleanPath, context);
            data = rewriteXmlResourceLinks(data, cleanPath, context.resourceMap);
        } else if (isCssFile(cleanPath)) {
            data = rewriteCssResourceLinks(data, cleanPath, context.resourceMap);
        }

        outZip.writeFile(cleanPath, data);
    }
}

struct CfiStep {
    int index = -1;
    int offset = 0;
    bool hasOffset = false;
    QString id;
};

using CfiPath = QList<CfiStep>;

struct ParsedCfi {
    QVector<CfiPath> parent;
    QVector<CfiPath> start;
    QVector<CfiPath> end;
    bool isRange = false;
};

struct CfiBoundary {
    QDomNode node;
    int offset = -1;
    bool before = false;
    bool after = false;

    bool isValid() const
    {
        return !node.isNull();
    }
};

struct IndexedCfiChild {
    enum Kind {
        Node,
        TextGroup,
        Before,
        After,
        First,
        Last,
        Empty,
    };

    Kind kind = Empty;
    QDomNode node;
    QList<QDomNode> textNodes;
};

static QString unwrapEpubCfi(QString cfi)
{
    cfi = cfi.trimmed();

    static const QRegularExpression cfiRegex(QStringLiteral(R"(^epubcfi\((.*)\)$)"));
    const QRegularExpressionMatch match = cfiRegex.match(cfi);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }

    return cfi;
}

static QStringList splitCfiExpression(const QString &value, const QChar separator)
{
    QStringList parts;
    qsizetype start = 0;
    int bracketDepth = 0;
    bool escaped = false;

    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);

        if (escaped) {
            escaped = false;
            continue;
        }

        if (ch == QLatin1Char('^')) {
            escaped = true;
            continue;
        }

        if (ch == QLatin1Char('[')) {
            ++bracketDepth;
            continue;
        }

        if (ch == QLatin1Char(']')) {
            bracketDepth = std::max(0, bracketDepth - 1);
            continue;
        }

        if (ch == separator && bracketDepth == 0) {
            parts.append(value.mid(start, i - start));
            start = i + 1;
        }
    }

    parts.append(value.mid(start));
    return parts;
}

static QString readCfiBracketPrimaryValue(const QString &path, qsizetype &position)
{
    QString value;
    bool escaped = false;

    if (position < path.size() && path.at(position) == QLatin1Char('[')) {
        ++position;
    }

    while (position < path.size()) {
        const QChar ch = path.at(position);

        if (escaped) {
            value.append(ch);
            escaped = false;
            ++position;
            continue;
        }

        if (ch == QLatin1Char('^')) {
            escaped = true;
            ++position;
            continue;
        }

        if (ch == QLatin1Char(']')) {
            ++position;
            break;
        }

        if (ch == QLatin1Char(';') || ch == QLatin1Char(',')) {
            while (position < path.size()) {
                const QChar skip = path.at(position);
                ++position;
                if (skip == QLatin1Char('^') && position < path.size()) {
                    ++position;
                    continue;
                }
                if (skip == QLatin1Char(']')) {
                    break;
                }
            }
            break;
        }

        value.append(ch);
        ++position;
    }

    return value;
}

static CfiPath parseCfiPath(const QString &path)
{
    CfiPath steps;
    qsizetype position = 0;

    while (position < path.size()) {
        if (path.at(position) != QLatin1Char('/')) {
            ++position;
            continue;
        }

        ++position;

        qsizetype numberStart = position;
        while (position < path.size() && path.at(position).isDigit()) {
            ++position;
        }

        bool ok = false;
        CfiStep step;
        step.index = path.mid(numberStart, position - numberStart).toInt(&ok);
        if (!ok) {
            continue;
        }

        while (position < path.size()) {
            const QChar ch = path.at(position);
            if (ch == QLatin1Char('/')) {
                break;
            }

            if (ch == QLatin1Char('[')) {
                if (step.id.isEmpty()) {
                    step.id = readCfiBracketPrimaryValue(path, position);
                } else {
                    readCfiBracketPrimaryValue(path, position);
                }
                continue;
            }

            if (ch == QLatin1Char(':')) {
                ++position;
                qsizetype offsetStart = position;
                while (position < path.size() && path.at(position).isDigit()) {
                    ++position;
                }

                step.offset = path.mid(offsetStart, position - offsetStart).toInt(&ok);
                step.hasOffset = ok;
                continue;
            }

            ++position;
        }

        steps.append(step);
    }

    return steps;
}

static QVector<CfiPath> parseCfiIndirections(const QString &value)
{
    QVector<CfiPath> paths;
    const QStringList parts = splitCfiExpression(value, QLatin1Char('!'));

    for (const QString &part : parts) {
        paths.append(parseCfiPath(part));
    }

    return paths;
}

static ParsedCfi parseCfi(const QString &cfi)
{
    ParsedCfi parsed;
    const QString inner = unwrapEpubCfi(cfi);
    const QStringList rangeParts = splitCfiExpression(inner, QLatin1Char(','));

    if (rangeParts.size() == 3) {
        parsed.isRange = true;
        parsed.parent = parseCfiIndirections(rangeParts.at(0));
        parsed.start = parseCfiIndirections(rangeParts.at(1));
        parsed.end = parseCfiIndirections(rangeParts.at(2));
        return parsed;
    }

    parsed.parent = parseCfiIndirections(inner);
    return parsed;
}

static int spineIndexFromTopCfiPath(const CfiPath &path)
{
    if (path.isEmpty()) {
        return -1;
    }

    const int cfiIndex = path.constLast().index;
    if (cfiIndex <= 0 || cfiIndex % 2 != 0) {
        return -1;
    }

    return cfiIndex / 2 - 1;
}

static CfiPath collapseCfiRangeSide(const QVector<CfiPath> &parent, const QVector<CfiPath> &side)
{
    CfiPath result;
    if (!parent.isEmpty()) {
        result = parent.constLast();
    }

    if (!side.isEmpty()) {
        for (const CfiStep &step : side.constFirst()) {
            result.append(step);
        }
    }

    return result;
}

static bool isTextLikeNode(const QDomNode &node)
{
    return node.isText() || node.isCDATASection();
}

static bool isElementNode(const QDomNode &node)
{
    return node.isElement();
}

static bool isSkippableAnchorNode(const QDomNode &node)
{
    const QDomElement element = node.toElement();
    if (element.isNull()) {
        return false;
    }

    return element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor");
}

static void appendFilteredCfiChildNodes(const QDomNode &node, QList<QDomNode> &nodes)
{
    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        if (isTextLikeNode(child)) {
            nodes.append(child);
        } else if (isElementNode(child)) {
            if (isSkippableAnchorNode(child)) {
                appendFilteredCfiChildNodes(child, nodes);
            } else {
                nodes.append(child);
            }
        }

        child = child.nextSibling();
    }
}

static IndexedCfiChild indexedNode(const QDomNode &node)
{
    IndexedCfiChild child;
    child.kind = IndexedCfiChild::Node;
    child.node = node;
    return child;
}

static IndexedCfiChild indexedTextGroup(const QDomNode &first, const QDomNode &second)
{
    IndexedCfiChild child;
    child.kind = IndexedCfiChild::TextGroup;
    child.textNodes = {first, second};
    return child;
}

static IndexedCfiChild indexedSentinel(const IndexedCfiChild::Kind kind)
{
    IndexedCfiChild child;
    child.kind = kind;
    return child;
}

static QVector<IndexedCfiChild> indexCfiChildNodes(const QDomNode &node)
{
    QList<QDomNode> childNodes;
    appendFilteredCfiChildNodes(node, childNodes);

    QVector<IndexedCfiChild> indexed;
    for (const QDomNode &childNode : std::as_const(childNodes)) {
        if (isTextLikeNode(childNode)) {
            if (indexed.isEmpty()) {
                indexed.append(indexedNode(childNode));
            } else if (indexed.last().kind == IndexedCfiChild::TextGroup) {
                indexed.last().textNodes.append(childNode);
            } else if (indexed.last().kind == IndexedCfiChild::Node && isTextLikeNode(indexed.last().node)) {
                const QDomNode previousText = indexed.last().node;
                indexed.last() = indexedTextGroup(previousText, childNode);
            } else {
                indexed.append(indexedNode(childNode));
            }
        } else {
            if (!indexed.isEmpty() && indexed.last().kind == IndexedCfiChild::Node && isElementNode(indexed.last().node)) {
                indexed.append(indexedSentinel(IndexedCfiChild::Empty));
            }
            indexed.append(indexedNode(childNode));
        }
    }

    if (!indexed.isEmpty() && indexed.first().kind == IndexedCfiChild::Node && isElementNode(indexed.first().node)) {
        indexed.prepend(indexedSentinel(IndexedCfiChild::First));
    }

    if (!indexed.isEmpty() && indexed.last().kind == IndexedCfiChild::Node && isElementNode(indexed.last().node)) {
        indexed.append(indexedSentinel(IndexedCfiChild::Last));
    }

    indexed.prepend(indexedSentinel(IndexedCfiChild::Before));
    indexed.append(indexedSentinel(IndexedCfiChild::After));

    return indexed;
}

static QDomElement findElementById(const QDomNode &node, const QString &id)
{
    const QDomElement element = node.toElement();
    if (!element.isNull() && element.attribute(QStringLiteral("id")) == id) {
        return element;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        const QDomElement found = findElementById(child, id);
        if (!found.isNull()) {
            return found;
        }
        child = child.nextSibling();
    }

    return {};
}

static CfiBoundary boundaryFromTextGroup(const QList<QDomNode> &nodes, const int offset)
{
    int consumed = 0;
    for (const QDomNode &node : nodes) {
        const int length = node.nodeValue().size();
        if (consumed + length >= offset) {
            return CfiBoundary{node, offset - consumed};
        }
        consumed += length;
    }

    return {};
}

static CfiBoundary resolveCfiBoundary(QDomDocument &doc, const CfiPath &path)
{
    if (path.isEmpty()) {
        return CfiBoundary{doc.documentElement(), 0};
    }

    const QString id = path.constLast().id;
    if (!id.isEmpty()) {
        const QDomElement element = findElementById(doc.documentElement(), id);
        if (!element.isNull()) {
            return CfiBoundary{element, 0};
        }
    }

    QDomNode node = doc.documentElement();

    for (int i = 0; i < path.size(); ++i) {
        if (node.isNull()) {
            return {};
        }

        const CfiStep &step = path.at(i);
        const QVector<IndexedCfiChild> indexed = indexCfiChildNodes(node);
        if (step.index < 0 || step.index >= indexed.size()) {
            return {};
        }

        const IndexedCfiChild child = indexed.at(step.index);
        const bool finalStep = i == path.size() - 1;

        switch (child.kind) {
        case IndexedCfiChild::Node:
            node = child.node;
            break;
        case IndexedCfiChild::TextGroup:
            if (!finalStep) {
                return {};
            }
            return boundaryFromTextGroup(child.textNodes, step.hasOffset ? step.offset : 0);
        case IndexedCfiChild::First:
            return CfiBoundary{node.firstChild().isNull() ? node : node.firstChild(), -1};
        case IndexedCfiChild::Last:
            return CfiBoundary{node.lastChild().isNull() ? node : node.lastChild(), -1};
        case IndexedCfiChild::Before:
            return CfiBoundary{node, -1, true, false};
        case IndexedCfiChild::After:
            return CfiBoundary{node, -1, false, true};
        case IndexedCfiChild::Empty:
            return {};
        }
    }

    const CfiStep last = path.constLast();
    return CfiBoundary{node, last.hasOffset ? last.offset : 0};
}

static bool isValidBoundary(const CfiBoundary &boundary)
{
    if (!boundary.isValid() || boundary.before || boundary.after || boundary.offset < 0) {
        return boundary.isValid();
    }

    const int length = isTextLikeNode(boundary.node) ? boundary.node.nodeValue().size() : boundary.node.childNodes().size();
    return boundary.offset <= length;
}

static QDomElement createAnchorElement(QDomDocument &doc,
                                       const QString &tagName,
                                       const QString &anchorId,
                                       const QString &anchorType,
                                       const QString &href = QString(),
                                       const QString &title = QString())
{
    const QString namespaceUri = doc.documentElement().namespaceURI();
    QDomElement anchor = namespaceUri.isEmpty() ? doc.createElement(tagName) : doc.createElementNS(namespaceUri, tagName);
    anchor.setAttribute(QStringLiteral("id"), anchorId);
    anchor.setAttribute(QStringLiteral("data-role"), QStringLiteral("anchor"));
    if (!anchorType.isEmpty()) {
        anchor.setAttribute(QStringLiteral("data-anchor-type"), anchorType);
    }
    if (!href.isEmpty()) {
        anchor.setAttribute(QStringLiteral("href"), href);
    }
    if (!title.isEmpty()) {
        anchor.setAttribute(QStringLiteral("title"), title);
    }
    return anchor;
}

static QDomElement createAnnotationSpanElement(QDomDocument &doc, const QString &anchorId, const QString &anchorType)
{
    return createAnchorElement(doc, QStringLiteral("span"), anchorId, anchorType);
}

static bool wrapSingleTextNodeRange(QDomDocument &doc,
                                    const CfiBoundary &start,
                                    const CfiBoundary &end,
                                    const QString &anchorId,
                                    const QString &anchorType,
                                    QString *selectedText,
                                    const QString &tagName = QStringLiteral("a"),
                                    const QString &href = QString(),
                                    const QString &title = QString())
{
    if (!isValidBoundary(start) || !isValidBoundary(end) || start.before || start.after || end.before || end.after) {
        return false;
    }

    if (!isTextLikeNode(start.node) || start.node != end.node) {
        return false;
    }

    const QString text = start.node.nodeValue();
    const int textSize = static_cast<int>(text.size());
    const int startOffset = std::clamp(start.offset, 0, textSize);
    const int endOffset = std::clamp(end.offset, 0, textSize);
    if (endOffset <= startOffset) {
        return false;
    }

    const QString before = text.left(startOffset);
    const QString selected = text.mid(startOffset, endOffset - startOffset);
    const QString after = text.mid(endOffset);

    QDomNode parent = start.node.parentNode();
    if (parent.isNull()) {
        return false;
    }

    QDomElement anchor = createAnchorElement(doc, tagName, anchorId, anchorType, href, title);
    anchor.appendChild(doc.createTextNode(selected));

    if (!before.isEmpty()) {
        parent.insertBefore(doc.createTextNode(before), start.node);
    }
    parent.insertBefore(anchor, start.node);
    if (!after.isEmpty()) {
        parent.insertBefore(doc.createTextNode(after), start.node);
    }
    parent.removeChild(start.node);

    if (selectedText) {
        *selectedText = selected;
    }

    return true;
}

static QString anchorRangeElementName(const QDomElement &element)
{
    QString name = element.localName();
    if (name.isEmpty()) {
        name = element.tagName();
    }

    const int namespaceSeparator = name.indexOf(QLatin1Char(':'));
    if (namespaceSeparator >= 0) {
        name = name.mid(namespaceSeparator + 1);
    }

    return name.toLower();
}

static bool isInvalidElementInsideAnchor(const QDomElement &element)
{
    static const QStringList invalidElements = {
        QStringLiteral("a"),      QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"),    QStringLiteral("blockquote"),
        QStringLiteral("body"),   QStringLiteral("caption"), QStringLiteral("dd"),      QStringLiteral("details"),  QStringLiteral("dialog"),
        QStringLiteral("div"),    QStringLiteral("dl"),      QStringLiteral("dt"),      QStringLiteral("fieldset"), QStringLiteral("figcaption"),
        QStringLiteral("figure"), QStringLiteral("footer"),  QStringLiteral("form"),    QStringLiteral("h1"),       QStringLiteral("h2"),
        QStringLiteral("h3"),     QStringLiteral("h4"),      QStringLiteral("h5"),      QStringLiteral("h6"),       QStringLiteral("head"),
        QStringLiteral("header"), QStringLiteral("hr"),      QStringLiteral("html"),    QStringLiteral("li"),       QStringLiteral("main"),
        QStringLiteral("nav"),    QStringLiteral("ol"),      QStringLiteral("p"),       QStringLiteral("pre"),      QStringLiteral("section"),
        QStringLiteral("table"),  QStringLiteral("tbody"),   QStringLiteral("td"),      QStringLiteral("tfoot"),    QStringLiteral("th"),
        QStringLiteral("thead"),  QStringLiteral("tr"),      QStringLiteral("ul"),
    };

    return invalidElements.contains(anchorRangeElementName(element));
}

static bool canPromoteWholeElementIntoAnchor(const QDomNode &node)
{
    const QDomElement element = node.toElement();
    return !element.isNull() && !isInvalidElementInsideAnchor(element);
}

static bool hasInvalidAnchorDescendant(const QDomNode &node)
{
    const QDomElement element = node.toElement();
    if (!element.isNull() && isInvalidElementInsideAnchor(element)) {
        return true;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        if (hasInvalidAnchorDescendant(child)) {
            return true;
        }
        child = child.nextSibling();
    }

    return false;
}

static bool canMoveNodeIntoAnchor(const QDomNode &node)
{
    return !hasInvalidAnchorDescendant(node);
}

static bool isAnnotatableInlineElement(const QDomElement &element)
{
    return !element.isNull() && !isInvalidElementInsideAnchor(element) && element.attribute(QStringLiteral("id")).isEmpty()
        && element.attribute(QStringLiteral("data-role")) != QStringLiteral("anchor");
}

static void setAnchorAttributes(QDomElement &element, const QString &anchorId, const QString &anchorType)
{
    element.setAttribute(QStringLiteral("id"), anchorId);
    element.setAttribute(QStringLiteral("data-role"), QStringLiteral("anchor"));
    if (!anchorType.isEmpty()) {
        element.setAttribute(QStringLiteral("data-anchor-type"), anchorType);
    }
}

static void appendPlainText(const QDomNode &node, QString &text)
{
    if (isTextLikeNode(node)) {
        text.append(node.nodeValue());
        return;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        appendPlainText(child, text);
        child = child.nextSibling();
    }
}

static bool containsNode(const QDomNode &ancestor, const QDomNode &node)
{
    if (ancestor.isNull() || node.isNull()) {
        return false;
    }
    if (ancestor == node) {
        return true;
    }

    QDomNode child = ancestor.firstChild();
    while (!child.isNull()) {
        if (containsNode(child, node)) {
            return true;
        }
        child = child.nextSibling();
    }

    return false;
}

static QDomNode firstTextLikeDescendant(const QDomNode &node)
{
    if (isTextLikeNode(node)) {
        return node;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        const QDomNode text = firstTextLikeDescendant(child);
        if (!text.isNull()) {
            return text;
        }
        child = child.nextSibling();
    }

    return {};
}

static QDomNode lastTextLikeDescendant(const QDomNode &node)
{
    if (isTextLikeNode(node)) {
        return node;
    }

    QDomNode child = node.lastChild();
    while (!child.isNull()) {
        const QDomNode text = lastTextLikeDescendant(child);
        if (!text.isNull()) {
            return text;
        }
        child = child.previousSibling();
    }

    return {};
}

static QDomNode childAt(const QDomNode &node, const int index)
{
    const QDomNodeList children = node.childNodes();
    if (index < 0 || index >= children.size()) {
        return {};
    }

    return children.at(index);
}

static bool annotateInlineElement(QDomElement element, const QString &anchorId, const QString &anchorType, QString *selectedText)
{
    if (!isAnnotatableInlineElement(element)) {
        return false;
    }

    QString text;
    appendPlainText(element, text);
    if (text.isEmpty()) {
        return false;
    }

    setAnchorAttributes(element, anchorId, anchorType);
    if (selectedText) {
        *selectedText = text;
    }
    return true;
}

static bool
annotateExactInlineElementRange(const CfiBoundary &start, const CfiBoundary &end, const QString &anchorId, const QString &anchorType, QString *selectedText)
{
    if (!isValidBoundary(start) || !isValidBoundary(end) || start.before || start.after || end.before || end.after) {
        return false;
    }

    if (start.node == end.node && isElementNode(start.node) && end.offset == start.offset + 1) {
        return annotateInlineElement(childAt(start.node, start.offset).toElement(), anchorId, anchorType, selectedText);
    }

    if (!isTextLikeNode(start.node) || !isTextLikeNode(end.node) || start.offset != 0 || end.offset != end.node.nodeValue().size()) {
        return false;
    }

    QDomNode candidate = start.node.parentNode();
    while (!candidate.isNull() && candidate.isElement()) {
        QDomElement element = candidate.toElement();
        if (isInvalidElementInsideAnchor(element)) {
            return false;
        }

        if (containsNode(candidate, end.node) && firstTextLikeDescendant(candidate) == start.node && lastTextLikeDescendant(candidate) == end.node) {
            return annotateInlineElement(element, anchorId, anchorType, selectedText);
        }

        candidate = candidate.parentNode();
    }

    return false;
}

static QDomNode splitTextRangeBoundary(QDomDocument &doc, const CfiBoundary &boundary, const bool startBoundary)
{
    if (!isTextLikeNode(boundary.node) || boundary.before || boundary.after) {
        return {};
    }

    QDomNode parent = boundary.node.parentNode();
    if (parent.isNull()) {
        return {};
    }

    const QString text = boundary.node.nodeValue();
    const int textSize = static_cast<int>(text.size());
    const int offset = std::clamp(boundary.offset, 0, textSize);

    if (startBoundary) {
        if (offset <= 0) {
            return boundary.node;
        }

        if (offset >= textSize) {
            return boundary.node.nextSibling();
        }

        const QString before = text.left(offset);
        const QString selected = text.mid(offset);
        parent.insertBefore(doc.createTextNode(before), boundary.node);
        QDomNode selectedNode = parent.insertBefore(doc.createTextNode(selected), boundary.node);
        parent.removeChild(boundary.node);
        return selectedNode;
    }

    if (offset <= 0) {
        return boundary.node.previousSibling();
    }

    if (offset >= textSize) {
        return boundary.node;
    }

    const QString selected = text.left(offset);
    const QString after = text.mid(offset);
    QDomNode selectedNode = parent.insertBefore(doc.createTextNode(selected), boundary.node);
    parent.insertBefore(doc.createTextNode(after), boundary.node);
    parent.removeChild(boundary.node);
    return selectedNode;
}

static QDomNode rangeStartNodeFromBoundary(QDomDocument &doc, const CfiBoundary &boundary)
{
    if (!isValidBoundary(boundary)) {
        return {};
    }

    if (boundary.before) {
        return boundary.node;
    }

    if (boundary.after) {
        return boundary.node.nextSibling();
    }

    if (isTextLikeNode(boundary.node)) {
        return splitTextRangeBoundary(doc, boundary, true);
    }

    return childAt(boundary.node, std::clamp(boundary.offset, 0, boundary.node.childNodes().size()));
}

static QDomNode rangeEndNodeFromBoundary(QDomDocument &doc, const CfiBoundary &boundary)
{
    if (!isValidBoundary(boundary)) {
        return {};
    }

    if (boundary.before) {
        return boundary.node.previousSibling();
    }

    if (boundary.after) {
        return boundary.node;
    }

    if (isTextLikeNode(boundary.node)) {
        return splitTextRangeBoundary(doc, boundary, false);
    }

    return childAt(boundary.node, std::clamp(boundary.offset, 0, boundary.node.childNodes().size()) - 1);
}

static bool insertBoundaryMarkerAtBoundary(QDomDocument &doc, const CfiBoundary &boundary, const QString &anchorId, const QString &anchorType)
{
    if (!isValidBoundary(boundary)) {
        return false;
    }

    QDomElement marker = createAnnotationSpanElement(doc, anchorId, anchorType);

    if (boundary.before || boundary.after) {
        QDomNode parent = boundary.node.parentNode();
        if (parent.isNull()) {
            return false;
        }

        if (boundary.before) {
            parent.insertBefore(marker, boundary.node);
        } else {
            parent.insertAfter(marker, boundary.node);
        }
        return true;
    }

    if (isTextLikeNode(boundary.node)) {
        QDomNode parent = boundary.node.parentNode();
        if (parent.isNull()) {
            return false;
        }

        const QString text = boundary.node.nodeValue();
        const int offset = std::clamp(boundary.offset, 0, static_cast<int>(text.size()));
        const QString before = text.left(offset);
        const QString after = text.mid(offset);

        if (!before.isEmpty()) {
            parent.insertBefore(doc.createTextNode(before), boundary.node);
        }
        parent.insertBefore(marker, boundary.node);
        if (!after.isEmpty()) {
            parent.insertBefore(doc.createTextNode(after), boundary.node);
        }
        parent.removeChild(boundary.node);
        return true;
    }

    if (!isElementNode(boundary.node)) {
        return false;
    }

    QDomNode node = boundary.node;
    const int offset = std::clamp(boundary.offset, 0, node.childNodes().size());
    const QDomNode referenceNode = childAt(node, offset);
    if (referenceNode.isNull()) {
        node.appendChild(marker);
    } else {
        node.insertBefore(marker, referenceNode);
    }
    return true;
}

static bool insertAnnotationBoundaryPair(QDomDocument &doc, const CfiBoundary &start, const CfiBoundary &end, const QString &anchorId)
{
    return insertBoundaryMarkerAtBoundary(doc, end, anchorId + QStringLiteral("_end"), QStringLiteral("annotation-end"))
        && insertBoundaryMarkerAtBoundary(doc, start, anchorId + QStringLiteral("_begin"), QStringLiteral("annotation-begin"));
}

static QDomNode promoteRangeStartNode(QDomNode node)
{
    while (!node.isNull()) {
        QDomNode parent = node.parentNode();
        if (parent.isNull() || !canPromoteWholeElementIntoAnchor(parent) || !node.previousSibling().isNull()) {
            break;
        }

        node = parent;
    }

    return node;
}

static QDomNode promoteRangeEndNode(QDomNode node)
{
    while (!node.isNull()) {
        QDomNode parent = node.parentNode();
        if (parent.isNull() || !canPromoteWholeElementIntoAnchor(parent) || !node.nextSibling().isNull()) {
            break;
        }

        node = parent;
    }

    return node;
}

static bool collectSiblingRange(const QDomNode &startNode, const QDomNode &endNode, QList<QDomNode> &nodes)
{
    QDomNode node = startNode;
    while (!node.isNull()) {
        nodes.append(node);
        if (node == endNode) {
            return true;
        }
        node = node.nextSibling();
    }

    nodes.clear();
    return false;
}

static bool wrapSiblingRange(QDomDocument &doc,
                             const CfiBoundary &start,
                             const CfiBoundary &end,
                             const QString &anchorId,
                             const QString &anchorType,
                             QString *selectedText,
                             const QString &href = QString(),
                             const QString &title = QString())
{
    if (!isValidBoundary(start) || !isValidBoundary(end) || start.node == end.node) {
        return false;
    }

    QDomNode startNode = promoteRangeStartNode(rangeStartNodeFromBoundary(doc, start));
    QDomNode endNode = promoteRangeEndNode(rangeEndNodeFromBoundary(doc, end));
    if (startNode.isNull() || endNode.isNull()) {
        return false;
    }

    QDomNode parent = startNode.parentNode();
    if (parent.isNull() || parent != endNode.parentNode()) {
        return false;
    }

    QList<QDomNode> nodesToWrap;
    if (!collectSiblingRange(startNode, endNode, nodesToWrap)) {
        return false;
    }

    QString rangeText;
    for (const QDomNode &node : std::as_const(nodesToWrap)) {
        if (!canMoveNodeIntoAnchor(node)) {
            return false;
        }
        appendPlainText(node, rangeText);
    }

    if (rangeText.isEmpty()) {
        return false;
    }

    QDomElement anchor = createAnchorElement(doc, QStringLiteral("a"), anchorId, anchorType, href, title);
    parent.insertBefore(anchor, startNode);

    for (const QDomNode &node : std::as_const(nodesToWrap)) {
        anchor.appendChild(node);
    }

    if (selectedText) {
        *selectedText = rangeText;
    }

    return true;
}

static void copyDirectoryReplacingFiles(KZip &outZip,
                                        const KArchiveDirectory *dir,
                                        const QString &prefix,
                                        const QHash<QString, QByteArray> &replacementDataByPath,
                                        QSet<QString> &replacedPaths)
{
    if (!dir) {
        return;
    }

    const QStringList entries = dir->entries();
    for (const QString &entryName : entries) {
        const KArchiveEntry *entry = dir->entry(entryName);
        if (!entry) {
            continue;
        }

        const QString fullPath = prefix.isEmpty() ? entryName : prefix + QStringLiteral("/") + entryName;
        const QString cleanPath = QDir::cleanPath(fullPath);

        if (cleanPath == QStringLiteral("mimetype")) {
            continue;
        }

        if (entry->isDirectory()) {
            const auto *subdir = dynamic_cast<const KArchiveDirectory *>(entry);
            if (subdir) {
                copyDirectoryReplacingFiles(outZip, subdir, cleanPath, replacementDataByPath, replacedPaths);
            }
            continue;
        }

        const auto *file = dynamic_cast<const KArchiveFile *>(entry);
        if (!file) {
            continue;
        }

        if (replacementDataByPath.contains(cleanPath)) {
            outZip.writeFile(cleanPath, replacementDataByPath.value(cleanPath));
            replacedPaths.insert(cleanPath);
            continue;
        }

        QScopedPointer<QIODevice> dev(file->createDevice());
        if (!dev) {
            continue;
        }

        outZip.writeFile(cleanPath, dev->readAll());
    }
}

static QByteArray createEpubReplacingFiles(const KArchiveDirectory *rootFolder, const QHash<QString, QByteArray> &replacementDataByPath)
{
    if (!rootFolder) {
        qWarning() << "No EPUB root folder available for replacement";
        return {};
    }
    if (replacementDataByPath.isEmpty()) {
        qWarning() << "No EPUB replacement files provided";
        return {};
    }

    QByteArray result;
    QBuffer buffer(&result);
    if (!buffer.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to open EPUB replacement buffer";
        return {};
    }

    KZip outZip(&buffer);
    if (!outZip.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to create replacement EPUB";
        return {};
    }

    outZip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));

    QSet<QString> replacedPaths;
    copyDirectoryReplacingFiles(outZip, rootFolder, QString(), replacementDataByPath, replacedPaths);
    outZip.close();
    buffer.close();

    if (replacedPaths.size() != replacementDataByPath.size()) {
        QStringList missing;
        for (auto it = replacementDataByPath.constBegin(); it != replacementDataByPath.constEnd(); ++it) {
            if (!replacedPaths.contains(it.key())) {
                missing.append(it.key());
            }
        }
        qWarning() << "Unable to find replacement documents in EPUB" << missing;
        return {};
    }

    return result;
}

static QByteArray createEpubReplacingFile(const KArchiveDirectory *rootFolder, const QString &replacementPath, const QByteArray &replacementData)
{
    return createEpubReplacingFiles(rootFolder, {{replacementPath, replacementData}});
}

static bool writeEpubReplacingFiles(const QString &outputPath, const KArchiveDirectory *rootFolder, const QHash<QString, QByteArray> &replacementDataByPath)
{
    const QByteArray epubData = createEpubReplacingFiles(rootFolder, replacementDataByPath);
    if (epubData.isEmpty()) {
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QTemporaryFile tempFile(outputInfo.dir().filePath(outputInfo.fileName() + QStringLiteral(".XXXXXX")));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        qWarning() << "Unable to create temporary replacement EPUB" << outputPath << tempFile.errorString();
        return false;
    }

    const QString tempPath = tempFile.fileName();
    if (tempFile.write(epubData) != epubData.size()) {
        qWarning() << "Unable to write temporary replacement EPUB" << tempPath << tempFile.errorString();
        tempFile.close();
        QFile::remove(tempPath);
        return false;
    }

    tempFile.close();
    if (tempFile.error() != QFileDevice::NoError) {
        qWarning() << "Unable to finalize temporary replacement EPUB" << tempPath << tempFile.errorString();
        QFile::remove(tempPath);
        return false;
    }

    QString backupPath;
    if (QFileInfo::exists(outputPath)) {
        backupPath = outputPath + QStringLiteral(".bak-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(outputPath, backupPath)) {
            qWarning() << "Unable to move existing EPUB aside" << outputPath << backupPath;
            QFile::remove(tempPath);
            return false;
        }
    }

    if (!QFile::rename(tempPath, outputPath)) {
        qWarning() << "Unable to move replacement EPUB into place" << tempPath << outputPath;
        if (!backupPath.isEmpty()) {
            QFile::rename(backupPath, outputPath);
        }
        QFile::remove(tempPath);
        return false;
    }

    if (!backupPath.isEmpty()) {
        QFile::remove(backupPath);
    }

    return true;
}

static bool
writeEpubReplacingFile(const QString &outputPath, const KArchiveDirectory *rootFolder, const QString &replacementPath, const QByteArray &replacementData)
{
    const QByteArray epubData = createEpubReplacingFile(rootFolder, replacementPath, replacementData);
    if (epubData.isEmpty()) {
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QTemporaryFile tempFile(outputInfo.dir().filePath(outputInfo.fileName() + QStringLiteral(".XXXXXX")));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        qWarning() << "Unable to create temporary anchored EPUB" << outputPath << tempFile.errorString();
        return false;
    }

    const QString tempPath = tempFile.fileName();
    if (tempFile.write(epubData) != epubData.size()) {
        qWarning() << "Unable to write temporary anchored EPUB" << tempPath << tempFile.errorString();
        tempFile.close();
        QFile::remove(tempPath);
        return false;
    }

    tempFile.close();
    if (tempFile.error() != QFileDevice::NoError) {
        qWarning() << "Unable to finalize temporary anchored EPUB" << tempPath << tempFile.errorString();
        QFile::remove(tempPath);
        return false;
    }

    QString backupPath;
    if (QFileInfo::exists(outputPath)) {
        backupPath = outputPath + QStringLiteral(".bak-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(outputPath, backupPath)) {
            qWarning() << "Unable to move existing anchored EPUB aside" << outputPath << backupPath;
            QFile::remove(tempPath);
            return false;
        }
    }

    if (!QFile::rename(tempPath, outputPath)) {
        qWarning() << "Unable to move anchored EPUB into place" << tempPath << outputPath;
        if (!backupPath.isEmpty()) {
            QFile::rename(backupPath, outputPath);
        }
        QFile::remove(tempPath);
        return false;
    }

    if (!backupPath.isEmpty()) {
        QFile::remove(backupPath);
    }

    return true;
}

static bool copyAnchorSafetyBackup(const QString &sourcePath, const QString &anchorId, const QString &backupMarker, QString *backupPath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        qWarning() << "Unable to create anchor backup because EPUB does not exist" << sourcePath;
        return false;
    }

    QString safeAnchorId = anchorId.trimmed();
    safeAnchorId.replace(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9._-])")), QStringLiteral("_"));
    if (safeAnchorId.isEmpty()) {
        safeAnchorId = QStringLiteral("anchor");
    }

    const QString suffix = sourceInfo.suffix().isEmpty() ? QStringLiteral("epub") : sourceInfo.suffix();
    const QString backupStem = sourceInfo.completeBaseName() + QLatin1Char('.') + backupMarker + QLatin1Char('_') + safeAnchorId;
    QDir directory = sourceInfo.dir();

    for (int attempt = 0; attempt < 1000; ++attempt) {
        const QString candidateName = backupStem + (attempt == 0 ? QString() : QStringLiteral("-%1").arg(attempt)) + QLatin1Char('.') + suffix;
        const QString candidatePath = directory.filePath(candidateName);
        if (QFileInfo::exists(candidatePath)) {
            continue;
        }

        if (!QFile::copy(sourceInfo.absoluteFilePath(), candidatePath)) {
            qWarning() << "Unable to create anchor backup" << candidatePath;
            return false;
        }

        if (backupPath) {
            *backupPath = candidatePath;
        }
        return true;
    }

    qWarning() << "Unable to create unique anchor backup for EPUB" << sourcePath;
    return false;
}

static bool copyAnchorSafetyBackup(const QString &sourcePath, const QString &anchorId, QString *backupPath)
{
    return copyAnchorSafetyBackup(sourcePath, anchorId, QStringLiteral("before_anchoring"), backupPath);
}

static bool copyAnchorRemovalSafetyBackup(const QString &sourcePath, const QString &anchorId, QString *backupPath)
{
    return copyAnchorSafetyBackup(sourcePath, anchorId, QStringLiteral("before.ananchored"), backupPath);
}

static bool copyAnchorUpdateSafetyBackup(const QString &sourcePath, const QString &anchorId, QString *backupPath)
{
    return copyAnchorSafetyBackup(sourcePath, anchorId, QStringLiteral("before.reference-title-update"), backupPath);
}

static bool copyImageClassSafetyBackup(const QString &sourcePath, const QString &imageMarker, QString *backupPath)
{
    return copyAnchorSafetyBackup(sourcePath, imageMarker, QStringLiteral("before.image-not-inverse"), backupPath);
}

static bool isImageElement(const QDomElement &element)
{
    return !element.isNull() && xmlLocalName(element).compare(QStringLiteral("img"), Qt::CaseInsensitive) == 0;
}

static QDomElement imageElementFromNode(const QDomNode &node)
{
    QDomNode candidate = node;
    while (!candidate.isNull() && !candidate.isDocument()) {
        const QDomElement element = candidate.toElement();
        if (isImageElement(element)) {
            return element;
        }
        candidate = candidate.parentNode();
    }

    return {};
}

static QDomElement childImageAtOffset(const QDomNode &node, const int offset)
{
    if (offset < 0 || offset >= node.childNodes().size()) {
        return {};
    }

    return imageElementFromNode(node.childNodes().item(offset));
}

static QDomElement imageElementFromCfiBoundaries(const CfiBoundary &start, const CfiBoundary &end)
{
    QDomElement image = imageElementFromNode(start.node);
    if (!image.isNull()) {
        return image;
    }

    image = imageElementFromNode(end.node);
    if (!image.isNull()) {
        return image;
    }

    if (start.node == end.node && start.offset >= 0 && end.offset >= start.offset) {
        image = childImageAtOffset(start.node, start.offset);
        if (!image.isNull()) {
            return image;
        }

        if (end.offset > 0) {
            return childImageAtOffset(end.node, end.offset - 1);
        }
    }

    if (start.before || end.after) {
        image = imageElementFromNode(start.node);
        if (!image.isNull()) {
            return image;
        }
        return imageElementFromNode(end.node);
    }

    return {};
}

static QString normalizedImageSrc(QString src, const QString &documentPath)
{
    src = pathWithoutFragment(src.trimmed());
    if (src.isEmpty()) {
        return {};
    }

    const QUrl url(src);
    if (url.isValid() && !url.scheme().isEmpty()) {
        return pathWithoutFragment(QUrl::fromPercentEncoding(url.path().toUtf8()));
    }

    return QDir::cleanPath(resolveRelativePath(documentPath, QUrl::fromPercentEncoding(src.toUtf8())));
}

static QDomElement firstImageBySrc(const QDomNode &node, const QString &documentPath, const QString &src)
{
    const QString normalizedSrc = normalizedImageSrc(src, documentPath);
    if (normalizedSrc.isEmpty()) {
        return {};
    }

    const QDomElement element = node.toElement();
    if (isImageElement(element) && normalizedImageSrc(element.attribute(QStringLiteral("src")), documentPath) == normalizedSrc) {
        return element;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        const QDomElement found = firstImageBySrc(child, documentPath, src);
        if (!found.isNull()) {
            return found;
        }
        child = child.nextSibling();
    }

    return {};
}

static bool addCssClass(QDomElement element, const QString &className)
{
    if (element.isNull() || className.isEmpty()) {
        return false;
    }

    QStringList classes = element.attribute(QStringLiteral("class")).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (classes.contains(className)) {
        return false;
    }

    classes.append(className);
    element.setAttribute(QStringLiteral("class"), classes.join(QLatin1Char(' ')));
    return true;
}

static bool isReferenceAnchorElement(const QDomElement &element, const QString &anchorId)
{
    if (element.isNull()) {
        return false;
    }

    const bool modernCrossReference = element.attribute(QStringLiteral("id")) == anchorId && xmlLocalName(element) == QStringLiteral("a")
        && element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor")
        && element.attribute(QStringLiteral("data-anchor-type")) == QStringLiteral("crossref");
    if (modernCrossReference) {
        return true;
    }

    const bool modernIntraBookReference = element.attribute(QStringLiteral("id")) == anchorId && xmlLocalName(element) == QStringLiteral("a")
        && element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor") && element.hasAttribute(QStringLiteral("href"))
        && element.attribute(QStringLiteral("data-anchor-type")) != QStringLiteral("crossref");
    if (modernIntraBookReference) {
        return true;
    }

    const QStringList classes = element.attribute(QStringLiteral("class")).split(u' ', Qt::SkipEmptyParts);
    return classes.contains(QStringLiteral("bookref")) && element.attribute(QStringLiteral("data-ref")) == anchorId;
}

static bool isGeneratedTargetRangeAnchorElement(const QDomElement &element, const QString &anchorId)
{
    if (element.isNull() || anchorId.isEmpty()) {
        return false;
    }
    if (!anchorId.startsWith(QStringLiteral("uuid_"))) {
        return false;
    }

    const QString anchorType = element.attribute(QStringLiteral("data-anchor-type")).trimmed();
    return element.attribute(QStringLiteral("id")) == anchorId && xmlLocalName(element) == QStringLiteral("a")
        && element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor") && (anchorType.isEmpty() || anchorType == QStringLiteral("target"));
}

static QDomElement findReferenceAnchorElement(const QDomNode &node, const QString &anchorId)
{
    const QDomElement element = node.toElement();
    if (isReferenceAnchorElement(element, anchorId)) {
        return element;
    }

    for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
        const QDomElement found = findReferenceAnchorElement(child, anchorId);
        if (!found.isNull()) {
            return found;
        }
    }

    return {};
}

static QDomElement findTargetRangeAnchorElement(const QDomNode &node, const QString &anchorId)
{
    const QDomElement element = node.toElement();
    if (isGeneratedTargetRangeAnchorElement(element, anchorId)) {
        return element;
    }

    for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
        const QDomElement found = findTargetRangeAnchorElement(child, anchorId);
        if (!found.isNull()) {
            return found;
        }
    }

    return {};
}

static bool unwrapElement(QDomElement element)
{
    QDomNode parent = element.parentNode();
    if (parent.isNull()) {
        return false;
    }

    while (element.hasChildNodes()) {
        QDomNode child = element.firstChild();
        element.removeChild(child);
        parent.insertBefore(child, element);
    }

    parent.removeChild(element);
    return true;
}

using XmlNamespaceDeclarations = QVector<QPair<QString, QString>>;

static XmlNamespaceDeclarations rootNamespaceDeclarations(const QByteArray &data)
{
    XmlNamespaceDeclarations declarations;
    QXmlStreamReader reader(data);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }

        for (const QXmlStreamNamespaceDeclaration &declaration : reader.namespaceDeclarations()) {
            declarations.append({declaration.prefix().toString(), declaration.namespaceUri().toString()});
        }
        break;
    }

    return declarations;
}

static void restoreRootNamespaceDeclarations(QDomDocument &doc, const XmlNamespaceDeclarations &declarations)
{
    QDomElement root = doc.documentElement();
    if (root.isNull()) {
        return;
    }

    for (const auto &[prefix, namespaceUri] : declarations) {
        if (namespaceUri.isEmpty()) {
            continue;
        }

        const QString attributeName = prefix.isEmpty() ? QStringLiteral("xmlns") : QStringLiteral("xmlns:") + prefix;
        if (root.attribute(attributeName) == namespaceUri) {
            continue;
        }
        if (prefix.isEmpty() && root.namespaceURI() == namespaceUri) {
            continue;
        }
        if (!prefix.isEmpty() && root.prefix() == prefix && root.namespaceURI() == namespaceUri) {
            continue;
        }

        root.setAttribute(attributeName, namespaceUri);
    }
}

static bool setDomContentWithNamespaces(QDomDocument &doc, const QByteArray &data)
{
    return static_cast<bool>(doc.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing));
}

static bool setDomContentPreservingMarkup(QDomDocument &doc, const QByteArray &data)
{
    return static_cast<bool>(doc.setContent(data));
}

static void collectArchiveFilePaths(const KArchiveDirectory *dir, const QString &prefix, QStringList &paths)
{
    if (!dir) {
        return;
    }

    const QStringList entries = dir->entries();
    for (const QString &entryName : entries) {
        const KArchiveEntry *entry = dir->entry(entryName);
        if (!entry) {
            continue;
        }

        const QString fullPath = prefix.isEmpty() ? entryName : prefix + QStringLiteral("/") + entryName;
        const QString cleanPath = QDir::cleanPath(fullPath);

        if (entry->isDirectory()) {
            const auto *subdir = dynamic_cast<const KArchiveDirectory *>(entry);
            if (subdir) {
                collectArchiveFilePaths(subdir, cleanPath, paths);
            }
            continue;
        }

        if (entry->isFile()) {
            paths.append(cleanPath);
        }
    }
}

static bool addDeviceDataToHash(QIODevice &device, QCryptographicHash &hash)
{
    QByteArray buffer;
    buffer.resize(64 * 1024);
    while (true) {
        const qint64 bytesRead = device.read(buffer.data(), buffer.size());
        if (bytesRead > 0) {
            hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(bytesRead)));
            continue;
        }

        if (bytesRead < 0) {
            return false;
        }

        return device.atEnd();
    }
}

static void addContentHashField(QCryptographicHash &hash, const QByteArray &field)
{
    hash.addData(QByteArray::number(field.size()));
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(field);
    hash.addData(QByteArrayLiteral("\n"));
}

static bool addContentHashDeviceField(QCryptographicHash &hash, QIODevice &device, qint64 size)
{
    hash.addData(QByteArray::number(size));
    hash.addData(QByteArrayLiteral("\n"));
    if (!addDeviceDataToHash(device, hash)) {
        return false;
    }
    hash.addData(QByteArrayLiteral("\n"));

    return true;
}

EPubContainer::EPubContainer(QObject *parent)
    : QObject(parent)
    , m_rootFolder(nullptr)
{
}

EPubContainer::~EPubContainer() = default;

bool EPubContainer::openFile(const QString &path)
{
    m_filename = path;
    m_archive = std::make_unique<KZip>(path);

    if (!m_archive->open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccured(i18n("Failed to open %1", path));

        return false;
    }

    m_rootFolder = m_archive->directory();
    if (!m_rootFolder) {
        Q_EMIT errorOccured(i18n("Failed to read %1", path));
        return false;
    }

    if (!parseMimetype()) {
        return false;
    }

    if (!parseContainer()) {
        return false;
    }

    return true;
}

const KArchiveDirectory *EPubContainer::rootDirectory() const
{
    return m_rootFolder;
}

const QHash<QString, EpubItem> &EPubContainer::manifestItems() const
{
    return m_items;
}

QString EPubContainer::getFileHash() const
{
    QFile file(m_filename);
    if (m_filename.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        qWarning() << "Unable to open EPUB file for hashing" << m_filename << file.errorString();
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!addDeviceDataToHash(file, hash)) {
        qWarning() << "Unable to read EPUB file for hashing" << m_filename << file.errorString();
        return {};
    }

    return QString::fromLatin1(hash.result().toHex());
}

QString EPubContainer::getContentHash() const
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available for content hash";
        return {};
    }

    QStringList paths;
    collectArchiveFilePaths(m_rootFolder, QString(), paths);
    paths.sort(Qt::CaseSensitive);

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("arianna-epub-content-sha256-v1\n"));

    for (const QString &path : std::as_const(paths)) {
        const KArchiveFile *archiveFile = file(path);
        if (!archiveFile) {
            qWarning() << "Unable to read EPUB content file for hashing" << path;
            return {};
        }

        QScopedPointer<QIODevice> device(archiveFile->createDevice());
        if (!device) {
            qWarning() << "Unable to open EPUB content file for hashing" << path;
            return {};
        }

        addContentHashField(hash, path.toUtf8());
        if (!addContentHashDeviceField(hash, *device, archiveFile->size())) {
            qWarning() << "Unable to read EPUB content file for hashing" << path << device->errorString();
            return {};
        }
    }

    return QString::fromLatin1(hash.result().toHex());
}

EpubNormalizationResult EPubContainer::normalizeForArianna(const QString &fallbackTitle) const
{
    EpubNormalizationResult result;
    if (!m_rootFolder || m_filename.isEmpty()) {
        result.success = false;
        result.errorMessage = i18n("No EPUB file is open for normalization");
        return result;
    }

    QString titleFallback = fallbackTitle.trimmed();
    if (titleFallback.isEmpty()) {
        titleFallback = m_metadata.value(QStringLiteral("title")).value(0).trimmed();
    }
    if (titleFallback.isEmpty()) {
        titleFallback = QFileInfo(m_filename).completeBaseName();
    }

    QHash<QString, QByteArray> replacementDataByPath;
    QSet<QString> scriptedItemIds;

    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        const QString itemId = it.key();
        const EpubItem item = it.value();
        if (!isDocumentItem(item)) {
            continue;
        }

        const KArchiveFile *archiveFile = file(item.path);
        if (!archiveFile) {
            result.success = false;
            result.errorMessage = i18n("Unable to read EPUB document %1", item.path);
            return result;
        }

        QScopedPointer<QIODevice> device(archiveFile->createDevice());
        if (!device) {
            result.success = false;
            result.errorMessage = i18n("Unable to open EPUB document %1", item.path);
            return result;
        }

        const QByteArray data = device->readAll();
        QDomDocument doc;
        const XmlNamespaceDeclarations namespaces = rootNamespaceDeclarations(data);
        if (!setDomContentWithNamespaces(doc, data)) {
            result.success = false;
            result.errorMessage = i18n("Unable to parse EPUB document %1", item.path);
            return result;
        }

        bool documentChanged = false;
        const QDomElement head = firstChildElementByLocalName(doc.documentElement(), QStringLiteral("head"));
        if (!head.isNull()) {
            QDomElement title = firstChildElementByLocalName(head, QStringLiteral("title"));
            if (!title.isNull() && title.text().trimmed().isEmpty()) {
                while (title.hasChildNodes()) {
                    title.removeChild(title.firstChild());
                }
                title.appendChild(doc.createTextNode(titleFallback));
                ++result.emptyTitlesFixed;
                documentChanged = true;
                result.messages.append(i18n("Filled empty title in %1", item.path));
            }
        }

        if (documentHasElementByLocalName(doc, QStringLiteral("script")) && !item.properties.contains(QStringLiteral("scripted"))) {
            scriptedItemIds.insert(itemId);
        }

        if (documentChanged) {
            restoreRootNamespaceDeclarations(doc, namespaces);
            replacementDataByPath.insert(item.path, doc.toByteArray());
        }
    }

    if (!scriptedItemIds.isEmpty()) {
        const KArchiveFile *opfFile = file(m_contentFilePath);
        if (!opfFile) {
            result.success = false;
            result.errorMessage = i18n("Unable to read EPUB package document %1", m_contentFilePath);
            return result;
        }

        QScopedPointer<QIODevice> device(opfFile->createDevice());
        if (!device) {
            result.success = false;
            result.errorMessage = i18n("Unable to open EPUB package document %1", m_contentFilePath);
            return result;
        }

        const QByteArray opfData = device->readAll();
        QDomDocument opfDoc;
        const XmlNamespaceDeclarations namespaces = rootNamespaceDeclarations(opfData);
        if (!setDomContentWithNamespaces(opfDoc, opfData)) {
            result.success = false;
            result.errorMessage = i18n("Unable to parse EPUB package document %1", m_contentFilePath);
            return result;
        }

        bool opfChanged = false;
        const QDomNodeList itemNodes = opfDoc.elementsByTagName(QStringLiteral("item"));
        for (int i = 0; i < itemNodes.count(); ++i) {
            QDomElement item = itemNodes.at(i).toElement();
            if (item.isNull() || !scriptedItemIds.contains(item.attribute(QStringLiteral("id")))) {
                continue;
            }

            const QString properties = item.attribute(QStringLiteral("properties"));
            const QString updatedProperties = appendProperty(properties, QStringLiteral("scripted"));
            if (updatedProperties == properties) {
                continue;
            }

            item.setAttribute(QStringLiteral("properties"), updatedProperties);
            ++result.scriptedPropertiesAdded;
            opfChanged = true;
            result.messages.append(i18n("Declared scripted property for %1", item.attribute(QStringLiteral("href"))));
        }

        if (opfChanged) {
            restoreRootNamespaceDeclarations(opfDoc, namespaces);
            replacementDataByPath.insert(m_contentFilePath, opfDoc.toByteArray());
        }
    }

    if (replacementDataByPath.isEmpty()) {
        result.messages.append(i18n("No EPUB normalization changes were needed"));
        return result;
    }

    if (!writeEpubReplacingFiles(m_filename, m_rootFolder, replacementDataByPath)) {
        result.success = false;
        result.errorMessage = i18n("Unable to write normalized EPUB file");
        return result;
    }

    result.changed = true;
    return result;
}

EpubNormalizationResult EPubContainer::updateSubjects(const QStringList &subjects) const
{
    EpubNormalizationResult result;
    if (!m_rootFolder || m_filename.isEmpty() || m_contentFilePath.isEmpty()) {
        result.success = false;
        result.errorMessage = i18n("No EPUB file is open for metadata editing");
        return result;
    }

    const KArchiveFile *opfFile = file(m_contentFilePath);
    if (!opfFile) {
        result.success = false;
        result.errorMessage = i18n("Unable to read EPUB package document %1", m_contentFilePath);
        return result;
    }

    QScopedPointer<QIODevice> device(opfFile->createDevice());
    if (!device) {
        result.success = false;
        result.errorMessage = i18n("Unable to open EPUB package document %1", m_contentFilePath);
        return result;
    }

    const QByteArray opfData = device->readAll();
    QDomDocument opfDoc;
    const XmlNamespaceDeclarations namespaces = rootNamespaceDeclarations(opfData);
    if (!setDomContentWithNamespaces(opfDoc, opfData)) {
        result.success = false;
        result.errorMessage = i18n("Unable to parse EPUB package document %1", m_contentFilePath);
        return result;
    }

    const QDomNodeList metadataNodes = opfDoc.elementsByTagName(QStringLiteral("metadata"));
    if (metadataNodes.isEmpty()) {
        result.success = false;
        result.errorMessage = i18n("Unable to find EPUB metadata document");
        return result;
    }

    QDomElement metadata = metadataNodes.at(0).toElement();
    const QString dublinCoreNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");
    const QString packageNamespace = opfDoc.documentElement().namespaceURI();
    const auto createPackageMetadataElement = [&opfDoc, &packageNamespace](const QString &name) {
        return packageNamespace.isEmpty() ? opfDoc.createElement(name) : opfDoc.createElementNS(packageNamespace, name);
    };

    QStringList existingSubjects;
    QVector<QDomNode> subjectNodes;
    QSet<QString> subjectRefines;
    QDomNode child = metadata.firstChild();
    while (!child.isNull()) {
        const QDomElement element = child.toElement();
        if (isDublinCoreSubjectElement(element)) {
            const QString subject = refinedSubjectPath(element, metadata.childNodes());
            if (!subject.isEmpty()) {
                existingSubjects.append(subject);
            }
            const QString id = element.attribute(QStringLiteral("id")).trimmed();
            if (!id.isEmpty()) {
                subjectRefines.insert(QStringLiteral("#") + id);
            }
            subjectNodes.append(child);
        }
        child = child.nextSibling();
    }

    const QStringList normalizedSubjects = subjectPathsWithAncestors(subjects);
    if (normalizedSubjectPathList(existingSubjects) == normalizedSubjects) {
        result.messages.append(i18n("No EPUB subject changes were needed"));
        return result;
    }

    QVector<QDomNode> subjectRefinementNodes;
    child = metadata.firstChild();
    while (!child.isNull()) {
        const QDomNode nextChild = child.nextSibling();
        const QDomElement element = child.toElement();
        if (element.tagName() == QStringLiteral("meta") && subjectRefines.contains(element.attribute(QStringLiteral("refines")).trimmed())) {
            subjectRefinementNodes.append(child);
        }
        child = nextChild;
    }

    for (const QDomNode &subjectNode : std::as_const(subjectNodes)) {
        metadata.removeChild(subjectNode);
    }
    for (const QDomNode &subjectRefinementNode : std::as_const(subjectRefinementNodes)) {
        metadata.removeChild(subjectRefinementNode);
    }

    QSet<QString> usedIds;
    const QDomNodeList allElements = opfDoc.elementsByTagName(QStringLiteral("*"));
    for (int i = 0; i < allElements.count(); ++i) {
        const QString id = allElements.at(i).toElement().attribute(QStringLiteral("id")).trimmed();
        if (!id.isEmpty()) {
            usedIds.insert(id);
        }
    }

    QHash<QString, QString> subjectIdsByPath;
    auto subjectId = [&usedIds](int index) {
        QString id;
        do {
            id = QStringLiteral("arianna-subject-%1").arg(index++);
        } while (usedIds.contains(id));
        usedIds.insert(id);
        return id;
    };

    QDomElement packageElement = opfDoc.documentElement();
    if (!normalizedSubjects.isEmpty()) {
        const QString prefix = packageElement.attribute(QStringLiteral("prefix"));
        static const QRegularExpression ariannaPrefixExpression(QStringLiteral("(^|\\s)arianna\\s*:"));
        if (!ariannaPrefixExpression.match(prefix).hasMatch()) {
            QString updatedPrefix = prefix.trimmed();
            if (!updatedPrefix.isEmpty()) {
                updatedPrefix += QLatin1Char(' ');
            }
            updatedPrefix += QStringLiteral("arianna: ") + QString(AriannaPackageVocabulary);
            packageElement.setAttribute(QStringLiteral("prefix"), updatedPrefix);
        }
    }

    int subjectIndex = 1;
    for (const QString &subject : normalizedSubjects) {
        subjectIdsByPath.insert(subject, subjectId(subjectIndex++));
    }

    for (const QString &subject : normalizedSubjects) {
        const QString subjectIdValue = subjectIdsByPath.value(subject);
        const QString parentPath = subjectParentPath(subject);

        QDomElement subjectElement = opfDoc.createElementNS(dublinCoreNamespace, QStringLiteral("dc:subject"));
        subjectElement.setAttribute(QStringLiteral("id"), subjectIdValue);
        subjectElement.appendChild(opfDoc.createTextNode(subjectLabelForPath(subject)));
        metadata.appendChild(subjectElement);

        QDomElement authorityElement = createPackageMetadataElement(QStringLiteral("meta"));
        authorityElement.setAttribute(QStringLiteral("refines"), QStringLiteral("#") + subjectIdValue);
        authorityElement.setAttribute(QStringLiteral("property"), QStringLiteral("authority"));
        authorityElement.appendChild(opfDoc.createTextNode(QString(AriannaSubjectAuthority)));
        metadata.appendChild(authorityElement);

        QDomElement termElement = createPackageMetadataElement(QStringLiteral("meta"));
        termElement.setAttribute(QStringLiteral("refines"), QStringLiteral("#") + subjectIdValue);
        termElement.setAttribute(QStringLiteral("property"), QStringLiteral("term"));
        termElement.appendChild(opfDoc.createTextNode(subject));
        metadata.appendChild(termElement);

        QDomElement levelElement = createPackageMetadataElement(QStringLiteral("meta"));
        levelElement.setAttribute(QStringLiteral("refines"), QStringLiteral("#") + subjectIdValue);
        levelElement.setAttribute(QStringLiteral("property"), QString(AriannaSubjectLevelProperty));
        levelElement.appendChild(opfDoc.createTextNode(QString::number(subjectLevelForPath(subject))));
        metadata.appendChild(levelElement);

        if (!parentPath.isEmpty()) {
            QDomElement parentElement = createPackageMetadataElement(QStringLiteral("meta"));
            parentElement.setAttribute(QStringLiteral("refines"), QStringLiteral("#") + subjectIdValue);
            parentElement.setAttribute(QStringLiteral("property"), QString(AriannaParentSubjectProperty));
            parentElement.appendChild(opfDoc.createTextNode(subjectIdsByPath.value(parentPath)));
            metadata.appendChild(parentElement);
        }
    }

    restoreRootNamespaceDeclarations(opfDoc, namespaces);
    if (!writeEpubReplacingFiles(m_filename, m_rootFolder, {{m_contentFilePath, opfDoc.toByteArray()}})) {
        result.success = false;
        result.errorMessage = i18n("Unable to write EPUB metadata");
        return result;
    }

    result.changed = true;
    result.messages.append(i18n("EPUB subjects updated"));
    return result;
}

EpubNormalizationResult EPubContainer::updateCreators(const QStringList &creators) const
{
    EpubNormalizationResult result;
    if (!m_rootFolder || m_filename.isEmpty() || m_contentFilePath.isEmpty()) {
        result.success = false;
        result.errorMessage = i18n("No EPUB file is open for metadata editing");
        return result;
    }

    const KArchiveFile *opfFile = file(m_contentFilePath);
    if (!opfFile) {
        result.success = false;
        result.errorMessage = i18n("Unable to read EPUB package document %1", m_contentFilePath);
        return result;
    }

    QScopedPointer<QIODevice> device(opfFile->createDevice());
    if (!device) {
        result.success = false;
        result.errorMessage = i18n("Unable to open EPUB package document %1", m_contentFilePath);
        return result;
    }

    const QByteArray opfData = device->readAll();
    QDomDocument opfDoc;
    const XmlNamespaceDeclarations namespaces = rootNamespaceDeclarations(opfData);
    if (!setDomContentWithNamespaces(opfDoc, opfData)) {
        result.success = false;
        result.errorMessage = i18n("Unable to parse EPUB package document %1", m_contentFilePath);
        return result;
    }

    const QDomNodeList metadataNodes = opfDoc.elementsByTagName(QStringLiteral("metadata"));
    if (metadataNodes.isEmpty()) {
        result.success = false;
        result.errorMessage = i18n("Unable to find EPUB metadata document");
        return result;
    }

    QDomElement metadata = metadataNodes.at(0).toElement();
    const QString dublinCoreNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");
    QStringList existingCreators;
    QVector<QDomNode> creatorNodes;
    QSet<QString> creatorRefines;

    QDomNode child = metadata.firstChild();
    while (!child.isNull()) {
        const QDomElement element = child.toElement();
        if (isDublinCoreCreatorElement(element)) {
            const QString creator = element.text().trimmed().simplified();
            if (!creator.isEmpty()) {
                existingCreators.append(creator);
            }
            const QString id = element.attribute(QStringLiteral("id")).trimmed();
            if (!id.isEmpty()) {
                creatorRefines.insert(QStringLiteral("#") + id);
            }
            creatorNodes.append(child);
        }
        child = child.nextSibling();
    }

    const QStringList normalizedCreators = normalizedMetadataList(creators);
    if (normalizedMetadataList(existingCreators) == normalizedCreators) {
        result.messages.append(i18n("No EPUB creator changes were needed"));
        return result;
    }

    QVector<QDomNode> creatorRefinementNodes;
    child = metadata.firstChild();
    while (!child.isNull()) {
        const QDomNode nextChild = child.nextSibling();
        const QDomElement element = child.toElement();
        if (element.tagName() == QStringLiteral("meta") && creatorRefines.contains(element.attribute(QStringLiteral("refines")).trimmed())) {
            creatorRefinementNodes.append(child);
        }
        child = nextChild;
    }

    QDomNode insertBeforeNode;
    for (const QDomNode &creatorNode : std::as_const(creatorNodes)) {
        if (insertBeforeNode.isNull()) {
            insertBeforeNode = creatorNode.nextSibling();
        }
        metadata.removeChild(creatorNode);
    }
    for (const QDomNode &creatorRefinementNode : std::as_const(creatorRefinementNodes)) {
        metadata.removeChild(creatorRefinementNode);
    }

    for (const QString &creator : normalizedCreators) {
        QDomElement creatorElement = opfDoc.createElementNS(dublinCoreNamespace, QStringLiteral("dc:creator"));
        creatorElement.appendChild(opfDoc.createTextNode(creator));
        if (!insertBeforeNode.isNull() && insertBeforeNode.parentNode() == metadata) {
            metadata.insertBefore(creatorElement, insertBeforeNode);
        } else {
            metadata.appendChild(creatorElement);
        }
    }

    restoreRootNamespaceDeclarations(opfDoc, namespaces);
    if (!writeEpubReplacingFiles(m_filename, m_rootFolder, {{m_contentFilePath, opfDoc.toByteArray()}})) {
        result.success = false;
        result.errorMessage = i18n("Unable to write EPUB metadata");
        return result;
    }

    result.changed = true;
    result.messages.append(i18n("EPUB creators updated"));
    return result;
}

QByteArray EPubContainer::createServerReadyEpub(const ResourceMap &resourceMap) const
{
    ServerReadyEpubOptions options;
    options.resourceMap = resourceMap;
    return createServerReadyEpub(options);
}

QByteArray EPubContainer::createServerReadyEpub(const ServerReadyEpubOptions &options) const
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return {};
    }

    QByteArray result;
    QBuffer buffer(&result);

    if (!buffer.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to open output buffer";
        return {};
    }

    KZip outZip(&buffer);

    if (!outZip.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to create server-ready EPUB";
        return {};
    }

    outZip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));

    ServerReadyCopyContext context;
    context.resourceMap = options.resourceMap;
    context.includeReferences = options.includeReferences;
    context.opfPath = m_contentFilePath;
    context.references = options.references;
    if (context.includeReferences && !context.opfPath.isEmpty()) {
        const QString opfFolder = QFileInfo(context.opfPath).path();
        context.referencesPath = QDir::cleanPath(opfFolder + QStringLiteral("/references.xhtml"));
        for (const EpubReference &reference : std::as_const(context.references)) {
            if (!reference.sourceAnchorId.isEmpty()) {
                context.referenceBySourceAnchor.insert(reference.sourceAnchorId, reference);
            }
        }
    }

    copyDirectoryServerReady(outZip, m_rootFolder, QString(), context);

    if (context.includeReferences && !context.referencesPath.isEmpty()) {
        outZip.writeFile(context.referencesPath, createReferencesDocument(context));
    }

    outZip.close();
    buffer.close();

    qDebug() << "Created server-ready EPUB with" << context.resourceMap.size() << "externalized resources and" << context.references.size()
             << "included references";

    return result;
}

QString EPubContainer::createAnchor(const QString &cfi)
{
    return createRangeAnchor(cfi, QStringLiteral("crossref"), QString(), QStringLiteral("cross-reference"));
}

QString EPubContainer::createAnchor(const QString &cfi, const QString &anchorType)
{
    return createRangeAnchor(cfi, anchorType, QString(), anchorType.isEmpty() ? QStringLiteral("target") : anchorType);
}

QString EPubContainer::createBookrefAnchor(const QString &cfi, const QString &targetLocation, const bool isCrossReference, const QString &title)
{
    const QString href = isCrossReference ? QString() : targetLocation.trimmed();
    if (!isCrossReference && href.isEmpty()) {
        qWarning() << "Unable to create intra-book reference anchor without target location";
        return {};
    }

    return createRangeAnchor(cfi,
                             isCrossReference ? QStringLiteral("crossref") : QString(),
                             href,
                             isCrossReference ? QStringLiteral("cross-reference") : QStringLiteral("intra-book reference"),
                             title.trimmed());
}

bool EPubContainer::updateReferenceAnchor(const QString &anchorId,
                                          const QString &targetLocation,
                                          const bool isCrossReference,
                                          const QString &title,
                                          bool *changed)
{
    if (changed) {
        *changed = false;
    }

    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return false;
    }

    const QString normalizedAnchorId = anchorId.trimmed();
    if (normalizedAnchorId.isEmpty()) {
        qWarning() << "Unable to update reference anchor without anchor id";
        return false;
    }

    const QString normalizedTargetLocation = targetLocation.trimmed();
    if (!isCrossReference && normalizedTargetLocation.isEmpty()) {
        qWarning() << "Unable to update intra-book reference anchor without target location";
        return false;
    }

    const QString normalizedTitle = title.trimmed();
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        const EpubItem item = it.value();
        if (!isDocumentItem(item)) {
            continue;
        }

        const QByteArray data = readData(item.path);
        if (data.isEmpty()) {
            qWarning() << "Unable to read EPUB document for reference anchor update" << item.path;
            return false;
        }

        const XmlNamespaceDeclarations rootNamespaces = rootNamespaceDeclarations(data);
        QDomDocument doc;
        if (!setDomContentPreservingMarkup(doc, data)) {
            qWarning() << "Unable to parse EPUB document for reference anchor update" << item.path;
            return false;
        }
        restoreRootNamespaceDeclarations(doc, rootNamespaces);

        QDomElement anchor = findReferenceAnchorElement(doc, normalizedAnchorId);
        if (anchor.isNull()) {
            continue;
        }

        const QString targetHref = isCrossReference || normalizedTargetLocation.isEmpty()
            ? QString()
            : (normalizedTargetLocation.startsWith(QStringLiteral("epubcfi(")) ? normalizedTargetLocation
                                                                               : relativePathFrom(item.path, normalizedTargetLocation));
        const bool hasTitle = anchor.hasAttribute(QStringLiteral("title"));
        const QString currentTitle = anchor.attribute(QStringLiteral("title")).trimmed();
        const bool titleUpToDate = currentTitle == normalizedTitle && (!normalizedTitle.isEmpty() || !hasTitle);
        const bool crossReferenceUpToDate =
            anchor.attribute(QStringLiteral("data-anchor-type")) == QStringLiteral("crossref") && !anchor.hasAttribute(QStringLiteral("href"));
        const bool intraBookReferenceUpToDate =
            anchor.attribute(QStringLiteral("data-anchor-type")) != QStringLiteral("crossref") && anchor.attribute(QStringLiteral("href")) == targetHref;
        if (titleUpToDate && (isCrossReference ? crossReferenceUpToDate : intraBookReferenceUpToDate)) {
            qDebug() << "Reference anchor already up to date" << normalizedAnchorId << "in" << m_filename;
            return true;
        }

        QString backupPath;
        if (!copyAnchorUpdateSafetyBackup(m_filename, normalizedAnchorId, &backupPath)) {
            return false;
        }

        if (normalizedTitle.isEmpty()) {
            anchor.removeAttribute(QStringLiteral("title"));
        } else {
            anchor.setAttribute(QStringLiteral("title"), normalizedTitle);
        }

        anchor.setAttribute(QStringLiteral("id"), normalizedAnchorId);
        anchor.setAttribute(QStringLiteral("data-role"), QStringLiteral("anchor"));
        if (isCrossReference) {
            anchor.setAttribute(QStringLiteral("data-anchor-type"), QStringLiteral("crossref"));
            anchor.removeAttribute(QStringLiteral("href"));
        } else {
            anchor.removeAttribute(QStringLiteral("data-anchor-type"));
            anchor.setAttribute(QStringLiteral("href"), targetHref);
        }

        if (!writeEpubReplacingFile(m_filename, m_rootFolder, item.path, doc.toByteArray())) {
            qWarning() << "Unable to write EPUB after updating reference anchor" << normalizedAnchorId << "in" << m_filename;
            return false;
        }

        if (changed) {
            *changed = true;
        }
        qDebug() << "Updated reference anchor" << normalizedAnchorId << "in" << m_filename << "backup:" << backupPath;
        return true;
    }

    qWarning() << "Reference anchor absent in EPUB during update" << normalizedAnchorId << m_filename;
    return true;
}

bool EPubContainer::deleteReferenceAnchor(const QString &anchorId)
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return false;
    }

    const QString normalizedAnchorId = anchorId.trimmed();
    if (normalizedAnchorId.isEmpty()) {
        qWarning() << "Unable to delete reference anchor without anchor id";
        return false;
    }

    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        const EpubItem item = it.value();
        if (!isDocumentItem(item)) {
            continue;
        }

        const QByteArray data = readData(item.path);
        if (data.isEmpty()) {
            qWarning() << "Unable to read EPUB document for reference anchor deletion" << item.path;
            return false;
        }

        const XmlNamespaceDeclarations rootNamespaces = rootNamespaceDeclarations(data);
        QDomDocument doc;
        if (!setDomContentPreservingMarkup(doc, data)) {
            qWarning() << "Unable to parse EPUB document for reference anchor deletion" << item.path;
            return false;
        }
        restoreRootNamespaceDeclarations(doc, rootNamespaces);

        const QDomElement anchor = findReferenceAnchorElement(doc, normalizedAnchorId);
        if (anchor.isNull()) {
            continue;
        }

        QString backupPath;
        if (!copyAnchorRemovalSafetyBackup(m_filename, normalizedAnchorId, &backupPath)) {
            return false;
        }

        if (!unwrapElement(anchor)) {
            qWarning() << "Unable to unwrap reference anchor" << normalizedAnchorId << "in" << item.path;
            return false;
        }

        if (!writeEpubReplacingFile(m_filename, m_rootFolder, item.path, doc.toByteArray())) {
            qWarning() << "Unable to write EPUB after deleting reference anchor" << normalizedAnchorId << "in" << m_filename;
            return false;
        }

        qDebug() << "Deleted reference anchor" << normalizedAnchorId << "from" << m_filename << "backup:" << backupPath;
        return true;
    }

    qWarning() << "Reference anchor already absent in EPUB" << normalizedAnchorId << m_filename;
    return true;
}

bool EPubContainer::deleteTargetRangeAnchor(const QString &anchorId)
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return false;
    }

    const QString normalizedAnchorId = anchorId.trimmed();
    if (normalizedAnchorId.isEmpty()) {
        qWarning() << "Unable to delete target range anchor without anchor id";
        return false;
    }

    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        const EpubItem item = it.value();
        if (!isDocumentItem(item)) {
            continue;
        }

        const QByteArray data = readData(item.path);
        if (data.isEmpty()) {
            qWarning() << "Unable to read EPUB document for target range anchor deletion" << item.path;
            return false;
        }

        const XmlNamespaceDeclarations rootNamespaces = rootNamespaceDeclarations(data);
        QDomDocument doc;
        if (!setDomContentPreservingMarkup(doc, data)) {
            qWarning() << "Unable to parse EPUB document for target range anchor deletion" << item.path;
            return false;
        }
        restoreRootNamespaceDeclarations(doc, rootNamespaces);

        const QDomElement anchor = findTargetRangeAnchorElement(doc, normalizedAnchorId);
        if (anchor.isNull()) {
            continue;
        }

        QString backupPath;
        if (!copyAnchorRemovalSafetyBackup(m_filename, normalizedAnchorId, &backupPath)) {
            return false;
        }

        if (!unwrapElement(anchor)) {
            qWarning() << "Unable to unwrap target range anchor" << normalizedAnchorId << "in" << item.path;
            return false;
        }

        if (!writeEpubReplacingFile(m_filename, m_rootFolder, item.path, doc.toByteArray())) {
            qWarning() << "Unable to write EPUB after deleting target range anchor" << normalizedAnchorId << "in" << m_filename;
            return false;
        }

        qDebug() << "Deleted target range anchor" << normalizedAnchorId << "from" << m_filename << "backup:" << backupPath;
        return true;
    }

    qDebug() << "Target range anchor absent or not generated by Arianna; keeping EPUB unchanged" << normalizedAnchorId << m_filename;
    return true;
}

QString EPubContainer::createRangeAnchor(const QString &cfi, const QString &anchorType, const QString &href, const QString &debugLabel, const QString &title)
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return {};
    }

    ParsedCfi parsed = parseCfi(cfi);
    if (!parsed.isRange || parsed.parent.isEmpty()) {
        qWarning() << "Unable to create anchor from non-range CFI" << cfi;
        return {};
    }

    const int spineIndex = spineIndexFromTopCfiPath(parsed.parent.constFirst());
    if (spineIndex < 0 || spineIndex >= m_orderedItems.size()) {
        qWarning() << "Unable to resolve spine item for CFI" << cfi;
        return {};
    }

    const QString itemId = m_orderedItems.at(spineIndex);
    const EpubItem item = m_items.value(itemId);
    if (!isDocumentItem(item)) {
        qWarning() << "Unable to create anchor in non-document EPUB item" << item.path << item.mimetype;
        return {};
    }

    parsed.parent.removeFirst();
    const CfiPath startPath = collapseCfiRangeSide(parsed.parent, parsed.start);
    const CfiPath endPath = collapseCfiRangeSide(parsed.parent, parsed.end);

    const QByteArray data = readData(item.path);
    if (data.isEmpty()) {
        qWarning() << "Unable to read EPUB document for anchor" << item.path;
        return {};
    }

    const XmlNamespaceDeclarations rootNamespaces = rootNamespaceDeclarations(data);
    QDomDocument doc;
    if (!setDomContentPreservingMarkup(doc, data)) {
        qWarning() << "Unable to parse EPUB document for anchor" << item.path;
        return {};
    }
    restoreRootNamespaceDeclarations(doc, rootNamespaces);

    const CfiBoundary startBoundary = resolveCfiBoundary(doc, startPath);
    const CfiBoundary endBoundary = resolveCfiBoundary(doc, endPath);

    const QString anchorId = QStringLiteral("uuid_") + QUuid::createUuidV7().toString(QUuid::WithoutBraces);
    QString anchorHref;
    if (!href.isEmpty()) {
        anchorHref = href.startsWith(QStringLiteral("epubcfi(")) ? href : relativePathFrom(item.path, href);
    }

    QString selectedText;
    if (!wrapSingleTextNodeRange(doc, startBoundary, endBoundary, anchorId, anchorType, &selectedText, QStringLiteral("a"), anchorHref, title.trimmed())
        && !wrapSiblingRange(doc, startBoundary, endBoundary, anchorId, anchorType, &selectedText, anchorHref, title.trimmed())) {
        qWarning() << "Unable to wrap CFI range with anchor" << cfi << "in" << item.path;
        return {};
    }

    QString backupPath;
    if (!copyAnchorSafetyBackup(m_filename, anchorId, &backupPath)) {
        return {};
    }

    if (!writeEpubReplacingFile(m_filename, m_rootFolder, item.path, doc.toByteArray())) {
        qWarning() << "Unable to write anchor into EPUB" << m_filename;
        return {};
    }

    qDebug() << "Created" << debugLabel << "anchor" << anchorId << "for" << selectedText << "in" << m_filename << "backup:" << backupPath;
    return anchorId;
}

QByteArray EPubContainer::createAnnotationAnchoredEpub(const QString &cfi, const QString &anchorId)
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return {};
    }

    const QString normalizedAnchorId = anchorId.trimmed();
    if (normalizedAnchorId.isEmpty()) {
        qWarning() << "Unable to create annotation anchor without anchor id";
        return {};
    }

    ParsedCfi parsed = parseCfi(cfi);
    if (!parsed.isRange || parsed.parent.isEmpty()) {
        qWarning() << "Unable to create annotation anchor from non-range CFI" << cfi;
        return {};
    }

    const int spineIndex = spineIndexFromTopCfiPath(parsed.parent.constFirst());
    if (spineIndex < 0 || spineIndex >= m_orderedItems.size()) {
        qWarning() << "Unable to resolve spine item for annotation CFI" << cfi;
        return {};
    }

    const QString itemId = m_orderedItems.at(spineIndex);
    const EpubItem item = m_items.value(itemId);
    if (!isDocumentItem(item)) {
        qWarning() << "Unable to create annotation anchor in non-document EPUB item" << item.path << item.mimetype;
        return {};
    }

    parsed.parent.removeFirst();
    const CfiPath startPath = collapseCfiRangeSide(parsed.parent, parsed.start);
    const CfiPath endPath = collapseCfiRangeSide(parsed.parent, parsed.end);

    const QByteArray data = readData(item.path);
    if (data.isEmpty()) {
        qWarning() << "Unable to read EPUB document for annotation anchor" << item.path;
        return {};
    }

    const XmlNamespaceDeclarations rootNamespaces = rootNamespaceDeclarations(data);
    QDomDocument doc;
    if (!setDomContentPreservingMarkup(doc, data)) {
        qWarning() << "Unable to parse EPUB document for annotation anchor" << item.path;
        return {};
    }
    restoreRootNamespaceDeclarations(doc, rootNamespaces);

    const CfiBoundary startBoundary = resolveCfiBoundary(doc, startPath);
    const CfiBoundary endBoundary = resolveCfiBoundary(doc, endPath);

    QString selectedText;
    const bool anchored = annotateExactInlineElementRange(startBoundary, endBoundary, normalizedAnchorId, QStringLiteral("annotation"), &selectedText)
        || wrapSingleTextNodeRange(doc, startBoundary, endBoundary, normalizedAnchorId, QStringLiteral("annotation"), &selectedText, QStringLiteral("span"))
        || insertAnnotationBoundaryPair(doc, startBoundary, endBoundary, normalizedAnchorId);
    if (!anchored) {
        qWarning() << "Unable to create annotation anchor for CFI range" << cfi << "in" << item.path;
        return {};
    }

    qDebug() << "Prepared annotation anchor" << normalizedAnchorId << "for" << selectedText << "in" << item.path;
    return createEpubReplacingFile(m_rootFolder, item.path, doc.toByteArray());
}

bool EPubContainer::setImageNotInverse(const QString &cfi, const QString &src, bool *changed)
{
    if (changed) {
        *changed = false;
    }
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return false;
    }

    ParsedCfi parsed = parseCfi(cfi);
    if (parsed.parent.isEmpty()) {
        qWarning() << "Unable to set image not_inverse without CFI" << cfi;
        return false;
    }

    const int spineIndex = spineIndexFromTopCfiPath(parsed.parent.constFirst());
    if (spineIndex < 0 || spineIndex >= m_orderedItems.size()) {
        qWarning() << "Unable to resolve spine item for image CFI" << cfi;
        return false;
    }

    const QString itemId = m_orderedItems.at(spineIndex);
    const EpubItem item = m_items.value(itemId);
    if (!isDocumentItem(item)) {
        qWarning() << "Unable to set image not_inverse in non-document EPUB item" << item.path << item.mimetype;
        return false;
    }

    parsed.parent.removeFirst();

    const QByteArray data = readData(item.path);
    if (data.isEmpty()) {
        qWarning() << "Unable to read EPUB document for image not_inverse" << item.path;
        return false;
    }

    const XmlNamespaceDeclarations rootNamespaces = rootNamespaceDeclarations(data);
    QDomDocument doc;
    if (!setDomContentPreservingMarkup(doc, data)) {
        qWarning() << "Unable to parse EPUB document for image not_inverse" << item.path;
        return false;
    }
    restoreRootNamespaceDeclarations(doc, rootNamespaces);

    QDomElement image;
    if (parsed.isRange) {
        const CfiPath startPath = collapseCfiRangeSide(parsed.parent, parsed.start);
        const CfiPath endPath = collapseCfiRangeSide(parsed.parent, parsed.end);
        image = imageElementFromCfiBoundaries(resolveCfiBoundary(doc, startPath), resolveCfiBoundary(doc, endPath));
    } else if (!parsed.parent.isEmpty()) {
        image = imageElementFromNode(resolveCfiBoundary(doc, parsed.parent.constLast()).node);
    }

    if (image.isNull()) {
        image = firstImageBySrc(doc.documentElement(), item.path, src);
    }

    if (image.isNull()) {
        qWarning() << "Unable to locate image for not_inverse marker" << cfi << src << item.path;
        return false;
    }

    if (!addCssClass(image, QStringLiteral("not_inverse"))) {
        qDebug() << "Image already marked not_inverse" << cfi << image.attribute(QStringLiteral("src"));
        return true;
    }

    QString backupPath;
    const QString marker =
        image.attribute(QStringLiteral("id")).trimmed().isEmpty() ? QStringLiteral("not_inverse_image") : image.attribute(QStringLiteral("id")).trimmed();
    if (!copyImageClassSafetyBackup(m_filename, marker, &backupPath)) {
        return false;
    }

    if (!writeEpubReplacingFile(m_filename, m_rootFolder, item.path, doc.toByteArray())) {
        qWarning() << "Unable to write image not_inverse marker into EPUB" << m_filename;
        return false;
    }

    if (changed) {
        *changed = true;
    }
    qDebug() << "Marked image not_inverse" << image.attribute(QStringLiteral("src")) << "in" << m_filename << "backup:" << backupPath;
    return true;
}

QSharedPointer<QIODevice> EPubContainer::ioDevice(const QString &path)
{
    const KArchiveFile *archive = file(path);
    if (!archive) {
        qWarning() << QStringLiteral("Unable to open file %1").arg(path.left(100));
        Q_EMIT errorOccured(i18n("Unable to open file %1", path.left(100)));
        return QSharedPointer<QIODevice>();
    }

    return QSharedPointer<QIODevice>(archive->createDevice());
}

QImage EPubContainer::image(const QString &id)
{
    if (!m_items.contains(id)) {
        qWarning() << "Asked for unknown item" << id << m_items.keys();
        return {};
    }

    const EpubItem item = m_items.value(id);

    if (!isSupportedImageItem(item)) {
        qWarning() << "Asked for unsupported type" << item.mimetype;
        return {};
    }

    return imageFromItem(id);
}

QImage EPubContainer::coverImage()
{
    auto imageForItem = [this](const QString &id) -> QImage {
        if (!m_items.contains(id)) {
            return {};
        }

        const EpubItem item = m_items.value(id);
        if (isSupportedImageItem(item)) {
            return imageFromItem(id);
        }
        if (isDocumentItem(item)) {
            return coverImageFromDocument(item.path);
        }

        return {};
    };

    QStringList candidates;
    auto addCandidate = [&candidates](const QString &id) {
        if (!id.isEmpty() && !candidates.contains(id)) {
            candidates.append(id);
        }
    };

    for (const QString &coverReference : std::as_const(m_metadata[QStringLiteral("cover")])) {
        const QString trimmedReference = coverReference.trimmed();
        addCandidate(trimmedReference);
        addCandidate(itemIdForPath(trimmedReference));
    }

    for (const QString &id : std::as_const(candidates)) {
        const QImage cover = imageForItem(id);
        if (!cover.isNull()) {
            return cover;
        }
    }

    const QString guideCoverTarget = m_standardReferences.value(EpubPageReference::CoverPage).target;
    if (!guideCoverTarget.isEmpty()) {
        const QString guideCoverId = itemIdForPath(guideCoverTarget);
        const QImage guideCover = imageForItem(guideCoverId);
        if (!guideCover.isNull()) {
            return guideCover;
        }

        const QImage guideDocumentCover = coverImageFromDocument(pathWithoutFragment(guideCoverTarget));
        if (!guideDocumentCover.isNull()) {
            return guideDocumentCover;
        }
    }

    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        if (!isSupportedImageItem(it.value())) {
            continue;
        }

        const QString coverHint = it.key() + QLatin1Char(' ') + it.value().path;
        if (!coverHint.contains(QStringLiteral("cover"), Qt::CaseInsensitive)) {
            continue;
        }

        const QImage cover = imageFromItem(it.key());
        if (!cover.isNull()) {
            return cover;
        }
    }

    return {};
}

QImage EPubContainer::imageFromItem(const QString &id)
{
    const EpubItem item = m_items.value(id);
    QSharedPointer<QIODevice> device = ioDevice(item.path);

    if (!device) {
        return {};
    }

    return QImage::fromData(device->readAll());
}

QImage EPubContainer::coverImageFromDocument(const QString &path)
{
    const QByteArray data = readData(pathWithoutFragment(path));
    if (data.isEmpty()) {
        return {};
    }

    QDomDocument document;
    if (!document.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing)) {
        return {};
    }

    QStringList imageReferences;
    collectImageReferences(document.documentElement(), imageReferences);

    for (const QString &reference : std::as_const(imageReferences)) {
        const QString imagePath = resolveRelativePath(pathWithoutFragment(path), reference);
        const QString imageId = itemIdForPath(imagePath);
        if (imageId.isEmpty()) {
            continue;
        }

        const QImage cover = imageFromItem(imageId);
        if (!cover.isNull()) {
            return cover;
        }
    }

    return {};
}

QByteArray EPubContainer::readData(const QString &path)
{
    auto device = ioDevice(path);
    if (!device) {
        return {};
    }
    return device->readAll();
}

QStringList EPubContainer::metadata(const QStringView &key)
{
    const QString keyString = key.toString();
    if (keyString == QStringLiteral("identifier")) {
        return m_metadata.value(QStringLiteral("identifiers"));
    }
    return m_metadata.value(keyString);
}

bool EPubContainer::parseMimetype()
{
    Q_ASSERT(m_rootFolder);

    const KArchiveFile *mimetypeFile = m_rootFolder->file(MIMETYPE_FILE);

    if (!mimetypeFile) {
        Q_EMIT errorOccured(i18n("Unable to find mimetype in file"));
        return false;
    }

    QScopedPointer<QIODevice> ioDevice(mimetypeFile->createDevice());
    QByteArray mimetype = ioDevice->readAll();
    if (mimetype != "application/epub+zip") {
        qWarning() << "Unexpected mimetype" << mimetype;
    }

    return true;
}

bool EPubContainer::parseContainer()
{
    Q_ASSERT(m_rootFolder);

    const KArchiveFile *containerFile = file(CONTAINER_FILE);
    if (!containerFile) {
        qWarning() << "no container file";
        Q_EMIT errorOccured(i18n("Unable to find container information"));
        return false;
    }

    QScopedPointer<QIODevice> ioDevice(containerFile->createDevice());
    Q_ASSERT(ioDevice);

    // The only thing we need from this file is the path to the root file
    QDomDocument document;
    document.setContent(ioDevice.data());
    QDomNodeList rootNodes = document.elementsByTagName(QStringLiteral("rootfile"));
    for (int i = 0; i < rootNodes.count(); i++) {
        QDomElement rootElement = rootNodes.at(i).toElement();
        QString rootfilePath = rootElement.attribute(QStringLiteral("full-path"));
        if (rootfilePath.isEmpty()) {
            qWarning() << "Invalid root file entry";
            continue;
        }
        if (parseContentFile(rootfilePath)) {
            return true;
        }
    }

    // Limitations:
    //  - We only read one rootfile
    //  - We don't read the following from META-INF/
    //     - manifest.xml (unknown contents, just reserved)
    //     - metadata.xml (unused according to spec, just reserved)
    //     - rights.xml (reserved for DRM, not standardized)
    //     - signatures.xml (signatures for files, standardized)

    Q_EMIT errorOccured(i18n("Unable to find and use any content files"));
    return false;
}

bool EPubContainer::parseContentFile(const QString &filepath)
{
    m_contentFilePath = filepath;

    const KArchiveFile *rootFile = file(filepath);
    if (!rootFile) {
        Q_EMIT errorOccured(i18n("Malformed metadata, unable to get content metadata path"));
        return false;
    }
    QScopedPointer<QIODevice> ioDevice(rootFile->createDevice());
    QDomDocument document;
    document.setContent(ioDevice.data(), QDomDocument::ParseOption::UseNamespaceProcessing); // turn on namespace processing

    const QString epubVersion = document.documentElement().attribute(QStringLiteral("version")).trimmed();
    if (!epubVersion.isEmpty()) {
        m_metadata[QStringLiteral("epub-version")] = QStringList{epubVersion};
    }

    const QString uniqueIdentifierId = document.documentElement().attribute(QStringLiteral("unique-identifier"));
    QString uniqueIdentifier;

    QDomNodeList metadataNodeList = document.elementsByTagName(QStringLiteral("metadata"));
    for (int i = 0; i < metadataNodeList.count(); i++) {
        QDomNodeList metadataChildList = metadataNodeList.at(i).childNodes();
        for (int j = 0; j < metadataChildList.count(); j++) {
            const QDomNode metadataNode = metadataChildList.at(j);
            const QDomElement metadataElement = metadataNode.toElement();
            if (!uniqueIdentifierId.isEmpty() && metadataElement.prefix() == QStringLiteral("dc") && metadataElement.tagName() == QStringLiteral("identifier")
                && metadataElement.attribute(QStringLiteral("id")) == uniqueIdentifierId) {
                uniqueIdentifier = metadataElement.text().trimmed();
            }
            parseMetadataItem(metadataNode, metadataChildList);
        }
    }

    if (uniqueIdentifier.isEmpty()) {
        uniqueIdentifier = m_metadata.value(QStringLiteral("identifiers")).value(0);
    }
    if (!uniqueIdentifier.isEmpty()) {
        m_metadata[QStringLiteral("unique-identifier")] = QStringList{uniqueIdentifier};
    }

    // Extract current path, for resolving relative paths
    QString contentFileFolder;
    int separatorIndex = filepath.lastIndexOf(QLatin1Char('/'));
    if (separatorIndex > 0) {
        contentFileFolder = filepath.left(separatorIndex + 1);
    }

    // Parse out all the components/items in the epub
    QDomNodeList manifestNodeList = document.elementsByTagName(QStringLiteral("manifest"));
    for (int i = 0; i < manifestNodeList.count(); i++) {
        QDomElement manifestElement = manifestNodeList.at(i).toElement();
        QDomNodeList manifestItemList = manifestElement.elementsByTagName(QStringLiteral("item"));

        for (int j = 0; j < manifestItemList.count(); j++) {
            parseManifestItem(manifestItemList.at(j), contentFileFolder);
        }
    }

    // Parse out the document order
    QDomNodeList spineNodeList = document.elementsByTagName(QStringLiteral("spine"));
    for (int i = 0; i < spineNodeList.count(); i++) {
        QDomElement spineElement = spineNodeList.at(i).toElement();

        QString tocId = spineElement.attribute(QStringLiteral("toc"));
        if (!tocId.isEmpty() && m_items.contains(tocId)) {
            EpubPageReference tocReference;
            tocReference.title = i18n("Table of Contents");
            tocReference.target = tocId;
            m_standardReferences.insert(EpubPageReference::TableOfContents, tocReference);
        }

        QDomNodeList spineItemList = spineElement.elementsByTagName(QStringLiteral("itemref"));
        for (int j = 0; j < spineItemList.count(); j++) {
            parseSpineItem(spineItemList.at(j));
        }
    }

    // Parse out standard items
    QDomNodeList guideNodeList = document.elementsByTagName(QStringLiteral("guide"));
    for (int i = 0; i < guideNodeList.count(); i++) {
        QDomElement guideElement = guideNodeList.at(i).toElement();

        QDomNodeList guideItemList = guideElement.elementsByTagName(QStringLiteral("reference"));
        for (int j = 0; j < guideItemList.count(); j++) {
            parseGuideItem(guideItemList.at(j), contentFileFolder);
        }
    }

    return true;
}

bool EPubContainer::parseMetadataPropertyItem(const QDomElement &metadataElement, const QDomNodeList &nodeList)
{
    if (metadataElement.attribute(QStringLiteral("property")) == QStringLiteral("belongs-to-collection")) {
        const QString id = QStringLiteral("#") + metadataElement.attribute(QStringLiteral("id"));
        const QString name = metadataElement.text();
        Collection::Type type = Collection::Type::Unknow;
        size_t position = 0;

        if (id.length() == 1) {
            m_collections.append(Collection{name, type, position});
            return true;
        }

        for (int i = 0; i < nodeList.size(); i++) {
            const auto node = nodeList.at(i);
            const auto element = node.toElement();
            if (element.tagName() != QStringLiteral("meta")) {
                continue;
            }

            if (element.attribute(QStringLiteral("refines")) != id) {
                continue;
            }

            if (element.attribute(QStringLiteral("property")) == QStringLiteral("collection-type")) {
                const auto typeString = element.text();
                if (typeString == QStringLiteral("set")) {
                    type = Collection::Type::Set;
                } else if (typeString == QStringLiteral("series")) {
                    type = Collection::Type::Series;
                }
                continue;
            }

            if (element.attribute(QStringLiteral("property")) == QStringLiteral("group-position")) {
                position = element.text().toInt();
                continue;
            }
        }

        m_collections.append(Collection{name, type, position});
        return true;
    }

    return false;
}

bool EPubContainer::parseMetadataItem(const QDomNode &metadataNode, const QDomNodeList &nodeList)
{
    QDomElement metadataElement = metadataNode.toElement();
    if (metadataElement.isNull()) {
        return false;
    }

    QString tagName = metadataElement.tagName();

    QString metaName;
    QString metaValue;

    if (tagName == QStringLiteral("meta")) {
        bool foundProperty = parseMetadataPropertyItem(metadataElement, nodeList);
        if (foundProperty) {
            return true;
        }
        metaName = metadataElement.attribute(QStringLiteral("name"));
        metaValue = metadataElement.attribute(QStringLiteral("content"));
    } else if (metadataElement.prefix() != QStringLiteral("dc") && tagName != QStringLiteral("description")) {
        qWarning() << "Unsupported metadata tag" << tagName;
        return false;
    } else if (tagName == QStringLiteral("date")) {
        metaName = metadataElement.attribute(QStringLiteral("event"));
        metaValue = metadataElement.text();
    } else {
        metaName = tagName;
        metaValue = metadataElement.text();
    }

    if (metaName == QStringLiteral("identifier")) {
        metaName = QStringLiteral("identifiers");
        metaValue = metaValue.trimmed();
    }

    if (metaName.isEmpty() || metaValue.isEmpty()) {
        return false;
    }
    if (!m_metadata.contains(metaName)) {
        m_metadata[metaName] = QStringList{};
    }

    if (metaName != QStringLiteral("subject")) {
        m_metadata[metaName].append(metaValue);
        return true;
    }

    metaValue = refinedSubjectPath(metadataElement, nodeList);

    if (metaValue.contains(QStringLiteral("--"))) {
        const auto metaValues = metaValue.split(QStringLiteral("--"));
        if (metaValues.count() <= 1) {
            return false;
        }

        metaValue = metaValues[metaValues.count() - 1].trimmed();
    }

    if (!m_metadata[metaName].contains(metaValue)) {
        m_metadata[metaName].append(metaValue);
        return true;
    }

    return false;
}

bool EPubContainer::parseManifestItem(const QDomNode &manifestNode, const QString &currentFolder)
{
    QDomElement manifestElement = manifestNode.toElement();
    QString id = manifestElement.attribute(QStringLiteral("id"));
    QString path = manifestElement.attribute(QStringLiteral("href"));
    QString type = manifestElement.attribute(QStringLiteral("media-type"));
    const QStringList properties = manifestElement.attribute(QStringLiteral("properties")).split(u' ', Qt::SkipEmptyParts);

    if (id.isEmpty() || path.isEmpty()) {
        qWarning() << "Invalid item at line" << manifestElement.lineNumber();
        return false;
    }

    // Resolve relative paths
    path = QDir::cleanPath(currentFolder + archivePathFromUri(path));

    EpubItem item;
    item.mimetype = type.toUtf8();
    item.path = path;
    item.properties = properties;
    m_items[id] = item;

    if (properties.contains(QStringLiteral("cover-image")) && !m_metadata[QStringLiteral("cover")].contains(id)) {
        m_metadata[QStringLiteral("cover")].append(id);
    }

    static QSet<QString> documentTypes({QStringLiteral("text/x-oeb1-document"),
                                        QStringLiteral("application/x-dtbook+xml"),
                                        QStringLiteral("application/xhtml+xml"),
                                        QStringLiteral("text/html")});
    // All items not listed in the spine should be in this
    if (documentTypes.contains(type)) {
        m_unorderedItems.insert(id);
    }

    return true;
}

bool EPubContainer::parseSpineItem(const QDomNode &spineNode)
{
    QDomElement spineElement = spineNode.toElement();

    // Ignore this for now
    if (spineElement.attribute(QStringLiteral("linear")) == QStringLiteral("no")) {
        //        return true;
    }

    QString referenceName = spineElement.attribute(QStringLiteral("idref"));
    if (referenceName.isEmpty()) {
        qWarning() << "Invalid spine item at line" << spineNode.lineNumber();
        return false;
    }

    if (!m_items.contains(referenceName)) {
        qWarning() << "Unable to find" << referenceName << "in items";
        return false;
    }

    m_unorderedItems.remove(referenceName);
    m_orderedItems.append(referenceName);

    return true;
}

bool EPubContainer::parseGuideItem(const QDomNode &guideItem, const QString &currentFolder)
{
    QDomElement guideElement = guideItem.toElement();
    QString target = guideElement.attribute(QStringLiteral("href"));
    QString title = guideElement.attribute(QStringLiteral("title"));
    QString type = guideElement.attribute(QStringLiteral("type"));

    if (target.isEmpty() || type.isEmpty()) {
        qWarning() << "Invalid guide item" << target << title << type;
        return false;
    }
    target = resolveRelativePath(currentFolder + QStringLiteral("content.opf"), target);

    EpubPageReference reference;
    reference.target = target;
    reference.title = title;

    EpubPageReference::StandardType standardType = EpubPageReference::typeFromString(type);
    if (standardType == EpubPageReference::Other) {
        m_otherReferences[type] = reference;
    } else {
        m_standardReferences[standardType] = reference;
    }

    return true;
}

const KArchiveFile *EPubContainer::file(const QString &path) const
{
    if (path.isEmpty()) {
        return nullptr;
    }

    const KArchiveDirectory *folder = m_rootFolder;

    // Try to walk down the correct path
    QStringList pathParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < pathParts.count() - 1; i++) {
        QString folderName = pathParts[i];
        const KArchiveEntry *entry = folder->entry(folderName);
        if (!entry) {
            qWarning() << "Unable to find folder name" << folderName << "in" << path.left(100);
            const QStringList entries = folder->entries();
            for (const QString &folderEntry : entries) {
                if (folderEntry.compare(folderName, Qt::CaseInsensitive) == 0) {
                    entry = folder->entry(folderEntry);
                    break;
                }
            }

            if (!entry) {
                qWarning() << "Didn't even find with case-insensitive matching";
                return nullptr;
            }
        }

        if (!entry->isDirectory()) {
            qWarning() << "Expected" << folderName << "to be a directory in path" << path;
            return nullptr;
        }

        folder = dynamic_cast<const KArchiveDirectory *>(entry);
        Q_ASSERT(folder);
    }

    QString filename;
    if (pathParts.isEmpty()) {
        filename = path;
    } else {
        filename = pathParts.last();
    }

    const KArchiveFile *file = folder->file(filename);
    if (!file) {
        qWarning() << "Unable to find file" << filename << "in" << folder->name();

        const QStringList entries = folder->entries();
        for (const QString &folderEntry : entries) {
            if (folderEntry.compare(filename, Qt::CaseInsensitive) == 0) {
                file = folder->file(folderEntry);
                break;
            }
        }

        if (!file) {
            qWarning() << "Unable to find file" << filename << "in" << folder->name() << "with case-insensitive matching" << entries;
        }
    }
    return file;
}
void EPubContainer::extractTargetAnchors()
{
    if (m_targetAnchorsExtracted) {
        return;
    }

    m_targetAnchorsExtracted = true;
    m_targetAnchors.clear();
    m_targetAnchorIndex.clear();

    qDebug() << "Extracting target anchors from EPUB";
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        if (it->mimetype != "application/xhtml+xml")
            continue;

        extractTargetAnchorsFromDocument(it.key(), it->path);
    }
}

QString buildCFI(QDomNode node)
{
    QList<int> steps;

    while (!node.parentNode().isNull()) {
        int index = 0;

        QDomNode sibling = node.previousSibling();

        while (!sibling.isNull()) {
            if (sibling.isElement())
                ++index;

            sibling = sibling.previousSibling();
        }

        steps.prepend((index + 1) * 2);

        node = node.parentNode();
    }

    QString result;

    for (int s : steps)
        result += QStringLiteral("/") + QString::number(s);
    return result;
}

static QString buildSpineCFI(int spineIndex)
{
    return QStringLiteral("/6/%1").arg((spineIndex + 1) * 2);
}

static QString buildEpubCfiForNode(const QStringList &orderedItems, const QString &itemId, const QDomNode &node)
{
    const QString nodeCfi = buildCFI(node);
    if (nodeCfi.isEmpty()) {
        return {};
    }

    const int spineIndex = orderedItems.indexOf(itemId);
    if (spineIndex >= 0) {
        return QStringLiteral("epubcfi(") + buildSpineCFI(spineIndex) + QStringLiteral("!") + nodeCfi + QStringLiteral(")");
    }

    return QStringLiteral("epubcfi(") + nodeCfi + QStringLiteral(")");
}

static void appendNodeText(const QDomNode &node, QString &text, const int maxLength)
{
    if (text.size() >= maxLength) {
        return;
    }

    if (node.isText() || node.isCDATASection()) {
        text += node.nodeValue();
        return;
    }

    const QDomElement element = node.toElement();
    const QString localName = element.localName().isEmpty() ? element.tagName() : element.localName();
    if (localName == QStringLiteral("script") || localName == QStringLiteral("style")) {
        return;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull() && text.size() < maxLength) {
        appendNodeText(child, text, maxLength);
        child = child.nextSibling();
    }
}

static QString textPreview(const QDomNode &node)
{
    QString text;
    appendNodeText(node, text, 600);
    return text.simplified().left(300);
}

static QDomNode documentPreviewNode(const QDomDocument &doc)
{
    const QDomNodeList bodies = doc.elementsByTagName(QStringLiteral("body"));
    if (!bodies.isEmpty()) {
        return bodies.at(0);
    }

    return doc.documentElement();
}

static QString elementTitle(const QDomElement &element)
{
    QString title = element.attribute(QStringLiteral("title")).trimmed();
    if (title.isEmpty()) {
        title = element.attribute(QStringLiteral("aria-label")).trimmed();
    }
    if (title.isEmpty()) {
        title = textPreview(element).left(80);
    }

    return title;
}

static QString elementLocalName(const QDomElement &element)
{
    return (element.localName().isEmpty() ? element.tagName() : element.localName()).toLower();
}

static bool isCrossReferenceSourceAnchor(const QDomElement &element)
{
    return element.attribute(QStringLiteral("data-role")) == QStringLiteral("anchor")
        && element.attribute(QStringLiteral("data-anchor-type")) == QStringLiteral("crossref");
}

static QString epubTypeAttribute(const QDomElement &element)
{
    QString value = element.attribute(QStringLiteral("epub:type")).trimmed();
    if (value.isEmpty()) {
        value = element.attributeNS(QStringLiteral("http://www.idpf.org/2007/ops"), QStringLiteral("type")).trimmed();
    }

    return value;
}

static bool elementHasEpubType(const QDomElement &element, const QString &type)
{
    const QStringList types = epubTypeAttribute(element).split(u' ', Qt::SkipEmptyParts);
    return types.contains(type);
}

static QVector<QDomElement> directElementChildren(const QDomNode &node, const QString &localName = {})
{
    QVector<QDomElement> elements;
    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        const QDomElement element = child.toElement();
        if (!element.isNull() && (localName.isEmpty() || elementLocalName(element) == localName)) {
            elements.append(element);
        }
        child = child.nextSibling();
    }

    return elements;
}

static QDomElement firstDirectElementChild(const QDomNode &node, const QString &localName)
{
    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        const QDomElement element = child.toElement();
        if (!element.isNull() && elementLocalName(element) == localName) {
            return element;
        }
        child = child.nextSibling();
    }

    return {};
}

static QStringList tocPathWithLabel(QStringList parentPath, const QString &label, const QString &fallback)
{
    const QString segment = label.trimmed().isEmpty() ? fallback : label.trimmed();
    if (!segment.isEmpty()) {
        parentPath.append(segment);
    }

    return parentPath;
}

static void applyTocContext(TargetAnchorInfo &target, const TargetAnchorInfo &tocTarget)
{
    target.tocTitle = tocTarget.title;
    target.tocPath = tocTarget.tocPath;
    target.tocDepth = tocTarget.tocDepth + 1;
}

static void appendReferenceableTarget(QVector<TargetAnchorInfo> &targets, QSet<QString> &seenLocations, TargetAnchorInfo target)
{
    if (target.location.isEmpty() || seenLocations.contains(target.location)) {
        return;
    }

    if (target.ref.isEmpty()) {
        target.ref = target.location;
    }
    if (target.title.isEmpty()) {
        target.title = target.location;
    }

    seenLocations.insert(target.location);
    targets.append(target);
}

void EPubContainer::extractNavigationTargetsFromDocument(const QString &itemId,
                                                         const QString &path,
                                                         QVector<TargetAnchorInfo> &targets,
                                                         QSet<QString> &seenLocations)
{
    Q_UNUSED(itemId)

    const QByteArray data = readData(path);
    if (data.isEmpty()) {
        return;
    }

    QDomDocument doc;
    if (!setDomContentWithNamespaces(doc, data)) {
        return;
    }

    const QString bookId = m_metadata[QStringLiteral("unique-identifier")].value(0);
    auto appendNavigationTarget = [&](const QDomElement &link, const QStringList &tocPath, const int depth) {
        const QString location = resolveReferenceTarget(path, link.attribute(QStringLiteral("href")));
        const QString targetItemId = itemIdForPath(location);
        if (location.isEmpty() || targetItemId.isEmpty()) {
            return;
        }

        const QString label = textPreview(link);
        TargetAnchorInfo target;
        target.bookId = bookId;
        target.ref = location;
        target.file = targetItemId;
        target.location = location;
        target.previewHtml = label;
        target.title = label;
        target.type = QStringLiteral("navigation");
        target.tocTitle = label;
        target.tocPath = tocPath;
        target.tocDepth = depth;
        appendReferenceableTarget(targets, seenLocations, target);
    };

    std::function<void(const QDomElement &, const QStringList &, int)> collectList = [&](const QDomElement &list, const QStringList &parentPath, int depth) {
        for (const QDomElement &item : directElementChildren(list, QStringLiteral("li"))) {
            QDomElement labelElement;
            for (const QDomElement &child : directElementChildren(item)) {
                const QString childName = elementLocalName(child);
                if (childName == QStringLiteral("a") || childName == QStringLiteral("span")) {
                    labelElement = child;
                    break;
                }
            }

            const QString label = textPreview(labelElement);
            const QString fallback = labelElement.attribute(QStringLiteral("href")).trimmed();
            const QStringList childPath = tocPathWithLabel(parentPath, label, fallback);
            if (!labelElement.isNull() && elementLocalName(labelElement) == QStringLiteral("a")
                && !labelElement.attribute(QStringLiteral("href")).trimmed().isEmpty()) {
                appendNavigationTarget(labelElement, childPath, depth);
            }

            for (const QDomElement &childList : directElementChildren(item, QStringLiteral("ol"))) {
                collectList(childList, childPath, depth + 1);
            }
        }
    };

    std::function<void(const QDomNode &)> visit = [&](const QDomNode &node) {
        const QDomElement element = node.toElement();
        if (!element.isNull() && elementLocalName(element) == QStringLiteral("nav") && elementHasEpubType(element, QStringLiteral("toc"))) {
            for (const QDomElement &list : directElementChildren(element, QStringLiteral("ol"))) {
                collectList(list, {}, 0);
            }
            return;
        }

        QDomNode child = node.firstChild();
        while (!child.isNull()) {
            visit(child);
            child = child.nextSibling();
        }
    };

    visit(doc.documentElement());
}

static QString ncxLabelForNavPoint(const QDomElement &navPoint)
{
    const QDomElement labelElement = firstDirectElementChild(navPoint, QStringLiteral("navlabel"));
    if (!labelElement.isNull()) {
        const QString label = textPreview(labelElement);
        if (!label.isEmpty()) {
            return label;
        }
    }

    return textPreview(navPoint);
}

void EPubContainer::extractNcxTargetsFromDocument(const QString &itemId, const QString &path, QVector<TargetAnchorInfo> &targets, QSet<QString> &seenLocations)
{
    Q_UNUSED(itemId)

    const QByteArray data = readData(path);
    if (data.isEmpty()) {
        return;
    }

    QDomDocument doc;
    if (!setDomContentWithNamespaces(doc, data)) {
        return;
    }

    const QString bookId = m_metadata[QStringLiteral("unique-identifier")].value(0);
    auto appendNavigationTarget = [&](const QDomElement &content, const QString &label, const QStringList &tocPath, const int depth) {
        const QString location = resolveReferenceTarget(path, content.attribute(QStringLiteral("src")));
        const QString targetItemId = itemIdForPath(location);
        if (location.isEmpty() || targetItemId.isEmpty()) {
            return;
        }

        TargetAnchorInfo target;
        target.bookId = bookId;
        target.ref = location;
        target.file = targetItemId;
        target.location = location;
        target.previewHtml = label;
        target.title = label;
        target.type = QStringLiteral("navigation");
        target.tocTitle = label;
        target.tocPath = tocPath;
        target.tocDepth = depth;
        appendReferenceableTarget(targets, seenLocations, target);
    };

    std::function<void(const QDomElement &, const QStringList &, int)> collectNavPoint =
        [&](const QDomElement &navPoint, const QStringList &parentPath, int depth) {
            const QString label = ncxLabelForNavPoint(navPoint);
            const QDomElement content = firstDirectElementChild(navPoint, QStringLiteral("content"));
            const QString fallback = content.attribute(QStringLiteral("src")).trimmed();
            const QStringList childPath = tocPathWithLabel(parentPath, label, fallback);
            if (!content.isNull()) {
                appendNavigationTarget(content, label, childPath, depth);
            }

            for (const QDomElement &childPoint : directElementChildren(navPoint, QStringLiteral("navpoint"))) {
                collectNavPoint(childPoint, childPath, depth + 1);
            }
        };

    for (const QDomElement &navMap : directElementChildren(doc.documentElement(), QStringLiteral("navmap"))) {
        for (const QDomElement &navPoint : directElementChildren(navMap, QStringLiteral("navpoint"))) {
            collectNavPoint(navPoint, {}, 0);
        }
    }
}

void EPubContainer::extractReferenceableTargetsFromDocument(const QString &itemId,
                                                            const QString &path,
                                                            QVector<TargetAnchorInfo> &targets,
                                                            QSet<QString> &seenLocations,
                                                            const QHash<QString, TargetAnchorInfo> &tocTargetsByLocation)
{
    const QByteArray data = readData(path);
    if (data.isEmpty()) {
        return;
    }

    QDomDocument doc;
    if (!setDomContentWithNamespaces(doc, data)) {
        return;
    }

    const QString bookId = m_metadata[QStringLiteral("unique-identifier")].value(0);
    const QString documentLocation = pathWithoutFragment(path);
    const QString documentCfiLocation = buildEpubCfiForNode(m_orderedItems, itemId, doc.documentElement());
    TargetAnchorInfo currentTocTarget;
    bool hasCurrentTocTarget = false;
    auto updateCurrentTocTarget = [&](const QString &location) {
        const auto it = tocTargetsByLocation.constFind(location);
        if (it == tocTargetsByLocation.constEnd()) {
            return false;
        }

        currentTocTarget = it.value();
        hasCurrentTocTarget = true;
        return true;
    };
    updateCurrentTocTarget(documentLocation);

    if (hasCurrentTocTarget) {
        TargetAnchorInfo tocTarget = currentTocTarget;
        if (tocTarget.cfiLocation.isEmpty()) {
            tocTarget.cfiLocation = documentCfiLocation;
        }
        appendReferenceableTarget(targets, seenLocations, tocTarget);
    } else {
        TargetAnchorInfo documentTarget;
        documentTarget.bookId = bookId;
        documentTarget.ref = documentLocation;
        documentTarget.file = itemId;
        documentTarget.location = documentLocation;
        documentTarget.cfiLocation = documentCfiLocation;
        documentTarget.previewHtml = textPreview(documentPreviewNode(doc));
        documentTarget.title = documentLocation;
        documentTarget.type = QStringLiteral("document");
        appendReferenceableTarget(targets, seenLocations, documentTarget);
    }

    std::function<void(const QDomNode &)> visit = [&](const QDomNode &node) {
        const QDomElement element = node.toElement();
        if (!element.isNull()) {
            const QString elementId = element.attribute(QStringLiteral("id")).trimmed();
            const QString elementName = element.attribute(QStringLiteral("name")).trimmed();
            const QString elementFragment = elementId.isEmpty() ? elementName : elementId;
            const QString dataRole = element.attribute(QStringLiteral("data-role"));
            const QString anchorType = element.attribute(QStringLiteral("data-anchor-type"));

            if (!elementFragment.isEmpty() && !isCrossReferenceSourceAnchor(element)) {
                const QString location = documentLocation + QStringLiteral("#") + elementFragment;
                const QString cfiLocation = buildEpubCfiForNode(m_orderedItems, itemId, element);
                const bool isTocLocation = updateCurrentTocTarget(location);
                if (isTocLocation) {
                    TargetAnchorInfo tocTarget = currentTocTarget;
                    if (tocTarget.cfiLocation.isEmpty()) {
                        tocTarget.cfiLocation = cfiLocation;
                    }
                    appendReferenceableTarget(targets, seenLocations, tocTarget);
                }

                TargetAnchorInfo target;
                target.bookId = bookId;
                target.file = itemId;
                target.location = location;
                target.cfiLocation = cfiLocation;
                target.previewHtml = textPreview(element);
                target.title = elementTitle(element);

                const bool legacyTarget = dataRole == QStringLiteral("target");
                const bool referenceableAnchor = dataRole == QStringLiteral("anchor") && anchorType != QStringLiteral("crossref");
                if (legacyTarget || referenceableAnchor) {
                    target.ref = element.attribute(QStringLiteral("data-ref")).trimmed();
                    if (target.ref.isEmpty()) {
                        target.ref = elementFragment;
                    }
                    target.type = anchorType.isEmpty() ? (legacyTarget ? QStringLiteral("target") : QStringLiteral("anchor")) : anchorType;
                } else {
                    target.ref = target.location;
                    target.type = QStringLiteral("element");
                }

                if (hasCurrentTocTarget) {
                    applyTocContext(target, currentTocTarget);
                }
                appendReferenceableTarget(targets, seenLocations, target);
            }
        }

        QDomNode child = node.firstChild();
        while (!child.isNull()) {
            visit(child);
            child = child.nextSibling();
        }
    };

    visit(doc.documentElement());
}

QVector<TargetAnchorInfo> EPubContainer::referenceableTargets()
{
    QVector<TargetAnchorInfo> targets;
    QSet<QString> seenLocations;
    QVector<TargetAnchorInfo> navigationTargets;
    QSet<QString> seenNavigationLocations;

    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        const EpubItem item = it.value();
        if (item.properties.contains(QStringLiteral("nav")) && isDocumentItem(item)) {
            extractNavigationTargetsFromDocument(it.key(), item.path, navigationTargets, seenNavigationLocations);
        } else if (item.mimetype == QByteArrayLiteral("application/x-dtbncx+xml")) {
            extractNcxTargetsFromDocument(it.key(), item.path, navigationTargets, seenNavigationLocations);
        }
    }

    QHash<QString, TargetAnchorInfo> tocTargetsByLocation;
    for (const TargetAnchorInfo &target : std::as_const(navigationTargets)) {
        if (target.type == QStringLiteral("navigation") && !target.location.isEmpty() && !tocTargetsByLocation.contains(target.location)) {
            tocTargetsByLocation.insert(target.location, target);
        }
    }

    QStringList itemIds = m_orderedItems;
    QStringList unorderedDocumentItemIds;
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        if (!isDocumentItem(it.value()) || isNavigationDocumentItem(it.value()) || itemIds.contains(it.key())) {
            continue;
        }

        unorderedDocumentItemIds.append(it.key());
    }

    std::sort(unorderedDocumentItemIds.begin(), unorderedDocumentItemIds.end(), [this](const QString &left, const QString &right) {
        return QString::localeAwareCompare(m_items.value(left).path, m_items.value(right).path) < 0;
    });
    itemIds.append(unorderedDocumentItemIds);

    for (const QString &itemId : std::as_const(itemIds)) {
        const EpubItem item = m_items.value(itemId);
        if (!isDocumentItem(item) || isNavigationDocumentItem(item)) {
            continue;
        }

        extractReferenceableTargetsFromDocument(itemId, item.path, targets, seenLocations, tocTargetsByLocation);
    }

    for (const TargetAnchorInfo &target : std::as_const(navigationTargets)) {
        appendReferenceableTarget(targets, seenLocations, target);
    }

    return targets;
}

void EPubContainer::extractTargetAnchorsFromDocument(const QString &itemId, const QString &path)
{
    QByteArray data = readData(path);

    QDomDocument doc;
    if (!doc.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing))
        return;
    int found = 0;
    std::function<void(const QDomNode &)> visit = [&](const QDomNode &node) {
        QDomElement e = node.toElement();
        if (!e.isNull()) {
            const QString dataRole = e.attribute(QStringLiteral("data-role"));
            const QString anchorType = e.attribute(QStringLiteral("data-anchor-type"));
            const QString elementId = e.attribute(QStringLiteral("id")).trimmed();
            const bool legacyTarget = dataRole == QStringLiteral("target");
            const bool referenceableAnchor = dataRole == QStringLiteral("anchor") && anchorType != QStringLiteral("crossref") && !elementId.isEmpty();
            if (legacyTarget || referenceableAnchor) {
                qDebug() << "Found target anchor in document" << path << "with id" << elementId << "and data-ref" << e.attribute(QStringLiteral("data-ref"));
                TargetAnchorInfo anchor;
                anchor.bookId = m_metadata[QStringLiteral("unique-identifier")].value(0);
                anchor.ref = e.attribute(QStringLiteral("data-ref")).trimmed();
                if (anchor.ref.isEmpty()) {
                    anchor.ref = elementId;
                }
                anchor.file = itemId;
                anchor.title = e.attribute(QStringLiteral("title")).trimmed();
                anchor.type = anchorType.isEmpty() ? (legacyTarget ? QStringLiteral("target") : QStringLiteral("anchor")) : anchorType;
                anchor.cfiLocation = buildEpubCfiForNode(m_orderedItems, itemId, e);

                if (!elementId.isEmpty()) {
                    anchor.location = pathWithoutFragment(path) + QStringLiteral("#") + elementId;
                } else {
                    anchor.location = anchor.cfiLocation;
                }

                QString innerHtml;
                for (QDomNode child = e.firstChild(); !child.isNull(); child = child.nextSibling()) {
                    QTextStream stream(&innerHtml);
                    child.save(stream, 0);
                }
                anchor.previewHtml = innerHtml;

                m_targetAnchors.append(anchor);
                ++found;
                // Maintain an index for fast lookup by ref
                if (!anchor.ref.isEmpty()) {
                    m_targetAnchorIndex.insert(anchor.ref, anchor);
                }
            }
        }

        QDomNode child = node.firstChild();
        while (!child.isNull()) {
            visit(child);
            child = child.nextSibling();
        }
    };

    visit(doc.documentElement());
}

QVector<TargetAnchorInfo> EPubContainer::targetAnchors() const
{
    return m_targetAnchors;
}

const TargetAnchorInfo *EPubContainer::targetAnchorByRef(const QString &ref) const
{
    auto it = m_targetAnchorIndex.constFind(ref);
    if (it == m_targetAnchorIndex.constEnd()) {
        return nullptr;
    }

    return &it.value();
}

QString EPubContainer::itemIdForPath(const QString &path) const
{
    const QString normalizedPath = pathWithoutFragment(path);
    const QString decodedPath = archivePathFromUri(path);
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        const QString itemPath = pathWithoutFragment(it.value().path);
        if (itemPath == normalizedPath || itemPath == decodedPath) {
            return it.key();
        }
    }

    return {};
}

EpubPageReference::StandardType EpubPageReference::typeFromString(const QString &name)
{
    const QString normalizedName = name.toLower();
    if (normalizedName == QStringLiteral("cover")) {
        return CoverPage;
    } else if (normalizedName == QStringLiteral("title-page")) {
        return TitlePage;
    } else if (normalizedName == QStringLiteral("toc")) {
        return TableOfContents;
    } else if (normalizedName == QStringLiteral("index")) {
        return Index;
    } else if (normalizedName == QStringLiteral("glossary")) {
        return Glossary;
    } else if (normalizedName == QStringLiteral("acknowledgements")) {
        return Acknowledgements;
    } else if (normalizedName == QStringLiteral("bibliography")) {
        return Bibliography;
    } else if (normalizedName == QStringLiteral("colophon")) {
        return Colophon;
    } else if (normalizedName == QStringLiteral("copyright-page")) {
        return CopyrightPage;
    } else if (normalizedName == QStringLiteral("dedication")) {
        return Dedication;
    } else if (normalizedName == QStringLiteral("epigraph")) {
        return Epigraph;
    } else if (normalizedName == QStringLiteral("foreword")) {
        return Foreword;
    } else if (normalizedName == QStringLiteral("loi")) {
        return ListOfIllustrations;
    } else if (normalizedName == QStringLiteral("lot")) {
        return ListOfTables;
    } else if (normalizedName == QStringLiteral("notes")) {
        return Notes;
    } else if (normalizedName == QStringLiteral("preface")) {
        return Preface;
    } else if (normalizedName == QStringLiteral("text")) {
        return Text;
    } else {
        return Other;
    }
}

QList<Collection> EPubContainer::collections() const
{
    return m_collections;
}

#include "moc_epubcontainer.cpp"
