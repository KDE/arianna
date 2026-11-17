// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "maintenancefiles.h"
#include "bookreferencecheck.h"
#include "bookdatabase.h"
#include "booklistmodel.h"
#include "booktruthstore.h"
#include "config.h"
#include "editorsessionstore.h"
#include "epubcontainer.h"
#include "externalprocess.h"
#include "pdfreferenceindex.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>

#include <podofo/main/PdfAnnotation_Types.h>
#include <podofo/main/PdfEncrypt.h>
#include <podofo/main/PdfMemDocument.h>
#include <podofo/main/PdfPage.h>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QQuickItem *findVisualItem(QQuickItem *parent, const QString &name, bool visibleOnly = false)
{
    if (parent->objectName() == name && (!visibleOnly || parent->isVisible())) {
        return parent;
    }
    for (auto *child : parent->childItems()) {
        if (auto *item = findVisualItem(child, name, visibleOnly)) {
            return item;
        }
    }
    return nullptr;
}

BookEntry makeBook(const QString &path, const QString &id, const QByteArray &body, bool malformed = false)
{
    KZip zip(path);
    const QByteArray chapter =
        QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Test</title></head><body>\n") + body + QByteArrayLiteral("</body></html>");
    if (!zip.open(QIODevice::WriteOnly) || !zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"))
        || !zip.writeFile(QStringLiteral("META-INF/container.xml"),
                          QByteArrayLiteral("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
                                            "<rootfile full-path=\"OPS/package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"))
        || !zip.writeFile(QStringLiteral("OPS/package.opf"),
                          QByteArrayLiteral("<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"3.0\">"
                                            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"bookid\">test-book</dc:identifier>"
                                            "<dc:title>Test</dc:title></metadata><manifest>"
                                            "<item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
                                            "<item id=\"target\" href=\"target%20file.xhtml\" media-type=\"application/xhtml+xml\"/>"
                                            "<item id=\"other\" href=\"other.xhtml\" media-type=\"application/xhtml+xml\"/>"
                                            "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
                                            "</manifest><spine><itemref idref=\"chapter\"/><itemref idref=\"target\"/></spine></package>"))
        || !zip.writeFile(QStringLiteral("OPS/chapter.xhtml"), chapter)
        || !zip.writeFile(QStringLiteral("OPS/target file.xhtml"),
                          QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p id=\"target\">Target</p>"
                                            "<p id=\"encoded#id\">Encoded</p><p id=\"duplicate\">One</p><p id=\"duplicate\">Two</p></body></html>"))
        || !zip.writeFile(QStringLiteral("OPS/other.xhtml"),
                          malformed ? QByteArrayLiteral("<broken>")
                                    : QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p id=\"other\">Other</p></body></html>"))
        || !zip.writeFile(QStringLiteral("OPS/toc.ncx"), QByteArrayLiteral("<ncx><navMap><navPoint><content src=\"chapter.xhtml\"/></navPoint></navMap></ncx>"))
        || !zip.close()) {
        QTest::qFail("Unable to create EPUB fixture", __FILE__, __LINE__);
        return BookEntry();
    }
    BookEntry book;
    book.filename = path;
    book.title = id;
    book.uniqueIdentifier = id;
    book.identifier = id;
    return book;
}

BookEntry makePdf(const QString &path, const QString &id, const QList<QPair<QString, int>> &anchors = {}, const QString &password = QString())
{
    PoDoFo::PdfMemDocument pdf;
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    pdf.GetPages().CreatePage(PoDoFo::PdfPageSize::A4);
    // Native links and ordinary annotations must not be treated as Arianna cross-references, even with an NM value.
    auto &native = pdf.GetPages().GetPageAt(0).GetAnnotations().CreateAnnot<PoDoFo::PdfAnnotationLink>(PoDoFo::Rect(10, 10, 20, 20));
    native.GetDictionary().AddKey(PoDoFo::PdfName("NM"), PoDoFo::PdfObject(PoDoFo::PdfString("ordinary-link")));
    for (const auto &anchor : anchors) {
        auto &annotation = pdf.GetPages()
                               .GetPageAt(static_cast<unsigned>(anchor.second))
                               .GetAnnotations()
                               .CreateAnnot<PoDoFo::PdfAnnotationHighlight>(PoDoFo::Rect(30, 30, 40, 20));
        annotation.GetDictionary().AddKey(PoDoFo::PdfName("AriannaAnchor"), PoDoFo::PdfObject(true));
        if (!anchor.first.isNull()) {
            annotation.GetDictionary().AddKey(PoDoFo::PdfName("NM"), PoDoFo::PdfObject(PoDoFo::PdfString(anchor.first.toStdString())));
        }
        annotation.SetFlags(static_cast<PoDoFo::PdfAnnotationFlags>(static_cast<uint32_t>(PoDoFo::PdfAnnotationFlags::Hidden)
                                                                    | static_cast<uint32_t>(PoDoFo::PdfAnnotationFlags::NoView)));
    }
    if (!password.isEmpty()) {
        pdf.SetEncrypted(password.toStdString(), "owner", PoDoFo::PdfPermissions::None, PoDoFo::PdfEncryptionAlgorithm::AESV2, PoDoFo::PdfKeyLength::L128);
    }
    pdf.Save(path.toStdString());
    BookEntry book;
    book.filename = path;
    book.title = id;
    book.uniqueIdentifier = id;
    book.identifier = id;
    return book;
}

QVariantMap reference(const QString &sourceBook, const QString &anchor, const QString &targetBook, const QString &location)
{
    return {{QStringLiteral("sourceBookId"), sourceBook},
            {QStringLiteral("sourceAnchorId"), anchor},
            {QStringLiteral("targetBookId"), targetBook},
            {QStringLiteral("targetLocation"), location}};
}

bool hasCode(const QVariantMap &report, const QString &code)
{
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        if (row.toMap().value(QStringLiteral("codes")).toStringList().contains(code)) {
            return true;
        }
    }
    return false;
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

QVariantList removals(const QVariantMap &report)
{
    QVariantList result;
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        const auto removal = row.toMap().value(QStringLiteral("removal")).toMap();
        if (!removal.isEmpty()) {
            result.append(removal);
        }
    }
    return result;
}

QVariantMap referenceEdit(const QVariantMap &report, const QString &sourceAnchorId)
{
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        if (row.toMap().value(QStringLiteral("sourceAnchorId")).toString() == sourceAnchorId) {
            const auto edit = row.toMap().value(QStringLiteral("referenceEdit")).toMap();
            if (!edit.isEmpty())
                return edit;
        }
    }
    return {};
}

QVariantMap titleEdit(const QVariantMap &report, const QString &sourceAnchorId)
{
    for (const auto &row : report.value(QStringLiteral("results")).toList()) {
        if (row.toMap().value(QStringLiteral("sourceAnchorId")).toString() == sourceAnchorId) {
            return row.toMap().value(QStringLiteral("sourceTitleEdit")).toMap();
        }
    }
    return {};
}

BookEntry removalBook(const QString &path, const QString &id, bool source, bool pdf = false)
{
    const QString anchorId = source ? QStringLiteral("source-anchor") : QStringLiteral("target-anchor");
    if (pdf)
        return makePdf(path, id, {{anchorId, 0}});
    return makeBook(
        path,
        id,
        source
            ? QByteArrayLiteral("<p>Before <span id=\"source-anchor\" class=\"formatting\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"S.B. "
                                "3.2.2\"><em>S.B.</em> 3.2.2</span> after</p>")
            : QByteArrayLiteral("<p>Before <span id=\"target-anchor\" data-role=\"anchor\" data-anchor-type=\"target\"><em>Target</em> text</span> after</p>"));
}

QString removalTarget(bool pdf)
{
    return pdf ? QStringLiteral("{\"format\":\"pdf\",\"anchorId\":\"target-anchor\",\"page\":0}") : QStringLiteral("OPS/chapter.xhtml#target-anchor");
}
}

class BookReferenceCheckTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dataDirectory;
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dataDirectory.isValid());
        qputenv("XDG_DATA_HOME", QFile::encodeName(m_dataDirectory.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_dataDirectory.path()));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(m_dataDirectory.path()));
        QCoreApplication::setApplicationName(QStringLiteral("arianna-bookreferencechecktest"));
        KLocalizedString::setApplicationDomain("arianna");
    }

    void init()
    {
        QVERIFY(MaintenanceFiles::sessionsRoot().startsWith(m_dataDirectory.path()));
        QVERIFY(QDir(MaintenanceFiles::sessionsRoot()).removeRecursively());
    }

    void checksBothDirectionsWithoutDoubleCounting()
    {
        QTemporaryDir dir;
        const auto first = makeBook(dir.filePath(QStringLiteral("first.epub")),
                                    QStringLiteral("first"),
                                    QByteArrayLiteral("<a id=\"out\" data-role=\"anchor\" data-anchor-type=\"crossref\">Out</a>"
                                                      "<a id=\"within\" data-role=\"anchor\" href=\"target%20file.xhtml#target\">Within</a>"));
        const auto second = makeBook(dir.filePath(QStringLiteral("second.epub")),
                                     QStringLiteral("second"),
                                     QByteArrayLiteral("<a id=\"back\" data-role=\"anchor\" data-anchor-type=\"crossref\">Back</a>"));
        const QVariantList references{
            reference(QStringLiteral("first"), QStringLiteral("out"), QStringLiteral("second"), QStringLiteral("OPS/target%20file.xhtml#target")),
            reference(QStringLiteral("first"), QStringLiteral("within"), QStringLiteral("first"), QStringLiteral("OPS/target%20file.xhtml#target")),
            reference(QStringLiteral("second"), QStringLiteral("back"), QStringLiteral("first"), QStringLiteral("OPS/target%20file.xhtml#target"))};
        const auto before = digest(first.filename);
        const auto otherBefore = digest(second.filename);
        const auto report = BookReferenceCheck::inspect(first.filename, {first, second}, references);
        QVERIFY2(report.value(QStringLiteral("success")).toBool(), qPrintable(report.value(QStringLiteral("message")).toString()));
        QVERIFY(!report.contains(QStringLiteral("internal")));
        QCOMPARE(report.value(QStringLiteral("outgoing")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("incoming")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 2);
        QCOMPARE(digest(first.filename), before);
        QCOMPARE(digest(second.filename), otherBefore);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2);
    }

    void acceptsSpanCrossReferenceSources_data()
    {
        QTest::addColumn<QByteArray>("tag");
        QTest::newRow("span") << QByteArrayLiteral("span");
        QTest::newRow("prefixed-span") << QByteArrayLiteral("xhtml:span");
        QTest::newRow("existing-a") << QByteArrayLiteral("a");
    }

    void acceptsSpanCrossReferenceSources()
    {
        QFETCH(QByteArray, tag);
        QTemporaryDir dir;
        const QString anchorId = QStringLiteral("uuid_01a0a4a8-a9f0-7d8d-8933-2bc7a047bb8d");
        const QByteArray markup = '<' + tag + QByteArrayLiteral(" xmlns:xhtml=\"http://www.w3.org/1999/xhtml\" id=\"") + anchorId.toUtf8()
            + QByteArrayLiteral("\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"B.S. Vers 5.33\">B.S. 5.33</") + tag
            + QByteArrayLiteral(">&#x20;");
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("span-source"), markup);
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("span-target"), {});
        const QVariantList references{reference(source.uniqueIdentifier, anchorId, target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml"))};
        for (const auto &checked : {source, target}) {
            const auto report = BookReferenceCheck::inspect(checked.filename, {source, target}, references);
            QVERIFY2(report.value(QStringLiteral("success")).toBool(), qPrintable(report.value(QStringLiteral("message")).toString()));
            QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 1);
            const auto rows = report.value(QStringLiteral("results")).toList();
            QCOMPARE(rows.size(), 1);
            QVERIFY(rows.first().toMap().value(QStringLiteral("repairs")).toList().isEmpty());
            QVERIFY(!hasCode(report, QStringLiteral("source-anchor-unresolved")));
        }
    }

    void reportsBrokenAndOrphanedAnchors()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("book.epub")),
                                   QStringLiteral("book"),
                                   QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Orphan</a>"
                                                     "<a id=\"absent\" data-role=\"anchor\" data-anchor-type=\"crossref\">Absent anchor</a>"
                                                     "<a id=\"file\" data-role=\"anchor\" data-anchor-type=\"crossref\">Absent document</a>"
                                                     "<a id=\"duplicate\" data-role=\"anchor\" data-anchor-type=\"crossref\">Duplicate</a>"
                                                     "<a id=\"mismatch\" data-role=\"anchor\" href=\"other.xhtml#other\">Wrong source type</a>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("destination"), {});
        const QVariantList references{
            reference(book.uniqueIdentifier, QStringLiteral("missing-source"), QStringLiteral("missing-book"), QStringLiteral("target")),
            reference(book.uniqueIdentifier, QStringLiteral("absent"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#absent")),
            reference(book.uniqueIdentifier, QStringLiteral("file"), target.uniqueIdentifier, QStringLiteral("OPS/absent.xhtml#target")),
            reference(book.uniqueIdentifier, QStringLiteral("duplicate"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#duplicate")),
            reference(book.uniqueIdentifier, QStringLiteral("mismatch"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#target"))};
        const auto report = BookReferenceCheck::inspect(book.filename, {book, target}, references);
        QVERIFY(!report.value(QStringLiteral("success")).toBool());
        QCOMPARE(report.value(QStringLiteral("outgoing")).toInt(), 6);
        for (const auto &code : {"orphan-source-anchor",
                                 "missing-target-anchor",
                                 "missing-target-file",
                                 "duplicate-target-anchor",
                                 "source-anchor-unresolved",
                                 "target-book-unresolved",
                                 "reference-type-mismatch"}) {
            QVERIFY2(hasCode(report, QString::fromLatin1(code)), code);
        }
    }

    void ignoresBookInternalAndOrdinaryLinks()
    {
        QTemporaryDir dir;
        auto book = makeBook(dir.filePath(QStringLiteral("book.epub")),
                             QStringLiteral("11111111-1111-4111-8111-111111111111"),
                             QByteArrayLiteral("<a href=\"#absent\">Missing local anchor</a>"
                                               "<a href=\"absent.xhtml\">Missing local document</a>"
                                               "<a href=\"target%20file.xhtml#duplicate\">Duplicate local target</a>"
                                               "<a href=\"https://example.invalid/\">Ordinary web link</a>"
                                               "<a href=\"epubcfi(/6/200!/4/2)\">Local CFI</a>"
                                               "<a id=\"within\" data-role=\"anchor\" href=\"absent.xhtml#absent\">Internal reference</a>"
                                               "<a id=\"unstored\" data-role=\"anchor\" href=\"absent.xhtml\">Unstored internal reference</a>"
                                               "<span class=\"bookref\" data-ref=\"legacy\">Stored internal legacy reference</span>"
                                               "<a id=\"wrong-type\" data-role=\"anchor\" data-anchor-type=\"crossref\">Stored internal reference</a>"));
        book.identifier = QStringLiteral("old-id");
        BookEntry sameFile = book;
        sameFile.uniqueIdentifier = QStringLiteral("same-file-id");
        sameFile.identifier.clear();
        const QVariantList references{
            reference(book.uniqueIdentifier, QStringLiteral("within"), book.uniqueIdentifier, QStringLiteral("OPS/absent.xhtml#absent")),
            reference(QStringLiteral("old-id"),
                      QStringLiteral("legacy"),
                      QStringLiteral("urn:uuid:11111111-1111-4111-8111-111111111111"),
                      QStringLiteral("missing-target")),
            reference(book.uniqueIdentifier, QStringLiteral("wrong-type"), sameFile.uniqueIdentifier, QString()),
            reference(book.uniqueIdentifier, QStringLiteral("missing-source"), book.uniqueIdentifier, QStringLiteral("missing-target"))};
        const auto report = BookReferenceCheck::inspect(book.filename, {book, sameFile}, references);
        QVERIFY(report.value(QStringLiteral("success")).toBool());
        QVERIFY(report.value(QStringLiteral("results")).toList().isEmpty());
        QCOMPARE(report.value(QStringLiteral("outgoing")).toInt(), 0);
        QCOMPARE(report.value(QStringLiteral("incoming")).toInt(), 0);
        QVERIFY(!report.contains(QStringLiteral("internal")));
    }

    void resolvesEncodedCrossReferencesAndReportsUnverifiedLocations()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")),
                                     QStringLiteral("source"),
                                     QByteArrayLiteral("<a id=\"encoded\" data-role=\"anchor\" data-anchor-type=\"crossref\">Encoded target</a>"
                                                       "<a id=\"cfi\" data-role=\"anchor\" data-anchor-type=\"crossref\">Unverified CFI</a>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("destination"), {});
        const auto report = BookReferenceCheck::inspect(
            source.filename,
            {source, target},
            {reference(source.uniqueIdentifier, QStringLiteral("encoded"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#encoded%23id")),
             reference(source.uniqueIdentifier, QStringLiteral("cfi"), target.uniqueIdentifier, QStringLiteral("epubcfi(/6/200!/4/2)"))});
        QCOMPARE(report.value(QStringLiteral("errors")).toInt(), 0);
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("warnings")).toInt(), 1);
        QVERIFY(hasCode(report, QStringLiteral("cfi-unverified")));
    }

    void validatesFileAndFragmentTogether()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")),
                                     QStringLiteral("source"),
                                     QByteArrayLiteral("<a id=\"link\" data-role=\"anchor\" data-anchor-type=\"crossref\">Wrong document</a>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("destination"), {});
        const auto report = BookReferenceCheck::inspect(
            source.filename,
            {source, target},
            {reference(source.uniqueIdentifier, QStringLiteral("link"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#target"))});
        QVERIFY(hasCode(report, QStringLiteral("missing-target-anchor")));
        QVERIFY(!report.value(QStringLiteral("success")).toBool());
    }

    void includesLegacyAliasesAndUnreadableIncomingSources()
    {
        QTemporaryDir dir;
        auto book = makeBook(dir.filePath(QStringLiteral("book.epub")),
                             QStringLiteral("11111111-1111-4111-8111-111111111111"),
                             QByteArrayLiteral("<span class=\"extra bookref\" data-ref=\"legacy\">Legacy</span>"));
        book.identifier = QStringLiteral("old-id, another-id");
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("destination"), {});
        BookEntry missing;
        missing.filename = dir.filePath(QStringLiteral("missing.epub"));
        missing.uniqueIdentifier = QStringLiteral("missing-source-book");
        const auto report =
            BookReferenceCheck::inspect(book.filename,
                                        {book, target, missing},
                                        {reference(QStringLiteral("old-id"), QStringLiteral("legacy"), target.uniqueIdentifier, QStringLiteral("target")),
                                         reference(QStringLiteral("missing-source-book"),
                                                   QStringLiteral("source"),
                                                   QStringLiteral("urn:uuid:11111111-1111-4111-8111-111111111111"),
                                                   QStringLiteral("OPS/target%20file.xhtml#target"))});
        QCOMPARE(report.value(QStringLiteral("incoming")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("outgoing")).toInt(), 1);
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 1);
        QVERIFY(hasCode(report, QStringLiteral("source-book-unreadable")));
        QVERIFY(!hasCode(report, QStringLiteral("orphan-source-anchor")));
        QVERIFY(!hasCode(report, QStringLiteral("source-anchor-unresolved")));
    }

    void malformedDocumentsDoNotPassAsEmpty()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("book.epub")), QStringLiteral("book"), {}, true);
        const auto report = BookReferenceCheck::inspect(book.filename, {book}, {});
        QVERIFY(hasCode(report, QStringLiteral("incomplete-cross-reference-scan")));
        QVERIFY(!report.value(QStringLiteral("success")).toBool());

        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")),
                                     QStringLiteral("source"),
                                     QByteArrayLiteral("<a id=\"link\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</a>"),
                                     true);
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("destination"), {});
        const auto incoming = BookReferenceCheck::inspect(
            target.filename,
            {source, target},
            {reference(source.uniqueIdentifier, QStringLiteral("link"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#target"))});
        QVERIFY(hasCode(incoming, QStringLiteral("incomplete-source-index")));
        QVERIFY(!incoming.value(QStringLiteral("success")).toBool());
    }

    void checksKnownCfiTargets()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")),
                                     QStringLiteral("source"),
                                     QByteArrayLiteral("<a id=\"link\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</a>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("destination"), {});
        EPubContainer container(nullptr);
        QVERIFY(container.openFile(target.filename));
        QString cfi;
        for (const auto &anchor : container.referenceableTargets()) {
            if (anchor.location.endsWith(QStringLiteral("#target"))) {
                cfi = anchor.cfiLocation;
            }
        }
        QVERIFY(!cfi.isEmpty());
        const auto report = BookReferenceCheck::inspect(source.filename,
                                                        {source, target},
                                                        {reference(source.uniqueIdentifier, QStringLiteral("link"), target.uniqueIdentifier, cfi)});
        QVERIFY2(report.value(QStringLiteral("success")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(report).toJson())));
    }

    void reportsAmbiguousBooksAndUnsupportedFormats()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")),
                                     QStringLiteral("source"),
                                     QByteArrayLiteral("<a id=\"link\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</a>"));
        const auto first = makeBook(dir.filePath(QStringLiteral("first.epub")), QStringLiteral("duplicate-book-id"), {});
        const auto second = makeBook(dir.filePath(QStringLiteral("second.epub")), first.uniqueIdentifier, {});
        const auto ambiguous =
            BookReferenceCheck::inspect(source.filename,
                                        {source, first, second},
                                        {reference(source.uniqueIdentifier, QStringLiteral("link"), first.uniqueIdentifier, QStringLiteral("target"))});
        QVERIFY(hasCode(ambiguous, QStringLiteral("target-book-unresolved")));
        BookEntry textBook;
        textBook.filename = dir.filePath(QStringLiteral("target.txt"));
        textBook.uniqueIdentifier = QStringLiteral("text-book");
        QFile textFile(textBook.filename);
        QVERIFY(textFile.open(QIODevice::WriteOnly));
        textFile.write("Plain text book\n");
        textFile.close();
        const auto unsupported =
            BookReferenceCheck::inspect(source.filename,
                                        {source, textBook},
                                        {reference(source.uniqueIdentifier, QStringLiteral("link"), textBook.uniqueIdentifier, QStringLiteral("target"))});
        QCOMPARE(unsupported.value(QStringLiteral("errors")).toInt(), 0);
        QCOMPARE(unsupported.value(QStringLiteral("warnings")).toInt(), 1);
        QVERIFY(!unsupported.value(QStringLiteral("success")).toBool());
    }

    void checksPdfAndEpubInBothDirections()
    {
        QTemporaryDir dir;
        const auto pdf = makePdf(dir.filePath(QStringLiteral("first.pdf")),
                                 QStringLiteral("first"),
                                 {{QStringLiteral("out-pdf"), 1}, {QStringLiteral("out-epub"), 0}, {QStringLiteral("internal"), 0}});
        const auto other = makePdf(dir.filePath(QStringLiteral("other.pdf")), QStringLiteral("other"), {{QStringLiteral("back-pdf"), 0}});
        const auto epub = makeBook(dir.filePath(QStringLiteral("book.epub")),
                                   QStringLiteral("epub"),
                                   QByteArrayLiteral("<a id=\"back-epub\" data-role=\"anchor\" data-anchor-type=\"crossref\">Back</a>"));
        const QString page = QStringLiteral("{\"format\":\"pdf\",\"page\":1,\"pageNumber\":2,\"x\":0.25,\"y\":0.5,\"position\":\"topLeft\"}");
        const QVariantList references{
            reference(pdf.uniqueIdentifier, QStringLiteral("out-pdf"), other.uniqueIdentifier, page),
            reference(pdf.uniqueIdentifier, QStringLiteral("out-epub"), epub.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#target")),
            reference(other.uniqueIdentifier, QStringLiteral("back-pdf"), pdf.uniqueIdentifier, page),
            reference(epub.uniqueIdentifier, QStringLiteral("back-epub"), pdf.uniqueIdentifier, QStringLiteral("{\"format\":\"pdf\",\"pageNumber\":2}")),
            reference(pdf.uniqueIdentifier, QStringLiteral("internal"), pdf.uniqueIdentifier, QStringLiteral("invalid internal target"))};
        const QList<BookEntry> books{pdf, other, epub};
        const QList<QByteArray> before{digest(pdf.filename), digest(other.filename), digest(epub.filename)};
        const auto report = BookReferenceCheck::inspect(pdf.filename, books, references);
        QVERIFY2(report.value(QStringLiteral("success")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(report).toJson())));
        QCOMPARE(report.value(QStringLiteral("outgoing")).toInt(), 2);
        QCOMPARE(report.value(QStringLiteral("incoming")).toInt(), 2);
        QCOMPARE(report.value(QStringLiteral("passed")).toInt(), 4);
        const auto firstRow = report.value(QStringLiteral("results")).toList().first().toMap();
        QCOMPARE(firstRow.value(QStringLiteral("sourcePage")).toInt(), 2);
        QCOMPARE(firstRow.value(QStringLiteral("targetPage")).toInt(), 2);
        for (const auto &book : {other, epub}) {
            const auto otherReport = BookReferenceCheck::inspect(book.filename, books, references);
            QVERIFY(otherReport.value(QStringLiteral("success")).toBool());
            QCOMPARE(otherReport.value(QStringLiteral("passed")).toInt(), 2);
        }
        QCOMPARE(digest(pdf.filename), before.at(0));
        QCOMPARE(digest(other.filename), before.at(1));
        QCOMPARE(digest(epub.filename), before.at(2));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 3);
    }

    void reportsBrokenPdfReferences()
    {
        QTemporaryDir dir;
        const auto pdf = makePdf(
            dir.filePath(QStringLiteral("source.pdf")),
            QStringLiteral("source"),
            {{QStringLiteral("orphan"), 0}, {QStringLiteral("duplicate"), 0}, {QStringLiteral("duplicate"), 1}, {QStringLiteral("valid"), 1}, {QString(), 0}});
        const auto target = makePdf(dir.filePath(QStringLiteral("target.pdf")), QStringLiteral("target"));
        const QString page = QStringLiteral("{\"format\":\"pdf\",\"page\":0}");
        const auto report = BookReferenceCheck::inspect(
            pdf.filename,
            {pdf, target},
            {reference(pdf.uniqueIdentifier, QStringLiteral("missing"), target.uniqueIdentifier, page),
             reference(pdf.uniqueIdentifier, QStringLiteral("duplicate"), target.uniqueIdentifier, page),
             reference(pdf.uniqueIdentifier, QStringLiteral("ordinary-link"), target.uniqueIdentifier, page),
             reference(pdf.uniqueIdentifier, QStringLiteral("valid"), target.uniqueIdentifier, QStringLiteral("{\"format\":\"pdf\",\"page\":99}"))});
        QCOMPARE(report.value(QStringLiteral("errors")).toInt(), 6);
        for (const auto &code : {"source-anchor-unresolved", "pdf-page-out-of-range", "orphan-source-anchor", "empty-source-anchor"}) {
            QVERIFY2(hasCode(report, QString::fromLatin1(code)), code);
        }
    }

    void validatesPdfTargetLocations_data()
    {
        QTest::addColumn<QString>("location");
        QTest::addColumn<QString>("code");
        QTest::newRow("page-zero") << QStringLiteral("{\"format\":\"pdf\",\"page\":0}") << QString();
        QTest::newRow("page-number-two") << QStringLiteral("{\"format\":\"pdf\",\"pageNumber\":2}") << QString();
        QTest::newRow("out-of-range") << QStringLiteral("{\"format\":\"pdf\",\"pageNumber\":3}") << QStringLiteral("pdf-page-out-of-range");
        QTest::newRow("negative") << QStringLiteral("{\"format\":\"pdf\",\"page\":-1}") << QStringLiteral("invalid-pdf-location");
        QTest::newRow("fractional") << QStringLiteral("{\"format\":\"pdf\",\"page\":0.5}") << QStringLiteral("invalid-pdf-location");
        QTest::newRow("missing-page") << QStringLiteral("{\"format\":\"pdf\"}") << QStringLiteral("invalid-pdf-location");
        QTest::newRow("wrong-format") << QStringLiteral("{\"format\":\"epub\",\"page\":0}") << QStringLiteral("invalid-pdf-location");
        QTest::newRow("invalid-json") << QStringLiteral("broken") << QStringLiteral("invalid-pdf-location");
        QTest::newRow("conflicting-pages") << QStringLiteral("{\"format\":\"pdf\",\"page\":0,\"pageNumber\":2}") << QStringLiteral("inconsistent-pdf-page");
        QTest::newRow("invalid-position") << QStringLiteral("{\"format\":\"pdf\",\"page\":0,\"x\":2,\"y\":0.5}") << QStringLiteral("invalid-pdf-position");
        QTest::newRow("missing-coordinate") << QStringLiteral("{\"format\":\"pdf\",\"page\":0,\"x\":0.2}") << QStringLiteral("invalid-pdf-position");
    }

    void validatesPdfTargetLocations()
    {
        QFETCH(QString, location);
        QFETCH(QString, code);
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")),
                                     QStringLiteral("source"),
                                     QByteArrayLiteral("<a id=\"link\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</a>"));
        const auto target = makePdf(dir.filePath(QStringLiteral("target.pdf")), QStringLiteral("target"));
        const auto report = BookReferenceCheck::inspect(target.filename,
                                                        {source, target},
                                                        {reference(source.uniqueIdentifier, QStringLiteral("link"), target.uniqueIdentifier, location)});
        QCOMPARE(report.value(QStringLiteral("success")).toBool(), code.isEmpty());
        if (!code.isEmpty()) {
            QVERIFY2(hasCode(report, code), qPrintable(code));
        }
        QCOMPARE(report.value(QStringLiteral("incoming")).toInt(), 1);
    }

    void reportsLockedAndUnreadablePdfBooks()
    {
        QTemporaryDir dir;
        const auto source = makePdf(dir.filePath(QStringLiteral("source.pdf")), QStringLiteral("source"), {{QStringLiteral("link"), 0}});
        const auto target = makePdf(dir.filePath(QStringLiteral("locked.pdf")), QStringLiteral("locked"), {}, QStringLiteral("reader"));
        const QString page = QStringLiteral("{\"format\":\"pdf\",\"page\":0}");
        const auto locked = BookReferenceCheck::inspect(source.filename,
                                                        {source, target},
                                                        {reference(source.uniqueIdentifier, QStringLiteral("link"), target.uniqueIdentifier, page)});
        QCOMPARE(locked.value(QStringLiteral("errors")).toInt(), 0);
        QCOMPARE(locked.value(QStringLiteral("warnings")).toInt(), 1);
        QVERIFY(hasCode(locked, QStringLiteral("target-book-unreadable")));
        const auto selectedLocked = BookReferenceCheck::inspect(target.filename, {target}, {});
        QCOMPARE(selectedLocked.value(QStringLiteral("warnings")).toInt(), 1);
        QVERIFY(!selectedLocked.value(QStringLiteral("success")).toBool());
        QFile brokenFile(target.filename);
        QVERIFY(brokenFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        brokenFile.write("%PDF-1.7\ninvalid PDF\n");
        brokenFile.close();
        const auto broken = BookReferenceCheck::inspect(source.filename,
                                                        {source, target},
                                                        {reference(source.uniqueIdentifier, QStringLiteral("link"), target.uniqueIdentifier, page)});
        QCOMPARE(broken.value(QStringLiteral("errors")).toInt(), 1);
        QVERIFY(!broken.value(QStringLiteral("success")).toBool());
    }

    void ignoresNativeAndInternalPdfLinks()
    {
        QTemporaryDir dir;
        auto book = makePdf(dir.filePath(QStringLiteral("source.pdf")), QStringLiteral("source"), {{QStringLiteral("within"), 0}});
        book.identifier = QStringLiteral("alias");
        const auto report = BookReferenceCheck::inspect(
            book.filename,
            {book},
            {reference(book.uniqueIdentifier, QStringLiteral("within"), QStringLiteral("alias"), QStringLiteral("invalid internal target"))});
        QVERIFY(report.value(QStringLiteral("success")).toBool());
        QVERIFY(report.value(QStringLiteral("results")).toList().isEmpty());
    }

    void runsAgainstDatabaseSnapshot()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("async.epub")),
                                   QStringLiteral("async-book"),
                                   QByteArrayLiteral("<a id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</a>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("async-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(book);
        database.addEntry(target);
        database.saveReference(
            reference(book.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#target")));
        database.saveReference(
            reference(book.uniqueIdentifier, QStringLiteral("internal-missing-source"), book.uniqueIdentifier, QStringLiteral("missing-target")));
        BookReferenceCheck checker;
        checker.start(book.filename);
        QVERIFY(checker.running());
        checker.start(QStringLiteral("ignored-while-running.epub"));
        QTRY_VERIFY_WITH_TIMEOUT(!checker.running(), 10000);
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        QCOMPARE(checker.report().value(QStringLiteral("outgoing")).toInt(), 1);
        QCOMPARE(checker.report().value(QStringLiteral("passed")).toInt(), 1);
        QCOMPARE(database.loadReferences(book.uniqueIdentifier).size(), 2);
    }

    void pdfMaintenanceStartsCrossReferenceCheck()
    {
        QTemporaryDir dir;
        const auto book = makePdf(dir.filePath(QStringLiteral("maintenance.pdf")), QStringLiteral("pdf-maintenance"));
        BookDatabase::self().addEntry(book);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testFileName"), book.filename);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 1100; height: 900; visible: true
                property var bookListModel: null
                BookDetailsPage {
                    anchors.fill: parent
                    metadata: ({filename: testFileName, title: "PDF", author: [], identifier: "pdf-maintenance",
                        uniqueIdentifier: "pdf-maintenance", description: "", publisher: "", language: "de",
                        pubdate: "", rights: "", subjects: [], genres: [], bookVersion: "1.7", source: ""})
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *button = window->findChild<QObject *>(QStringLiteral("crossReferenceCheckButton"));
        auto *checker = window->findChild<BookReferenceCheck *>();
        QVERIFY(button);
        QVERIFY(checker);
        QVERIFY(button->property("visible").toBool());
        QVERIFY(button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(!checker->running(), 10000);
        QCOMPARE(checker->report().value(QStringLiteral("fileName")).toString(), book.filename);
        QVERIFY(checker->report().value(QStringLiteral("success")).toBool());
    }

    void missingEpubAnchorsHaveCorrectRepairTemplates()
    {
        QTemporaryDir dir;
        auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("source"), {});
        source.identifier = QStringLiteral("old-source-id");
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("target-book"), {});
        const QString sourceAnchor = QStringLiteral("missing\"&<source");
        const QString targetAnchor = QStringLiteral("missing\"&<target");
        auto ref = reference(QStringLiteral("old-source-id"),
                             sourceAnchor,
                             target.uniqueIdentifier,
                             QStringLiteral("OPS/other.xhtml#") + QString::fromUtf8(QUrl::toPercentEncoding(targetAnchor)));
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Title \"<&>"));
        const auto report = BookReferenceCheck::inspect(source.filename, {source, target}, {ref});
        const auto row = report.value(QStringLiteral("results")).toList().first().toMap();
        const auto repairs = row.value(QStringLiteral("repairs")).toList();
        QCOMPARE(repairs.size(), 2);
        for (int i = 0; i < repairs.size(); ++i) {
            const auto repair = repairs.at(i).toMap();
            QDomDocument document;
            QVERIFY(document.setContent(repair.value(QStringLiteral("htmlSource")).toString()));
            const auto anchor = document.documentElement();
            QCOMPARE(anchor.tagName(), i == 0 ? QStringLiteral("span") : QStringLiteral("a"));
            QCOMPARE(anchor.attribute(QStringLiteral("id")), i == 0 ? sourceAnchor : targetAnchor);
            QCOMPARE(anchor.attribute(QStringLiteral("data-anchor-type")), i == 0 ? QStringLiteral("crossref") : QStringLiteral("target"));
            QCOMPARE(repair.value(QStringLiteral("bookId")).toString(), i == 0 ? source.uniqueIdentifier : target.uniqueIdentifier);
            QCOMPARE(repair.value(QStringLiteral("fileName")).toString(), i == 0 ? source.filename : target.filename);
        }
        QCOMPARE(repairs.at(1).toMap().value(QStringLiteral("documentFile")).toString(), QStringLiteral("OPS/other.xhtml"));
        const auto hint = row.value(QStringLiteral("targetAnchorRepair")).toMap();
        QCOMPARE(hint, repairs.at(1).toMap());
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        for (const auto &snapshot : {BookReferenceCheck::inspectStoredReferences({source, target}, {ref}),
                                     BookReferenceCheck::inspectLibraryReferences({source, target}, {ref}, {})}) {
            QVERIFY(!snapshot.isEmpty());
            QCOMPARE(snapshot.first().toMap().value(QStringLiteral("targetAnchorRepair")).toMap(), hint);
            QVERIFY(!snapshot.first().toMap().contains(QStringLiteral("repairs")));
        }
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        const auto ambiguous = BookReferenceCheck::inspect(
            source.filename,
            {source, target},
            {reference(source.uniqueIdentifier, QStringLiteral("duplicate"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#duplicate"))});
        QVERIFY(ambiguous.value(QStringLiteral("results")).toList().first().toMap().value(QStringLiteral("repairs")).toList().isEmpty());
    }

    void repairActionsOnlyRunWhenRequested_data()
    {
        QTest::addColumn<bool>("librarySnapshot");
        QTest::newRow("per-book-report") << false;
        QTest::newRow("library-snapshot") << true;
    }

    void repairActionsOnlyRunWhenRequested()
    {
        QFETCH(bool, librarySnapshot);
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("repair-source-") + QString::fromLatin1(QTest::currentDataTag()), {});
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("repair-target-") + QString::fromLatin1(QTest::currentDataTag()), QByteArrayLiteral("<p>Only in the target book</p>"));
        BookDatabase::self().addEntry(source);
        BookDatabase::self().addEntry(target);
        const QVariantList refs{reference(source.uniqueIdentifier, QStringLiteral("missing-source"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#missing-target"))};
        const auto report = librarySnapshot ? QVariantMap{{QStringLiteral("results"), BookReferenceCheck::inspectStoredReferences({source, target}, refs)}}
                                            : BookReferenceCheck::inspect(source.filename, {source, target}, refs);
        const auto repairRow = report.value(QStringLiteral("results")).toList().first().toMap();
        const QString html = (librarySnapshot ? repairRow.value(QStringLiteral("targetAnchorRepair")).toMap()
            : repairRow.value(QStringLiteral("repairs")).toList().first().toMap()).value(QStringLiteral("htmlSource")).toString();
        const QString capturePath = dir.filePath(QStringLiteral("editor-argument.txt"));
        const QString capturedBookPath = dir.filePath(QStringLiteral("editor-opened.epub"));
        QFile editor(dir.filePath(QStringLiteral("fake-editor.sh")));
        QVERIFY(editor.open(QIODevice::WriteOnly));
        editor.write("#!/bin/sh\nprintf '%s' \"$1\" > \"");
        editor.write(capturePath.toUtf8());
        editor.write("\"\ncp -- \"$1\" \"");
        editor.write(capturedBookPath.toUtf8());
        editor.write("\"\n");
        editor.close();
        QVERIFY(editor.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString previousCommand = Config::self()->editorCommand();
        const auto resetCommand = qScopeGuard([previousCommand]() {
            Config::self()->setEditorCommand(previousCommand);
        });
        Config::self()->setEditorCommand(editor.fileName());
        QGuiApplication::clipboard()->setText(QStringLiteral("unchanged"));
        const auto before = digest(source.filename);
        const auto targetBefore = digest(target.filename);

        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testReport"), report);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1100; height: 900; visible: true
                ReferenceReportDialog {
                    objectName: "repairReport"
                    report: testReport
                    Component.onCompleted: open()
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("unchanged"));
        QVERIFY(!QFileInfo::exists(capturePath));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QObject *copyButton = nullptr;
        QObject *openButton = nullptr;
        QTRY_VERIFY(copyButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("copyMissingAnchorHtml")));
        QTRY_VERIFY(openButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("openMissingAnchorEditor")));
        QVERIFY(QMetaObject::invokeMethod(copyButton, "clicked"));
        QCOMPARE(QGuiApplication::clipboard()->text(), html);
        QVERIFY(!QFileInfo::exists(capturePath));
        QGuiApplication::clipboard()->setText(QStringLiteral("keep while opening"));
        auto *process = engine.singletonInstance<ExternalProcess *>("org.kde.arianna", "ExternalProcess");
        QVERIFY(process);
        QSignalSpy finished(process, &ExternalProcess::editorSessionFinished);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QTRY_VERIFY2_WITH_TIMEOUT(!finished.isEmpty(), qPrintable(window->findChild<QObject *>(QStringLiteral("repairReport"))->property("actionStatus").toString()), 5000);
        QFile captured(capturePath);
        QVERIFY(captured.open(QIODevice::ReadOnly));
        const QString workingCopy = QString::fromUtf8(captured.readAll());
        QTRY_VERIFY(!QFileInfo::exists(workingCopy));
        QVERIFY(workingCopy != source.filename);
        QCOMPARE(digest(capturedBookPath), librarySnapshot ? targetBefore : before);
        QCOMPARE(digest(source.filename), before);
        QCOMPARE(digest(target.filename), targetBefore);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("keep while opening"));
    }

    void missingTargetEditorOpensOnlyOnRequest()
    {
        QTemporaryDir dir;
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("missing-target-editor"), {});
        BookDatabase::self().addEntry(target);
        const auto repair = BookReferenceCheck::targetAnchorRepair(target.uniqueIdentifier, target.filename,
            QStringLiteral("OPS/other.xhtml"), QStringLiteral("missing\"&कृष्ण"));
        const auto before = digest(target.filename);
        const QString capturedPath = dir.filePath(QStringLiteral("opened.txt"));
        const QString copiedPath = dir.filePath(QStringLiteral("opened.epub"));
        QFile fake(dir.filePath(QStringLiteral("fake-editor.sh")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(QStringLiteral("#!/bin/sh\nprintf '%%s\\n' \"$1\" >> \"%1\"\ncp -- \"$1\" \"%2\"\n")
                       .arg(capturedPath, copiedPath).replace(QStringLiteral("%%s"), QStringLiteral("%s")).toUtf8());
        fake.close();
        QVERIFY(fake.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto previousCommand = Config::editorCommand();
        const auto restore = qScopeGuard([&] { Config::setEditorCommand(previousCommand); });
        Config::setEditorCommand(fake.fileName());
        QGuiApplication::clipboard()->setText(QStringLiteral("unchanged"));
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testRepair"), repair);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 900; height: 500; visible: true
                TargetAnchorRepair { objectName: "offer"; width: parent.width; repair: testRepair }
                Connections {
                    target: ExternalProcess
                    function onEditorSessionProcessStarted(id, pid) { EditorSessionStore.noteEditorProcessStarted(id, pid); }
                    function onEditorSessionFinished(id, code, status) { EditorSessionStore.finishSession(id); }
                }
            }
        )", QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto *offer = window->findChild<QObject *>(QStringLiteral("offer"));
        auto *open = window->findChild<QObject *>(QStringLiteral("openMissingTargetEditor"));
        auto *copy = window->findChild<QObject *>(QStringLiteral("copyMissingTargetAnchor"));
        QVERIFY(offer && open && copy);
        QVERIFY(!QFileInfo::exists(capturedPath));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("unchanged"));
        QVERIFY(QMetaObject::invokeMethod(copy, "clicked"));
        QCOMPARE(QGuiApplication::clipboard()->text(), repair.value(QStringLiteral("htmlSource")).toString());
        QGuiApplication::clipboard()->setText(QStringLiteral("keep while opening"));
        auto *process = engine.singletonInstance<ExternalProcess *>("org.kde.arianna", "ExternalProcess");
        QSignalSpy finished(process, &ExternalProcess::editorSessionFinished);
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QTRY_VERIFY(!finished.isEmpty());
        QFile captured(capturedPath);
        QVERIFY(captured.open(QIODevice::ReadOnly));
        const auto paths = captured.readAll().trimmed().split('\n');
        QCOMPARE(paths.size(), 1);
        QVERIFY(QString::fromUtf8(paths.first()) != target.filename);
        QCOMPARE(digest(copiedPath), before);
        QCOMPARE(digest(target.filename), before);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("keep while opening"));
    }

    // Optional cross-project test: the Sigil MainWindow harness starts this
    // on its private bus, then inspects its own real loaded resource.
    void missingTargetWithRealSigil()
    {
        const QString command = qEnvironmentVariable("ARIANNA_TEST_SIGIL");
        if (command.isEmpty()) QSKIP("Run from Sigil's DBus navigation harness for real editor validation.");
        BookEntry target;
        target.filename = qEnvironmentVariable("SIGIL_SELECT_TEXT_TEST_BOOK");
        target.uniqueIdentifier = QStringLiteral("real-sigil-target");
        target.identifier = target.uniqueIdentifier;
        target.title = QStringLiteral("Real Sigil target");
        BookDatabase::self().addEntry(target);
        const QString documentFile = QStringLiteral("OEBPS/second.xhtml");
        const auto repair = BookReferenceCheck::targetAnchorRepair(target.uniqueIdentifier, target.filename, documentFile, QStringLiteral("missing-anchor"));
        const auto before = digest(target.filename);
        const auto previousCommand = Config::editorCommand();
        const auto restore = qScopeGuard([&] { Config::setEditorCommand(previousCommand); });
        Config::setEditorCommand(command);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testRepair"), repair);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 900; height: 500; visible: true
                TargetAnchorRepair { objectName: "offer"; width: parent.width; repair: testRepair }
                Connections {
                    target: ExternalProcess
                    function onEditorSessionProcessStarted(id, pid) { EditorSessionStore.noteEditorProcessStarted(id, pid); }
                }
            }
        )", QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY(window);
        auto *open = window->findChild<QObject *>(QStringLiteral("openMissingTargetEditor"));
        QVERIFY(open);
        auto *verseField = window->findChild<QObject *>(QStringLiteral("targetAnchorVerseReference"));
        QVERIFY(verseField);
        verseField->setProperty("text", QStringLiteral("18–20"));
        QVERIFY(QMetaObject::invokeMethod(verseField, "textEdited"));
        auto *process = engine.singletonInstance<ExternalProcess *>("org.kde.arianna", "ExternalProcess");
        QSignalSpy navigation(process, &ExternalProcess::editorSessionNavigationFinished);
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QTRY_COMPARE_WITH_TIMEOUT(navigation.size(), 1, 20000);
        QCOMPARE(navigation.first().at(1).toString(), QStringLiteral("anchor-prepared"));
        auto *store = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        const auto session = store->sessionInfo(navigation.first().at(0).toString());
        const auto copy = session.value(QStringLiteral("workingCopyPath")).toString();
        QVERIFY(copy != target.filename);
        QCOMPARE(digest(copy), before);
        QCOMPARE(digest(target.filename), before);
        qInfo().noquote() << "DOCUMENT_PROBE" << QJsonDocument(QJsonObject{{QStringLiteral("bookPath"), copy},
            {QStringLiteral("documentFile"), documentFile}}).toJson(QJsonDocument::Compact);
    }

    void sourceEditorRequiresExistingEpubId_data()
    {
        QTest::addColumn<QByteArray>("markup");
        QTest::addColumn<QString>("expectedId");
        QTest::addColumn<bool>("pdf");
        QTest::newRow("missing") << QByteArray() << QString() << false;
        QTest::newRow("duplicate") << QByteArrayLiteral("<span id=\"source\"/><span id=\"source\"/>") << QString() << false;
        QTest::newRow("wrong-type-can-be-corrected") << QByteArrayLiteral("<span id=\"source\">Text</span>") << QStringLiteral("source") << false;
        QTest::newRow("legacy-without-id") << QByteArrayLiteral("<span class=\"bookref\" data-ref=\"source\">Text</span>") << QString() << false;
        QTest::newRow("legacy-with-id") << QByteArrayLiteral("<span id=\"actual-id\" class=\"bookref\" data-ref=\"source\">Text</span>")
                                        << QStringLiteral("actual-id") << false;
        QTest::newRow("pdf") << QByteArray() << QString() << true;
    }

    void sourceEditorRequiresExistingEpubId()
    {
        QFETCH(QByteArray, markup);
        QFETCH(QString, expectedId);
        QFETCH(bool, pdf);
        QTemporaryDir dir;
        const auto source = pdf ? makePdf(dir.filePath(QStringLiteral("source.pdf")), QStringLiteral("source"), {{QStringLiteral("source"), 0}})
                                : makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("source"), markup);
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("target"), {});
        const auto report = BookReferenceCheck::inspect(
            source.filename,
            {source, target},
            {reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("absent.xhtml"))});
        const auto row = report.value(QStringLiteral("results")).toList().first().toMap();
        const auto editor = row.value(QStringLiteral("sourceEditor")).toMap();
        QCOMPARE(editor.isEmpty(), expectedId.isEmpty());
        if (!expectedId.isEmpty()) {
            QCOMPARE(editor.value(QStringLiteral("anchorId")).toString(), expectedId);
            QCOMPARE(editor.value(QStringLiteral("fileName")).toString(), source.filename);
        }
    }

    void opensCrossReferenceSourceAtAnchor_data()
    {
        QTest::addColumn<bool>("incoming");
        QTest::addColumn<bool>("reimport");
        QTest::addColumn<bool>("fromDialog");
        QTest::addColumn<bool>("brokenTarget");
        QTest::addColumn<bool>("alreadyRunning");
        QTest::newRow("intact-report") << false << false << false << false << false;
        QTest::newRow("broken-target-dialog") << false << false << true << true << false;
        QTest::newRow("incoming-from-pdf-report") << true << false << false << false << false;
        QTest::newRow("reimport-dialog") << false << true << true << true << false;
        QTest::newRow("running-editor-blocks-second-launch") << false << false << false << false << true;
    }

    void opensCrossReferenceSourceAtAnchor()
    {
        QFETCH(bool, incoming);
        QFETCH(bool, reimport);
        QFETCH(bool, fromDialog);
        QFETCH(bool, brokenTarget);
        QFETCH(bool, alreadyRunning);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("source-editor-") + QString::fromLatin1(QTest::currentDataTag());
        const QString anchorId = QStringLiteral("uuid_01a0a4a8-a9f0-7d8d-8933-2bc7a047bb8d");
        const QByteArray body = QByteArrayLiteral("<span id=\"") + anchorId.toUtf8()
            + QByteArrayLiteral("\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"B.S. Vers 5.33\">B.S. 5.33</span>");
        // The rejected working copy must introduce a missing anchor; stale
        // references already absent from the original no longer block imports.
        const QByteArray originalBody = body
            + (reimport ? QByteArrayLiteral("<span id=\"z-missing\" data-role=\"anchor\" data-anchor-type=\"crossref\">Remove this anchor</span>") : QByteArray());
        auto source = makeBook(dir.filePath(QStringLiteral("source with spaces.epub")), prefix + QStringLiteral("-source"), originalBody);
        source.identifier = prefix + QStringLiteral("-source-alias");
        const auto target = incoming ? makePdf(dir.filePath(QStringLiteral("target.pdf")), prefix + QStringLiteral("-target"))
                                     : makeBook(dir.filePath(QStringLiteral("target.epub")), prefix + QStringLiteral("-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        const QString targetLocation = brokenTarget ? QStringLiteral("missing.xhtml")
            : incoming                              ? QStringLiteral("{\"format\":\"pdf\",\"page\":1}")
                                                    : QStringLiteral("OPS/other.xhtml#other");
        database.saveReference(reference(source.identifier, anchorId, target.uniqueIdentifier, targetLocation));
        const auto sourceHash = digest(source.filename);
        const auto targetHash = digest(target.filename);

        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        auto *sessions = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        auto *process = engine.singletonInstance<ExternalProcess *>("org.kde.arianna", "ExternalProcess");
        QVERIFY(sessions && process);
        QVariantMap session;
        QString sessionId;
        QString expectedCopy;
        if (reimport || alreadyRunning) {
            session = sessions->createEditorSession(source.uniqueIdentifier, source.filename);
            sessionId = session.value(QStringLiteral("sessionId")).toString();
            expectedCopy = session.value(QStringLiteral("workingCopyPath")).toString();
            QVERIFY(!expectedCopy.isEmpty());
        }
        if (reimport) {
            database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("z-missing"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml")));
            makeBook(expectedCopy, source.uniqueIdentifier, body + QByteArrayLiteral("<p>Keep the rejected edits</p>"));
            QCOMPARE(sessions->finishSession(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        }
        const auto copyHash = expectedCopy.isEmpty() ? QByteArray() : digest(expectedCopy);
        const QString capturePath = dir.filePath(QStringLiteral("editor-arguments.txt"));
        QFile editor(dir.filePath(QStringLiteral("fake editor.sh")));
        QVERIFY(editor.open(QIODevice::WriteOnly));
        editor.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"");
        editor.write(capturePath.toUtf8());
        editor.write("\"\n");
        editor.close();
        QVERIFY(editor.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString previousCommand = Config::self()->editorCommand();
        const auto resetCommand = qScopeGuard([=]() {
            Config::self()->setEditorCommand(previousCommand);
        });
        Config::self()->setEditorCommand(editor.fileName());
        const auto stopEditor = qScopeGuard([process]() {
            process->stop();
        });
        if (alreadyRunning) {
            QSignalSpy started(process, &ExternalProcess::editorSessionProcessStarted);
            QVERIFY(process->startEditorSession(sessionId, QStringLiteral("/bin/sh -c \"exec sleep 30\""), {expectedCopy}));
            QTRY_VERIFY(!started.isEmpty());
        }

        BookReferenceCheck checker;
        checker.start(incoming ? target.filename : source.filename);
        QTRY_VERIFY(!checker.running());
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        engine.rootContext()->setContextProperty(QStringLiteral("testSession"), session);
        QQmlComponent component(&engine);
        const QByteArray dialog = reimport
            ? QByteArrayLiteral(
                  "EpubImportRepairDialog { Component.onCompleted: showForImport(testSession.bookId, testSession.workingCopyPath, testSession.sessionId) }")
            : QByteArrayLiteral(
                  "ReferenceReportDialog { report: testChecker.report; running: testChecker.running; referenceEditor: testChecker; Component.onCompleted: "
                  "open() }");
        component.setData(QByteArrayLiteral("import QtQuick\nimport org.kde.arianna\nWindow { width: 1100; height: 900; visible: true; ") + dialog
                              + QByteArrayLiteral(" }"),
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        auto *activeChecker = reimport ? window->findChild<BookReferenceCheck *>() : &checker;
        QVERIFY(activeChecker);
        QTRY_VERIFY(!activeChecker->running());
        QCOMPARE(activeChecker->report().value(QStringLiteral("success")).toBool(), !brokenTarget && !reimport);
        QQuickItem *openButton = nullptr;
        QTRY_VERIFY(openButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("editReferenceSource"), true));
        if (fromDialog) {
            // Both action rows belong to the same report card.
            auto *editButton = findVisualItem(openButton->parentItem()->parentItem(), QStringLiteral("editCrossReference"));
            QVERIFY(editButton);
            QVERIFY(QMetaObject::invokeMethod(editButton, "clicked"));
            QTRY_VERIFY(openButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("editReferenceSourceInDialog")));
            QVERIFY(openButton->isVisible());
        }
        QVERIFY(!QFileInfo::exists(capturePath));
        QSignalSpy finished(process, &ExternalProcess::editorSessionFinished);
        QSignalSpy blocked(sessions, &EditorSessionStore::editorStartBlocked);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        if (alreadyRunning) {
            // Reuse the owned session while still suppressing a second editor process.
            QCOMPARE(blocked.size(), 0);
            QCOMPARE(sessions->activeSessionForBook(source.uniqueIdentifier).value(QStringLiteral("workingCopyPath")).toString(), expectedCopy);
            QVERIFY(!QFileInfo::exists(capturePath));
            QVERIFY(process->isEditorSessionRunning(sessionId));
            QVERIFY(finished.isEmpty());
            QCOMPARE(digest(source.filename), sourceHash);
            return;
        }
        QTRY_VERIFY(QFileInfo(capturePath).size() > 0);
        if (!alreadyRunning)
            QTRY_VERIFY(!finished.isEmpty());
        QFile captured(capturePath);
        QVERIFY(captured.open(QIODevice::ReadOnly));
        const auto args = QString::fromUtf8(captured.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(args.size(), 3);
        if (reimport) QVERIFY(QFileInfo::exists(args.first()));
        else QTRY_VERIFY(!QFileInfo::exists(args.first()));
        QVERIFY(args.first() != source.filename);
        if (!expectedCopy.isEmpty())
            QCOMPARE(args.first(), expectedCopy);
        QCOMPARE(args.at(1), QStringLiteral("--anchor-id"));
        QCOMPARE(args.at(2), anchorId);
        if (reimport) QCOMPARE(digest(args.first()), copyHash);
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        if (alreadyRunning) {
            QVERIFY(process->isEditorSessionRunning(sessionId));
            QVERIFY(finished.isEmpty());
        }
        QTRY_VERIFY(!activeChecker->running());
    }

    void removesOrphanEpubAnchor_data()
    {
        QTest::addColumn<QByteArray>("anchors");
        QTest::addColumn<int>("removeIndex");
        QTest::newRow("modern") << QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Keep <em>words</em></a>") << 0;
        QTest::newRow("span") << QByteArrayLiteral("<span id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Keep <em>words</em></span>") << 0;
        QTest::newRow("span-without-id") << QByteArrayLiteral("<span data-role=\"anchor\" data-anchor-type=\"crossref\">Keep <em>words</em></span>") << 0;
        QTest::newRow("legacy") << QByteArrayLiteral("<span class=\"bookref\" data-ref=\"orphan\">Keep <em>words</em></span>") << 0;
        QTest::newRow("without-id") << QByteArrayLiteral("<a data-role=\"anchor\" data-anchor-type=\"crossref\">Keep <em>words</em></a>") << 0;
        QTest::newRow("duplicate") << QByteArrayLiteral(
            "<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">First</a>"
            "<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Keep <em>words</em></a>")
                                   << 1;
    }

    void removesOrphanEpubAnchor()
    {
        QFETCH(QByteArray, anchors);
        QFETCH(int, removeIndex);
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("orphan.epub")),
                                   QStringLiteral("remove-epub-") + QString::fromLatin1(QTest::currentDataTag()),
                                   anchors + QByteArrayLiteral("<a href=\"#native\">Native</a><p id=\"native\">Native target</p>"));
        BookDatabase::self().addEntry(book);
        // An unrelated existing broken reference must not prevent orphan cleanup.
        BookDatabase::self().saveReference(
            reference(book.uniqueIdentifier, QStringLiteral("already-missing"), QStringLiteral("absent-book"), QStringLiteral("missing-target")));
        const auto before = digest(book.filename);
        BookReferenceCheck checker;
        checker.start(book.filename);
        QTRY_VERIFY(!checker.running());
        const auto actions = removals(checker.report());
        QCOMPARE(actions.size(), removeIndex + 1);
        const auto result = checker.removeOrphanAnchor(actions.at(removeIndex).toMap());
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QCOMPARE(digest(result.value(QStringLiteral("backupPath")).toString()), before);
        QTRY_VERIFY(!checker.running());
        QCOMPARE(removals(checker.report()).size(), removeIndex);
        QVERIFY(hasCode(checker.report(), QStringLiteral("source-anchor-unresolved")));
        EPubContainer updated(nullptr);
        QVERIFY(updated.openFile(book.filename));
        QDomDocument document;
        QVERIFY(document.setContent(updated.readData(QStringLiteral("OPS/chapter.xhtml"))));
        QCOMPARE(document.elementsByTagName(QStringLiteral("em")).size(), 1);
        QCOMPARE(document.elementsByTagName(QStringLiteral("em")).at(0).toElement().text(), QStringLiteral("words"));
        QVERIFY(document.documentElement().text().simplified().contains(QStringLiteral("Keep words")));
        const auto links = document.elementsByTagName(QStringLiteral("a"));
        QCOMPARE(links.size(), removeIndex + 1);
        QCOMPARE(links.at(links.size() - 1).toElement().attribute(QStringLiteral("href")), QStringLiteral("#native"));
        QCOMPARE(BookDatabase::self().currentBookState(book.uniqueIdentifier)->epubFileHash, digest(book.filename).toHex());
        QVERIFY(!checker.removeOrphanAnchor(actions.at(removeIndex).toMap()).value(QStringLiteral("success")).toBool());
    }

    void refusesStaleOrAssignedOrphan()
    {
        QTemporaryDir dir;
        auto book = makeBook(dir.filePath(QStringLiteral("stale.epub")),
                             QStringLiteral("stale-orphan"),
                             QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Keep</a>"));
        book.identifier = QStringLiteral("stale-orphan-alias");
        BookDatabase::self().addEntry(book);
        BookReferenceCheck checker;
        checker.start(book.filename);
        QTRY_VERIFY(!checker.running());
        const auto removal = removals(checker.report()).first().toMap();
        auto altered = removal;
        altered.insert(QStringLiteral("fileName"), dir.filePath(QStringLiteral("other.epub")));
        const auto before = digest(book.filename);
        QVERIFY(!checker.removeOrphanAnchor(altered).value(QStringLiteral("success")).toBool());
        BookDatabase::self().saveReference(reference(book.identifier, QStringLiteral("orphan"), QStringLiteral("missing-book"), QStringLiteral("target")));
        QVERIFY(!checker.removeOrphanAnchor(removal).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(book.filename), before);
        BookDatabase::self().removeReference(book.identifier, QStringLiteral("orphan"));
        QFile changed(book.filename);
        QVERIFY(changed.open(QIODevice::Append));
        changed.write("\n");
        changed.close();
        const auto modified = digest(book.filename);
        QVERIFY(!checker.removeOrphanAnchor(removal).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(book.filename), modified);
    }

    void preservesAnchorsUsedByIncomingReferencesOrAnnotations_data()
    {
        QTest::addColumn<bool>("annotation");
        QTest::newRow("incoming-alias") << false;
        QTest::newRow("annotation") << true;
    }

    void preservesAnchorsUsedByIncomingReferencesOrAnnotations()
    {
        QFETCH(bool, annotation);
        QTemporaryDir dir;
        const auto id = QStringLiteral("used-orphan-") + QString::fromLatin1(QTest::currentDataTag());
        auto book = makeBook(dir.filePath(QStringLiteral("used.epub")),
                             id,
                             QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Keep</a>"));
        book.identifier = id + QStringLiteral("-alias");
        BookDatabase::self().addEntry(book);
        if (annotation) {
            BookDatabase::self().saveAnnotation(id,
                                                {{QStringLiteral("annotationId"), QStringLiteral("note")},
                                                 {QStringLiteral("anchorId"), QStringLiteral("orphan")},
                                                 {QStringLiteral("text"), QStringLiteral("Keep")}});
        } else {
            const auto other = makeBook(dir.filePath(QStringLiteral("incoming.epub")),
                                        id + QStringLiteral("-other"),
                                        QByteArrayLiteral("<a id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">Source</a>"));
            BookDatabase::self().addEntry(other);
            BookDatabase::self().saveReference(
                reference(other.uniqueIdentifier, QStringLiteral("source"), book.identifier, QStringLiteral("OPS/chapter.xhtml#orphan")));
        }
        BookReferenceCheck checker;
        checker.start(book.filename);
        QTRY_VERIFY(!checker.running());
        const auto before = digest(book.filename);
        const auto result = checker.removeOrphanAnchor(removals(checker.report()).first().toMap());
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(book.filename), before);
    }

    void removesOnlySelectedPdfOrphan()
    {
        QTemporaryDir dir;
        const auto book = makePdf(dir.filePath(QStringLiteral("orphans.pdf")),
                                  QStringLiteral("remove-pdf-orphan"),
                                  {{QStringLiteral("same-id"), 0}, {QStringLiteral("same-id"), 1}, {QString(), 1}});
        BookDatabase::self().addEntry(book);
        BookReferenceCheck checker;
        checker.start(book.filename);
        QTRY_VERIFY(!checker.running());
        auto actions = removals(checker.report());
        QCOMPARE(actions.size(), 3);
        const auto before = digest(book.filename);
        const auto result = checker.removeOrphanAnchor(actions.at(1).toMap());
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QCOMPARE(digest(result.value(QStringLiteral("backupPath")).toString()), before);
        QTRY_VERIFY(!checker.running());
        const auto index = readPdfReferenceIndex(book.filename);
        QCOMPARE(index.pageCount, 2);
        QCOMPARE(index.anchors.size(), 2);
        QCOMPARE(index.anchors.first().page, 0);
        QCOMPARE(index.anchors.first().id, QStringLiteral("same-id"));
        QCOMPARE(index.anchors.last().id, QString());
        PoDoFo::PdfMemDocument pdf;
        pdf.Load(book.filename.toStdString());
        QCOMPARE(pdf.GetPages().GetPageAt(0).GetAnnotations().GetCount(), 2u); // Ordinary link retained.
        actions = removals(checker.report());
        QVERIFY(checker.removeOrphanAnchor(actions.last().toMap()).value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(readPdfReferenceIndex(book.filename).anchors.size(), 1);
    }

    void staleDeletionCannotRemoveTheNextDuplicate()
    {
        QTemporaryDir dir;
        const auto book = makePdf(dir.filePath(QStringLiteral("duplicates.pdf")),
                                  QStringLiteral("duplicate-pdf-orphans"),
                                  {{QStringLiteral("same-id"), 0}, {QStringLiteral("same-id"), 0}});
        BookDatabase::self().addEntry(book);
        BookReferenceCheck checker;
        checker.start(book.filename);
        QTRY_VERIFY(!checker.running());
        const auto removal = removals(checker.report()).first().toMap();
        QVERIFY(checker.removeOrphanAnchor(removal).value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(removals(checker.report()).size(), 1);
        QVERIFY(!checker.removeOrphanAnchor(removal).value(QStringLiteral("success")).toBool());
        QCOMPARE(readPdfReferenceIndex(book.filename).anchors.size(), 1);
    }

    void orphanRemovalRequiresConfirmation()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("confirm.epub")),
                                   QStringLiteral("confirmed-orphan"),
                                   QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Keep</a>"));
        BookDatabase::self().addEntry(book);
        BookReferenceCheck checker;
        checker.start(book.filename);
        QTRY_VERIFY(!checker.running());
        const auto before = digest(book.filename);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1100; height: 900; visible: true
                ReferenceReportDialog {
                    report: testChecker.report
                    running: testChecker.running
                    onRemoveOrphanRequested: removal => testChecker.removeOrphanAnchor(removal)
                    Component.onCompleted: open()
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = findVisualItem(quickWindow->contentItem(), QStringLiteral("removeOrphanAnchor")));
        QVERIFY(button->isVisible());
        auto *confirmation = window->findChild<QObject *>(QStringLiteral("removeOrphanConfirmation"));
        QVERIFY(confirmation);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(digest(book.filename), before);
        QVERIFY(QMetaObject::invokeMethod(confirmation, "reject"));
        QCOMPARE(digest(book.filename), before);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QVERIFY(QMetaObject::invokeMethod(confirmation, "accept"));
        QTRY_VERIFY(!checker.running());
        QVERIFY(removals(checker.report()).isEmpty());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
    }

    void restoresMissingRemarkAnchorInWorkingCopy()
    {
        QTemporaryDir dir;
        const QByteArray heading = QByteArrayLiteral(
            "<h2><a href=\"Contents.html#rChap33\" id=\"Chap33\">O<small>N THE</small> N<small>UMBER OF</small> "
            "P<small>RIME</small> N<small>UMBERS LESS THAN A</small> G<small>IVEN</small> Q<small>UANTITY</small></a>"
            "<a href=\"Chapter33.html#rChap33_ufn01\" id=\"Chap33_ufn01\">*</a></h2>");
        const QString anchorId = QStringLiteral("01a0f37a-34f2-76f7-94d8-4d12226703af");
        const QByteArray anchoredHeading = QByteArrayLiteral("<div data-role=\"anchor\" data-anchor-type=\"annotation\" id=\"uuid_")
            + anchorId.toUtf8() + QByteArrayLiteral("\">") + heading + QByteArrayLiteral("</div>");
        const auto book = makeBook(dir.filePath(QStringLiteral("missing-remark.epub")), QStringLiteral("remark-recovery"), anchoredHeading);
        BookDatabase::self().addEntry(book);
        QVERIFY(BookTruthStore().openBook(book.uniqueIdentifier).success);
        const auto originalHash = digest(book.filename);
        const auto originalState = BookDatabase::self().currentBookState(book.uniqueIdentifier)->stateId;
        const QVariantMap note{{QStringLiteral("annotationId"), QStringLiteral("01a0f37a-4074-7114-9068-37937895c154")},
                               {QStringLiteral("anchorId"), anchorId},
                               {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/4/2,/2[Chap33]/1:0,/4[Chap33_ufn01]/1:1)")},
                               {QStringLiteral("text"), QStringLiteral("ON THE NUMBER OF PRIME NUMBERS LESS THAN A GIVEN QUANTITY*")},
                               {QStringLiteral("note"), QStringLiteral("A saved remark")}};
        BookDatabase::self().saveAnnotation(book.uniqueIdentifier, note);
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY(!session.value(QStringLiteral("error")).toBool());
        const auto copyPath = session.value(QStringLiteral("workingCopyPath")).toString();
        const auto sessionId = session.value(QStringLiteral("sessionId")).toString();
        makeBook(copyPath, book.uniqueIdentifier, heading + QByteArrayLiteral("<p>Edited working copy</p>"));
        QCOMPARE(sessions.finishSession(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        BookReferenceCheck checker;
        checker.startWorkingCopy(book.uniqueIdentifier, copyPath);
        QTRY_VERIFY(!checker.running());
        QVERIFY(hasCode(checker.report(), QStringLiteral("annotation-anchor-unresolved")));
        QVariantMap repair;
        for (const auto &value : checker.report().value(QStringLiteral("results")).toList()) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("annotationId")) == note.value(QStringLiteral("annotationId")))
                repair = row.value(QStringLiteral("annotationRepair")).toMap();
        }
        QVERIFY2(!repair.isEmpty(), QJsonDocument::fromVariant(checker.report()).toJson().constData());
        QCOMPARE(repair.value(QStringLiteral("fileName")).toString(), copyPath);
        const auto copyHash = digest(copyPath);
        auto forged = repair;
        forged.insert(QStringLiteral("text"), QStringLiteral("Different text"));
        QVERIFY(!checker.restoreAnnotationAnchor(forged).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(copyPath), copyHash);
        {
            QQmlEngine engine;
            engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
            engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
            QQmlComponent component(&engine);
            component.setData(QByteArrayLiteral("import QtQuick\nimport org.kde.arianna\n"
                                                "Window { width: 1100; height: 900; visible: true; "
                                                "ReferenceReportDialog { report: testChecker.report; running: testChecker.running; "
                                                "referenceEditor: testChecker; Component.onCompleted: open() } }"), QUrl());
            QTRY_VERIFY(component.status() != QQmlComponent::Loading);
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            std::unique_ptr<QObject> window(component.create());
            QVERIFY2(window != nullptr, qPrintable(component.errorString()));
            auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
            QVERIFY(quickWindow);
            QQuickItem *button = nullptr;
            QTRY_VERIFY(button = findVisualItem(quickWindow->contentItem(), QStringLiteral("restoreRemarkAnchor")));
            QVERIFY(button->isVisible());
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
            auto *confirmation = window->findChild<QObject *>(QStringLiteral("restoreRemarkConfirmation"));
            QVERIFY(confirmation);
            QTRY_VERIFY(confirmation->property("visible").toBool());
            QVERIFY(QMetaObject::invokeMethod(confirmation, "reject"));
            QCOMPARE(digest(copyPath), copyHash);
        }
        const auto result = checker.restoreAnnotationAnchor(repair);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(result.value(QStringLiteral("backupPath")).toString()), copyHash);
        QCOMPARE(digest(book.filename), originalHash);
        QCOMPARE(BookDatabase::self().currentBookState(book.uniqueIdentifier)->stateId, originalState);
        QVERIFY(!hasCode(checker.report(), QStringLiteral("annotation-anchor-unresolved")));
        QVERIFY(!checker.restoreAnnotationAnchor(repair).value(QStringLiteral("success")).toBool());
        QCOMPARE(BookDatabase::self().loadAnnotations(book.uniqueIdentifier).size(), 1);
        QCOMPARE(sessions.importNow(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QCOMPARE(digest(book.filename), digest(copyPath));
    }

    void workingCopyCorrectionsDoNotImport()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("original.epub")),
                                   QStringLiteral("working-copy-repair"),
                                   QByteArrayLiteral("<a id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">Original</a>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("working-copy-target"), {});
        BookDatabase::self().addEntry(book);
        BookDatabase::self().addEntry(target);
        BookDatabase::self().saveReference(
            reference(book.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/target%20file.xhtml#target")));
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY(!session.value(QStringLiteral("error")).toBool());
        const QString sessionId = session.value(QStringLiteral("sessionId")).toString();
        const QString copyPath = session.value(QStringLiteral("workingCopyPath")).toString();
        const auto originalHash = digest(book.filename);
        const auto originalState = BookDatabase::self().currentBookState(book.uniqueIdentifier)->stateId;
        makeBook(copyPath, book.uniqueIdentifier, QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Edited text</a>"));
        QCOMPARE(sessions.finishSession(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        const auto editedHash = digest(copyPath);
        const auto resumed = sessions.resumeEditorSession(sessionId);
        QVERIFY(!resumed.value(QStringLiteral("error")).toBool());
        QCOMPARE(resumed.value(QStringLiteral("workingCopyPath")).toString(), copyPath);
        QCOMPARE(digest(copyPath), editedHash);
        QCOMPARE(sessions.finishSession(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));

        BookReferenceCheck checker;
        checker.startWorkingCopy(book.uniqueIdentifier, copyPath);
        QTRY_VERIFY(!checker.running());
        QVERIFY(hasCode(checker.report(), QStringLiteral("source-anchor-unresolved")));
        bool hasCopyRepair = false;
        for (const auto &row : checker.report().value(QStringLiteral("results")).toList()) {
            for (const auto &repair : row.toMap().value(QStringLiteral("repairs")).toList()) {
                hasCopyRepair |= repair.toMap().value(QStringLiteral("fileName")).toString() == copyPath;
            }
        }
        QVERIFY(hasCopyRepair);
        const auto result = checker.removeOrphanAnchor(removals(checker.report()).first().toMap());
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(checker.report().value(QStringLiteral("fileName")).toString(), copyPath);
        QVERIFY(removals(checker.report()).isEmpty());
        QCOMPARE(digest(result.value(QStringLiteral("backupPath")).toString()), editedHash);
        QCOMPARE(digest(book.filename), originalHash);
        QCOMPARE(BookDatabase::self().currentBookState(book.uniqueIdentifier)->stateId, originalState);
        QCOMPARE(sessions.importNow(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        makeBook(copyPath, book.uniqueIdentifier, QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">Edited text</span>"));
        checker.startWorkingCopy(book.uniqueIdentifier, copyPath);
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        QCOMPARE(sessions.importNow(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QCOMPARE(digest(book.filename), digest(copyPath));

        const auto newSession = sessions.createOrReuseEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY(newSession.value(QStringLiteral("blocked")).toBool());
        QVERIFY(QFileInfo::exists(copyPath));
    }

    void activeFileCorrectionPreservesPendingImportState()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("external.epub")), QStringLiteral("external-import-repair"), {});
        BookDatabase::self().addEntry(book);
        const auto state = BookTruthStore().openBook(book.uniqueIdentifier);
        QVERIFY(state.success);
        makeBook(book.filename,
                 book.uniqueIdentifier,
                 QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">External edit</a>"));
        BookReferenceCheck checker;
        checker.startWorkingCopy(book.uniqueIdentifier, book.filename);
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.removeOrphanAnchor(removals(checker.report()).first().toMap()).value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(BookDatabase::self().currentBookState(book.uniqueIdentifier)->stateId, state.state->stateId);
        const auto imported = BookTruthStore().commitActiveFileChangeIfNeeded(book.uniqueIdentifier);
        QVERIFY(imported.success);
        QVERIFY(imported.newStateId != state.state->stateId);
    }

    void importRepairDialogRemovesThenRetries()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("reimport.epub")), QStringLiteral("reimport-dialog"), {});
        BookDatabase::self().addEntry(book);
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        auto *sessions = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        QVERIFY(sessions);
        const auto session = sessions->createEditorSession(book.uniqueIdentifier, book.filename);
        QVERIFY(!session.value(QStringLiteral("error")).toBool());
        const QString copyPath = session.value(QStringLiteral("workingCopyPath")).toString();
        makeBook(copyPath, book.uniqueIdentifier, QByteArrayLiteral("<a id=\"orphan\" data-role=\"anchor\" data-anchor-type=\"crossref\">Retain my edits</a>"));
        const auto originalHash = digest(book.filename);
        engine.rootContext()->setContextProperty(QStringLiteral("testSession"), session);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1100; height: 900; visible: true
                EpubImportRepairDialog {
                    objectName: "importDialog"
                    Component.onCompleted: showForImport(testSession.bookId, testSession.workingCopyPath, testSession.sessionId)
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *checker = window->findChild<BookReferenceCheck *>();
        QVERIFY(checker);
        QTRY_VERIFY(!checker->running());
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *removeButton = nullptr;
        QTRY_VERIFY(removeButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("removeOrphanAnchor")));
        QVERIFY(QMetaObject::invokeMethod(removeButton, "clicked"));
        auto *confirmation = window->findChild<QObject *>(QStringLiteral("removeOrphanConfirmation"));
        QVERIFY(confirmation);
        QVERIFY(QMetaObject::invokeMethod(confirmation, "accept"));
        QTRY_VERIFY(!checker->running());
        QVERIFY(removals(checker->report()).isEmpty());
        QCOMPARE(digest(book.filename), originalHash);
        auto *retry = findVisualItem(quickWindow->contentItem(), QStringLiteral("retryEpubImport"));
        QVERIFY(retry);
        QVERIFY(retry->isVisible());
        QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
        QTRY_VERIFY(!checker->running());
        QCOMPARE(digest(book.filename), digest(copyPath));
        QVERIFY(digest(book.filename) != originalHash);
    }

    void importRepairReopensRejectedWorkingCopy()
    {
        QTemporaryDir dir;
        const auto book = makeBook(dir.filePath(QStringLiteral("reopen.epub")),
                                   QStringLiteral("reopen-rejected-copy"),
                                   QByteArrayLiteral("<a id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">Original</a>"));
        BookDatabase::self().addEntry(book);
        BookDatabase::self().saveReference(
            reference(book.uniqueIdentifier, QStringLiteral("source"), QStringLiteral("absent-target"), QStringLiteral("target")));
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        auto *sessions = engine.singletonInstance<EditorSessionStore *>("org.kde.arianna", "EditorSessionStore");
        QVERIFY(sessions);
        const auto session = sessions->createEditorSession(book.uniqueIdentifier, book.filename);
        const QString sessionId = session.value(QStringLiteral("sessionId")).toString();
        const QString copyPath = session.value(QStringLiteral("workingCopyPath")).toString();
        QVERIFY(!copyPath.isEmpty());
        makeBook(copyPath, book.uniqueIdentifier, QByteArrayLiteral("<p>Keep the rejected edits</p>"));
        QCOMPARE(sessions->finishSession(sessionId).value(QStringLiteral("status")).toString(), QStringLiteral("anchor-validation-failed"));
        const auto originalHash = digest(book.filename);
        const auto editedHash = digest(copyPath);

        const QString capturePath = dir.filePath(QStringLiteral("opened-copy.txt"));
        QFile editor(dir.filePath(QStringLiteral("fake-editor.sh")));
        QVERIFY(editor.open(QIODevice::WriteOnly));
        editor.write("#!/bin/sh\nprintf '%s' \"$1\" > \"");
        editor.write(capturePath.toUtf8());
        editor.write("\"\n");
        editor.close();
        QVERIFY(editor.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString previousCommand = Config::self()->editorCommand();
        const auto resetCommand = qScopeGuard([previousCommand]() {
            Config::self()->setEditorCommand(previousCommand);
        });
        Config::self()->setEditorCommand(editor.fileName());
        engine.rootContext()->setContextProperty(QStringLiteral("testSession"), session);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1100; height: 900; visible: true
                EpubImportRepairDialog {
                    Component.onCompleted: showForImport(testSession.bookId, testSession.workingCopyPath, testSession.sessionId)
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *openButton = nullptr;
        QQuickItem *copyButton = nullptr;
        QTRY_VERIFY(openButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("openMissingAnchorEditor")));
        QTRY_VERIFY(copyButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("copyMissingAnchorHtml")));
        QVERIFY(QMetaObject::invokeMethod(copyButton, "clicked"));
        QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("id=\"source\"")));
        auto *process = engine.singletonInstance<ExternalProcess *>("org.kde.arianna", "ExternalProcess");
        QVERIFY(process);
        QSignalSpy finished(process, &ExternalProcess::editorSessionFinished);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QTRY_VERIFY(!finished.isEmpty());
        QFile captured(capturePath);
        QVERIFY(captured.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(captured.readAll()), copyPath);
        QCOMPARE(digest(copyPath), editedHash);
        QCOMPARE(digest(book.filename), originalHash);
    }

    void editsOnlyTheReferenceDestination()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("edit-source"), {});
        auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("edit-target"), {});
        target.identifier = QStringLiteral("edit-target-alias");
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        auto ref = reference(source.uniqueIdentifier, QStringLiteral("missing-source"), target.identifier, QStringLiteral("missing.xhtml"));
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Retain this title"));
        ref.insert(QStringLiteral("targetPreviewHtml"), QStringLiteral("<p>Old preview</p>"));
        ref.insert(QStringLiteral("created"), QStringLiteral("2025-01-01T00:00:00Z"));
        database.saveReference(ref);
        const auto sourceHash = digest(source.filename);
        const auto targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto edit = referenceEdit(checker.report(), QStringLiteral("missing-source"));
        QVERIFY(!edit.isEmpty());
        bool selected = false;
        for (const auto &value : checker.referenceTargetBooks(edit)) {
            const auto book = value.toMap();
            QVERIFY(book.value(QStringLiteral("bookId")).toString() != source.uniqueIdentifier);
            if (book.value(QStringLiteral("selected")).toBool()) {
                QCOMPARE(book.value(QStringLiteral("bookId")).toString(), target.uniqueIdentifier);
                selected = true;
            }
        }
        QVERIFY(selected);
        const QString destination = QStringLiteral("OPS/target%20file.xhtml#encoded%23id");
        QVERIFY(checker.validateReferenceTarget(edit, target.uniqueIdentifier, destination).value(QStringLiteral("success")).toBool());
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("missing-source")).value(QStringLiteral("targetLocation")),
                 ref.value(QStringLiteral("targetLocation")));
        const auto result = checker.saveReferenceTarget(edit, target.uniqueIdentifier, QStringLiteral("  ") + destination + QStringLiteral("  "));
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        const auto saved = database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("missing-source"));
        QCOMPARE(saved.value(QStringLiteral("targetBookId")).toString(), target.uniqueIdentifier);
        QCOMPARE(saved.value(QStringLiteral("targetLocation")).toString(), destination);
        QCOMPARE(saved.value(QStringLiteral("sourceAnchorTitle")), ref.value(QStringLiteral("sourceAnchorTitle")));
        QCOMPARE(saved.value(QStringLiteral("created")), ref.value(QStringLiteral("created")));
        QVERIFY(!saved.value(QStringLiteral("modified")).toString().isEmpty());
        QVERIFY(saved.value(QStringLiteral("targetPreviewHtml")).toString().isEmpty());
        QVERIFY(hasCode(checker.report(), QStringLiteral("source-anchor-unresolved")));
        QVERIFY(!hasCode(checker.report(), QStringLiteral("missing-target-file")));
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
    }

    void validatesEditedTargets_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("location");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("file") << QStringLiteral("epub") << QStringLiteral("OPS/target%20file.xhtml") << true;
        QTest::newRow("fragment") << QStringLiteral("epub") << QStringLiteral("OPS/target%20file.xhtml#target") << true;
        QTest::newRow("empty") << QStringLiteral("epub") << QString() << false;
        QTest::newRow("missing-file") << QStringLiteral("epub") << QStringLiteral("OPS/missing.xhtml") << false;
        QTest::newRow("wrong-file") << QStringLiteral("epub") << QStringLiteral("OPS/other.xhtml#target") << false;
        QTest::newRow("missing-anchor") << QStringLiteral("epub") << QStringLiteral("OPS/other.xhtml#absent") << false;
        QTest::newRow("duplicate-anchor") << QStringLiteral("epub") << QStringLiteral("OPS/target%20file.xhtml#duplicate") << false;
        QTest::newRow("unknown-cfi") << QStringLiteral("epub") << QStringLiteral("epubcfi(/6/998!/4/2)") << false;
        QTest::newRow("external") << QStringLiteral("epub") << QStringLiteral("https://example.org/chapter") << false;
        QTest::newRow("pdf-page") << QStringLiteral("pdf") << QStringLiteral("{\"format\":\"pdf\",\"page\":1,\"x\":0.2,\"y\":0.3}") << true;
        QTest::newRow("pdf-missing-page") << QStringLiteral("pdf") << QStringLiteral("{\"format\":\"pdf\",\"page\":8}") << false;
        QTest::newRow("pdf-invalid-position") << QStringLiteral("pdf") << QStringLiteral("{\"format\":\"pdf\",\"page\":1,\"x\":2,\"y\":0.3}") << false;
        QTest::newRow("locked-pdf") << QStringLiteral("locked") << QStringLiteral("{\"format\":\"pdf\",\"page\":1}") << false;
        QTest::newRow("unknown-book") << QStringLiteral("missing") << QStringLiteral("OPS/other.xhtml") << false;
        QTest::newRow("internal") << QStringLiteral("source") << QStringLiteral("OPS/other.xhtml") << false;
    }

    void validatesEditedTargets()
    {
        QFETCH(QString, type);
        QFETCH(QString, location);
        QFETCH(bool, accepted);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("validate-edit-") + QString::fromLatin1(QTest::currentDataTag());
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), prefix + QStringLiteral("-source"), {});
        const auto epub = makeBook(dir.filePath(QStringLiteral("target.epub")), prefix + QStringLiteral("-epub"), {});
        const auto pdf = makePdf(dir.filePath(QStringLiteral("target.pdf")), prefix + QStringLiteral("-pdf"));
        const auto locked = makePdf(dir.filePath(QStringLiteral("locked.pdf")), prefix + QStringLiteral("-locked"), {}, QStringLiteral("reader"));
        auto &database = BookDatabase::self();
        for (const auto &book : {source, epub, pdf, locked})
            database.addEntry(book);
        const auto ref = reference(source.uniqueIdentifier, QStringLiteral("source"), epub.uniqueIdentifier, QStringLiteral("missing.xhtml"));
        database.saveReference(ref);
        const auto before = database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source"));
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto edit = referenceEdit(checker.report(), QStringLiteral("source"));
        const QString targetId = prefix + QLatin1Char('-') + type;
        const auto validation = checker.validateReferenceTarget(edit, targetId, location);
        QCOMPARE(validation.value(QStringLiteral("success")).toBool(), accepted);
        const auto repair = validation.value(QStringLiteral("targetAnchorRepair")).toMap();
        const bool missingAnchor = QByteArray(QTest::currentDataTag()) == "missing-anchor" || QByteArray(QTest::currentDataTag()) == "wrong-file";
        QCOMPARE(!repair.isEmpty(), missingAnchor);
        if (missingAnchor) {
            QCOMPARE(repair.value(QStringLiteral("fileName")).toString(), epub.filename);
            QCOMPARE(repair.value(QStringLiteral("bookId")).toString(), targetId);
            QCOMPARE(repair.value(QStringLiteral("documentFile")).toString(), QStringLiteral("OPS/other.xhtml"));
        }
        const auto result = checker.saveReferenceTarget(edit, targetId, location);
        QCOMPARE(result.value(QStringLiteral("success")).toBool(), accepted);
        QTRY_VERIFY(!checker.running());
        const auto after = database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source"));
        if (accepted) {
            QCOMPARE(after.value(QStringLiteral("targetBookId")).toString(), targetId);
            QCOMPARE(after.value(QStringLiteral("targetLocation")).toString(), location);
        } else {
            QCOMPARE(after, before);
            QVERIFY(!result.value(QStringLiteral("message")).toString().isEmpty());
        }
    }

    void targetEditsRejectStaleResults()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("stale-edit-source"), {});
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("stale-edit-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        const auto ref = reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("missing.xhtml"));
        database.saveReference(ref);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto edit = referenceEdit(checker.report(), QStringLiteral("source"));
        const QString location = QStringLiteral("OPS/other.xhtml#other");
        QVERIFY(checker.validateReferenceTarget(edit, target.uniqueIdentifier, location).value(QStringLiteral("success")).toBool());
        QVERIFY(QFile::remove(target.filename));
        QVERIFY(!checker.saveReferenceTarget(edit, target.uniqueIdentifier, location).value(QStringLiteral("success")).toBool());
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("targetLocation")),
                 ref.value(QStringLiteral("targetLocation")));
        makeBook(target.filename, target.uniqueIdentifier, {});
        const auto concurrent = reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/chapter.xhtml"));
        database.saveReference(concurrent);
        QVERIFY(!checker.saveReferenceTarget(edit, target.uniqueIdentifier, location).value(QStringLiteral("success")).toBool());
        bool conflict = false;
        QVERIFY(!database.updateReferenceTarget(edit, target.uniqueIdentifier, location, &conflict));
        QVERIFY(conflict);
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("targetLocation")),
                 concurrent.value(QStringLiteral("targetLocation")));
        database.removeReference(source.uniqueIdentifier, QStringLiteral("source"));
        QVERIFY(!checker.saveReferenceTarget(edit, target.uniqueIdentifier, location).value(QStringLiteral("success")).toBool());
        QVERIFY(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).isEmpty());
    }

    void repairsEmptyStoredTarget()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("empty-edit-source"), {});
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("empty-edit-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("missing.xhtml")));
        // Incomplete rows with an empty destination must remain repairable.
        auto connection = QSqlDatabase::database();
        QVERIFY(connection.isOpen());
        QSqlQuery query(connection);
        query.prepare(QStringLiteral("UPDATE \"references\" SET targetBookId = '', targetLocation = '' WHERE sourceBookId = :bookId"));
        query.bindValue(QStringLiteral(":bookId"), source.uniqueIdentifier);
        QVERIFY(query.exec());
        query.finish();
        connection.close();
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto edit = referenceEdit(checker.report(), QStringLiteral("source"));
        QVERIFY(!edit.isEmpty());
        const auto result = checker.saveReferenceTarget(edit, target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#other"));
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("targetLocation")).toString(),
                 QStringLiteral("OPS/other.xhtml#other"));
    }

    void editsRequireAnAuthoritativeTargetDuringReimport()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("import-edit-source"), {});
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("import-edit-target"), {});
        const auto working = makeBook(dir.filePath(QStringLiteral("working.epub")), target.uniqueIdentifier, QByteArrayLiteral("<p id=\"unimported\">New</p>"));
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("missing.xhtml")));
        BookReferenceCheck checker;
        checker.startWorkingCopy(target.uniqueIdentifier, working.filename);
        QTRY_VERIFY(!checker.running());
        const auto edit = referenceEdit(checker.report(), QStringLiteral("source"));
        QVERIFY(!edit.isEmpty());
        QVERIFY(!checker.saveReferenceTarget(edit, target.uniqueIdentifier, QStringLiteral("OPS/chapter.xhtml#unimported"))
                     .value(QStringLiteral("success"))
                     .toBool());
        QVERIFY(checker.saveReferenceTarget(edit, target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#other")).value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(checker.report().value(QStringLiteral("fileName")).toString(), working.filename);
    }

    void editsTargetsFromReportDialog_data()
    {
        QTest::addColumn<bool>("reimport");
        QTest::newRow("maintenance") << false;
        QTest::newRow("reimport") << true;
    }

    void editsTargetsFromReportDialog()
    {
        QFETCH(bool, reimport);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("dialog-edit-") + QString::fromLatin1(QTest::currentDataTag());
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), prefix + QStringLiteral("-source"), {});
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), prefix + QStringLiteral("-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("missing.xhtml")));
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        engine.rootContext()->setContextProperty(QStringLiteral("testBookId"), source.uniqueIdentifier);
        engine.rootContext()->setContextProperty(QStringLiteral("testFileName"), source.filename);
        QQmlComponent component(&engine);
        const QByteArray dialog = reimport ? QByteArrayLiteral("EpubImportRepairDialog { Component.onCompleted: showForImport(testBookId, testFileName, '') }")
                                           : QByteArrayLiteral(
                                                 "ReferenceReportDialog { report: testChecker.report; running: testChecker.running; referenceEditor: "
                                                 "testChecker; Component.onCompleted: open() }");
        component.setData(QByteArrayLiteral("import QtQuick\nimport org.kde.arianna\nWindow { width: 1100; height: 900; visible: true; ") + dialog
                              + QByteArrayLiteral(" }"),
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *editButton = nullptr;
        QTRY_VERIFY(editButton = findVisualItem(quickWindow->contentItem(), QStringLiteral("editCrossReference")));
        QVERIFY(editButton->isVisible());
        QVERIFY(QMetaObject::invokeMethod(editButton, "clicked"));
        auto *targetDialog = window->findChild<QObject *>(QStringLiteral("referenceTargetDialog"));
        QVERIFY(targetDialog);
        auto *location = targetDialog->findChild<QObject *>(QStringLiteral("referenceTargetLocation"));
        auto *validate = targetDialog->findChild<QObject *>(QStringLiteral("validateReferenceTarget"));
        auto *save = targetDialog->findChild<QObject *>(QStringLiteral("saveReferenceTarget"));
        QVERIFY(location && validate && save);
        location->setProperty("text", QStringLiteral("OPS/other.xhtml#absent"));
        QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
        QVERIFY(targetDialog->property("visible").toBool());
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("targetLocation")).toString(),
                 QStringLiteral("missing.xhtml"));
        location->setProperty("text", QStringLiteral("OPS/other.xhtml#other"));
        QVERIFY(QMetaObject::invokeMethod(validate, "clicked"));
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("targetLocation")).toString(),
                 QStringLiteral("missing.xhtml"));
        QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
        QTRY_VERIFY(!targetDialog->property("visible").toBool());
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("targetLocation")).toString(),
                 QStringLiteral("OPS/other.xhtml#other"));
        auto *activeChecker = reimport ? window->findChild<BookReferenceCheck *>() : &checker;
        QVERIFY(activeChecker);
        QTRY_VERIFY(!activeChecker->running());
    }

    void editsEpubAnchorTitles_data()
    {
        QTest::addColumn<QByteArray>("tag");
        QTest::addColumn<QString>("newTitle");
        QTest::addColumn<bool>("incoming");
        QTest::newRow("span") << QByteArrayLiteral("span") << QStringLiteral("S.B. 3.2.2") << false;
        QTest::newRow("existing-a") << QByteArrayLiteral("a") << QStringLiteral("S.B. 3.2.2") << false;
        QTest::newRow("incoming") << QByteArrayLiteral("span") << QStringLiteral("S.B. 3.2.2") << true;
        QTest::newRow("prefixed-span") << QByteArrayLiteral("x:span") << QStringLiteral("Titel \"<&> – śrī") << false;
        QTest::newRow("empty-removes-attribute") << QByteArrayLiteral("span") << QStringLiteral("  ") << false;
    }

    void editsEpubAnchorTitles()
    {
        QFETCH(QByteArray, tag);
        QFETCH(QString, newTitle);
        QFETCH(bool, incoming);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("title-") + QString::fromLatin1(QTest::currentDataTag());
        const QByteArray body = QByteArrayLiteral("<p>Before <") + tag
            + QByteArrayLiteral(" xmlns:x=\"http://www.w3.org/1999/xhtml\" id=\"source\" class=\"keep\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"S.B. 3.2.1\"><em>S.B.</em> <strong>3.2.2</strong></")
            + tag + QByteArrayLiteral("> after</p>");
        auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), prefix + QStringLiteral("-source"), body);
        source.identifier = prefix + QStringLiteral("-alias");
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), prefix + QStringLiteral("-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        auto ref = reference(source.identifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#other"));
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Different database title"));
        ref.insert(QStringLiteral("targetPreviewHtml"), QStringLiteral("<p>Keep preview</p>"));
        ref.insert(QStringLiteral("created"), QStringLiteral("2025-01-01T00:00:00Z"));
        database.saveReference(ref);
        const auto beforeHash = digest(source.filename);
        const auto targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(incoming ? target.filename : source.filename);
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        const auto edit = titleEdit(checker.report(), QStringLiteral("source"));
        QVERIFY(!edit.isEmpty());
        QCOMPARE(edit.value(QStringLiteral("htmlTitle")).toString(), QStringLiteral("S.B. 3.2.1"));
        QCOMPARE(edit.value(QStringLiteral("fileName")).toString(), source.filename);
        const auto result = checker.saveReferenceTitle(edit, newTitle);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(result.value(QStringLiteral("backupPath")).toString()), beforeHash);
        QCOMPARE(digest(target.filename), targetHash);
        QCOMPARE(database.currentBookState(source.uniqueIdentifier)->epubFileHash, digest(source.filename).toHex());
        const auto saved = database.loadReferenceBySource(source.identifier, QStringLiteral("source"));
        QCOMPARE(saved.value(QStringLiteral("sourceAnchorTitle")).toString(), newTitle.trimmed());
        for (const auto &key : {"sourceBookId", "sourceAnchorId", "targetBookId", "targetLocation", "targetPreviewHtml", "created"}) {
            QCOMPARE(saved.value(QString::fromLatin1(key)), ref.value(QString::fromLatin1(key)));
        }
        EPubContainer epub(nullptr);
        QVERIFY(epub.openFile(source.filename));
        QDomDocument document;
        QVERIFY(document.setContent(epub.readData(QStringLiteral("OPS/chapter.xhtml")), QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
        const auto paragraph = document.elementsByTagName(QStringLiteral("p")).at(0).toElement();
        QCOMPARE(paragraph.text(), QStringLiteral("Before S.B. 3.2.2 after"));
        const auto anchor = paragraph.firstChildElement();
        QCOMPARE(anchor.tagName(), QString::fromLatin1(tag));
        QCOMPARE(anchor.attribute(QStringLiteral("title")), newTitle.trimmed());
        QCOMPARE(anchor.hasAttribute(QStringLiteral("title")), !newTitle.trimmed().isEmpty());
        QCOMPARE(anchor.attribute(QStringLiteral("class")), QStringLiteral("keep"));
        QCOMPARE(anchor.attribute(QStringLiteral("data-anchor-type")), QStringLiteral("crossref"));
        QCOMPARE(anchor.firstChildElement().tagName(), QStringLiteral("em"));
        QCOMPARE(anchor.lastChildElement().tagName(), QStringLiteral("strong"));
        QVERIFY(!checker.saveReferenceTitle(edit, QStringLiteral("Stale title")).value(QStringLiteral("success")).toBool());
        const auto savedHash = digest(source.filename);
        const auto unchanged = checker.saveReferenceTitle(titleEdit(checker.report(), QStringLiteral("source")), newTitle);
        QVERIFY(unchanged.value(QStringLiteral("success")).toBool());
        QVERIFY(unchanged.value(QStringLiteral("backupPath")).toString().isEmpty());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(source.filename), savedHash);
    }

    void titleEditsDoNotRequireAValidTarget()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("broken-target-title"),
                                      QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">S.B. 3.2.2</span>"));
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source"), QStringLiteral("missing-book"), QStringLiteral("missing.xhtml")));
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto result = checker.saveReferenceTitle(titleEdit(checker.report(), QStringLiteral("source")), QStringLiteral("S.B. 3.2.2"));
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QVERIFY(hasCode(checker.report(), QStringLiteral("target-book-unresolved")));
        QCOMPARE(titleEdit(checker.report(), QStringLiteral("source")).value(QStringLiteral("htmlTitle")).toString(), QStringLiteral("S.B. 3.2.2"));
    }

    void doesNotOfferTitleEditingForMissingOrAmbiguousAnchors()
    {
        QTemporaryDir dir;
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("no-title-target"), {});
        const QList<QByteArray> bodies{
            {},
            QByteArrayLiteral("<span id=\"source\">Ordinary span</span>"),
            QByteArrayLiteral("<a id=\"source\" data-role=\"anchor\" href=\"other.xhtml\">Internal</a>"),
            QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">One</span><p id=\"source\">Duplicate</p>"),
            QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">One</span>"
                              "<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\">Two</span>")};
        for (const auto &body : bodies) {
            const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("no-title-source"), body);
            const auto report = BookReferenceCheck::inspect(source.filename, {source, target},
                {reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml"))});
            QVERIFY(titleEdit(report, QStringLiteral("source")).isEmpty());
        }
        const auto source = makePdf(dir.filePath(QStringLiteral("source.pdf")), QStringLiteral("no-title-pdf"), {{QStringLiteral("source"), 0}});
        const auto report = BookReferenceCheck::inspect(source.filename, {source, target},
            {reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml"))});
        QVERIFY(titleEdit(report, QStringLiteral("source")).isEmpty());
    }

    void titleEditsRejectChangedSourcesAndReferences()
    {
        QTemporaryDir dir;
        const QByteArray body = QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"Old\">Text</span>");
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("stale-title-source"), body);
        auto &database = BookDatabase::self();
        database.addEntry(source);
        auto ref = reference(source.uniqueIdentifier, QStringLiteral("source"), QStringLiteral("missing-target"), QStringLiteral("missing.xhtml"));
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Old"));
        database.saveReference(ref);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto edit = titleEdit(checker.report(), QStringLiteral("source"));
        const auto originalHash = digest(source.filename);
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Concurrent title"));
        database.saveReference(ref);
        QVERIFY(!checker.saveReferenceTitle(edit, QStringLiteral("New")).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(source.filename), originalHash);
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Old"));
        database.saveReference(ref);
        makeBook(source.filename, source.uniqueIdentifier, body + QByteArrayLiteral("<p>External edit</p>"));
        const auto editedHash = digest(source.filename);
        QVERIFY(!checker.saveReferenceTitle(edit, QStringLiteral("New")).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(source.filename), editedHash);
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("sourceAnchorTitle")).toString(),
                 QStringLiteral("Old"));
        database.removeReference(source.uniqueIdentifier, QStringLiteral("source"));
        QVERIFY(!checker.saveReferenceTitle(edit, QStringLiteral("New")).value(QStringLiteral("success")).toBool());
        QVERIFY(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).isEmpty());
    }

    void titleEditsRestoreEpubOnDatabaseFailure()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("rollback-title-source"),
                                      QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"Old\">Text</span>"));
        auto &database = BookDatabase::self();
        database.addEntry(source);
        auto ref = reference(source.uniqueIdentifier, QStringLiteral("source"), QStringLiteral("missing-target"), QStringLiteral("missing.xhtml"));
        ref.insert(QStringLiteral("sourceAnchorTitle"), QStringLiteral("Old"));
        database.saveReference(ref);
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        // The trigger must survive the application's closing and reopening of the connection.
        QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER reject_title BEFORE UPDATE OF sourceAnchorTitle ON \"references\" "
                                          "BEGIN SELECT RAISE(ABORT, 'test failure'); END")));
        query.finish();
        connection.close();
        const auto cleanup = qScopeGuard([] {
            auto db = QSqlDatabase::database();
            QSqlQuery remove(db);
            remove.exec(QStringLiteral("DROP TRIGGER IF EXISTS reject_title"));
        });
        const auto before = digest(source.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto result = checker.saveReferenceTitle(titleEdit(checker.report(), QStringLiteral("source")), QStringLiteral("New"));
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(source.filename), before);
        QCOMPARE(database.currentBookState(source.uniqueIdentifier)->epubFileHash, before.toHex());
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("sourceAnchorTitle")).toString(),
                 QStringLiteral("Old"));
    }

    void titleEditsPreservePendingReimport()
    {
        QTemporaryDir dir;
        const QByteArray body = QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"Old\">Text</span>");
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("pending-title-source"), body);
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("pending-title-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#other")));
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(source.uniqueIdentifier, source.filename);
        QVERIFY(!session.value(QStringLiteral("error")).toBool());
        const QString copy = session.value(QStringLiteral("workingCopyPath")).toString();
        makeBook(copy, source.uniqueIdentifier, body + QByteArrayLiteral("<p>Pending edit</p>"));
        const auto originalHash = digest(source.filename);
        const auto copyHash = digest(copy);
        const auto originalState = database.currentBookState(source.uniqueIdentifier)->stateId;
        BookReferenceCheck checker;
        checker.startWorkingCopy(source.uniqueIdentifier, copy);
        QTRY_VERIFY(!checker.running());
        const auto result = checker.saveReferenceTitle(titleEdit(checker.report(), QStringLiteral("source")), QStringLiteral("S.B. 3.2.2"));
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(result.value(QStringLiteral("backupPath")).toString()), copyHash);
        QCOMPARE(digest(source.filename), originalHash);
        QCOMPARE(database.currentBookState(source.uniqueIdentifier)->stateId, originalState);
        QCOMPARE(checker.report().value(QStringLiteral("fileName")).toString(), copy);
        QCOMPARE(titleEdit(checker.report(), QStringLiteral("source")).value(QStringLiteral("htmlTitle")).toString(), QStringLiteral("S.B. 3.2.2"));
        QCOMPARE(sessions.importNow(session.value(QStringLiteral("sessionId")).toString()).value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QCOMPARE(digest(source.filename), digest(copy));
    }

    void titleEditingIsVisibleForValidCrossReferences()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("valid-title-dialog-source"),
                                      QByteArrayLiteral("<span id=\"source\" data-role=\"anchor\" data-anchor-type=\"crossref\" title=\"S.B. 3.2.1\">Text</span>"));
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("valid-title-dialog-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source"), target.uniqueIdentifier, QStringLiteral("OPS/other.xhtml#other")));
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nimport org.kde.arianna\nWindow { width: 1100; height: 900; visible: true; "
                                            "ReferenceReportDialog { report: testChecker.report; running: testChecker.running; referenceEditor: testChecker; "
                                            "Component.onCompleted: open() } }"), QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = findVisualItem(quickWindow->contentItem(), QStringLiteral("editReferenceTitle")));
        QVERIFY(button->isVisible());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("referenceTitleDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        auto *field = dialog->findChild<QObject *>(QStringLiteral("referenceAnchorTitle"));
        auto *save = dialog->findChild<QObject *>(QStringLiteral("saveReferenceTitle"));
        QVERIFY(field && save);
        QCOMPARE(field->property("text").toString(), QStringLiteral("S.B. 3.2.1"));
        field->setProperty("text", QStringLiteral("S.B. 3.2.2"));
        QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(titleEdit(checker.report(), QStringLiteral("source")).value(QStringLiteral("htmlTitle")).toString(), QStringLiteral("S.B. 3.2.2"));
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source")).value(QStringLiteral("sourceAnchorTitle")).toString(),
                 QStringLiteral("S.B. 3.2.2"));
    }

    void deletesCrossReferenceFilesAndDatabase_data()
    {
        QTest::addColumn<bool>("sourcePdf");
        QTest::addColumn<bool>("targetPdf");
        QTest::addColumn<bool>("incoming");
        QTest::newRow("epub-epub") << false << false << false;
        QTest::newRow("incoming") << false << false << true;
        QTest::newRow("epub-pdf") << false << true << false;
        QTest::newRow("pdf-epub") << true << false << false;
        QTest::newRow("pdf-pdf") << true << true << false;
    }

    void deletesCrossReferenceFilesAndDatabase()
    {
        QFETCH(bool, sourcePdf);
        QFETCH(bool, targetPdf);
        QFETCH(bool, incoming);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("remove-") + QString::fromLatin1(QTest::currentDataTag());
        auto source = removalBook(dir.filePath(sourcePdf ? QStringLiteral("source.pdf") : QStringLiteral("source.epub")),
                                  prefix + QStringLiteral("-source"),
                                  true,
                                  sourcePdf);
        auto target = removalBook(dir.filePath(targetPdf ? QStringLiteral("target.pdf") : QStringLiteral("target.epub")),
                                  prefix + QStringLiteral("-target"),
                                  false,
                                  targetPdf);
        source.identifier = prefix + QStringLiteral("-source-alias");
        target.identifier = prefix + QStringLiteral("-target-alias");
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.identifier, QStringLiteral("source-anchor"), target.identifier, removalTarget(targetPdf)));
        const auto stored = database.loadReferenceBySource(source.identifier, QStringLiteral("source-anchor"));
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(incoming ? target.filename : source.filename);
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        QVERIFY(!checker.removeReference({}).value(QStringLiteral("success")).toBool());
        const auto prepared = checker.prepareReferenceRemoval(referenceEdit(checker.report(), QStringLiteral("source-anchor")));
        QVERIFY2(prepared.value(QStringLiteral("success")).toBool(), qPrintable(prepared.value(QStringLiteral("message")).toString()));
        const auto plan = prepared.value(QStringLiteral("plan")).toMap();
        QCOMPARE(plan.value(QStringLiteral("source")).toMap().value(QStringLiteral("action")).toString(), QStringLiteral("remove"));
        QCOMPARE(plan.value(QStringLiteral("target")).toMap().value(QStringLiteral("action")).toString(), QStringLiteral("remove"));
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        QCOMPARE(database.loadReferenceBySource(source.identifier, QStringLiteral("source-anchor")), stored);
        const auto result = checker.removeReference(plan);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QVERIFY(database.loadReferenceBySource(source.identifier, QStringLiteral("source-anchor")).isEmpty());
        const auto backups = result.value(QStringLiteral("backupPaths")).toStringList();
        QCOMPARE(backups.size(), 2);
        QCOMPARE(digest(backups.at(0)), sourceHash);
        QCOMPARE(digest(backups.at(1)), targetHash);
        for (const auto &book : {source, target}) {
            if (book.filename.endsWith(QStringLiteral(".pdf"))) {
                const auto index = readPdfReferenceIndex(book.filename);
                QVERIFY(index.error.isEmpty());
                QVERIFY(index.anchors.isEmpty());
                QCOMPARE(index.pageCount, 2);
                PoDoFo::PdfMemDocument pdf;
                pdf.Load(book.filename.toStdString());
                QCOMPARE(pdf.GetPages().GetPageAt(0).GetAnnotations().GetCount(), 1u); // Native link remains.
            } else {
                QCOMPARE(database.currentBookState(book.uniqueIdentifier)->epubFileHash, digest(book.filename).toHex());
                EPubContainer epub(nullptr);
                QVERIFY(epub.openFile(book.filename));
                QDomDocument document;
                QVERIFY(document.setContent(epub.readData(QStringLiteral("OPS/chapter.xhtml")), QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
                const auto paragraph = document.elementsByTagName(QStringLiteral("p")).at(0).toElement();
                QCOMPARE(paragraph.text(),
                         book.filename == source.filename ? QStringLiteral("Before S.B. 3.2.2 after") : QStringLiteral("Before Target text after"));
                QVERIFY(!document.toString().contains(QStringLiteral("id=\"source-anchor\"")));
                QVERIFY(!document.toString().contains(QStringLiteral("id=\"target-anchor\"")));
                if (book.filename == source.filename)
                    QCOMPARE(paragraph.firstChildElement().attribute(QStringLiteral("class")), QStringLiteral("formatting"));
            }
        }
        QVERIFY(!checker.removeReference(plan).value(QStringLiteral("success")).toBool());
    }

    void preservesSharedOrStructuralAnchors_data()
    {
        QTest::addColumn<QString>("use");
        for (const auto &name :
             {"target-crossref", "target-annotation", "target-native", "target-structure", "document-target", "source-incoming", "source-annotation"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void preservesSharedOrStructuralAnchors()
    {
        QFETCH(QString, use);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("shared-") + use;
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), prefix + QStringLiteral("-source"), true);
        const auto target = removalBook(dir.filePath(QStringLiteral("target.epub")), prefix + QStringLiteral("-target"), false);
        const auto other = removalBook(dir.filePath(QStringLiteral("other.epub")), prefix + QStringLiteral("-other"), true);
        auto &database = BookDatabase::self();
        for (const auto &entry : {source, target, other})
            database.addEntry(entry);
        database.saveReference(reference(source.uniqueIdentifier,
                                         QStringLiteral("source-anchor"),
                                         target.uniqueIdentifier,
                                         use == QStringLiteral("document-target") ? QStringLiteral("OPS/chapter.xhtml") : removalTarget(false)));
        if (use == QStringLiteral("target-crossref") || use == QStringLiteral("source-incoming")) {
            database.saveReference(
                reference(other.uniqueIdentifier,
                          QStringLiteral("source-anchor"),
                          use == QStringLiteral("target-crossref") ? target.uniqueIdentifier : source.uniqueIdentifier,
                          use == QStringLiteral("target-crossref") ? removalTarget(false) : QStringLiteral("OPS/chapter.xhtml#source-anchor")));
        } else if (use.endsWith(QStringLiteral("annotation"))) {
            database.saveAnnotation(
                use.startsWith(QStringLiteral("target")) ? target.uniqueIdentifier : source.uniqueIdentifier,
                {{QStringLiteral("annotationId"), QStringLiteral("annotation")},
                 {QStringLiteral("anchorId"), use.startsWith(QStringLiteral("target")) ? QStringLiteral("target-anchor") : QStringLiteral("source-anchor")},
                 {QStringLiteral("cfiRange"), QStringLiteral("test")},
                 {QStringLiteral("text"), QStringLiteral("Keep this annotation")}});
        } else if (use == QStringLiteral("target-native")) {
            makeBook(target.filename,
                     target.uniqueIdentifier,
                     QByteArrayLiteral("<span id=\"target-anchor\" data-role=\"anchor\" data-anchor-type=\"target\">Target text</span><a "
                                       "href=\"#target-anchor\">Native link</a>"));
        } else if (use == QStringLiteral("target-structure")) {
            makeBook(target.filename, target.uniqueIdentifier, QByteArrayLiteral("<h2 id=\"target-anchor\">Chapter heading</h2>"));
        }
        const auto targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto prepared =
            checker.prepareReferenceRemoval(reference(source.uniqueIdentifier,
                                                      QStringLiteral("source-anchor"),
                                                      target.uniqueIdentifier,
                                                      use == QStringLiteral("document-target") ? QStringLiteral("OPS/chapter.xhtml") : removalTarget(false)));
        QVERIFY2(prepared.value(QStringLiteral("success")).toBool(), qPrintable(prepared.value(QStringLiteral("message")).toString()));
        const auto plan = prepared.value(QStringLiteral("plan")).toMap();
        const bool sharedSource = use.startsWith(QStringLiteral("source-"));
        QCOMPARE(plan.value(sharedSource ? QStringLiteral("source") : QStringLiteral("target")).toMap().value(QStringLiteral("action")).toString(),
                 sharedSource ? QStringLiteral("detach") : QStringLiteral("keep"));
        const auto result = checker.removeReference(plan);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QVERIFY(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
        if (sharedSource) {
            EPubContainer epub(nullptr);
            QVERIFY(epub.openFile(source.filename));
            const auto data = epub.readData(QStringLiteral("OPS/chapter.xhtml"));
            QVERIFY(data.contains("id=\"source-anchor\""));
            QVERIFY(!data.contains("crossref"));
            QVERIFY(data.contains("data-anchor-type=\"target\""));
        } else
            QCOMPARE(digest(target.filename), targetHash);
        if (use == QStringLiteral("target-crossref") || use == QStringLiteral("source-incoming")) {
            const auto otherReport = BookReferenceCheck::inspect(other.filename, {source, target, other}, database.loadAllReferences());
            QVERIFY2(otherReport.value(QStringLiteral("success")).toBool(), qPrintable(otherReport.value(QStringLiteral("message")).toString()));
        }
    }

    void keepsSharedPdfTargetMarker()
    {
        QTemporaryDir dir;
        const auto source = removalBook(dir.filePath(QStringLiteral("source.pdf")), QStringLiteral("shared-pdf-source"), true, true);
        const auto target = removalBook(dir.filePath(QStringLiteral("target.pdf")), QStringLiteral("shared-pdf-target"), false, true);
        const auto other = removalBook(dir.filePath(QStringLiteral("other.epub")), QStringLiteral("shared-pdf-other"), true);
        auto &database = BookDatabase::self();
        for (const auto &entry : {source, target, other}) database.addEntry(entry);
        const auto ref = reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(true));
        database.saveReference(ref);
        database.saveReference(reference(other.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier,
            QStringLiteral("{\"format\":\"pdf\",\"anchorId\":\"uuid_target-anchor\",\"page\":0}")));
        const auto before = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto prepared = checker.prepareReferenceRemoval(ref);
        QVERIFY(prepared.value(QStringLiteral("success")).toBool());
        const auto plan = prepared.value(QStringLiteral("plan")).toMap();
        QCOMPARE(plan.value(QStringLiteral("target")).toMap().value(QStringLiteral("action")).toString(), QStringLiteral("keep"));
        QVERIFY(checker.removeReference(plan).value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(target.filename), before);
        QCOMPARE(readPdfReferenceIndex(target.filename).anchors.size(), 1);
        QVERIFY(readPdfReferenceIndex(source.filename).anchors.isEmpty());
        QVERIFY(!database.loadReferenceBySource(other.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
    }

    void deletesDatabaseEntryWhenAnchorsAreAlreadyMissing()
    {
        QTemporaryDir dir;
        const auto source = makeBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("missing-delete-source"), {});
        const auto target = makeBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("missing-delete-target"), {});
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        const auto ref = reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(false));
        database.saveReference(ref);
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto prepared = checker.prepareReferenceRemoval(ref);
        QVERIFY(prepared.value(QStringLiteral("success")).toBool());
        const auto result = checker.removeReference(prepared.value(QStringLiteral("plan")).toMap());
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QVERIFY(result.value(QStringLiteral("backupPaths")).toStringList().isEmpty());
        QVERIFY(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
    }

    void rejectsStaleDeletionPlans()
    {
        QTemporaryDir dir;
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("stale-delete-source"), true);
        const auto target = removalBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("stale-delete-target"), false);
        const auto other = removalBook(dir.filePath(QStringLiteral("other.epub")), QStringLiteral("stale-delete-other"), true);
        auto &database = BookDatabase::self();
        for (const auto &entry : {source, target, other})
            database.addEntry(entry);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(false)));
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        auto edit = referenceEdit(checker.report(), QStringLiteral("source-anchor"));
        const auto plan = checker.prepareReferenceRemoval(edit).value(QStringLiteral("plan")).toMap();
        QVERIFY(!plan.isEmpty());
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        database.saveReference(reference(other.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(false)));
        QVERIFY(!checker.removeReference(plan).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        QVERIFY(!database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
        const auto sharedPlan = checker.prepareReferenceRemoval(edit).value(QStringLiteral("plan")).toMap();
        QVERIFY(!sharedPlan.isEmpty());
        makeBook(source.filename, source.uniqueIdentifier, QByteArrayLiteral("<p>External edit</p>"));
        const auto changedHash = digest(source.filename);
        QVERIFY(!checker.removeReference(sharedPlan).value(QStringLiteral("success")).toBool());
        QCOMPARE(digest(source.filename), changedHash);
        QCOMPARE(digest(target.filename), targetHash);
        const auto missingPlan = checker.prepareReferenceRemoval(edit).value(QStringLiteral("plan")).toMap();
        QVERIFY(!missingPlan.isEmpty());
        database.removeReference(source.uniqueIdentifier, QStringLiteral("source-anchor"));
        QVERIFY(!checker.removeReference(missingPlan).value(QStringLiteral("success")).toBool());
    }

    void failedDeletionRestoresBothFilesAndDatabase_data()
    {
        QTest::addColumn<bool>("failStateWrite");
        QTest::newRow("delete-fails") << false;
        QTest::newRow("state-write-fails") << true;
    }

    void failedDeletionRestoresBothFilesAndDatabase()
    {
        QFETCH(bool, failStateWrite);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("rollback-delete-") + QString::fromLatin1(QTest::currentDataTag());
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), prefix + QStringLiteral("-source"), true);
        const auto target = removalBook(dir.filePath(QStringLiteral("target.epub")), prefix + QStringLiteral("-target"), false);
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(false)));
        QVERIFY(BookTruthStore().openBook(source.uniqueIdentifier).success);
        QVERIFY(BookTruthStore().openBook(target.uniqueIdentifier).success);
        const auto sourceState = database.currentBookState(source.uniqueIdentifier)->stateId;
        const auto targetState = database.currentBookState(target.uniqueIdentifier)->stateId;
        const auto stored = database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor"));
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        QVERIFY(query.exec(
            failStateWrite
                ? QStringLiteral("CREATE TRIGGER reject_removal BEFORE INSERT ON book_current_state BEGIN SELECT RAISE(ABORT, 'test state failure'); END")
                : QStringLiteral("CREATE TRIGGER reject_removal BEFORE DELETE ON \"references\" BEGIN SELECT RAISE(ABORT, 'test delete failure'); END")));
        query.finish();
        connection.close();
        const auto cleanup = qScopeGuard([] {
            auto db = QSqlDatabase::database();
            QSqlQuery query(db);
            query.exec(QStringLiteral("DROP TRIGGER IF EXISTS reject_removal"));
        });
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        const auto prepared = checker.prepareReferenceRemoval(referenceEdit(checker.report(), QStringLiteral("source-anchor")));
        QVERIFY(prepared.value(QStringLiteral("success")).toBool());
        const auto result = checker.removeReference(prepared.value(QStringLiteral("plan")).toMap());
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")), stored);
        QCOMPARE(database.currentBookState(source.uniqueIdentifier)->stateId, sourceState);
        QCOMPARE(database.currentBookState(target.uniqueIdentifier)->stateId, targetState);
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.bak")}, QDir::Files).size(), 2);
    }

    void deletesReferenceDuringReimport()
    {
        QTemporaryDir dir;
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("import-delete-source"), true);
        const auto target = removalBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("import-delete-target"), false);
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(false)));
        EditorSessionStore sessions;
        const auto session = sessions.createEditorSession(source.uniqueIdentifier, source.filename);
        QVERIFY(!session.value(QStringLiteral("error")).toBool());
        const QString copy = session.value(QStringLiteral("workingCopyPath")).toString();
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        const auto oldState = database.currentBookState(source.uniqueIdentifier)->stateId;
        BookReferenceCheck checker;
        checker.startWorkingCopy(source.uniqueIdentifier, copy);
        QTRY_VERIFY(!checker.running());
        const auto prepared = checker.prepareReferenceRemoval(referenceEdit(checker.report(), QStringLiteral("source-anchor")));
        QVERIFY(prepared.value(QStringLiteral("success")).toBool());
        const auto result = checker.removeReference(prepared.value(QStringLiteral("plan")).toMap());
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QTRY_VERIFY(!checker.running());
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(database.currentBookState(source.uniqueIdentifier)->stateId, oldState);
        QVERIFY(digest(target.filename) != targetHash);
        QVERIFY(digest(copy) != sourceHash);
        QVERIFY(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
        QCOMPARE(sessions.importNow(session.value(QStringLiteral("sessionId")).toString()).value(QStringLiteral("status")).toString(),
                 QStringLiteral("imported"));
        QCOMPARE(digest(source.filename), digest(copy));
    }

    void bookRemovalCleansReferencesAndAnnotations_data()
    {
        QTest::addColumn<bool>("sourcePdf");
        QTest::addColumn<bool>("targetPdf");
        QTest::addColumn<bool>("deleteFile");
        for (const bool sourcePdf : {false, true}) {
            for (const bool targetPdf : {false, true}) {
                for (const bool deleteFile : {false, true}) {
                    const QByteArray name = QByteArray(sourcePdf ? "pdf-" : "epub-") + (targetPdf ? "pdf-" : "epub-")
                        + (deleteFile ? "delete-file" : "keep-file");
                    QTest::newRow(name.constData()) << sourcePdf << targetPdf << deleteFile;
                }
            }
        }
    }

    void bookRemovalCleansReferencesAndAnnotations()
    {
        QFETCH(bool, sourcePdf);
        QFETCH(bool, targetPdf);
        QFETCH(bool, deleteFile);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("book-removal-") + QString::fromLatin1(QTest::currentDataTag());
        const QString removedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto deleted = removalBook(dir.filePath(sourcePdf ? QStringLiteral("deleted.pdf") : QStringLiteral("deleted.epub")), removedId, false, sourcePdf);
        deleted.identifier = prefix + QStringLiteral("-legacy, ") + QStringLiteral("urn:uuid:") + removedId;
        const auto makeRemaining = [&](const QString &name, bool pdf, bool source) {
            const QStringList ids = source ? QStringList{QStringLiteral("in-one"), QStringLiteral("in-two"), QStringLiteral("keep-source")}
                                           : QStringList{QStringLiteral("out-one"), QStringLiteral("out-two"), QStringLiteral("keep-target")};
            const QString path = dir.filePath(name + (pdf ? QStringLiteral(".pdf") : QStringLiteral(".epub")));
            if (pdf) {
                return makePdf(path, prefix + name, {{ids.at(0), 0}, {ids.at(1), 0}, {ids.at(2), 0}});
            }
            QByteArray body;
            for (const auto &id : ids) {
                body += QByteArrayLiteral("<span id=\"") + id.toUtf8() + QByteArrayLiteral("\" data-role=\"anchor\" data-anchor-type=\"")
                    + (source ? "crossref" : "target") + QByteArrayLiteral("\">Readable text</span>");
            }
            return makeBook(path, prefix + name, body);
        };
        const auto source = makeRemaining(QStringLiteral("source"), sourcePdf, true);
        const auto target = makeRemaining(QStringLiteral("target"), targetPdf, false);
        const auto location = [](bool pdf, const QString &id) {
            return pdf ? QStringLiteral("{\"format\":\"pdf\",\"page\":0,\"anchorId\":\"%1\"}").arg(id)
                       : QStringLiteral("OPS/chapter.xhtml#") + id;
        };
        auto &database = BookDatabase::self();
        for (const auto &book : {deleted, source, target}) {
            database.addEntry(book);
        }
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("in-one"), removedId, removalTarget(sourcePdf)));
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("in-two"), prefix + QStringLiteral("-legacy"), removalTarget(sourcePdf)));
        database.saveReference(reference(removedId, QStringLiteral("out-one"), target.uniqueIdentifier, location(targetPdf, QStringLiteral("out-one"))));
        database.saveReference(reference(QStringLiteral("URN:UUID:") + removedId.toUpper(), QStringLiteral("out-two"),
                                         target.uniqueIdentifier, location(targetPdf, QStringLiteral("out-two"))));
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("keep-source"), target.uniqueIdentifier,
                                         location(targetPdf, QStringLiteral("keep-target"))));
        const auto keptReference = database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("keep-source"));
        const QVariantMap note{{QStringLiteral("annotationId"), QStringLiteral("note")},
                               {QStringLiteral("anchorId"), QStringLiteral("keep-source")},
                               {QStringLiteral("note"), QStringLiteral("Keep other books' notes")}};
        const QStringList removedKeys{removedId, prefix + QStringLiteral("-legacy"), deleted.filename,
                                     QStringLiteral("{") + removedId.toUpper() + QStringLiteral("}")};
        for (const auto &id : removedKeys) {
            database.saveAnnotation(id, note);
            BookState state{QUuid::createUuid(), id, QByteArrayLiteral("text"), QByteArrayLiteral("document"), QByteArrayLiteral("file")};
            QVERIFY(database.saveBookState(state));
        }
        database.saveAnnotation(source.uniqueIdentifier, note);
        const auto keptNotes = database.loadAnnotations(source.uniqueIdentifier);
        const auto deletedHash = digest(deleted.filename);

        BookListModel model;
        model.componentComplete();
        QTRY_VERIFY(model.cacheLoaded());
        QVERIFY(model.knownBookFiles().contains(deleted.filename));
        const auto result = model.removeBook(deleted.filename, deleteFile);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        QVERIFY(!model.knownBookFiles().contains(deleted.filename));
        QVERIFY(!database.loadEntry(deleted.filename));
        QCOMPARE(QFileInfo::exists(deleted.filename), !deleteFile);
        if (!deleteFile) {
            QCOMPARE(digest(deleted.filename), deletedHash);
        }
        for (const auto &id : removedKeys) {
            QVERIFY(database.loadAnnotations(id).isEmpty());
            QVERIFY(!database.currentBookState(id));
        }
        QCOMPARE(database.loadReferences(source.uniqueIdentifier).size(), 1);
        QCOMPARE(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("keep-source")), keptReference);
        QCOMPARE(database.loadAnnotations(source.uniqueIdentifier), keptNotes);
        QVERIFY(database.loadReferences(removedId).isEmpty());
        QVERIFY(database.loadReferences(QStringLiteral("URN:UUID:") + removedId.toUpper()).isEmpty());
        const auto remainingBooks = database.loadEntries();
        const auto remainingReferences = database.loadAllReferences();
        for (const auto &book : {source, target}) {
            const auto report = BookReferenceCheck::inspect(book.filename, remainingBooks, remainingReferences);
            QVERIFY2(report.value(QStringLiteral("success")).toBool(), QJsonDocument::fromVariant(report).toJson().constData());
            if (book.filename.endsWith(QStringLiteral(".pdf"))) {
                const auto index = readPdfReferenceIndex(book.filename);
                QVERIFY(index.error.isEmpty());
                QCOMPARE(index.anchors.size(), 1);
                QVERIFY(index.anchors.first().id.startsWith(QStringLiteral("keep-")));
            } else {
                EPubContainer epub(nullptr);
                QVERIFY(epub.openFile(book.filename));
                const auto chapter = epub.readData(QStringLiteral("OPS/chapter.xhtml"));
                QVERIFY(!chapter.contains("id=\"in-one\"") && !chapter.contains("id=\"in-two\""));
                QVERIFY(!chapter.contains("id=\"out-one\"") && !chapter.contains("id=\"out-two\""));
                QCOMPARE(chapter.count("Readable text"), 3);
                const auto state = BookTruthStore().stateForFile(book.uniqueIdentifier, book.filename);
                QVERIFY(state);
                QCOMPARE(database.currentBookState(book.uniqueIdentifier)->epubFileHash, state->epubFileHash);
            }
        }
        QCOMPARE(result.value(QStringLiteral("backupPaths")).toStringList().size(), 2);
    }

    void bookRemovalRetainsSharedAnchors()
    {
        QTemporaryDir dir;
        const auto deleted = removalBook(dir.filePath(QStringLiteral("deleted.epub")), QStringLiteral("remove-shared-deleted"), false);
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("remove-shared-source"), true);
        const auto other = removalBook(dir.filePath(QStringLiteral("other.epub")), QStringLiteral("remove-shared-other"), true);
        auto &database = BookDatabase::self();
        for (const auto &book : {deleted, source, other}) {
            database.addEntry(book);
        }
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), deleted.uniqueIdentifier, removalTarget(false)));
        database.saveReference(reference(other.uniqueIdentifier, QStringLiteral("source-anchor"), source.uniqueIdentifier,
                                         QStringLiteral("OPS/chapter.xhtml#source-anchor")));
        database.saveAnnotation(source.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("note")},
                                                         {QStringLiteral("anchorId"), QStringLiteral("source-anchor")}});
        const auto notes = database.loadAnnotations(source.uniqueIdentifier);
        const auto result = BookReferenceCheck::removeBook(deleted);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(), qPrintable(result.value(QStringLiteral("message")).toString()));
        EPubContainer epub(nullptr);
        QVERIFY(epub.openFile(source.filename));
        const auto chapter = epub.readData(QStringLiteral("OPS/chapter.xhtml"));
        QVERIFY(chapter.contains("id=\"source-anchor\""));
        QVERIFY(!chapter.contains("data-anchor-type=\"crossref\""));
        QCOMPARE(database.loadAnnotations(source.uniqueIdentifier), notes);
        QVERIFY(!database.loadReferenceBySource(other.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
    }

    void bookRemovalRollsBackOnFailure_data()
    {
        QTest::addColumn<QString>("table");
        QTest::addColumn<QString>("operation");
        for (const auto &table : {"annotations", "references", "books", "book_current_state"}) {
            QTest::newRow(table) << QString::fromLatin1(table) << QStringLiteral("DELETE");
        }
        QTest::newRow("save-surviving-book-state") << QStringLiteral("book_current_state") << QStringLiteral("INSERT");
    }

    void bookRemovalRollsBackOnFailure()
    {
        QFETCH(QString, table);
        QFETCH(QString, operation);
        QTemporaryDir dir;
        const QString prefix = QStringLiteral("remove-book-rollback-") + table + operation;
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), prefix + QStringLiteral("-source"), true);
        const auto deleted = removalBook(dir.filePath(QStringLiteral("deleted.epub")), prefix + QStringLiteral("-deleted"), false);
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(deleted);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), deleted.uniqueIdentifier, removalTarget(false)));
        database.saveAnnotation(deleted.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("note")},
                                                          {QStringLiteral("anchorId"), QStringLiteral("target-anchor")}});
        QVERIFY(BookTruthStore().openBook(source.uniqueIdentifier).success);
        QVERIFY(BookTruthStore().openBook(deleted.uniqueIdentifier).success);
        const auto sourceHash = digest(source.filename), deletedHash = digest(deleted.filename);
        const auto notes = database.loadAnnotations(deleted.uniqueIdentifier);
        const auto sourceState = database.currentBookState(source.uniqueIdentifier)->stateId;
        const auto deletedState = database.currentBookState(deleted.uniqueIdentifier)->stateId;
        BookListModel model;
        model.componentComplete();
        QTRY_VERIFY(model.cacheLoaded());
        const auto references = database.loadAllReferences();
        auto connection = QSqlDatabase::database();
        QSqlQuery query(connection);
        QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER reject_book_removal BEFORE %1 ON \"%2\" "
                                          "BEGIN SELECT RAISE(ABORT, 'test book removal failure'); END").arg(operation, table)));
        query.finish();
        connection.close();
        const auto cleanup = qScopeGuard([] {
            auto connection = QSqlDatabase::database();
            QSqlQuery query(connection);
            query.exec(QStringLiteral("DROP TRIGGER IF EXISTS reject_book_removal"));
        });
        const auto result = model.removeBook(deleted.filename, true);
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        QVERIFY(model.knownBookFiles().contains(deleted.filename));
        QVERIFY(database.loadEntry(deleted.filename));
        QCOMPARE(digest(deleted.filename), deletedHash);
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(database.loadAllReferences(), references);
        QCOMPARE(database.loadAnnotations(deleted.uniqueIdentifier), notes);
        QCOMPARE(database.currentBookState(source.uniqueIdentifier)->stateId, sourceState);
        QCOMPARE(database.currentBookState(deleted.uniqueIdentifier)->stateId, deletedState);
    }

    void bookRemovalKeepsDataForAnotherLibraryCopy()
    {
        QTemporaryDir dir;
        const auto deleted = removalBook(dir.filePath(QStringLiteral("deleted.epub")), QStringLiteral("remove-book-shared-id"), false);
        const auto copy = removalBook(dir.filePath(QStringLiteral("copy.epub")), deleted.uniqueIdentifier, false);
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("remove-book-shared-id-source"), true);
        auto &database = BookDatabase::self();
        for (const auto &book : {deleted, copy, source}) {
            database.addEntry(book);
        }
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), deleted.uniqueIdentifier, removalTarget(false)));
        database.saveAnnotation(deleted.uniqueIdentifier, {{QStringLiteral("annotationId"), QStringLiteral("note")},
                                                          {QStringLiteral("anchorId"), QStringLiteral("target-anchor")}});
        const auto notes = database.loadAnnotations(deleted.uniqueIdentifier);
        const auto sourceHash = digest(source.filename);
        QVERIFY(BookReferenceCheck::removeBook(deleted).value(QStringLiteral("success")).toBool());
        QVERIFY(database.loadEntry(copy.filename));
        QCOMPARE(database.loadAnnotations(copy.uniqueIdentifier), notes);
        QCOMPARE(digest(source.filename), sourceHash);
        QVERIFY(!database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
        QVERIFY(BookReferenceCheck::removeBook(copy).value(QStringLiteral("success")).toBool());
        QVERIFY(database.loadAnnotations(copy.uniqueIdentifier).isEmpty());
        QVERIFY(database.loadReferences(source.uniqueIdentifier).isEmpty());
    }

    void intactReferenceDeletionRequiresConfirmation()
    {
        QTemporaryDir dir;
        const auto source = removalBook(dir.filePath(QStringLiteral("source.epub")), QStringLiteral("confirm-delete-source"), true);
        const auto target = removalBook(dir.filePath(QStringLiteral("target.epub")), QStringLiteral("confirm-delete-target"), false);
        auto &database = BookDatabase::self();
        database.addEntry(source);
        database.addEntry(target);
        database.saveReference(reference(source.uniqueIdentifier, QStringLiteral("source-anchor"), target.uniqueIdentifier, removalTarget(false)));
        const auto sourceHash = digest(source.filename), targetHash = digest(target.filename);
        BookReferenceCheck checker;
        checker.start(source.filename);
        QTRY_VERIFY(!checker.running());
        QVERIFY(checker.report().value(QStringLiteral("success")).toBool());
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("testChecker"), &checker);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nimport org.kde.arianna\nWindow { width: 1100; height: 900; visible: true; "
                                            "ReferenceReportDialog { report: testChecker.report; running: testChecker.running; referenceEditor: testChecker; "
                                            "Component.onCompleted: open() } }"),
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
        QVERIFY(quickWindow);
        QQuickItem *button = nullptr;
        QTRY_VERIFY(button = findVisualItem(quickWindow->contentItem(), QStringLiteral("deleteCrossReference")));
        QVERIFY(button->isVisible());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("referenceRemovalConfirmation"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QVERIFY(!database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
        QCOMPARE(digest(source.filename), sourceHash);
        QCOMPARE(digest(target.filename), targetHash);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        auto *confirm = dialog->findChild<QObject *>(QStringLiteral("confirmReferenceRemoval"));
        QVERIFY(confirm);
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!checker.running());
        QVERIFY(database.loadReferenceBySource(source.uniqueIdentifier, QStringLiteral("source-anchor")).isEmpty());
    }

    void reportCanBeFilteredAndCopied()
    {
        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import org.kde.arianna
            Window {
                width: 1100; height: 900; visible: true
                ReferenceReportDialog {
                    objectName: "reportDialog"
                    report: ({message: "2 results", fileName: "test.epub", results: [
                        {status: "ok", direction: "outgoing", sourceAnchorTitle: "Good", sourcePage: 2, sourceAnnotation: 3, targetPage: 5, messages: ["Found"]},
                        {status: "error", direction: "incoming", sourceAnchorTitle: "<b>Broken</b>", messages: ["Missing target"]}
                    ]})
                    Component.onCompleted: open()
                }
            }
        )",
                          QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window != nullptr, qPrintable(component.errorString()));
        auto *dialog = window->findChild<QObject *>(QStringLiteral("reportDialog"));
        QVERIFY(dialog);
        auto count = [dialog]() {
            return dialog->property("filteredResults").value<QJSValue>().toVariant().toList().size();
        };
        QTRY_COMPARE(count(), 2);
        auto *filter = window->findChild<QObject *>(QStringLiteral("referenceProblemsOnly"));
        QVERIFY(filter);
        filter->setProperty("checked", true);
        QTRY_COMPARE(count(), 1);
        auto *search = window->findChild<QObject *>(QStringLiteral("referenceSearch"));
        QVERIFY(search);
        search->setProperty("text", QStringLiteral("nothing matches"));
        QTRY_COMPARE(count(), 0);
        auto *copy = window->findChild<QObject *>(QStringLiteral("copyReferenceReport"));
        QVERIFY(copy);
        QVERIFY(QMetaObject::invokeMethod(copy, "clicked"));
        const QString copied = QGuiApplication::clipboard()->text();
        QVERIFY(copied.contains(QStringLiteral("Good")));
        QVERIFY(copied.contains(QStringLiteral("<b>Broken</b>")));
        QVERIFY(copied.contains(QStringLiteral("Missing target")));
        QVERIFY(copied.contains(i18n("Source page: %1", 2)));
        QVERIFY(copied.contains(i18n("Source annotation: %1", 3)));
        QVERIFY(copied.contains(i18n("Target page: %1", 5)));
    }
};

QTEST_MAIN(BookReferenceCheckTest)

#include "bookreferencechecktest.moc"
