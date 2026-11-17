// SPDX-FileCopyrightText: 2018 Martin T. H. Sandsmark <martin.sandsmark@kde.org>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <KZip>
#include <QDomNode>
#include <QHash>
#include <QList>
#include <QMimeDatabase>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QVariantList>
#include <functional>
#include <memory>

using ResourceMap = QMap<QString, QString>;

class KArchiveDirectory;
class KArchiveFile;
class QXmlStreamReader;

struct EpubItem {
    QString path;
    QByteArray mimetype;
    QStringList properties;
};

struct EpubSearchMatch {
    QString location;
    QString section;
    QString before;
    QString text;
    QString after;
};

struct EpubPageReference {
    enum StandardType {
        CoverPage,
        TitlePage,
        TableOfContents,
        Index,
        Glossary,
        Acknowledgements,
        Bibliography,
        Colophon,
        CopyrightPage,
        Dedication,
        Epigraph,
        Foreword,
        ListOfIllustrations,
        ListOfTables,
        Notes,
        Preface,
        Text,
        Other
    };

    static StandardType typeFromString(const QString &name);

    QString target;
    QString title;
};

struct TargetAnchorInfo {
    QString bookId;
    QString ref;
    QString file;
    QString location;
    QString cfiLocation;
    QString previewHtml;
    QString title;
    QString type;
    QString tocTitle;
    QStringList tocPath;
    QStringList tocIdPath; // Transient TOC node identities, independent of target hrefs.
    int tocDepth = 0;
};

struct EpubReference {
    QString sourceAnchorId;
    QString sourceAnchorTitle;
    QString targetBookId;
    QString targetLocation;
    QString targetPreviewHtml;
};

struct ServerReadyEpubOptions {
    ResourceMap resourceMap;
    QString mathJaxBootstrapUrl;
    bool includeReferences = false;
    QVector<EpubReference> references;
};

struct EpubNormalizationResult {
    bool success = true;
    bool changed = false;
    int emptyTitlesFixed = 0;
    int scriptedPropertiesAdded = 0;
    QStringList messages;
    QString errorMessage;
};

struct Collection {
    enum Type {
        Set,
        Series,
        Unknow,
    };

    QString name;
    Type type;
    size_t position;
};

class EPubContainer : public QObject
{
    Q_OBJECT
public:
    explicit EPubContainer(QObject *parent);
    ~EPubContainer() override;

    bool openFile(const QString &path);

    /// Literal, case-insensitive search in spine order, with navigable CFI ranges.
    /// Collapses whitespace across inline markup and excludes scripts and styles.
    /// Safe on a worker when this container belongs to that worker.
    QVector<EpubSearchMatch> searchText(const QString &query,
                                      const std::function<bool()> &cancelled = {},
                                      QString *error = nullptr);

    EpubItem epubItem(const QString &id) const
    {
        return m_items.value(id);
    }
    QSharedPointer<QIODevice> ioDevice(const QString &path);
    QByteArray readData(const QString &path);
    QImage image(const QString &id);
    QImage coverImage();
    QList<Collection> collections() const;
    QStringList metadata(const QStringView &key);
    QStringList items() const
    {
        return m_orderedItems;
    }
    QString getFileHash() const;
    QString getContentHash() const;
    QString packagePath() const { return m_contentFilePath; }
    QString cfiForNode(const QString &itemId, const QDomNode &node) const;
    QByteArray createMetadataCandidate(const QHash<QString, QByteArray> &replacements,
                                      const QHash<QString, QByteArray> &additions = {}) const;
    EpubNormalizationResult normalizeForArianna(const QString &fallbackTitle = QString()) const;
    EpubNormalizationResult updateSubjects(const QStringList &subjects) const;
    EpubNormalizationResult updateCreators(const QStringList &creators) const;
    EpubNormalizationResult replaceBookIdentifiers(const QStringList &oldIds, const QString &newId) const;

    QStringList manifestItemIds() const
    {
        return m_items.keys();
    }

public:
    const QHash<QString, EpubItem> &manifestItems() const;

