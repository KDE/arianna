// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "epubcontainer.h"

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KZip>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>

class EPubContainerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testUniqueIdentifierMetadata();
    void testDublinCoreMetadataNamespaces_data();
    void testDublinCoreMetadataNamespaces();
    void testUnprefixedDescriptionMetadata();
    void testCoverImageFromMetadataCover();
    void testCoverImageFromManifestProperty();
    void testCoverImageFromPercentEncodedManifestHref();
    void testCoverImageFromGuideCoverPage();
    void testCreateAnchorWritesActiveEpubAndBackup_data();
    void testCreateAnchorWritesActiveEpubAndBackup();
    void testCreateAnchorWrapsInlineSiblingRange();
    void testCreateAnchorRejectsBlockRange();
    void testCreateAnnotationAnchoredEpubAnnotatesWholeInlineElement();
    void testCreateAnnotationAnchoredEpubWrapsTextRange();
    void testCreateAnnotationAnchoredEpubCreatesBoundaryPair();
    void testRestoreRemarkAcrossLinkedHeading();
    void testCreateBookrefAnchorCreatesIntraHref();
    void testCreateBookrefAnchorCreatesCrossrefTitle();
    void testCreateBookrefAnchorWrapsPartialInlineRange_data();
    void testCreateBookrefAnchorWrapsPartialInlineRange();
    void testCreateBookrefAnchorRejectsUnsafeInlineRange_data();
    void testCreateBookrefAnchorRejectsUnsafeInlineRange();
    void testUpdateReferenceAnchorTitleWritesCrossrefTitle_data();
    void testUpdateReferenceAnchorTitleWritesCrossrefTitle();
    void testDeleteReferenceAnchorUnwrapsCrossrefAndWritesBackup_data();
    void testDeleteReferenceAnchorUnwrapsCrossrefAndWritesBackup();
    void testDeleteTargetRangeAnchorUnwrapsGeneratedAnchor();
    void testDeleteTargetRangeAnchorKeepsNonGeneratedTarget();
    void testSetImageNotInverseWritesClass_data();
    void testSetImageNotInverseWritesClass();
    void testSetSvgImageNotInverse_data();
    void testSetSvgImageNotInverse();
    void testNormalizeForAriannaFixesEmptyTitleAndScriptedProperty();
    void testUpdateCreatorsWritesDcCreatorMetadata();
    void testUpdateSubjectsWritesDcSubjectMetadata();
    void testUpdateSubjectsWritesHierarchicalMetadata();
    void testUpdateSubjectsKeepsPackageNamespaceValid();
    void testFileHashMatchesEpubBytes();
    void testContentHashIsStableAcrossZipEntryOrder();
    void testCreateTargetAnchorIsReferenceable();
    void testServerReadyEpubCanIncludeReferencesDocument_data();
    void testServerReadyEpubCanIncludeReferencesDocument();
    void testReferenceableTargetsIncludeDocumentsAndElementIds();
};

#ifndef Q_MOC_RUN

static QByteArray testPngData()
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::red);

    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}

static void writeContainer(KZip &zip)
{
    zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/package.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)"));
}

static QByteArray hashTestPackage()
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">bookid</dc:identifier>
    <dc:title>Hash Test</dc:title>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
    <item id="style" href="style.css" media-type="text/css"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)");
}

static QByteArray hashTestChapter(const QByteArray &bodyText)
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>)")
        + bodyText + QByteArray(R"(</p></body></html>)");
}

static bool writeHashTestEpub(const QString &epubPath, bool reverseOrder, const QByteArray &bodyText)
{
    KZip zip(epubPath);
    if (!zip.open(QIODevice::WriteOnly)) {
        return false;
    }

    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));

    if (reverseOrder) {
        zip.writeFile(QStringLiteral("OEBPS/style.css"), QByteArrayLiteral("p { font-style: italic; }\n"));
        zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), hashTestChapter(bodyText));
        zip.writeFile(QStringLiteral("OEBPS/package.opf"), hashTestPackage());
        writeContainer(zip);
    } else {
        writeContainer(zip);
        zip.writeFile(QStringLiteral("OEBPS/package.opf"), hashTestPackage());
        zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), hashTestChapter(bodyText));
        zip.writeFile(QStringLiteral("OEBPS/style.css"), QByteArrayLiteral("p { font-style: italic; }\n"));
    }

    return zip.close();
}

void EPubContainerTest::testUniqueIdentifierMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("book.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="unique-identifier" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="unique-identifier">040d189b-0546-410e-a09d-c0c98be894aa</dc:identifier>
    <dc:identifier>urn:isbn:9783161484100</dc:identifier>
    <dc:title>Identifier Test</dc:title>
  </metadata>
</package>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    QStringList expectedIdentifiers;
    expectedIdentifiers << QStringLiteral("040d189b-0546-410e-a09d-c0c98be894aa") << QStringLiteral("urn:isbn:9783161484100");

    QCOMPARE(epub.metadata(QStringLiteral("identifiers")), expectedIdentifiers);
    QCOMPARE(epub.metadata(QStringLiteral("identifier")), expectedIdentifiers);
    QCOMPARE(epub.metadata(QStringLiteral("unique-identifier")).value(0), QStringLiteral("040d189b-0546-410e-a09d-c0c98be894aa"));
}

void EPubContainerTest::testDublinCoreMetadataNamespaces_data()
{
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("namespaceUri");
    QTest::addColumn<bool>("accepted");
    const QString dc = QStringLiteral("http://purl.org/dc/elements/1.1/");
    QTest::newRow("dc-prefix") << QStringLiteral("dc") << dc << true;
    QTest::newRow("alternative-prefix") << QStringLiteral("dublin") << dc << true;
    QTest::newRow("default-namespace") << QString() << dc << true;
    QTest::newRow("dc-prefix-wrong-namespace") << QStringLiteral("dc") << QStringLiteral("urn:other-metadata") << false;
    QTest::newRow("opf-namespace") << QString() << QStringLiteral("http://www.idpf.org/2007/opf") << false;
    QTest::newRow("no-namespace") << QString() << QString() << false;
}

void EPubContainerTest::testDublinCoreMetadataNamespaces()
{
    QFETCH(QString, prefix);
    QFETCH(QString, namespaceUri);
    QFETCH(bool, accepted);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("namespaces.epub"));
    const QString qualifiedPrefix = prefix.isEmpty() ? QString() : prefix + QLatin1Char(':');
    const QString declaration = prefix.isEmpty() ? QStringLiteral("xmlns") : QStringLiteral("xmlns:") + prefix;
    const QByteArray package = QString::fromLatin1(R"(
<package xmlns="http://www.idpf.org/2007/opf" xmlns:opf="http://www.idpf.org/2007/opf" unique-identifier="primary" version="3.0">
  <opf:metadata %1="%2">
    <%3identifier id="secondary">urn:isbn:9783161484100</%3identifier>
    <%3identifier id="primary">namespace-book</%3identifier>
    <%3title>Namespace Test</%3title>
    <%3language>de</%3language>
    <%3publisher>Test Publisher</%3publisher>
    <%3source>Test Source</%3source>
    <%3creator>Test Author</%3creator>
    <%3subject>Test Subject</%3subject>
  </opf:metadata>
</package>)").arg(declaration, namespaceUri, qualifiedPrefix).toUtf8();
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
    writeContainer(zip);
    QVERIFY(zip.writeFile(QStringLiteral("OEBPS/package.opf"), package));
    QVERIFY(zip.close());
    QFile original(epubPath);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto originalBytes = original.readAll();
    original.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    const QMap<QString, QStringList> expected{
        {QStringLiteral("unique-identifier"), {QStringLiteral("namespace-book")}},
        {QStringLiteral("identifiers"), {QStringLiteral("urn:isbn:9783161484100"), QStringLiteral("namespace-book")}},
        {QStringLiteral("title"), {QStringLiteral("Namespace Test")}},
        {QStringLiteral("language"), {QStringLiteral("de")}},
        {QStringLiteral("publisher"), {QStringLiteral("Test Publisher")}},
        {QStringLiteral("source"), {QStringLiteral("Test Source")}},
        {QStringLiteral("creator"), {QStringLiteral("Test Author")}},
        {QStringLiteral("subject"), {QStringLiteral("Test Subject")}},
    };
    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        QCOMPARE(epub.metadata(it.key()), accepted ? it.value() : QStringList());
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), originalBytes);
}

