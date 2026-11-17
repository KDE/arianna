// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "workstructure.h"
#include "bookdatabase.h"
#include "bookserver.h"
#include "booktruthstore.h"
#include "categoryentriesmodel.h"
#include "config.h"
#include "epubcontainer.h"
#include "fakespeechdispatcher.h"
#include "sanskritnumbers.h"
#include "speechcontroller.h"
#include "workstructurecommit.h"
#include "workstructurereview.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QtWebEngineQuick>
#include <iostream>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
class WorkRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (info.requestUrl().host() == QLatin1String("127.0.0.1"))
            info.setHttpHeader(QByteArrayLiteral("X-Arianna-Session-Token"), QByteArrayLiteral("work-token"));
    }
};
bool fixture(const QString &path, int version = 3, const QString &id = QStringLiteral("work-test"), bool generic = false)
{
    KZip zip(path);
    if (!zip.open(QIODevice::WriteOnly))
        return false;
    zip.setCompression(KZip::NoCompression);
    if (!zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip")))
        return false;
    zip.setCompression(KZip::DeflateCompression);
    const QByteArray container = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\"><rootfiles><rootfile "
        "full-path=\"OEBPS/package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    const QString package = QStringLiteral(
                                "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" "
                                "version=\"%1.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier "
                                "id=\"bookid\">%2</dc:identifier><dc:title>Kṛṣṇa-karṇāmṛta</dc:title><dc:creator>Example "
                                "author</dc:creator><dc:language>sa</dc:language>%3</metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" "
                                "media-type=\"application/xhtml+xml\"/><item id=\"navigation\" href=\"%4\" media-type=\"%5\" %6/></manifest><spine %7><itemref "
                                "idref=\"chapter\"/></spine></package>")
                                .arg(QString::number(version),
                                     id,
                                     version == 3 ? QStringLiteral("<meta property=\"dcterms:modified\">2026-10-13T00:00:00Z</meta>") : QString(),
                                     version == 3 ? QStringLiteral("nav.xhtml") : QStringLiteral("toc.ncx"),
                                     version == 3 ? QStringLiteral("application/xhtml+xml") : QStringLiteral("application/x-dtbncx+xml"),
                                     version == 3 ? QStringLiteral("properties=\"nav\"") : QString(),
                                     version == 2 ? QStringLiteral("toc=\"navigation\"") : QString());
    const QString html =
        QStringLiteral(
            "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"sa\"><head><title>Part One</title></head><body><h1 "
            "id=\"section1\">%1</h1><p id=\"verse4\"><span id=\"uuid_11111111-1111-4111-8111-111111111111\">नमस्ते</span> सर्वे भवन्तु सुखिनः पूर्वं १-४ पठ। "
            "॥१-४॥</p><p>Ordinary "
            "prose 2 and 3, with a range 1-4.</p></body></html>")
            .arg(generic ? QStringLiteral("Chapter 1") : QStringLiteral("आश्वास १"));
    const QString nav = QStringLiteral(
        "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" "
        "xml:lang=\"sa\"><head><title>Contents</title></head><body><nav epub:type=\"toc\"><ol><li><span>Work</span><ol><li><a "
        "href=\"chapter.xhtml#section1\">Chapter 1</a><ol><li><a href=\"chapter.xhtml#verse4\">Verse 4</a></li></ol></li></ol></li></ol></nav></body></html>");
    const QString ncx = QStringLiteral(
                            "<?xml version=\"1.0\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head><meta name=\"dtb:uid\" "
                            "content=\"%1\"/><meta name=\"dtb:depth\" content=\"1\"/><meta name=\"dtb:totalPageCount\" content=\"0\"/><meta "
                            "name=\"dtb:maxPageNumber\" content=\"0\"/></head><docTitle><text>Test</text></docTitle><navMap><navPoint id=\"part1\" "
                            "playOrder=\"1\"><navLabel><text>Chapter 1</text></navLabel><content src=\"chapter.xhtml#section1\"/></navPoint></navMap></ncx>")
                            .arg(id);
    return zip.writeFile(QStringLiteral("META-INF/container.xml"), container) && zip.writeFile(QStringLiteral("OEBPS/package.opf"), package.toUtf8())
        && zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), html.toUtf8())
        && zip.writeFile(version == 3 ? QStringLiteral("OEBPS/nav.xhtml") : QStringLiteral("OEBPS/toc.ncx"), (version == 3 ? nav : ncx).toUtf8())
        && zip.close();
}
QJsonObject reviewed(const QJsonObject &snap)
{
    auto draft = WorkStructure::suggest(snap);
    auto assignments = draft.value(QStringLiteral("assignments")).toArray();
    for (int i = 0; i < assignments.size(); ++i) {
        auto a = assignments[i].toObject();
        a.insert(QStringLiteral("confirmed"), true);
        assignments[i] = a;
    }
    draft.insert(QStringLiteral("assignments"), assignments);
    return draft;
}
bool write(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
BookEntry registerBook(const QString &path, const QString &id)
{
    BookEntry entry;
    entry.filename = path;
    entry.identifier = id;
    entry.uniqueIdentifier = id;
    entry.title = QStringLiteral("Work test");
    entry.filetitle = QStringLiteral("test.epub");
    entry.created = QDateTime::currentDateTimeUtc();
    BookDatabase::self().addEntry(entry);
    return entry;
}
}