    QByteArray createServerReadyEpub(const ResourceMap &resourceMap) const;
    bool needsMathJax() const;
    QByteArray createServerReadyEpub(const ServerReadyEpubOptions &options) const;
    QString createAnchor(const QString &cfi);
    QString createAnchor(const QString &cfi, const QString &anchorType, bool createBackup = true);
    QString createBookrefAnchor(const QString &cfi,
                               const QString &targetLocation,
                               bool isCrossReference,
                               const QString &title = QString(),
                               bool createBackup = true);
    bool updateReferenceAnchor(const QString &anchorId, const QString &targetLocation, bool isCrossReference, const QString &title, bool *changed = nullptr);
    // Update just the title in a prepared candidate; the caller handles backup and commit.
    bool updateReferenceAnchorTitle(const QString &documentPath, const QString &anchorId, const QString &title);
    // Apply an exact, previously verified removal to a candidate file, preserving its text and formatting.
    bool removeReferenceMarker(const QString &documentPath, int elementIndex, const QString &anchorId, bool source, bool retainAnchor);
    bool deleteReferenceAnchor(const QString &anchorId);
    // Remove exactly one cross-reference element, retaining its child content.
    bool deleteOrphanReferenceAnchor(const QString &documentPath, int elementIndex, const QString &anchorId);
    bool deleteTargetRangeAnchor(const QString &anchorId);
    bool setImageNotInverse(const QString &cfi, const QString &src = QString(), bool *changed = nullptr, bool createBackup = false);
    QByteArray createAnnotationAnchoredEpub(const QString &cfi, const QString &anchorId, const QString &expectedText = {});
    QVariantList findAnnotationText(const QString &text);
    int countAnnotationText(const QString &text);
    QByteArray createAnnotationAnchoredEpubAtText(const QString &documentPath, int occurrence, const QString &text, const QString &anchorId);
    /// Returns only the navigation-document / NCX targets. Unlike
    /// referenceableTargets(), this does not parse every spine document.
    QVector<TargetAnchorInfo> navigationTargets();
    QVector<TargetAnchorInfo> referenceableTargets();
    QVector<TargetAnchorInfo> targetAnchors() const;
    const TargetAnchorInfo *targetAnchorByRef(const QString &ref) const;
    void extractTargetAnchors();
    const KArchiveDirectory *rootDirectory() const;

    QString standardPage(EpubPageReference::StandardType type) const
    {
        return m_standardReferences.value(type).target;
    }

    QString filename() const
    {
        return m_filename;
    }

Q_SIGNALS:
    void errorOccured(const QString &error);

private:
    bool parseMimetype();
    bool parseContainer();
    bool parseContentFile(const QString &filepath);
    bool parseMetadataItem(const QDomNode &metadataNode, const QDomNodeList &nodeList);
    bool parseMetadataPropertyItem(const QDomElement &metadataElemenent, const QDomNodeList &nodeList);
    bool parseManifestItem(const QDomNode &manifestNodes, const QString &currentFolder);
    bool parseSpineItem(const QDomNode &spineNode);
    bool parseGuideItem(const QDomNode &guideItem, const QString &currentFolder);
    void extractNavigationTargetsFromDocument(const QString &itemId, const QString &path, QVector<TargetAnchorInfo> &targets, QSet<QString> &seenLocations);
    void extractNcxTargetsFromDocument(const QString &itemId, const QString &path, QVector<TargetAnchorInfo> &targets, QSet<QString> &seenLocations);
    void extractReferenceableTargetsFromDocument(const QString &itemId,
                                                 const QString &path,
                                                 QVector<TargetAnchorInfo> &targets,
                                                 QSet<QString> &seenLocations,
                                                 const QHash<QString, TargetAnchorInfo> &tocTargetsByLocation);
    void extractTargetAnchorsFromDocument(const QString &itemId, const QString &path);

    QString createRangeAnchor(const QString &cfi,
                              const QString &anchorType,
                              const QString &href,
                              const QString &debugLabel,
                              const QString &title = QString(),
                              bool createBackup = true);
    const KArchiveFile *file(const QString &path) const;
    QString itemIdForPath(const QString &path) const;
    QImage imageFromItem(const QString &id);
    QImage coverImageFromDocument(const QString &path);

    std::unique_ptr<KZip> m_archive;
    const KArchiveDirectory *m_rootFolder;

    QHash<QString, QStringList> m_metadata;
    QList<Collection> m_collections;

    QHash<QString, EpubItem> m_items;
    QStringList m_orderedItems;
    QSet<QString> m_unorderedItems;

    QHash<EpubPageReference::StandardType, EpubPageReference> m_standardReferences;
    QHash<QString, EpubPageReference> m_otherReferences;
    QString m_filename;
    QString m_contentFilePath;
    QMimeDatabase m_mimeDatabase;
    QVector<TargetAnchorInfo> m_targetAnchors;
    QHash<QString, TargetAnchorInfo> m_targetAnchorIndex;
    bool m_targetAnchorsExtracted = false;
};