void EPubContainerTest::testUnprefixedDescriptionMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("description.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">description-book</dc:identifier>
    <dc:title>Description Test</dc:title>
    <description>Plain OPF description</description>
  </metadata>
</package>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    QCOMPARE(epub.metadata(QStringLiteral("description")), QStringList{QStringLiteral("Plain OPF description")});
}

void EPubContainerTest::testCoverImageFromMetadataCover()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("metadata-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Metadata Cover Test</dc:title>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
  </manifest>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/cover.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCoverImageFromManifestProperty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("manifest-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Manifest Cover Test</dc:title>
  </metadata>
  <manifest>
    <item id="cover" href="images/cover.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/cover.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCoverImageFromPercentEncodedManifestHref()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("encoded-manifest-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Encoded Manifest Cover Test</dc:title>
  </metadata>
  <manifest>
    <item id="cover" href="Images/Blake%2C%20William%20-%20Complete%20Poems%20%28Penguin%2C%202004%29.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/Images/Blake, William - Complete Poems (Penguin, 2004).png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QCOMPARE(epub.epubItem(QStringLiteral("cover")).path, QStringLiteral("OEBPS/Images/Blake, William - Complete Poems (Penguin, 2004).png"));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCoverImageFromGuideCoverPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("guide-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Guide Cover Test</dc:title>
  </metadata>
  <manifest>
    <item id="cover-page" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
  </manifest>
  <guide>
    <reference type="cover" href="cover.xhtml"/>
  </guide>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/cover.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <body><img src="images/cover.png"/></body>
</html>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/cover.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCreateAnchorWritesActiveEpubAndBackup_data()
{
    QTest::addColumn<bool>("createBackup");
    QTest::newRow("with safety backup") << true;
    QTest::newRow("without safety backup") << false;
}

void EPubContainerTest::testCreateAnchorWritesActiveEpubAndBackup()
{
    QFETCH(bool, createBackup);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello anchor world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:12)"), QStringLiteral("crossref"), createBackup);
    QVERIFY(!anchorId.isEmpty());
    QVERIFY(anchorId.startsWith(QStringLiteral("uuid_")));

    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
    const QString backupPath = dir.filePath(QStringLiteral("source.before_anchoring_%1.epub").arg(anchorId));
    QCOMPARE(QFileInfo::exists(backupPath), createBackup);
    QCOMPARE(QFileInfo::exists(backupPath + QStringLiteral(".arianna.json")), createBackup);

    KZip anchoredZip(epubPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral(">anchor</span>")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);

    if (!createBackup)
        return;
    KZip backupZip(backupPath);
    QVERIFY(backupZip.open(QIODevice::ReadOnly));
    const KArchiveFile *backupChapter = backupZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(backupChapter);
    QScopedPointer<QIODevice> backupDevice(backupChapter->createDevice());
    QVERIFY(backupDevice);
    const QString backupDocument = QString::fromUtf8(backupDevice->readAll());
    QVERIFY(!backupDocument.contains(anchorId));
}

void EPubContainerTest::testCreateAnchorWrapsInlineSiblingRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("inline-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Inline Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body epub:type="bodymatter"><p><span class="calibre17">Srimad-Bhagavatam,</span> Third Canto, 29th Chapter, 10th verse</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/2/1:0,/3:12)"));
    QVERIFY(!anchorId.isEmpty());

    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("inline-source.before_anchoring_%1.epub").arg(anchorId))));

    KZip anchoredZip(epubPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral("<span")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
    QVERIFY(document.contains(QStringLiteral("xmlns:epub=\"http://www.idpf.org/2007/ops\"")));
    QVERIFY(document.contains(QStringLiteral("epub:type=\"bodymatter\"")));

    const qsizetype anchorIndex = document.indexOf(QStringLiteral("id=\"") + anchorId + QStringLiteral("\""));
    const qsizetype spanTextIndex = document.indexOf(QStringLiteral("Srimad-Bhagavatam,"));
    const qsizetype followingTextIndex = document.indexOf(QStringLiteral("Third Canto"));
    const qsizetype anchorEndIndex = document.indexOf(QStringLiteral("</span>"), followingTextIndex);
    QVERIFY(anchorIndex >= 0);
    QVERIFY(anchorIndex < spanTextIndex);
    QVERIFY(spanTextIndex < followingTextIndex);
    QVERIFY(followingTextIndex < anchorEndIndex);
    QVERIFY(document.indexOf(QStringLiteral(", 29th Chapter"), anchorEndIndex) > anchorEndIndex);
}

void EPubContainerTest::testCreateAnchorRejectsBlockRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("block-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Block Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>First paragraph</p><p>Second paragraph</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2,/2/1:6,/4/1:6)"));
    QVERIFY(anchorId.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
}

void EPubContainerTest::testCreateAnnotationAnchoredEpubAnnotatesWholeInlineElement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("annotation-inline-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Annotation Inline Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p><span class="term">Bhakti</span> devotion</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = QStringLiteral("uuid_test_annotation_inline");
    const QByteArray anchoredEpub = epub.createAnnotationAnchoredEpub(QStringLiteral("epubcfi(/6/2!/2/2,/2/1:0,/2/1:6)"), anchorId);
    QVERIFY(!anchoredEpub.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));

    QBuffer buffer;
    buffer.setData(anchoredEpub);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip anchoredZip(&buffer);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("class=\"term\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation\"")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_begin")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_end")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateAnnotationAnchoredEpubWrapsTextRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("annotation-text-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Annotation Text Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello annotation world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = QStringLiteral("uuid_test_annotation_text");
    const QByteArray anchoredEpub = epub.createAnnotationAnchoredEpub(QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:16)"), anchorId);
    QVERIFY(!anchoredEpub.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));

    QBuffer buffer;
    buffer.setData(anchoredEpub);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip anchoredZip(&buffer);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation\"")));
    QVERIFY(document.contains(QStringLiteral(">annotation</span>")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_begin")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_end")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateAnnotationAnchoredEpubCreatesBoundaryPair()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("annotation-pair-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Annotation Pair Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello <em>annotation</em> world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = QStringLiteral("uuid_test_annotation_pair");
    const QByteArray anchoredEpub = epub.createAnnotationAnchoredEpub(QStringLiteral("epubcfi(/6/2!/2/2,/1:3,/3:4)"), anchorId);
    QVERIFY(!anchoredEpub.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));

    QBuffer buffer;
    buffer.setData(anchoredEpub);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip anchoredZip(&buffer);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    const QString beginId = anchorId + QStringLiteral("_begin");
    const QString endId = anchorId + QStringLiteral("_end");
    QVERIFY(document.contains(QStringLiteral("id=\"") + beginId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("id=\"") + endId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation-begin\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation-end\"")));
    QVERIFY(document.indexOf(beginId) < document.indexOf(QStringLiteral("<em>annotation</em>")));
    QVERIFY(document.indexOf(QStringLiteral("<em>annotation</em>")) < document.indexOf(endId));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testRestoreRemarkAcrossLinkedHeading()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("heading.epub"));
    KZip zip(path);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")));
    writeContainer(zip);
    QVERIFY(zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Linked heading</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)")));
    QVERIFY(zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Title</title></head><body><h2><a href="Contents.html#rChap33" id="Chap33">O<small>N THE</small> N<small>UMBER OF</small> P<small>RIME</small> N<small>UMBERS LESS THAN A</small> G<small>IVEN</small> Q<small>UANTITY</small></a><a href="Chapter33.html#rChap33_ufn01" id="Chap33_ufn01">*</a></h2></body></html>)")));
    QVERIFY(zip.close());
    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(path));
    const auto cfi = QStringLiteral("epubcfi(/6/2!/4/2,/2[Chap33]/1:0,/4[Chap33_ufn01]/1:1)");
    const auto id = QStringLiteral("uuid_01a0f37a-34f2-76f7-94d8-4d12226703af");
    const auto expected = QStringLiteral("ON THE NUMBER OF PRIME NUMBERS LESS THAN A GIVEN QUANTITY*");
    QVERIFY(epub.createAnnotationAnchoredEpub(cfi, id, QStringLiteral("Wrong selection")).isEmpty());
    const auto result = epub.createAnnotationAnchoredEpub(cfi, id, expected);
    QVERIFY(!result.isEmpty());
    QBuffer buffer;
    buffer.setData(result);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip output(&buffer);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const auto *chapter = output.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);
    QScopedPointer<QIODevice> reader(chapter->createDevice());
    const auto html = QString::fromUtf8(reader->readAll());
    QVERIFY(html.contains(QStringLiteral("id=\"") + id + QStringLiteral("_begin\"")));
    QVERIFY(html.contains(QStringLiteral("id=\"") + id + QStringLiteral("_end\"")));
    QVERIFY(html.contains(QStringLiteral("href=\"Contents.html#rChap33\"")));
    QVERIFY(html.contains(QStringLiteral("href=\"Chapter33.html#rChap33_ufn01\"")));
    QDomDocument document;
    QVERIFY(document.setContent(html));
    QCOMPARE(document.elementsByTagName(QStringLiteral("a")).size(), 2);
}

