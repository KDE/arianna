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
#include <memory>

class KArchiveDirectory;
class KArchiveFile;
class QXmlStreamReader;

struct EpubItem {
    QString path;
    QByteArray mimetype;
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

    EpubItem epubItem(const QString &id) const
    {
        return m_items.value(id);
    }

    QSharedPointer<QIODevice> ioDevice(const QString &path);
    QImage image(const QString &id);
    QList<Collection> collections() const;
    QStringList metadata(const QStringView &key);
    QStringList items() const
    {
        return m_orderedItems;
    }

    QString standardPage(EpubPageReference::StandardType type) const
    {
        return m_standardReferences.value(type).target;
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
    bool parseGuideItem(const QDomNode &guideItem);

    const KArchiveFile *file(const QString &path);

    std::unique_ptr<KZip> m_archive;
    const KArchiveDirectory *m_rootFolder;

    QHash<QString, QStringList> m_metadata;
    QList<Collection> m_collections;

    QHash<QString, EpubItem> m_items;
    QStringList m_orderedItems;
    QSet<QString> m_unorderedItems;

    QHash<EpubPageReference::StandardType, EpubPageReference> m_standardReferences;
    QHash<QString, EpubPageReference> m_otherReferences;
    QMimeDatabase m_mimeDatabase;
};