class WorkStructureTest : public QObject
{
    Q_OBJECT
    QTemporaryDir environment;
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(environment.isValid());
        for (const auto *key : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
            qputenv(key, environment.path().toUtf8());
        QCoreApplication::setApplicationName(QStringLiteral("arianna-workstructuretest"));
        KLocalizedString::setApplicationDomain("arianna");
    }
    void sourceEvidenceAndRules()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(fixture(path));
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(path));
        const auto hash = book.getFileHash();
        QVERIFY(!WorkStructure::hasStoredRecord(book));
        const auto snap = WorkStructure::snapshot(book, QStringLiteral("work-test"));
        QCOMPARE(snap.value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity")).toString(), QStringLiteral("absent"));
        const auto evidence = snap.value(QStringLiteral("evidence")).toArray();
        QCOMPARE(evidence.size(), 5);
        QCOMPARE(evidence[4].toObject().value(QStringLiteral("parentEvidenceId")), evidence[3].toObject().value(QStringLiteral("id")));
        const auto draft = WorkStructure::suggest(snap);
        QCOMPARE(draft.value(QStringLiteral("levels")).toArray(), (QJsonArray{QStringLiteral("asvasa"), QStringLiteral("sloka")}));
        QCOMPARE(draft.value(QStringLiteral("assignments")).toArray().size(), 2);
        QCOMPARE(draft.value(QStringLiteral("separators")).toArray(), (QJsonArray{QStringLiteral("."), QStringLiteral("-")}));
        QCOMPARE(book.getFileHash(), hash);
        QString error;
        QVERIFY(WorkStructure::confirmedRecord(snap, draft, &error).isEmpty());
        const auto record = WorkStructure::confirmedRecord(snap, reviewed(snap), &error);
        QVERIFY2(!record.isEmpty(), qPrintable(error));
        QCOMPARE(WorkStructure::resolve(record, QStringLiteral("१-४"), QStringLiteral("navigation")).value(QStringLiteral("coordinate")).toArray(),
                 (QJsonArray{1, 4}));
        QVERIFY(WorkStructure::resolve(record, QStringLiteral("१-४"), QStringLiteral("prose")).isEmpty());
        QVERIFY(WorkStructure::resolve(record, QStringLiteral("१-५"), QStringLiteral("navigation")).isEmpty());
        QVERIFY(WorkStructure::resolve(record, QStringLiteral("०१-४"), QStringLiteral("navigation")).isEmpty());
    }
    void explicitPadasUseVerseParents()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(fixture(path));
        EPubContainer source(nullptr);
        QVERIFY(source.openFile(path));
        auto chapter = source.readData(QStringLiteral("OEBPS/chapter.xhtml"));
        chapter.replace("<p>Ordinary", "<p data-verse-number=\"5\" id=\"verse5\"><span data-pada=\"1\" id=\"pada1\">अ</span></p><p>Ordinary");
        const auto target = dir.filePath(QStringLiteral("padas.epub"));
        QVERIFY(write(target, source.createMetadataCandidate({{QStringLiteral("OEBPS/chapter.xhtml"), chapter}})));
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(target));
        const auto snap = WorkStructure::snapshot(book, QStringLiteral("work-test"));
        const auto draft = reviewed(snap);
        QCOMPARE(draft.value(QStringLiteral("levels")).toArray().size(), 3);
        QString error;
        const auto record = WorkStructure::confirmedRecord(snap, draft, &error);
        QVERIFY2(!record.isEmpty(), qPrintable(error));
        const auto pada = WorkStructure::resolve(record, QStringLiteral("1.5.1"), QStringLiteral("navigation"));
        QVERIFY(!pada.isEmpty());
        QCOMPARE(pada.value(QStringLiteral("level")).toString(), QStringLiteral("pada"));
        QVERIFY(WorkStructure::resolve(record, QStringLiteral("1.5.2"), QStringLiteral("navigation")).isEmpty());
    }
    void labelOnlyGroupsUseExistingChildTargets()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(fixture(path));
        EPubContainer source(nullptr);
        QVERIFY(source.openFile(path));
        auto chapter = source.readData(QStringLiteral("OEBPS/chapter.xhtml"));
        chapter.replace(QStringLiteral("आश्वास १").toUtf8(), QStringLiteral("अध्याय १").toUtf8());
        chapter.replace(QStringLiteral("॥१-४॥").toUtf8(), QStringLiteral("॥१-१-४॥").toUtf8());
        auto nav = source.readData(QStringLiteral("OEBPS/nav.xhtml"));
        nav.replace("<span>Work</span>", QStringLiteral("<span>स्कन्ध १</span>").toUtf8());
        const auto target = dir.filePath(QStringLiteral("grouped.epub"));
        QVERIFY(write(target, source.createMetadataCandidate({{QStringLiteral("OEBPS/chapter.xhtml"), chapter}, {QStringLiteral("OEBPS/nav.xhtml"), nav}})));
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(target));
        const auto snap = WorkStructure::snapshot(book, QStringLiteral("work-test"));
        auto draft = reviewed(snap);
        QCOMPARE(draft.value(QStringLiteral("levels")).toArray(), (QJsonArray{QStringLiteral("skandha"), QStringLiteral("adhyaya"), QStringLiteral("sloka")}));
        QString error;
        const auto record = WorkStructure::confirmedRecord(snap, draft, &error);
        QVERIFY2(!record.isEmpty(), qPrintable(error));
        QVERIFY(!WorkStructure::resolve(record, QStringLiteral("1.1.4"), QStringLiteral("navigation")).isEmpty());
        const auto group = WorkStructure::resolve(record, QStringLiteral("1"), QStringLiteral("navigation"));
        QCOMPARE(group.value(QStringLiteral("semanticPath")).toArray().size(), 1);
        QCOMPARE(group.value(QStringLiteral("level")).toString(), QStringLiteral("skandha"));
        QVERIFY(!group.value(QStringLiteral("locators")).toArray().isEmpty());
        QCOMPARE(WorkStructure::atLocation(record, QStringLiteral("OEBPS/chapter.xhtml#section1")).value(QStringLiteral("level")).toString(),
                 QStringLiteral("adhyaya"));
    }
    void navigationConflictsAreReported()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(fixture(path));
        EPubContainer source(nullptr);
        QVERIFY(source.openFile(path));
        auto opf = source.readData(source.packagePath());
        opf.replace("</manifest>", "<item id=\"conflict\" href=\"conflict.ncx\" media-type=\"application/x-dtbncx+xml\"/></manifest>");
        const auto target = dir.filePath(QStringLiteral("conflict.epub"));
        const auto ncx =
            QStringLiteral(
                "<ncx><navMap><navPoint><navLabel><text>आश्वास २</text></navLabel><content src=\"chapter.xhtml#section1\"/></navPoint></navMap></ncx>")
                .toUtf8();
        QVERIFY(write(target, source.createMetadataCandidate({{source.packagePath(), opf}}, {{QStringLiteral("OEBPS/conflict.ncx"), ncx}})));
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(target));
        const auto snap = WorkStructure::snapshot(book, QStringLiteral("work-test"));
        QVERIFY(!snap.value(QStringLiteral("issues")).toArray().isEmpty());
    }
    void titleDoesNotInventHierarchy()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("generic.epub"));
        QVERIFY(fixture(path, 3, QStringLiteral("work-test"), true));
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(path));
        const auto snap = WorkStructure::snapshot(book, QStringLiteral("work-test"));
        const auto draft = WorkStructure::suggest(snap);
        QVERIFY(!draft.value(QStringLiteral("levels")).toArray().contains(QStringLiteral("asvasa")));
        QVERIFY(draft.value(QStringLiteral("assignments")).toArray().isEmpty());
    }
    void roundtrip_data()
    {
        QTest::addColumn<int>("version");
        QTest::newRow("epub2") << 2;
        QTest::newRow("epub3") << 3;
    }
    void roundtrip()
    {
        QFETCH(int, version);
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("original.epub"));
        QVERIFY(fixture(path, version));
        EPubContainer before(nullptr);
        QVERIFY(before.openFile(path));
        const auto snap = WorkStructure::snapshot(before, QStringLiteral("work-test"));
        QString error;
        const auto record = WorkStructure::confirmedRecord(snap, reviewed(snap), &error);
        QVERIFY2(!record.isEmpty(), qPrintable(error));
        const auto bytes = WorkStructure::candidate(before, record, &error);
        QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        const auto target = dir.filePath(QStringLiteral("candidate.epub"));
        QVERIFY(write(target, bytes));
        EPubContainer after(nullptr);
        QVERIFY(after.openFile(target));
        QVERIFY(WorkStructure::hasStoredRecord(after));
        QVERIFY2(WorkStructure::preservesSource(before, after, &error), qPrintable(error));
        QCOMPARE(before.readData(QStringLiteral("OEBPS/chapter.xhtml")), after.readData(QStringLiteral("OEBPS/chapter.xhtml")));
        const auto next = WorkStructure::snapshot(after, QStringLiteral("work-test"));
        QCOMPARE(next.value(QStringLiteral("sourceBasisHash")), snap.value(QStringLiteral("sourceBasisHash")));
        QCOMPARE(next.value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity")).toString(), QStringLiteral("valid"));
        QCOMPARE(next.value(QStringLiteral("existing")).toObject().value(QStringLiteral("record")).toObject(), record);
        QCOMPARE(next.value(QStringLiteral("evidence")), snap.value(QStringLiteral("evidence")));
        const auto opf = after.readData(after.packagePath());
        if (version == 3) {
            QVERIFY(opf.contains("rel=\"record\""));
            QVERIFY(opf.contains("application/json"));
        } else {
            QVERIFY(opf.contains("arianna:work-structure"));
            QVERIFY(!opf.contains("<link"));
        }
        if (!QFile::exists(QStringLiteral("/usr/share/java/epubcheck.jar")))
            QSKIP("EPUBCheck not installed");
        QProcess validator;
        validator.start(QStringLiteral("java"), {QStringLiteral("-jar"), QStringLiteral("/usr/share/java/epubcheck.jar"), target});
        QSignalSpy finished(&validator, &QProcess::finished);
        QVERIFY(finished.wait(30000));
        QCOMPARE(validator.exitStatus(), QProcess::NormalExit);
        const QByteArray output = validator.readAllStandardOutput() + validator.readAllStandardError();
        QVERIFY2(validator.exitCode() == 0, output.constData());
    }
    void rejectsInventedModelMappings()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(fixture(path));
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(path));
        const auto snap = WorkStructure::snapshot(book, QStringLiteral("work-test"));
        auto answer = reviewed(snap);
        answer.remove(QStringLiteral("snapshotId"));
        answer.remove(QStringLiteral("separators"));
        answer.insert(QStringLiteral("sources"), QJsonArray{});
        answer.insert(QStringLiteral("uncertainties"), QJsonArray{});
        QString error;
        auto applied = WorkStructure::applyModel(snap, answer, &error);
        QVERIFY2(!applied.isEmpty(), qPrintable(error));
        for (const auto &v : applied.value(QStringLiteral("assignments")).toArray())
            QVERIFY(!v.toObject().value(QStringLiteral("confirmed")).toBool());
        auto bad = answer;
        bad.insert(QStringLiteral("levels"), QJsonArray{QStringLiteral("invented")});
        QVERIFY(WorkStructure::applyModel(snap, bad, &error).isEmpty());
        auto assignments = answer.value(QStringLiteral("assignments")).toArray();
        auto a = assignments[0].toObject();
        a.insert(QStringLiteral("evidenceId"), QStringLiteral("unknown"));
        assignments[0] = a;
        bad = answer;
        bad.insert(QStringLiteral("assignments"), assignments);
        QVERIFY(WorkStructure::applyModel(snap, bad, &error).isEmpty());
        assignments = answer.value(QStringLiteral("assignments")).toArray();
        a = assignments[1].toObject();
        a.insert(QStringLiteral("coordinate"), QJsonArray{1, 99});
        assignments[1] = a;
        bad = answer;
        bad.insert(QStringLiteral("assignments"), assignments);
        QVERIFY(WorkStructure::applyModel(snap, bad, &error).isEmpty());
    }
    void preservesUnsupportedRecordsAndDetectsStaleSource()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        QVERIFY(fixture(path));
        EPubContainer before(nullptr);
        QVERIFY(before.openFile(path));
        const auto snap = WorkStructure::snapshot(before, QStringLiteral("work-test"));
        QString error;
        auto record = WorkStructure::confirmedRecord(snap, reviewed(snap), &error);
        record.insert(QStringLiteral("schemaVersion"), 99);
        const auto target = dir.filePath(QStringLiteral("version.epub"));
        QVERIFY(write(target, WorkStructure::candidate(before, record, &error)));
        EPubContainer unsupported(nullptr);
        QVERIFY(unsupported.openFile(target));
        const auto next = WorkStructure::snapshot(unsupported, QStringLiteral("work-test"));
        QCOMPARE(next.value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity")).toString(), QStringLiteral("unsupported"));
        QVERIFY(WorkStructure::candidate(unsupported, record, &error).isEmpty());
        record.insert(QStringLiteral("schemaVersion"), 1);
        QVERIFY(write(target, WorkStructure::candidate(before, record, &error)));
        EPubContainer valid(nullptr);
        QVERIFY(valid.openFile(target));
        auto changed = valid.readData(QStringLiteral("OEBPS/chapter.xhtml"));
        changed.replace("Ordinary", "Altered");
        const auto stalePath = dir.filePath(QStringLiteral("stale.epub"));
        QVERIFY(write(stalePath, valid.createMetadataCandidate({{QStringLiteral("OEBPS/chapter.xhtml"), changed}})));
        EPubContainer stale(nullptr);
        QVERIFY(stale.openFile(stalePath));
        const auto state = WorkStructure::snapshot(stale, QStringLiteral("work-test")).value(QStringLiteral("existing")).toObject();
        QCOMPARE(state.value(QStringLiteral("validity")).toString(), QStringLiteral("stale"));
        QCOMPARE(state.value(QStringLiteral("record")).toObject(), record);
        QVERIFY(!WorkStructure::preservesSource(valid, stale, &error));
    }
    void commitPreservesAnnotationsAndRejectsOldHash()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        const QString id = QStringLiteral("commit-test");
        QVERIFY(fixture(path, 3, id));
        registerBook(path, id);
        auto &db = BookDatabase::self();
        QVariantMap annotation{{QStringLiteral("annotationId"), QStringLiteral("note")},
                               {QStringLiteral("anchorId"), QStringLiteral("uuid_11111111-1111-4111-8111-111111111111")},
                               {QStringLiteral("cfiRange"), QStringLiteral("epubcfi(/6/2!/2/4/2/1:0)")},
                               {QStringLiteral("text"), QStringLiteral("Personal correction")}};
        db.saveAnnotation(id, annotation);
        const auto annotations = db.loadAnnotations(id);
        auto state = BookTruthStore().openBook(id);
        QVERIFY(state.success);
        EPubContainer original(nullptr);
        QVERIFY(original.openFile(path));
        const auto snap = WorkStructure::snapshot(original, id);
        QString error;
        const auto target = dir.filePath(QStringLiteral("candidate.epub"));
        const auto record = WorkStructure::confirmedRecord(snap, reviewed(snap), &error);
        QVERIFY(write(target, WorkStructure::candidate(original, record, &error)));
        const auto rejected = BookTruthStore().commitCandidateBookFile(id, target, false, true, QByteArrayLiteral("old-hash"), true);
        QVERIFY(!rejected.success);
        QCOMPARE(BookTruthStore().openBook(id).epubFileHash, state.epubFileHash);
        const auto result = BookTruthStore().commitCandidateBookFile(id, target, false, true, state.epubFileHash, true);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(result.textContentHash, state.textContentHash);
        QCOMPARE(db.loadAnnotations(id), annotations);
        EPubContainer saved(nullptr);
        QVERIFY(saved.openFile(path));
        QCOMPARE(saved.readData(QStringLiteral("OEBPS/chapter.xhtml")), original.readData(QStringLiteral("OEBPS/chapter.xhtml")));
    }
    void processLockBlocksMutation()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        const QString id = QStringLiteral("lock-test");
        QVERIFY(fixture(path, 3, id));
        registerBook(path, id);
        const auto state = BookTruthStore().openBook(id);
        QVERIFY(state.success);
        QProcess peer;
        peer.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-lock"), path + QStringLiteral(".arianna-book.lock")});
        QVERIFY(peer.waitForReadyRead(5000));
        QCOMPARE(peer.readAllStandardOutput().trimmed(), QByteArrayLiteral("locked"));
        const auto rejected = BookTruthStore().commitCandidateBookFile(id, path);
        QVERIFY(!rejected.success);
        peer.write("exit\n");
        peer.closeWriteChannel();
        QVERIFY(peer.waitForFinished(5000));
        const auto result = BookTruthStore().commitCandidateBookFile(id, path);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
    }
    void asynchronousValidatorAndIdempotence()
    {
        if (!QFile::exists(QStringLiteral("/usr/share/java/epubcheck.jar")))
            QSKIP("EPUBCheck not installed");
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        const QString id = QStringLiteral("async-test");
        QVERIFY(fixture(path, 3, id));
        registerBook(path, id);
        EPubContainer book(nullptr);
        QVERIFY(book.openFile(path));
        auto snap = WorkStructure::snapshot(book, id);
        const auto draft = reviewed(snap);
        bool done = false;
        QJsonObject result;
        const auto sourceHash = book.getFileHash();
        WorkStructureCommit::start(this, id, path, snap, draft, {}, QStringLiteral("/bin/false"), [&](const auto &r) {
            result = r;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!result.value(QStringLiteral("success")).toBool());
        EPubContainer rejected(nullptr);
        QVERIFY(rejected.openFile(path));
        QCOMPARE(rejected.getFileHash(), sourceHash);
        QCOMPARE(WorkStructure::snapshot(rejected, id).value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity")).toString(),
                 QStringLiteral("absent"));
        done = false;
        WorkStructureCommit::start(this, id, path, snap, draft, {}, QStringLiteral("java -jar /usr/share/java/epubcheck.jar"), [&](const auto &r) {
            result = r;
            done = true;
        });
        QVERIFY(!done);
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(result.value(QStringLiteral("success")).toBool(),
                 qPrintable(result.value(QStringLiteral("message")).toString() + result.value(QStringLiteral("validatorOutput")).toString()));
        EPubContainer saved(nullptr);
        QVERIFY(saved.openFile(path));
        snap = WorkStructure::snapshot(saved, id);
        const auto oldRecord = snap.value(QStringLiteral("existing")).toObject().value(QStringLiteral("record")).toObject();
        const auto originalHash = saved.getFileHash();
        done = false;
        WorkStructureCommit::start(this,
                                   id,
                                   path,
                                   snap,
                                   reviewed(snap),
                                   oldRecord.value(QStringLiteral("revision")).toString(),
                                   QStringLiteral("epubcheck"),
                                   [&](const auto &r) {
                                       result = r;
                                       done = true;
                                   });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY(result.value(QStringLiteral("success")).toBool());
        QVERIFY(result.value(QStringLiteral("unchanged")).toBool());
        QCOMPARE(saved.getFileHash(), originalHash);
    }
    void serverApiAndReviewDialog()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("book.epub"));
        const QString id = QStringLiteral("api-test");
        QVERIFY(fixture(path, 3, id));
        registerBook(path, id);
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
        Config::setBookServerPort(probe.serverPort());
        probe.close();
        Config::setEpubCheckCommand(QStringLiteral("java -jar /usr/share/java/epubcheck.jar"));
        BookServer server(QStringLiteral("work-token"));
        QVERIFY(server.isRunning());
        const QString url = QStringLiteral("http://127.0.0.1:%1").arg(Config::bookServerPort());
        WorkStructureReview review;
        review.analyze(id, url, QStringLiteral("work-token"));
        QTRY_VERIFY_WITH_TIMEOUT(!review.busy(), 5000);
        QVERIFY2(!review.snapshot().isEmpty(), qPrintable(review.message()));
        const auto snap = QJsonObject::fromVariantMap(review.snapshot());
        QCOMPARE(snap.value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity")).toString(), QStringLiteral("absent"));
        QSignalSpy saved(&review, &WorkStructureReview::saved);
        review.commit(reviewed(snap).toVariantMap());
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!review.busy(), 5000);
        QCOMPARE(QJsonObject::fromVariantMap(review.snapshot()).value(QStringLiteral("existing")).toObject().value(QStringLiteral("validity")).toString(),
                 QStringLiteral("valid"));
        const auto request = [&](const QString &suffix, const QJsonObject &body = QJsonObject{}, const QByteArray &token = QByteArrayLiteral("work-token")) {
            QNetworkAccessManager network;
            QNetworkRequest r{QUrl(url + QLatin1Char('/') + id + suffix)};
            r.setRawHeader("X-Arianna-Session-Token", token);
            r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            auto reply = body.isEmpty() ? network.get(r) : network.post(r, QJsonDocument(body).toJson());
            QSignalSpy done(reply, &QNetworkReply::finished);
            if (!reply->isFinished())
                done.wait(5000);
            const auto result = QJsonDocument::fromJson(reply->readAll()).object();
            return result;
        };
        const auto resolved = request(QStringLiteral("/workStructure/resolve"),
                                      QJsonObject{{QStringLiteral("text"), QStringLiteral("१-४")}, {QStringLiteral("role"), QStringLiteral("navigation")}});
        QVERIFY(resolved.value(QStringLiteral("resolved")).toBool());
        QCOMPARE(resolved.value(QStringLiteral("target")).toObject().value(QStringLiteral("semanticPath")).toArray().size(), 2);
        QVERIFY(!request(QStringLiteral("/workStructure/resolve"),
                         QJsonObject{{QStringLiteral("text"), QStringLiteral("१-५")}, {QStringLiteral("role"), QStringLiteral("navigation")}})
                     .value(QStringLiteral("resolved"))
                     .toBool());
        const auto targets = request(QStringLiteral("/targetLocations")).value(QStringLiteral("targetLocations")).toArray();
        bool enriched = false;
        for (const auto &target : targets)
            if (target.toObject().value(QStringLiteral("hrefLocation")).toString().endsWith(QStringLiteral("#verse4")))
                enriched = target.toObject().value(QStringLiteral("semanticPath")).toArray().size() == 2;
        QVERIFY(enriched);
        QVERIFY(request(QStringLiteral("/workStructure"), {}, QByteArrayLiteral("wrong-token")).isEmpty());
        server.registerReaderSession(QStringLiteral("work-token"));
        QNetworkAccessManager readOnlyNetwork;
        QNetworkRequest readOnlyRegistration{QUrl(url + QStringLiteral("/read-only-books"))};
        readOnlyRegistration.setRawHeader("X-Arianna-Session-Token", QByteArrayLiteral("work-token"));
        readOnlyRegistration.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        auto registeredReply = readOnlyNetwork.post(readOnlyRegistration, QJsonDocument(QJsonObject{{QStringLiteral("fileName"), path}}).toJson());
        QSignalSpy registered(registeredReply, &QNetworkReply::finished);
        QVERIFY(registered.wait(5000));
        const QString alias = QJsonDocument::fromJson(registeredReply->readAll()).object().value(QStringLiteral("identifier")).toString();
        QVERIFY(!alias.isEmpty());
        QNetworkRequest readOnlyRequest{QUrl(url + QLatin1Char('/') + alias + QStringLiteral("/workStructure"))};
        readOnlyRequest.setRawHeader("X-Arianna-Session-Token", QByteArrayLiteral("work-token"));
        auto readOnlyReply = readOnlyNetwork.get(readOnlyRequest);
        QSignalSpy readOnlyDone(readOnlyReply, &QNetworkReply::finished);
        QVERIFY(readOnlyDone.wait(5000));
        QCOMPARE(QJsonDocument::fromJson(readOnlyReply->readAll()).object().value(QStringLiteral("validity")).toString(), QStringLiteral("valid"));

        const auto idlessPath = dir.filePath(QStringLiteral("idless.epub"));
        QVERIFY(fixture(idlessPath, 3, QStringLiteral("idless-test")));
        EPubContainer idlessSource(nullptr);
        QVERIFY(idlessSource.openFile(idlessPath));
        auto idlessChapter = idlessSource.readData(QStringLiteral("OEBPS/chapter.xhtml"));
        idlessChapter.replace("<p>Ordinary", QStringLiteral("<p>भवतु ॥१-५॥</p><p>Ordinary").toUtf8());
        const auto withVerse = idlessSource.createMetadataCandidate({{QStringLiteral("OEBPS/chapter.xhtml"), idlessChapter}});
        QVERIFY(write(idlessPath, withVerse));
        registerBook(idlessPath, QStringLiteral("idless-test"));
        EPubContainer idlessBook(nullptr);
        QVERIFY(idlessBook.openFile(idlessPath));
        const auto idlessSnapshot = WorkStructure::snapshot(idlessBook, QStringLiteral("idless-test"));
        QString idlessError;
        const auto idlessRecord = WorkStructure::confirmedRecord(idlessSnapshot, reviewed(idlessSnapshot), &idlessError);
        const auto idlessCandidate = dir.filePath(QStringLiteral("idless-candidate.epub"));
        QVERIFY(write(idlessCandidate, WorkStructure::candidate(idlessBook, idlessRecord, &idlessError)));
        const auto idlessCommitted = BookTruthStore().commitCandidateBookFile(QStringLiteral("idless-test"),
                                                                              idlessCandidate,
                                                                              false,
                                                                              true,
                                                                              idlessSnapshot.value(QStringLiteral("sourceFileHash")).toString().toLatin1(),
                                                                              true);
        QVERIFY2(idlessCommitted.success, qPrintable(idlessCommitted.errorMessage));
        QNetworkAccessManager idlessNetwork;
        QNetworkRequest idlessRequest{QUrl(url + QStringLiteral("/idless-test/targetLocations"))};
        idlessRequest.setRawHeader("X-Arianna-Session-Token", QByteArrayLiteral("work-token"));
        auto idlessReply = idlessNetwork.get(idlessRequest);
        QSignalSpy idlessDone(idlessReply, &QNetworkReply::finished);
        QVERIFY(idlessDone.wait(5000));
        const auto idlessTargets = QJsonDocument::fromJson(idlessReply->readAll()).object().value(QStringLiteral("targetLocations")).toArray();
        bool idlessFound = false;
        for (const auto &v : idlessTargets) {
            const auto target = v.toObject();
            const auto path = target.value(QStringLiteral("semanticPath")).toArray();
            if (path.size() == 2 && path.last().toObject().value(QStringLiteral("number")).toInt() == 5)
                idlessFound = target.value(QStringLiteral("location")).toString().startsWith(QStringLiteral("epubcfi("));
        }
        QVERIFY(idlessFound);

        QQmlEngine engine;
        engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), url);
        engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("work-token"));
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"QML(
            import QtQuick
            import QtQuick.Controls
            import org.kde.arianna
            ApplicationWindow {
                width: 1100; height: 800; visible: true
                WorkStructureDialog { id: dialog; objectName: "review"; parent: Overlay.overlay }
            }
        )QML"),
                          QUrl());
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY(window);
        auto dialog = window->findChild<QObject *>(QStringLiteral("review"));
        QVERIFY(dialog);
        const auto eval = [&](const QString &code) {
            QQmlExpression expression(engine.rootContext(), dialog, code);
            const auto result = expression.evaluate();
            if (expression.hasError())
                QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
            return result;
        };
        eval(QStringLiteral("showForBook('api-test')"));
        QTRY_VERIFY_WITH_TIMEOUT(eval(QStringLiteral("rows.length")).toInt() > 0, 5000);
        QCOMPARE(eval(QStringLiteral("levels.length")).toInt(), 2);
        QCOMPARE(eval(QStringLiteral("rows.filter(r => r.confirmed).length")).toInt(), 2);
        eval(QStringLiteral("editRow(0, 'coordinateText', '2')"));
        QVERIFY(eval(QStringLiteral("dirty")).toBool());
        QVERIFY(!eval(QStringLiteral("rows[0].confirmed")).toBool());
        eval(QStringLiteral("close()"));
        window.reset();
        FakeSpeechDispatcher speechPeer(dir.filePath(QStringLiteral("speech.sock")), false, QStringLiteral("vits"), true);
        QVERIFY(speechPeer.startServer());
        qputenv("SPEECHD_ADDRESS", QByteArray(QByteArrayLiteral("unix_socket:") + dir.filePath(QStringLiteral("speech.sock")).toUtf8()));
        Config::setSpeechExpandSanskritNumbers(true);
        Config::setSpeechVoice(QString());
        engine.rootContext()->setContextProperty(QStringLiteral("testBookUrl"), QString(url + QStringLiteral("/api-test/book.epub")));
        engine.rootContext()->setContextProperty(QStringLiteral("testBookPath"), path);
        WorkRequestInterceptor interceptor;
        QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(&interceptor);
        const auto resetInterceptor = qScopeGuard([] {
            QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(nullptr);
        });
        QQmlComponent viewer(&engine);
        viewer.setData(QByteArrayLiteral(R"QML(
            import QtQuick
            import org.kde.kirigami as Kirigami
            import org.kde.arianna
            Kirigami.ApplicationWindow {
                width: 1100; height: 800; visible: true
                property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
                contextDrawer: Kirigami.ContextDrawer {
                    property string currentTocId: ""
                    property string currentHref: ""
                    property var model: QtObject { function importFromJson(json) {} }
                    function clearCurrentItem() {}
                    function setCurrentItem(id) {}
                    signal goTo(var location)
                }
                pageStack.initialPage: Kirigami.Page {}
                Component.onCompleted: pageStack.layers.push(readerComponent)
                Component {
                    id: readerComponent
                    EpubViewerPage {
                        objectName: "reader"
                        entry: ({uniqueIdentifier: "api-test", filename: testBookPath})
                        filename: testBookPath
                        url: testBookUrl
                        property bool scriptDone: false
                        property var scriptResult
                    }
                }
            }
        )QML"),
                       QUrl());
        QTRY_VERIFY(viewer.status() != QQmlComponent::Loading);
        QVERIFY2(viewer.isReady(), qPrintable(viewer.errorString()));
        QScopedPointer<QObject> readerWindow(viewer.create());
        QVERIFY(readerWindow);
        QTRY_VERIFY(readerWindow->findChild<QObject *>(QStringLiteral("reader")));
        auto page = readerWindow->findChild<QObject *>(QStringLiteral("reader"));
        auto speech = page->findChild<SpeechController *>(QStringLiteral("epubSpeechController"));
        QVERIFY(speech);
        const auto readerEval = [&](const QString &code) {
            QQmlExpression expression(qmlContext(speech), speech, code);
            const auto result = expression.evaluate();
            if (expression.hasError())
                QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
            return result;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readerEval(QStringLiteral("view.bookReady")).toBool(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(page->property("hasWorkStructure").toBool(), 5000);
        const auto script = [&](const QString &code) {
            const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
            readerEval(
                QStringLiteral("root.scriptDone=false; view.runJavaScript(%1[0], result => {root.scriptResult=result; root.scriptDone=true})").arg(encoded));
            QElapsedTimer timer;
            timer.start();
            while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000)
                QTest::qWait(10);
            if (!page->property("scriptDone").toBool())
                QTest::qFail("Reader JavaScript timed out", __FILE__, __LINE__);
            return page->property("scriptResult");
        };
        const QString beforeText = script(QStringLiteral("reader.view.renderer.getContents()[0].doc.body.textContent")).toString();
        script(QStringLiteral(R"JS((() => {
            const {doc, index} = reader.view.renderer.getContents()[0];
            const range = doc.createRange(); range.selectNodeContents(doc.getElementById('verse4'));
            reader.prepareSpeech(doc, range, index).then(() => {globalThis.structureSegments = reader.collectSpeechSegments(range, index)});
            return true;
        })())JS"));
        QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("!!globalThis.structureSegments")).toBool(), 5000);
        const auto segments = script(QStringLiteral("globalThis.structureSegments")).toList();
        QVariantMap coordinate;
        for (const auto &segment : segments)
            if (segment.toMap().value(QStringLiteral("type")).toString() == QLatin1String("coordinate"))
                coordinate = segment.toMap();
        int coordinateCount = 0;
        for (const auto &segment : segments)
            if (segment.toMap().value(QStringLiteral("type")).toString() == QLatin1String("coordinate"))
                ++coordinateCount;
        QCOMPARE(coordinateCount, 1);
        QVERIFY2(!coordinate.isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(segments).toJson())));
        QCOMPARE(coordinate.value(QStringLiteral("text")).toString(), QStringLiteral("१-४"));
        QCOMPARE(coordinate.value(QStringLiteral("marks")).toList().size(), 1);
        QCOMPARE(coordinate.value(QStringLiteral("structureCoordinate")).toList().size(), 2);

        QVERIFY(speech);
        speech->speakSegments(segments);
        QTRY_VERIFY_WITH_TIMEOUT(!speechPeer.messages().isEmpty(), 5000);
        bool coordinateSpoken = false;
        int acknowledged = 0;
        QElapsedTimer speechTimer;
        speechTimer.start();
        while (!coordinateSpoken && speechTimer.elapsed() < 5000) {
            QTest::qWait(10);
            const auto messages = speechPeer.messages();
            while (acknowledged < messages.size()) {
                const auto message = messages.at(acknowledged++);
                coordinateSpoken |= message.text.contains(QStringLiteral("आश्वास एकम्, श्लोक चत्वारि"));
                speechPeer.sendOldBegin(message.id, message.client);
                speechPeer.sendOldEnd(message.id, message.client);
            }
        }
        QVERIFY(coordinateSpoken);
        speech->stop();
        QTRY_COMPARE(speech->state(), SpeechController::Idle);
        readerEval(QStringLiteral("workCoordinateDialog.open(); workCoordinateInput.text='१-४'; workCoordinateButton.clicked()"));
        QTRY_VERIFY_WITH_TIMEOUT(!page->property("coordinateBusy").toBool(), 5000);
        QVERIFY(!readerEval(QStringLiteral("workCoordinateDialog.visible")).toBool());
        QCOMPARE(script(QStringLiteral("reader.view.renderer.getContents()[0].doc.body.textContent")).toString(), beforeText);
        readerEval(QStringLiteral("url = ''"));
        qunsetenv("SPEECHD_ADDRESS");
    }

    void sanskritCoordinatePayload()
    {
        QCOMPARE(SanskritNumbers::coordinateForSpeech({QVariantMap{{QStringLiteral("number"), 1}, {QStringLiteral("devanagari"), QStringLiteral("आश्वास")}},
                                                       QVariantMap{{QStringLiteral("number"), 4}, {QStringLiteral("devanagari"), QStringLiteral("श्लोक")}}}),
                 QStringLiteral("आश्वास एकम्, श्लोक चत्वारि"));
        QVERIFY(SanskritNumbers::coordinateForSpeech({QVariantMap{{QStringLiteral("number"), 999}, {QStringLiteral("devanagari"), QStringLiteral("श्लोक")}}})
                    .isEmpty());
        QCOMPARE(SanskritNumbers::expandForSpeech(QStringLiteral("१-४")), QStringLiteral("१-४"));
    }
};
int main(int argc, char **argv)
{
    if (argc == 3 && QByteArray(argv[1]) == QByteArrayLiteral("--hold-lock")) {
        QCoreApplication app(argc, argv);
        QLockFile lock(QString::fromLocal8Bit(argv[2]));
        if (!lock.tryLock())
            return 2;
        std::cout << "locked\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
        return 0;
    }
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    WorkStructureTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "workstructuretest.moc"