void EPubContainerTest::testCreateBookrefAnchorCreatesIntraHref()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("intra-bookref-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Intra Bookref Test</dc:title></metadata><manifest><item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/><item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter1"/><itemref idref="chapter2"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter1.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>See target now</p></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter2.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p id="target">Target paragraph</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createBookrefAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:4,/1:10)"), QStringLiteral("OEBPS/chapter2.xhtml#target"), false);
    QVERIFY(!anchorId.isEmpty());

    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("intra-bookref-source.before_anchoring_%1.epub").arg(anchorId))));

    KZip anchoredZip(epubPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter1.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("href=\"chapter2.xhtml#target\"")));
    QVERIFY(!document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral(">target</a>")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateBookrefAnchorCreatesCrossrefTitle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("cross-bookref-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Cross Bookref Test</dc:title></metadata><manifest><item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter1"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter1.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>See target now</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createBookrefAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:4,/1:10)"),
                                                      QStringLiteral("other-book.xhtml#target"),
                                                      true,
                                                      QStringLiteral("S.B. Vers 1.1.1"));
    QVERIFY(!anchorId.isEmpty());

    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("cross-bookref-source.before_anchoring_%1.epub").arg(anchorId))));

    KZip anchoredZip(epubPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter1.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral("title=\"S.B. Vers 1.1.1\"")));
    QVERIFY(!document.contains(QStringLiteral("href=\"other-book.xhtml#target\"")));
    QVERIFY(document.contains(QStringLiteral(">target</span>")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateBookrefAnchorWrapsPartialInlineRange_data()
{
    QTest::addColumn<QString>("content");
    QTest::addColumn<QString>("range");
    QTest::addColumn<QString>("expectedContent");

    QTest::newRow("reported-bs-5.33")
        << QStringLiteral("Before <span class=\"italic\">advaitam acyutam anādim ananta-rūpam [Bs </span>5.33] after") << QStringLiteral("/2/1:38,/3:4")
        << QStringLiteral("Before <span class=\"italic\">advaitam acyutam anādim ananta-rūpam [</span><span><span class=\"italic\">Bs </span>5.33</span>] after");
    QTest::newRow("partial-end") << QStringLiteral("Before Bs <em class=\"italic\">5.33 after</em> end") << QStringLiteral("/1:7,/2/1:4")
                                 << QStringLiteral("Before <span>Bs <em class=\"italic\">5.33</em></span><em class=\"italic\"> after</em> end");
    QTest::newRow("nested-partial-ends")
        << QStringLiteral("Before <em><strong>left Bs</strong></em> <span><i>5.33 right</i></span> after") << QStringLiteral("/2/2/1:5,/4/2/1:4")
        << QStringLiteral("Before <em><strong>left </strong></em><span><em><strong>Bs</strong></em> <span><i>5.33</i></span></span><span><i> right</i></span> after");
    QTest::newRow("shared-inline-parent") << QStringLiteral("Before <span class=\"outer\"><i>Bs</i> 5.33 after</span> end") << QStringLiteral("/2/2/1:0,/2/3:5")
                                          << QStringLiteral("Before <span class=\"outer\"><span><i>Bs</i> 5.33</span> after</span> end");
    QTest::newRow("keep-ids-on-leading-fragments")
        << QStringLiteral(
               "Before <span id=\"left\" xml:id=\"xml-left\" lang=\"sa\" class=\"italic\">left Bs</span> "
               "<span id=\"right\" xml:id=\"xml-right\" lang=\"en\" class=\"bold\">5.33 right</span> after")
        << QStringLiteral("/2/1:5,/4/1:4")
        << QStringLiteral(
               "Before <span id=\"left\" xml:id=\"xml-left\" lang=\"sa\" class=\"italic\">left </span>"
               "<span><span lang=\"sa\" class=\"italic\">Bs</span> <span id=\"right\" xml:id=\"xml-right\" lang=\"en\" class=\"bold\">5.33</span></span>"
               "<span lang=\"en\" class=\"bold\"> right</span> after");
}

void EPubContainerTest::testCreateBookrefAnchorWrapsPartialInlineRange()
{
    QFETCH(QString, content);
    QFETCH(QString, range);
    QFETCH(QString, expectedContent);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("partial-inline.epub"));
    const QByteArray originalChapter = hashTestChapter(content.toUtf8());
    QVERIFY(writeHashTestEpub(epubPath, false, content.toUtf8()));

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    const QString anchorId =
        epub.createBookrefAnchor(QStringLiteral("epubcfi(/6/2!/2/2,%1)").arg(range), QStringLiteral("text_5.33.xhtml"), true, QStringLiteral("B.S. Vers 5.33"));
    QVERIFY(!anchorId.isEmpty());

    KZip anchoredZip(epubPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);
    QDomDocument document;
    const auto parseOptions =
        QDomDocument::ParseOptions(QDomDocument::ParseOption::UseNamespaceProcessing) | QDomDocument::ParseOption::PreserveSpacingOnlyNodes;
    QVERIFY(document.setContent(chapter->data(), parseOptions));
    const QDomNodeList elements = document.elementsByTagName(QStringLiteral("span"));
    QDomElement anchor;
    for (int i = 0; i < elements.size(); ++i) {
        const auto element = elements.at(i).toElement();
        if (element.attribute(QStringLiteral("id")) == anchorId) {
            QVERIFY(anchor.isNull());
            anchor = element;
        }
    }
    QVERIFY(!anchor.isNull());
    QCOMPARE(document.elementsByTagName(QStringLiteral("a")).size(), 0);
    QCOMPARE(anchor.attribute(QStringLiteral("id")), anchorId);
    QCOMPARE(anchor.attribute(QStringLiteral("data-role")), QStringLiteral("anchor"));
    QCOMPARE(anchor.attribute(QStringLiteral("data-anchor-type")), QStringLiteral("crossref"));
    QCOMPARE(anchor.attribute(QStringLiteral("title")), QStringLiteral("B.S. Vers 5.33"));
    QCOMPARE(anchor.text(), QStringLiteral("Bs 5.33"));
    QVERIFY(!anchor.hasAttribute(QStringLiteral("href")));
    anchor.removeAttribute(QStringLiteral("id"));
    anchor.removeAttribute(QStringLiteral("data-role"));
    anchor.removeAttribute(QStringLiteral("data-anchor-type"));
    anchor.removeAttribute(QStringLiteral("title"));

    QDomDocument expected;
    QVERIFY(expected.setContent(hashTestChapter(expectedContent.toUtf8()), parseOptions));
    const QDomElement paragraph = document.elementsByTagName(QStringLiteral("p")).at(0).toElement();
    const QDomElement expectedParagraph = expected.elementsByTagName(QStringLiteral("p")).at(0).toElement();
    const auto attributes = [](const QDomNode &node) {
        QMap<QString, QPair<QString, QString>> result;
        for (int i = 0; i < node.attributes().size(); ++i) {
            const QDomNode attribute = node.attributes().item(i);
            result.insert(attribute.nodeName(), qMakePair(attribute.namespaceURI(), attribute.nodeValue()));
        }
        return result;
    };
    const auto compareNodes = [&attributes](const auto &self, const QDomNode &actualNode, const QDomNode &expectedNode) -> void {
        QCOMPARE(actualNode.nodeType(), expectedNode.nodeType());
        QCOMPARE(actualNode.nodeName(), expectedNode.nodeName());
        QCOMPARE(actualNode.namespaceURI(), expectedNode.namespaceURI());
        QCOMPARE(actualNode.nodeValue(), expectedNode.nodeValue());
        QCOMPARE(actualNode.attributes().size(), expectedNode.attributes().size());
        QCOMPARE(attributes(actualNode), attributes(expectedNode));
        QCOMPARE(actualNode.childNodes().size(), expectedNode.childNodes().size());
        for (int i = 0; i < expectedNode.childNodes().size(); ++i) {
            self(self, actualNode.childNodes().at(i), expectedNode.childNodes().at(i));
        }
    };
    compareNodes(compareNodes, paragraph, expectedParagraph);

    KZip backupZip(dir.filePath(QStringLiteral("partial-inline.before_anchoring_%1.epub").arg(anchorId)));
    QVERIFY(backupZip.open(QIODevice::ReadOnly));
    const KArchiveFile *backupChapter = backupZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(backupChapter);
    QCOMPARE(backupChapter->data(), originalChapter);
}

void EPubContainerTest::testCreateBookrefAnchorRejectsUnsafeInlineRange_data()
{
    QTest::addColumn<QString>("content");
    QTest::addColumn<QString>("range");

    QTest::newRow("starts-in-existing-link") << QStringLiteral("Before <a href=\"other.xhtml\">left Bs </a>5.33 after") << QStringLiteral("/2/1:5,/3:4");
    QTest::newRow("contains-existing-link") << QStringLiteral("Before Bs <a href=\"other.xhtml\">5.</a>33 after") << QStringLiteral("/1:7,/3:2");
    QTest::newRow("inside-existing-link") << QStringLiteral("Before <a href=\"other.xhtml\"><i>Bs</i> 5.33 after</a> end") << QStringLiteral("/2/2/1:0,/2/3:5");
    QTest::newRow("reversed-inline-range") << QStringLiteral("Before <i>Bs</i> 5.33 after") << QStringLiteral("/3:5,/2/1:0");
    QTest::newRow("stale-cfi-after-formatting-edit")
        << QStringLiteral("Before <span class=\"italic\">advaitam acyutam anādim ananta-rūpam</span> [Bs 5.33] after") << QStringLiteral("/2/1:38,/3:4");
}

void EPubContainerTest::testCreateBookrefAnchorRejectsUnsafeInlineRange()
{
    QFETCH(QString, content);
    QFETCH(QString, range);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString epubPath = dir.filePath(QStringLiteral("unsafe-inline.epub"));
    QVERIFY(writeHashTestEpub(epubPath, false, content.toUtf8()));
    QFile originalFile(epubPath);
    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = originalFile.readAll();
    originalFile.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(epub.createBookrefAnchor(QStringLiteral("epubcfi(/6/2!/2/2,%1)").arg(range), QStringLiteral("text_5.33.xhtml"), true).isEmpty());

    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    QCOMPARE(originalFile.readAll(), originalBytes);
}

void EPubContainerTest::testUpdateReferenceAnchorTitleWritesCrossrefTitle_data()
{
    QTest::addColumn<QByteArray>("sourceTag");
    QTest::addColumn<bool>("crossReference");
    QTest::newRow("existing-a-to-internal") << QByteArrayLiteral("a") << false;
    QTest::newRow("span-to-internal") << QByteArrayLiteral("span") << false;
    QTest::newRow("prefixed-span-to-internal") << QByteArrayLiteral("xhtml:span") << false;
    QTest::newRow("existing-a-title") << QByteArrayLiteral("a") << true;
    QTest::newRow("span-title") << QByteArrayLiteral("span") << true;
}

void EPubContainerTest::testUpdateReferenceAnchorTitleWritesCrossrefTitle()
{
    QFETCH(QByteArray, sourceTag);
    QFETCH(bool, crossReference);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("update-reference-title.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Update Reference Title Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:xhtml="http://www.w3.org/1999/xhtml"><body><p>See <%1 id="source-ref" data-role="anchor" data-anchor-type="crossref" title="Old title">reference text</%1> now</p></body></html>)").arg(QString::fromLatin1(sourceTag)).toUtf8());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    bool changed = false;
    QVERIFY(epub.updateReferenceAnchor(QStringLiteral("source-ref"),
                                       QStringLiteral("OEBPS/chapter.xhtml#new-target"),
                                       crossReference,
                                       QStringLiteral("New title"),
                                       &changed));
    QVERIFY(changed);

    const QString backupPath = dir.filePath(QStringLiteral("update-reference-title.before.reference-title-update_source-ref.epub"));
    QVERIFY(QFileInfo::exists(backupPath));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));

    KZip activeZip(epubPath);
    QVERIFY(activeZip.open(QIODevice::ReadOnly));
    const KArchiveFile *activeChapter = activeZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(activeChapter);
    QScopedPointer<QIODevice> activeDevice(activeChapter->createDevice());
    QVERIFY(activeDevice);
    const QString activeDocument = QString::fromUtf8(activeDevice->readAll());
    QVERIFY(activeDocument.contains(QStringLiteral("id=\"source-ref\"")));
    QCOMPARE(activeDocument.contains(QStringLiteral("href=\"chapter.xhtml#new-target\"")), !crossReference);
    QVERIFY(activeDocument.contains(QStringLiteral("title=\"New title\"")));
    QCOMPARE(activeDocument.contains(QStringLiteral("data-anchor-type=\"crossref\"")), crossReference);
    QVERIFY(!activeDocument.contains(QStringLiteral("title=\"Old title\"")));
    QDomDocument activeDom;
    QVERIFY(activeDom.setContent(activeDocument, QDomDocument::ParseOption::UseNamespaceProcessing));
    const auto anchor = activeDom.elementsByTagName(QStringLiteral("p")).at(0).firstChildElement();
    QCOMPARE(anchor.namespaceURI(), QStringLiteral("http://www.w3.org/1999/xhtml"));
    QCOMPARE(anchor.localName(), crossReference ? QString::fromLatin1(sourceTag) : QStringLiteral("a"));
    QCOMPARE(anchor.text(), QStringLiteral("reference text"));

    KZip backupZip(backupPath);
    QVERIFY(backupZip.open(QIODevice::ReadOnly));
    const KArchiveFile *backupChapter = backupZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(backupChapter);
    QScopedPointer<QIODevice> backupDevice(backupChapter->createDevice());
    QVERIFY(backupDevice);
    const QString backupDocument = QString::fromUtf8(backupDevice->readAll());
    QVERIFY(backupDocument.contains(QStringLiteral("title=\"Old title\"")));
}

void EPubContainerTest::testDeleteReferenceAnchorUnwrapsCrossrefAndWritesBackup_data()
{
    QTest::addColumn<QByteArray>("sourceTag");
    QTest::newRow("existing-a") << QByteArrayLiteral("a");
    QTest::newRow("span") << QByteArrayLiteral("span");
}

void EPubContainerTest::testDeleteReferenceAnchorUnwrapsCrossrefAndWritesBackup()
{
    QFETCH(QByteArray, sourceTag);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("delete-reference-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Delete Reference Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>See <%1 id="source-ref" data-role="anchor" data-anchor-type="crossref" title="Bg. 9.26">Bg. 9.26</%1> now</p></body></html>)").arg(QString::fromLatin1(sourceTag)).toUtf8());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(epub.deleteReferenceAnchor(QStringLiteral("source-ref")));

    const QString backupPath = dir.filePath(QStringLiteral("delete-reference-source.before.ananchored_source-ref.epub"));
    QVERIFY(QFileInfo::exists(backupPath));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));

    KZip activeZip(epubPath);
    QVERIFY(activeZip.open(QIODevice::ReadOnly));
    const KArchiveFile *activeChapter = activeZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(activeChapter);
    QScopedPointer<QIODevice> activeDevice(activeChapter->createDevice());
    QVERIFY(activeDevice);
    const QString activeDocument = QString::fromUtf8(activeDevice->readAll());
    QVERIFY(activeDocument.contains(QStringLiteral("Bg. 9.26")));
    QVERIFY(!activeDocument.contains(QStringLiteral("id=\"source-ref\"")));
    QVERIFY(!activeDocument.contains(QStringLiteral("data-anchor-type=\"crossref\"")));

    KZip backupZip(backupPath);
    QVERIFY(backupZip.open(QIODevice::ReadOnly));
    const KArchiveFile *backupChapter = backupZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(backupChapter);
    QScopedPointer<QIODevice> backupDevice(backupChapter->createDevice());
    QVERIFY(backupDevice);
    const QString backupDocument = QString::fromUtf8(backupDevice->readAll());
    QVERIFY(backupDocument.contains(QStringLiteral("id=\"source-ref\"")));
    QVERIFY(backupDocument.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
}

void EPubContainerTest::testDeleteTargetRangeAnchorUnwrapsGeneratedAnchor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("delete-target-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Delete Target Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Target <a id="uuid_target_range" data-role="anchor">range text</a> end</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(epub.deleteTargetRangeAnchor(QStringLiteral("uuid_target_range")));

    const QString backupPath = dir.filePath(QStringLiteral("delete-target-source.before.ananchored_uuid_target_range.epub"));
    QVERIFY(QFileInfo::exists(backupPath));

    KZip activeZip(epubPath);
    QVERIFY(activeZip.open(QIODevice::ReadOnly));
    const KArchiveFile *activeChapter = activeZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(activeChapter);
    QScopedPointer<QIODevice> activeDevice(activeChapter->createDevice());
    QVERIFY(activeDevice);
    const QString activeDocument = QString::fromUtf8(activeDevice->readAll());
    QVERIFY(activeDocument.contains(QStringLiteral("range text")));
    QVERIFY(!activeDocument.contains(QStringLiteral("id=\"uuid_target_range\"")));
    QVERIFY(!activeDocument.contains(QStringLiteral("data-role=\"anchor\"")));
}

void EPubContainerTest::testDeleteTargetRangeAnchorKeepsNonGeneratedTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("keep-target-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Keep Target Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p id="section1">Section target</p><p><a id="manual-target" data-role="anchor">Manual target</a></p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(epub.deleteTargetRangeAnchor(QStringLiteral("section1")));
    QVERIFY(epub.deleteTargetRangeAnchor(QStringLiteral("manual-target")));

    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("keep-target-source.before.ananchored_section1.epub"))));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("keep-target-source.before.ananchored_manual-target.epub"))));

    KZip activeZip(epubPath);
    QVERIFY(activeZip.open(QIODevice::ReadOnly));
    const KArchiveFile *activeChapter = activeZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(activeChapter);
    QScopedPointer<QIODevice> activeDevice(activeChapter->createDevice());
    QVERIFY(activeDevice);
    const QString activeDocument = QString::fromUtf8(activeDevice->readAll());
    QVERIFY(activeDocument.contains(QStringLiteral("id=\"section1\"")));
    QVERIFY(activeDocument.contains(QStringLiteral("id=\"manual-target\"")));
}

void EPubContainerTest::testSetImageNotInverseWritesClass_data()
{
    QTest::addColumn<bool>("createBackup");
    QTest::newRow("default-no-backup") << false;
    QTest::newRow("backup-enabled") << true;
}

void EPubContainerTest::testSetImageNotInverseWritesClass()
{
    QFETCH(bool, createBackup);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("image-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Image Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="photo" href="images/photo.png" media-type="image/png"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Before <img id="photo" src="images/photo.png"/> after</p></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/photo.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    bool changed = false;
    const QString cfi = QStringLiteral("epubcfi(/6/2!/2,/2:1,/2:2)");
    QVERIFY(createBackup ? epub.setImageNotInverse(cfi, QStringLiteral("images/photo.png"), &changed, true)
                         : epub.setImageNotInverse(cfi, QStringLiteral("images/photo.png"), &changed));
    QVERIFY(changed);

    const QString backupPath = dir.filePath(QStringLiteral("image-source.before.image-not-inverse_photo.epub"));
    QCOMPARE(QFileInfo::exists(backupPath), createBackup);
    QCOMPARE(QFileInfo::exists(backupPath + QStringLiteral(".arianna.json")), createBackup);

    KZip activeZip(epubPath);
    QVERIFY(activeZip.open(QIODevice::ReadOnly));
    const KArchiveFile *activeChapter = activeZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(activeChapter);
    QScopedPointer<QIODevice> activeDevice(activeChapter->createDevice());
    QVERIFY(activeDevice);
    const QString activeDocument = QString::fromUtf8(activeDevice->readAll());
    QVERIFY(activeDocument.contains(QStringLiteral("<img")));
    QVERIFY(activeDocument.contains(QStringLiteral("id=\"photo\"")));
    QVERIFY(activeDocument.contains(QStringLiteral("class=\"not_inverse\"")));

    if (createBackup) {
        KZip backupZip(backupPath);
        QVERIFY(backupZip.open(QIODevice::ReadOnly));
        const KArchiveFile *backupChapter = backupZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
        QVERIFY(backupChapter);
        QScopedPointer<QIODevice> backupDevice(backupChapter->createDevice());
        QVERIFY(backupDevice);
        const QString backupDocument = QString::fromUtf8(backupDevice->readAll());
        QVERIFY(!backupDocument.contains(QStringLiteral("not_inverse")));
    } else {
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{QStringLiteral("image-source.epub")});
    }
}

void EPubContainerTest::testSetSvgImageNotInverse_data()
{
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("hrefAttribute");
    QTest::addColumn<QString>("cfi");
    QTest::addColumn<QString>("svgClass");
    QTest::addColumn<QString>("imageClass");
    QTest::addColumn<bool>("createBackup");
    const QString svgRange = QStringLiteral("epubcfi(/6/2!/2,/2:1,/2:2)");
    QTest::newRow("xlink-svg-selection") << QString() << QStringLiteral("xlink:href") << svgRange << QStringLiteral("cover") << QStringLiteral("picture") << true;
    QTest::newRow("svg2-image-selection") << QString() << QStringLiteral("href") << QStringLiteral("epubcfi(/6/2!/2/2,/2:0,/2:1)")
                                         << QStringLiteral("cover") << QStringLiteral("picture") << true;
    QTest::newRow("prefixed-svg-src-fallback") << QStringLiteral("s:") << QStringLiteral("xlink:href") << QStringLiteral("epubcfi(/6/2!/2)")
                                             << QStringLiteral("cover") << QStringLiteral("picture") << true;
    QTest::newRow("image-already-marked") << QString() << QStringLiteral("xlink:href") << svgRange << QStringLiteral("cover")
                                         << QStringLiteral("picture not_inverse") << true;
    QTest::newRow("svg-already-marked") << QString() << QStringLiteral("xlink:href") << svgRange << QStringLiteral("cover not_inverse")
                                       << QStringLiteral("picture") << true;
    QTest::newRow("both-already-marked") << QString() << QStringLiteral("xlink:href") << svgRange << QStringLiteral("cover not_inverse")
                                        << QStringLiteral("picture not_inverse") << true;
    QTest::newRow("svg-no-backup") << QString() << QStringLiteral("xlink:href") << svgRange << QStringLiteral("cover") << QStringLiteral("picture") << false;
}

void EPubContainerTest::testSetSvgImageNotInverse()
{
    QFETCH(QString, prefix);
    QFETCH(QString, hrefAttribute);
    QFETCH(QString, cfi);
    QFETCH(QString, svgClass);
    QFETCH(QString, imageClass);
    QFETCH(bool, createBackup);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString svgNamespace = QStringLiteral("http://www.w3.org/2000/svg");
    const QString xlinkNamespace = QStringLiteral("http://www.w3.org/1999/xlink");
    const QString namespaceAttribute = prefix.isEmpty() ? QStringLiteral("xmlns") : QStringLiteral("xmlns:s");
    const QByteArray body = QStringLiteral("Before <%1svg %2=\"%3\" xmlns:xlink=\"%4\" id=\"cover\" class=\"%5\" "
                                          "width=\"100%\" height=\"100%\" viewBox=\"0 0 1053 1499\" preserveAspectRatio=\"none\">"
                                          "<%1image id=\"photo\" class=\"%6\" width=\"1053\" height=\"1499\" %7=\"cover.jpeg\"/>"
                                          "</%1svg> after <img id=\"other\" src=\"other.jpeg\"/>")
                                .arg(prefix, namespaceAttribute, svgNamespace, xlinkNamespace, svgClass, imageClass, hrefAttribute).toUtf8();
    const QString epubPath = dir.filePath(QStringLiteral("svg-cover.epub"));
    QVERIFY(writeHashTestEpub(epubPath, false, body));
    QFile originalFile(epubPath);
    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = originalFile.readAll();
    originalFile.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    bool changed = false;
    QVERIFY(epub.setImageNotInverse(cfi, QStringLiteral("cover.jpeg"), &changed, createBackup));
    const bool expectedChange = !svgClass.contains(QStringLiteral("not_inverse")) || !imageClass.contains(QStringLiteral("not_inverse"));
    QCOMPARE(changed, expectedChange);

    KZip activeZip(epubPath);
    QVERIFY(activeZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = activeZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);
    QDomDocument document;
    QVERIFY(document.setContent(chapter->data(), QDomDocument::ParseOption::UseNamespaceProcessing));
    const QDomNodeList svgs = document.elementsByTagNameNS(svgNamespace, QStringLiteral("svg"));
    const QDomNodeList images = document.elementsByTagNameNS(svgNamespace, QStringLiteral("image"));
    QCOMPARE(svgs.size(), 1);
    QCOMPARE(images.size(), 1);
    const QDomElement svg = svgs.at(0).toElement();
    const QDomElement image = images.at(0).toElement();
    QCOMPARE(svg.attribute(QStringLiteral("class")), QStringLiteral("cover not_inverse"));
    QCOMPARE(image.attribute(QStringLiteral("class")), QStringLiteral("picture not_inverse"));
    QCOMPARE(svg.attribute(QStringLiteral("id")), QStringLiteral("cover"));
    QCOMPARE(svg.attribute(QStringLiteral("width")), QStringLiteral("100%"));
    QCOMPARE(svg.attribute(QStringLiteral("height")), QStringLiteral("100%"));
    QCOMPARE(svg.attribute(QStringLiteral("viewBox")), QStringLiteral("0 0 1053 1499"));
    QCOMPARE(svg.attribute(QStringLiteral("preserveAspectRatio")), QStringLiteral("none"));
    QCOMPARE(image.attribute(QStringLiteral("id")), QStringLiteral("photo"));
    QCOMPARE(image.attribute(QStringLiteral("width")), QStringLiteral("1053"));
    QCOMPARE(image.attribute(QStringLiteral("height")), QStringLiteral("1499"));
    QCOMPARE(hrefAttribute == QStringLiteral("href") ? image.attribute(hrefAttribute) : image.attributeNS(xlinkNamespace, QStringLiteral("href")),
             QStringLiteral("cover.jpeg"));
    QVERIFY(!document.elementsByTagName(QStringLiteral("img")).at(0).toElement().hasAttribute(QStringLiteral("class")));

    const QStringList backups = QDir(dir.path()).entryList({QStringLiteral("*.before.image-not-inverse_*.epub")}, QDir::Files);
    QCOMPARE(backups.size(), expectedChange && createBackup ? 1 : 0);
    if (expectedChange && createBackup) {
        QFile backup(dir.filePath(backups.constFirst()));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), originalBytes);
    }

    // Reapplying the action is a no-op, including when only one tag was marked before.
    EPubContainer reopened(nullptr);
    QVERIFY(reopened.openFile(epubPath));
    changed = true;
    QVERIFY(reopened.setImageNotInverse(cfi, QStringLiteral("cover.jpeg"), &changed, !createBackup));
    QVERIFY(!changed);
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.before.image-not-inverse_*.epub")}, QDir::Files), backups);
}

void EPubContainerTest::testNormalizeForAriannaFixesEmptyTitleAndScriptedProperty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("normalize-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">bookid</dc:identifier>
    <dc:title>Krishna Karnamrita</dc:title>
  </metadata>
  <manifest>
    <item id="chapter2" href="chapter_2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter2"/></spine>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter_2.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title></title><script>window.foo = true;</script></head>
  <body><p>Normalization test.</p></body>
</html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const EpubNormalizationResult result = epub.normalizeForArianna();
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.changed);
    QCOMPARE(result.emptyTitlesFixed, 1);
    QCOMPARE(result.scriptedPropertiesAdded, 1);

    KZip normalizedZip(epubPath);
    QVERIFY(normalizedZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = normalizedZip.directory()->file(QStringLiteral("OEBPS/chapter_2.xhtml"));
    QVERIFY(chapter);
    QScopedPointer<QIODevice> chapterDevice(chapter->createDevice());
    QVERIFY(chapterDevice);
    const QString chapterDocument = QString::fromUtf8(chapterDevice->readAll());
    QVERIFY(chapterDocument.contains(QStringLiteral("Krishna Karnamrita")));
    QVERIFY(!chapterDocument.contains(QStringLiteral("<title></title>")));

    const KArchiveFile *packageFile = normalizedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());
    QVERIFY(packageDocument.contains(QStringLiteral("properties=\"scripted\"")));

    EPubContainer normalized(nullptr);
    QVERIFY(normalized.openFile(epubPath));
    const EpubNormalizationResult second = normalized.normalizeForArianna();
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QVERIFY(!second.changed);
    QCOMPARE(second.emptyTitlesFixed, 0);
    QCOMPARE(second.scriptedPropertiesAdded, 0);
}

void EPubContainerTest::testUpdateCreatorsWritesDcCreatorMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("creators.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">bookid</dc:identifier>
    <dc:title>Creator Test</dc:title>
    <dc:creator id="creator-old">Old Author</dc:creator>
    <meta refines="#creator-old" property="role" scheme="marc:relators">aut</meta>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Creator test.</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const EpubNormalizationResult result = epub.updateCreators({QStringLiteral("New Author"), QStringLiteral("Second Author"), QStringLiteral("new author")});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.changed);

    EPubContainer updated(nullptr);
    QVERIFY(updated.openFile(epubPath));
    QCOMPARE(updated.metadata(QStringLiteral("creator")), QStringList({QStringLiteral("New Author"), QStringLiteral("Second Author")}));

    KZip updatedZip(epubPath);
    QVERIFY(updatedZip.open(QIODevice::ReadOnly));
    const KArchiveFile *packageFile = updatedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());
    QVERIFY(!packageDocument.contains(QStringLiteral("Old Author")));
    QVERIFY(!packageDocument.contains(QStringLiteral("#creator-old")));

    const EpubNormalizationResult second = updated.updateCreators({QStringLiteral("New Author"), QStringLiteral("Second Author")});
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QVERIFY(!second.changed);
}

void EPubContainerTest::testUpdateSubjectsWritesDcSubjectMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("subjects.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">bookid</dc:identifier>
    <dc:title>Subject Test</dc:title>
    <dc:subject>Old Topic</dc:subject>
    <dc:subject>Fiction</dc:subject>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Subject test.</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const EpubNormalizationResult result = epub.updateSubjects({QStringLiteral("Philosophy"), QStringLiteral("History"), QStringLiteral("philosophy")});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.changed);

    EPubContainer updated(nullptr);
    QVERIFY(updated.openFile(epubPath));
    QCOMPARE(updated.metadata(QStringLiteral("subject")), QStringList({QStringLiteral("Philosophy"), QStringLiteral("History")}));

    KZip updatedZip(epubPath);
    QVERIFY(updatedZip.open(QIODevice::ReadOnly));
    const KArchiveFile *packageFile = updatedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());

    QDomDocument packageDom;
    QVERIFY(packageDom.setContent(packageDocument.toUtf8(), QDomDocument::ParseOption::UseNamespaceProcessing));
    const QDomNodeList subjectNodes = packageDom.elementsByTagNameNS(QStringLiteral("http://purl.org/dc/elements/1.1/"), QStringLiteral("subject"));
    QStringList packageSubjects;
    for (int i = 0; i < subjectNodes.count(); ++i) {
        packageSubjects.append(subjectNodes.at(i).toElement().text().trimmed());
    }
    QCOMPARE(packageSubjects, QStringList({QStringLiteral("Philosophy"), QStringLiteral("History")}));
    QVERIFY(!packageDocument.contains(QStringLiteral("Old Topic")));

    const EpubNormalizationResult second = updated.updateSubjects({QStringLiteral("Philosophy"), QStringLiteral("History")});
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QVERIFY(!second.changed);
}

void EPubContainerTest::testUpdateSubjectsWritesHierarchicalMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("hierarchical-subjects.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">bookid</dc:identifier>
    <dc:title>Hierarchical Subject Test</dc:title>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hierarchical subject test.</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const EpubNormalizationResult result = epub.updateSubjects({QStringLiteral("Bhakti Yoga/Sadhana Bhakti"), QStringLiteral("Bhakti Yoga/Raganuga Bhakti")});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.changed);

    EPubContainer updated(nullptr);
    QVERIFY(updated.openFile(epubPath));
    QCOMPARE(updated.metadata(QStringLiteral("subject")),
             QStringList({QStringLiteral("Bhakti Yoga"), QStringLiteral("Bhakti Yoga/Sadhana Bhakti"), QStringLiteral("Bhakti Yoga/Raganuga Bhakti")}));

    KZip updatedZip(epubPath);
    QVERIFY(updatedZip.open(QIODevice::ReadOnly));
    const KArchiveFile *packageFile = updatedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());
    QVERIFY(packageDocument.contains(QStringLiteral("arianna: https://kde.org/ns/arianna/epub/vocab/#")));
    QVERIFY(!packageDocument.contains(QStringLiteral("class=\"level_")));

    QDomDocument packageDom;
    QVERIFY(packageDom.setContent(packageDocument.toUtf8(), QDomDocument::ParseOption::UseNamespaceProcessing));
    const QDomNodeList metadataNodes = packageDom.elementsByTagName(QStringLiteral("metadata"));
    QVERIFY(!metadataNodes.isEmpty());
    const QDomNodeList metadataChildren = metadataNodes.at(0).childNodes();

    QHash<QString, QString> subjectIdsByLabel;
    QHash<QString, QString> authorityBySubjectId;
    QHash<QString, QString> termBySubjectId;
    QHash<QString, QString> levelBySubjectId;
    QHash<QString, QString> parentBySubjectId;

    for (int i = 0; i < metadataChildren.count(); ++i) {
        const QDomElement element = metadataChildren.at(i).toElement();
        if (element.isNull()) {
            continue;
        }

        if (element.localName() == QStringLiteral("subject") || element.tagName() == QStringLiteral("dc:subject")) {
            subjectIdsByLabel.insert(element.text().trimmed(), element.attribute(QStringLiteral("id")));
            continue;
        }

        if (element.tagName() != QStringLiteral("meta")) {
            continue;
        }

        const QString subjectId = element.attribute(QStringLiteral("refines")).mid(1);
        const QString property = element.attribute(QStringLiteral("property"));
        if (property == QStringLiteral("authority")) {
            authorityBySubjectId.insert(subjectId, element.text().trimmed());
        } else if (property == QStringLiteral("term")) {
            termBySubjectId.insert(subjectId, element.text().trimmed());
        } else if (property == QStringLiteral("arianna:subject-level")) {
            levelBySubjectId.insert(subjectId, element.text().trimmed());
        } else if (property == QStringLiteral("arianna:parent-subject")) {
            parentBySubjectId.insert(subjectId, element.text().trimmed());
        }
    }

    const QString bhaktiId = subjectIdsByLabel.value(QStringLiteral("Bhakti Yoga"));
    const QString sadhanaId = subjectIdsByLabel.value(QStringLiteral("Sadhana Bhakti"));
    const QString raganugaId = subjectIdsByLabel.value(QStringLiteral("Raganuga Bhakti"));
    QVERIFY(!bhaktiId.isEmpty());
    QVERIFY(!sadhanaId.isEmpty());
    QVERIFY(!raganugaId.isEmpty());

    QCOMPARE(authorityBySubjectId.value(bhaktiId), QStringLiteral("Arianna"));
    QCOMPARE(termBySubjectId.value(bhaktiId), QStringLiteral("Bhakti Yoga"));
    QCOMPARE(levelBySubjectId.value(bhaktiId), QStringLiteral("1"));
    QVERIFY(parentBySubjectId.value(bhaktiId).isEmpty());

    QCOMPARE(authorityBySubjectId.value(sadhanaId), QStringLiteral("Arianna"));
    QCOMPARE(termBySubjectId.value(sadhanaId), QStringLiteral("Bhakti Yoga/Sadhana Bhakti"));
    QCOMPARE(levelBySubjectId.value(sadhanaId), QStringLiteral("2"));
    QCOMPARE(parentBySubjectId.value(sadhanaId), bhaktiId);

    QCOMPARE(authorityBySubjectId.value(raganugaId), QStringLiteral("Arianna"));
    QCOMPARE(termBySubjectId.value(raganugaId), QStringLiteral("Bhakti Yoga/Raganuga Bhakti"));
    QCOMPARE(levelBySubjectId.value(raganugaId), QStringLiteral("2"));
    QCOMPARE(parentBySubjectId.value(raganugaId), bhaktiId);
}

void EPubContainerTest::testUpdateSubjectsKeepsPackageNamespaceValid()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("calibre-subjects.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package version="3.0" unique-identifier="uuid_id" prefix="calibre: https://calibre-ebook.com" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title id="id-1" xmlns:dc="http://purl.org/dc/elements/1.1/">The Nectar of Devotion</dc:title>
    <dc:creator id="id-2" xmlns:dc="http://purl.org/dc/elements/1.1/">His Divine Grace A. C. Bhaktivedanta Swami Prabhupada</dc:creator>
    <dc:identifier id="uuid_id" xmlns:dc="http://purl.org/dc/elements/1.1/">uuid:0872329c-506d-4546-b3b7-63f0b715147a</dc:identifier>
    <dc:language xmlns:dc="http://purl.org/dc/elements/1.1/">en</dc:language>
    <dc:subject xmlns:dc="http://purl.org/dc/elements/1.1/">Old Topic</dc:subject>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Subject namespace test.</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const EpubNormalizationResult result = epub.updateSubjects({QStringLiteral("Bhakti Yoga"), QStringLiteral("Spirituality")});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.changed);

    KZip updatedZip(epubPath);
    QVERIFY(updatedZip.open(QIODevice::ReadOnly));
    const KArchiveFile *packageFile = updatedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());

    const qsizetype packageTagStart = packageDocument.indexOf(QStringLiteral("<package"));
    QVERIFY(packageTagStart >= 0);
    const qsizetype packageTagEnd = packageDocument.indexOf(QLatin1Char('>'), packageTagStart);
    QVERIFY(packageTagEnd > 0);
    const QString packageTag = packageDocument.mid(packageTagStart, packageTagEnd - packageTagStart + 1);
    QCOMPARE(packageTag.count(QStringLiteral("xmlns=\"http://www.idpf.org/2007/opf\"")), 1);

    QDomDocument packageDom;
    QVERIFY(packageDom.setContent(packageDocument.toUtf8(), QDomDocument::ParseOption::UseNamespaceProcessing));
    const QDomNodeList subjectNodes = packageDom.elementsByTagNameNS(QStringLiteral("http://purl.org/dc/elements/1.1/"), QStringLiteral("subject"));
    QStringList packageSubjects;
    for (int i = 0; i < subjectNodes.count(); ++i) {
        packageSubjects.append(subjectNodes.at(i).toElement().text().trimmed());
    }
    QCOMPARE(packageSubjects, QStringList({QStringLiteral("Bhakti Yoga"), QStringLiteral("Spirituality")}));
}

void EPubContainerTest::testFileHashMatchesEpubBytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("hash-source.epub"));
    QVERIFY(writeHashTestEpub(epubPath, false, QByteArrayLiteral("Chain of truth")));

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    QFile file(epubPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString expectedHash = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());

    QCOMPARE(epub.getFileHash(), expectedHash);
    QCOMPARE(epub.getFileHash().size(), 64);
}

void EPubContainerTest::testContentHashIsStableAcrossZipEntryOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString firstPath = dir.filePath(QStringLiteral("hash-first.epub"));
    const QString secondPath = dir.filePath(QStringLiteral("hash-second.epub"));
    const QString changedPath = dir.filePath(QStringLiteral("hash-changed.epub"));
    QVERIFY(writeHashTestEpub(firstPath, false, QByteArrayLiteral("Same logical content")));
    QVERIFY(writeHashTestEpub(secondPath, true, QByteArrayLiteral("Same logical content")));
    QVERIFY(writeHashTestEpub(changedPath, true, QByteArrayLiteral("Changed logical content")));

    EPubContainer first(nullptr);
    QVERIFY(first.openFile(firstPath));
    EPubContainer second(nullptr);
    QVERIFY(second.openFile(secondPath));
    EPubContainer changed(nullptr);
    QVERIFY(changed.openFile(changedPath));

    const QString firstHash = first.getContentHash();
    QVERIFY(!firstHash.isEmpty());
    QCOMPARE(firstHash.size(), 64);
    QCOMPARE(second.getContentHash(), firstHash);
    QVERIFY(changed.getContentHash() != firstHash);
}

void EPubContainerTest::testCreateTargetAnchorIsReferenceable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("target-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Target Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello target world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:12)"), QString());
    QVERIFY(!anchorId.isEmpty());

    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("target-source.before_anchoring_%1.epub").arg(anchorId))));

    EPubContainer anchored(nullptr);
    QVERIFY(anchored.openFile(epubPath));
    anchored.extractTargetAnchors();

    const TargetAnchorInfo *anchor = anchored.targetAnchorByRef(anchorId);
    QVERIFY(anchor);
    QCOMPARE(anchor->location, QStringLiteral("OEBPS/chapter.xhtml#") + anchorId);
    QCOMPARE(anchor->previewHtml, QStringLiteral("target"));
    QCOMPARE(anchor->type, QStringLiteral("anchor"));

    KZip anchoredZip(epubPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());
    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(!document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
}

void EPubContainerTest::testServerReadyEpubCanIncludeReferencesDocument_data()
{
    QTest::addColumn<QByteArray>("sourceTag");
    QTest::newRow("existing-a") << QByteArrayLiteral("a");
    QTest::newRow("span") << QByteArrayLiteral("span");
    QTest::newRow("prefixed-span") << QByteArrayLiteral("xhtml:span");
}

void EPubContainerTest::testServerReadyEpubCanIncludeReferencesDocument()
{
    QFETCH(QByteArray, sourceTag);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("references-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Reference Include Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:xhtml="http://www.w3.org/1999/xhtml"><body><p><%1 id="source1" title="S.B. 4.4.1" data-role="anchor" data-anchor-type="crossref">S.B. 4.4.1</%1></p></body></html>)").arg(QString::fromLatin1(sourceTag)).toUtf8());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    EpubReference reference;
    reference.sourceAnchorId = QStringLiteral("source1");
    reference.sourceAnchorTitle = QStringLiteral("S.B. Vers 4.4.1");
    reference.targetBookId = QStringLiteral("target-book");
    reference.targetLocation = QStringLiteral("chapter.xhtml#target-anchor");
    reference.targetPreviewHtml = QStringLiteral("Target preview");

    ServerReadyEpubOptions options;
    options.includeReferences = true;
    options.references = {reference};

    const QByteArray data = epub.createServerReadyEpub(options);
    QVERIFY(!data.isEmpty());

    QBuffer buffer;
    buffer.setData(data);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip servedZip(&buffer);
    QVERIFY(servedZip.open(QIODevice::ReadOnly));

    const KArchiveFile *packageFile = servedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());
    QVERIFY(packageDocument.contains(QStringLiteral("references.xhtml")));
    QVERIFY(packageDocument.contains(QStringLiteral("idref=\"arianna-references\"")));

    const KArchiveFile *chapterFile = servedZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapterFile);
    QScopedPointer<QIODevice> chapterDevice(chapterFile->createDevice());
    QVERIFY(chapterDevice);
    const QString chapterDocument = QString::fromUtf8(chapterDevice->readAll());
    QVERIFY(chapterDocument.contains(QStringLiteral("href=\"references.xhtml#source1\"")));
    QDomDocument linkedDocument;
    QVERIFY(linkedDocument.setContent(chapterDocument, QDomDocument::ParseOption::UseNamespaceProcessing));
    const auto exportedLinks = linkedDocument.elementsByTagNameNS(QStringLiteral("http://www.w3.org/1999/xhtml"), QStringLiteral("a"));
    QCOMPARE(exportedLinks.size(), 1);
    QCOMPARE(exportedLinks.at(0).toElement().text(), QStringLiteral("S.B. 4.4.1"));
    QVERIFY(!epub.readData(QStringLiteral("OEBPS/chapter.xhtml")).contains("href="));

    const KArchiveFile *referencesFile = servedZip.directory()->file(QStringLiteral("OEBPS/references.xhtml"));
    QVERIFY(referencesFile);
    QScopedPointer<QIODevice> referencesDevice(referencesFile->createDevice());
    QVERIFY(referencesDevice);
    const QString referencesDocument = QString::fromUtf8(referencesDevice->readAll());
    QVERIFY(referencesDocument.contains(QStringLiteral("id=\"source1\"")));
    QVERIFY(referencesDocument.contains(QStringLiteral("S.B. Vers 4.4.1")));
    QVERIFY(referencesDocument.contains(QStringLiteral("target-book")));
    QVERIFY(referencesDocument.contains(QStringLiteral("Target preview")));
}

void EPubContainerTest::testReferenceableTargetsIncludeDocumentsAndElementIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("referenceable-targets.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Referenceable Targets Test</dc:title></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/><item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter1"/><itemref idref="chapter2"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/nav.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="chapter1.xhtml">First Chapter</a><ol><li><a href="chapter1.xhtml#section1">Section One</a></li></ol></li><li><a href="chapter2.xhtml">Second Chapter</a></li></ol></nav><nav epub:type="page-list"><ol><li><a href="chapter1.xhtml#page-list-only">Page 1</a></li></ol></nav></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter1.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><section id="section1"><h1>Section One</h1><p id="paragraph-id">First section text.</p><a id="target-anchor" data-role="anchor" data-anchor-type="target" title="Target Anchor">Target anchor text.</a></section><p>No id paragraph target.</p><a id="source-anchor" data-role="anchor" data-anchor-type="crossref">source</a></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter2.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><a name="named-target">Named Target</a><p>Second chapter.</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QVector<TargetAnchorInfo> targets = epub.referenceableTargets();
    auto findByLocation = [&targets](const QString &location) -> const TargetAnchorInfo * {
        for (const TargetAnchorInfo &target : targets) {
            if (target.location == location) {
                return &target;
            }
        }
        return nullptr;
    };
    auto indexOfLocation = [&targets](const QString &location) {
        for (qsizetype i = 0; i < targets.size(); ++i) {
            if (targets.at(i).location == location) {
                return i;
            }
        }
        return qsizetype(-1);
    };

    const TargetAnchorInfo *chapter = findByLocation(QStringLiteral("OEBPS/chapter1.xhtml"));
    QVERIFY(chapter);
    QCOMPARE(chapter->ref, QStringLiteral("OEBPS/chapter1.xhtml"));
    QCOMPARE(chapter->type, QStringLiteral("navigation"));
    QCOMPARE(chapter->title, QStringLiteral("First Chapter"));
    QCOMPARE(chapter->tocTitle, QStringLiteral("First Chapter"));
    QCOMPARE(chapter->tocDepth, 0);
    QCOMPARE(chapter->tocPath, QStringList{QStringLiteral("First Chapter")});

    const TargetAnchorInfo *section = findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#section1"));
    QVERIFY(section);
    QCOMPARE(section->ref, QStringLiteral("OEBPS/chapter1.xhtml#section1"));
    QCOMPARE(section->type, QStringLiteral("navigation"));
    QCOMPARE(section->title, QStringLiteral("Section One"));
    QCOMPARE(section->tocTitle, QStringLiteral("Section One"));
    QCOMPARE(section->tocDepth, 1);
    QCOMPARE(section->tocPath, (QStringList{QStringLiteral("First Chapter"), QStringLiteral("Section One")}));

    const TargetAnchorInfo *paragraph = findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#paragraph-id"));
    QVERIFY(paragraph);
    QCOMPARE(paragraph->ref, QStringLiteral("OEBPS/chapter1.xhtml#paragraph-id"));
    QCOMPARE(paragraph->type, QStringLiteral("element"));
    QCOMPARE(paragraph->tocTitle, QStringLiteral("Section One"));
    QCOMPARE(paragraph->tocDepth, 2);
    QCOMPARE(paragraph->tocPath, (QStringList{QStringLiteral("First Chapter"), QStringLiteral("Section One")}));

    const TargetAnchorInfo *targetAnchor = findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#target-anchor"));
    QVERIFY(targetAnchor);
    QCOMPARE(targetAnchor->ref, QStringLiteral("target-anchor"));
    QCOMPARE(targetAnchor->type, QStringLiteral("target"));
    QCOMPARE(targetAnchor->tocTitle, QStringLiteral("Section One"));

    QVERIFY(!findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#source-anchor")));
    QVERIFY(!findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#page-list-only")));
    QVERIFY(findByLocation(QStringLiteral("OEBPS/chapter2.xhtml")));
    QVERIFY(findByLocation(QStringLiteral("OEBPS/chapter2.xhtml#named-target")));
    QVERIFY(!findByLocation(QStringLiteral("OEBPS/nav.xhtml")));
    QVERIFY(indexOfLocation(QStringLiteral("OEBPS/chapter1.xhtml")) < indexOfLocation(QStringLiteral("OEBPS/chapter1.xhtml#section1")));
    QVERIFY(indexOfLocation(QStringLiteral("OEBPS/chapter1.xhtml#section1")) < indexOfLocation(QStringLiteral("OEBPS/chapter1.xhtml#paragraph-id")));
    QVERIFY(indexOfLocation(QStringLiteral("OEBPS/chapter1.xhtml#target-anchor")) < indexOfLocation(QStringLiteral("OEBPS/chapter2.xhtml")));

    bool foundGeneratedCfiTarget = false;
    for (const TargetAnchorInfo &target : targets) {
        if (target.location.startsWith(QStringLiteral("epubcfi("))) {
            foundGeneratedCfiTarget = true;
            break;
        }
    }
    QVERIFY(!foundGeneratedCfiTarget);
}

#endif

QTEST_MAIN(EPubContainerTest)
#include "epubcontainertest.moc"
