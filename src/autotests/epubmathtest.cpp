// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "epubcontainer.h"
#include "epubmath.h"
#include "speechcontroller.h"
#ifndef Q_MOC_RUN
#include "fakespeechdispatcher.h"
#endif

#include <KArchiveFile>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QDomDocument>
#include <QGuiApplication>
#include <QHttpServer>
#include <QHttpServerRouter>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QPointer>
#include <QTest>
#include <QUrlQuery>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>
#include <QtWebEngineQuick>
#include <QtQml/QQmlExtensionPlugin>
#include <algorithm>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

namespace
{
QByteArray document(const QByteArray &body, const QByteArray &head = {})
{
    return QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"de\"><head><title>Math</title>")
        + head + QByteArrayLiteral("</head><body>") + body + QByteArrayLiteral("</body></html>");
}

class OfflineRequests : public QWebEngineUrlRequestInterceptor
{
public:
    QStringList remote;
    QStringList local;
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        if (url.host() == QLatin1String("127.0.0.1")) local.append(url.path());
        if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) && url.host() != QLatin1String("127.0.0.1")) {
            remote.append(url.toString());
            info.block(true);
        }
    }
};

class CheckedPage : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;
    QStringList diagnostics;
protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int, const QString &) override
    {
        if (message.contains(QStringLiteral("Invalid option")) || message.contains(QStringLiteral("Refused to load the font")))
            diagnostics.append(message);
    }
};

QVariant run(QWebEnginePage &page, const QString &code)
{
    QVariant result;
    bool done = false;
    page.runJavaScript(code, [&](const QVariant &value) { result = value; done = true; });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 5000) QTest::qWait(10);
    return result;
}

QString speechHelpers(const QString &source)
{
    const auto section = [&source](const QString &start, const QString &stop) {
        const auto begin = source.indexOf(start);
        const auto end = source.indexOf(stop, begin);
        if (begin < 0 || end <= begin) {
            QTest::qFail("Reader speech helper section not found", __FILE__, __LINE__);
            return QString();
        }
        return source.mid(begin, end - begin);
    };
    return section(QStringLiteral("const rangeForNode ="), QStringLiteral("const blobToBase64 ="))
        + section(QStringLiteral("const crossReferenceAnchorSelector ="), QStringLiteral("const compactAnnotationId ="))
        + section(QStringLiteral("const compactText ="), QStringLiteral("const createReference ="))
        + QStringLiteral(";\n");
}
}

class EpubMathTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void detection_data();
    void detection();
    void deliveryCopyKeepsSourceArchive();
    void bookServerDelivery();
    void offlineTypesettingAndCFI();
    void speechAbbreviationSentenceBoundary();
    void ownedMathJaxBaseline();
    void paginationWaitsForMath_data();
    void paginationWaitsForMath();
    void scriptPolicies();
    void disabledEpubScripts();
    void previewMenuIsolated();
    void nativeFormulaWindow();
    void viewerFormulaContextMenu();
    void strictFontPolicy();
    void previewSpeechLocale();
    void baselSeriesReading_data();
    void baselSeriesReading();
    void displaySettings_data();
    void displaySettings();
    void previewDisplaySettings();
    void settingsPreview();
private:
    QString startServer(BookServer &server);
};

#ifndef Q_MOC_RUN
QString EpubMathTest::startServer(BookServer &server)
{
    Q_UNUSED(server)
    return BookServerConfig::baseUrl();
}

void EpubMathTest::detection_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QByteArray>("head");
    QTest::addColumn<bool>("expected");
    QTest::newRow("tex-inline") << QByteArrayLiteral("<p>Before \\(x^2+1\\) after.</p>") << QByteArray{} << true;
    QTest::newRow("tex-display") << QByteArrayLiteral("<p>\\[x^2\\]</p>") << QByteArray{} << true;
    QTest::newRow("tex-dollars") << QByteArrayLiteral("<p>$$x+1$$</p>") << QByteArray{} << true;
    QTest::newRow("tex-single-dollar") << QByteArrayLiteral("<p>$x^2+1$</p>") << QByteArray{} << true;
    QTest::newRow("mathml") << QByteArrayLiteral("<math xmlns=\"http://www.w3.org/1998/Math/MathML\"><mi>x</mi></math>") << QByteArray{} << true;
    QTest::newRow("calibre-empty-config") << QByteArrayLiteral("<p>$x$</p>") << QByteArrayLiteral("<script type=\"text/x-mathjax-config\"></script>") << true;
    QTest::newRow("calibre-config-code") << QByteArrayLiteral("<p>$x$</p>") << QByteArrayLiteral("<script type=\"text/x-mathjax-config\">window.authorConfigExecuted=true</script>") << true;
    QTest::newRow("v2-tex-script") << QByteArrayLiteral("<script type=\"math/tex; mode=display\">x+1</script>") << QByteArray{} << true;
    QTest::newRow("prose") << QByteArrayLiteral("<p>Plain text with an <a id=\"ref\">anchor</a>.</p>") << QByteArray{} << false;
    QTest::newRow("currency") << QByteArrayLiteral("<p>$10 and $20</p>") << QByteArray{} << false;
    QTest::newRow("currency-range") << QByteArrayLiteral("<p>$10–$20</p>") << QByteArray{} << false;
    QTest::newRow("code-example") << QByteArrayLiteral("<pre>\\(x\\)</pre><code>$$x$$</code>") << QByteArray{} << false;
    QTest::newRow("script-text") << QByteArrayLiteral("<p>Prose.</p>") << QByteArrayLiteral("<script>const x='\\(x\\)'</script>") << false;
}

void EpubMathTest::detection()
{
    QFETCH(QByteArray, body);
    QFETCH(QByteArray, head);
    QFETCH(bool, expected);
    const auto original = document(body, head);
    QCOMPARE(EpubMath::needsMathJax(original), expected);
    const auto served = EpubMath::injectMathJax(original, QStringLiteral("http://127.0.0.1/mathjax/token/mathjax-bootstrap.js"));
    QCOMPARE(served.contains("data-arianna-mathjax=\"4\""), expected);
    if (!expected) QCOMPARE(served, original);
    else QCOMPARE(EpubMath::injectMathJax(served, QStringLiteral("ignored")), served);
}

void EpubMathTest::deliveryCopyKeepsSourceArchive()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("math.epub"));
    KZip zip(path);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArrayLiteral("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    zip.writeFile(QStringLiteral("package.opf"), QByteArrayLiteral("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Math</dc:title><dc:identifier id=\"id\">math</dc:identifier></metadata><manifest><item id=\"math\" href=\"math.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"prose\" href=\"prose.xhtml\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"math\"/><itemref idref=\"prose\"/></spine></package>"));
    const auto math = document(QByteArrayLiteral("\n<p>Before \\(x\\) after.</p>\n"));
    const auto prose = document(QByteArrayLiteral("<p>Plain.</p>"));
    zip.writeFile(QStringLiteral("math.xhtml"), math);
    zip.writeFile(QStringLiteral("prose.xhtml"), prose);
    zip.close();
    QFile archive(path);
    QVERIFY(archive.open(QIODevice::ReadOnly));
    const auto original = archive.readAll();
    archive.close();
    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(path));
    QVERIFY(epub.needsMathJax());
    ServerReadyEpubOptions options;
    options.mathJaxBootstrapUrl = QStringLiteral("http://127.0.0.1/mathjax/token/mathjax-bootstrap.js");
    QBuffer buffer;
    buffer.setData(epub.createServerReadyEpub(options));
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip delivered(&buffer);
    QVERIFY(delivered.open(QIODevice::ReadOnly));
    const auto bytes = [&](const QString &name) {
        QScopedPointer<QIODevice> device(delivered.directory()->file(name)->createDevice());
        return device->readAll();
    };
    QCOMPARE(bytes(QStringLiteral("prose.xhtml")), prose);
    const auto served = bytes(QStringLiteral("math.xhtml"));
    QVERIFY(served.contains("data-arianna-mathjax"));
    QDomDocument before, after;
    const auto parseOptions = QDomDocument::ParseOptions(QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    QVERIFY(before.setContent(math, parseOptions));
    QVERIFY(after.setContent(served, parseOptions));
    const auto beforeBody = before.elementsByTagName(QStringLiteral("body")).at(0);
    const auto afterBody = after.elementsByTagName(QStringLiteral("body")).at(0);
    QCOMPARE(afterBody.childNodes().size(), beforeBody.childNodes().size());
    QCOMPARE(afterBody.toElement().text(), beforeBody.toElement().text());
    QVERIFY(archive.open(QIODevice::ReadOnly));
    QCOMPARE(archive.readAll(), original);
}

void EpubMathTest::bookServerDelivery()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    server.registerReaderSession(QStringLiteral("math-test"));
    QNetworkAccessManager network;
    const auto request = [&](const QString &path, const QJsonObject &body = QJsonObject{}, const QString &token = QStringLiteral("math-test")) {
        QNetworkRequest req(QUrl(startServer(server) + path));
        req.setRawHeader("X-Arianna-Session-Token", token.toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QScopedPointer<QNetworkReply> reply(body.isEmpty() ? network.get(req) : network.post(req, QJsonDocument(body).toJson()));
        QSignalSpy done(reply.data(), &QNetworkReply::finished);
        if (!reply->isFinished()) done.wait(5000);
        return QPair<int, QByteArray>{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll()};
    };
    QCOMPARE(request(QStringLiteral("/mathjax/wrong-token/tex-mml-svg.js")).first, 401);
    QCOMPARE(request(QStringLiteral("/mathjax/math-test/sre/mathmaps/de.json")).first, 200);
    QCOMPARE(request(QStringLiteral("/mathjax/math-test/fonts/mathjax-newcm-font/svg/dynamic/cyrillic.js")).first, 200);
    QCOMPARE(request(QStringLiteral("/mathjax/math-test/absent.js")).first, 404);
    QTemporaryDir dir;
    for (bool mathematics : {false, true}) {
        const QString path = dir.filePath(mathematics ? QStringLiteral("math.epub") : QStringLiteral("prose.epub"));
        KZip zip(path);
        QVERIFY(zip.open(QIODevice::WriteOnly));
        zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
        zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArrayLiteral("<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"package.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
        zip.writeFile(QStringLiteral("package.opf"), QByteArrayLiteral("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Delivery</dc:title><dc:identifier id=\"id\">delivery</dc:identifier></metadata><manifest><item id=\"chapter\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"chapter\"/></spine></package>"));
        zip.writeFile(QStringLiteral("chapter.xhtml"), document(mathematics ? QByteArrayLiteral("<p>\\(x\\)</p>") : QByteArrayLiteral("<p>Prose.</p>")));
        zip.close();
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto source = original.readAll();
        const auto registered = request(QStringLiteral("/read-only-books"), QJsonObject{{QStringLiteral("fileName"), path}});
        QCOMPARE(registered.first, 200);
        const auto id = QJsonDocument::fromJson(registered.second).object().value(QStringLiteral("identifier")).toString();
        QVERIFY(!id.isEmpty());
        const auto delivered = request(QStringLiteral("/%1/book.epub?resourceMode=include").arg(id));
        QCOMPARE(delivered.first, 200);
        if (!mathematics) {
            QCOMPARE(delivered.second, source);
        } else {
            const auto disabled = request(QStringLiteral("/%1/book.epub?resourceMode=include&mathJaxSupport=false").arg(id));
            QCOMPARE(disabled.first, 200);
            QCOMPARE(disabled.second, source);
            // Resource processing must also respect the switch when delivery
            // needs a generated copy instead of the original archive.
            const auto rewritten = request(QStringLiteral("/%1/book.epub?resourceMode=outsource&mathJaxSupport=false").arg(id));
            QCOMPARE(rewritten.first, 200);
            QBuffer disabledData;
            disabledData.setData(rewritten.second);
            QVERIFY(disabledData.open(QIODevice::ReadOnly));
            KZip disabledArchive(&disabledData);
            QVERIFY(disabledArchive.open(QIODevice::ReadOnly));
            QScopedPointer<QIODevice> disabledChapter(disabledArchive.directory()->file(QStringLiteral("chapter.xhtml"))->createDevice());
            QCOMPARE(disabledChapter->readAll(), document(QByteArrayLiteral("<p>\\(x\\)</p>")));
            // Re-enable in the same server session; the container cache must
            // not retain the preceding request's disabled delivery behavior.
            const QString preferences = QStringLiteral(R"({"scale":1.15,"align":"right","overflow":"linebreak","inline":true,"fontCache":"global"})");
            const auto reenabled = request(QStringLiteral("/%1/book.epub?resourceMode=include&mathJaxSupport=true&mathJaxRendering=%2")
                .arg(id, QString::fromUtf8(QUrl::toPercentEncoding(preferences))));
            QCOMPARE(reenabled.first, 200);
            QBuffer data;
            data.setData(reenabled.second);
            QVERIFY(data.open(QIODevice::ReadOnly));
            KZip archive(&data);
            QVERIFY(archive.open(QIODevice::ReadOnly));
            QScopedPointer<QIODevice> chapter(archive.directory()->file(QStringLiteral("chapter.xhtml"))->createDevice());
            const auto bytes = chapter->readAll();
            QVERIFY(bytes.contains("/mathjax/math-test/mathjax-bootstrap.js"));
            QVERIFY(bytes.contains("\\(x\\)"));
            QDomDocument chapterDocument;
            QVERIFY(chapterDocument.setContent(bytes));
            const auto scripts = chapterDocument.elementsByTagName(QStringLiteral("script"));
            QCOMPARE(scripts.size(), 1);
            const QUrlQuery bootstrapQuery(QUrl(scripts.at(0).toElement().attribute(QStringLiteral("src"))));
            QCOMPARE(bootstrapQuery.queryItemValue(QStringLiteral("rendering"), QUrl::FullyDecoded), preferences);
            original.seek(0);
            QCOMPARE(original.readAll(), source);
        }
    }
}

void EpubMathTest::offlineTypesettingAndCFI()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    QHttpServer modules;
    modules.router()->addConverter(QMetaType::fromType<QUrl>(), QStringLiteral("[A-Za-z0-9_./-]+"));
    modules.route(QStringLiteral("/<arg>"), [&](const QUrl &path) {
        QFile file(QFINDTESTDATA("../qml/foliate-js/") + path.toString());
        if (!file.open(QIODevice::ReadOnly)) return QHttpServerResponse(QHttpServerResponder::StatusCode::NotFound);
        return QHttpServerResponse(QByteArrayLiteral("text/javascript"), file.readAll());
    });
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QVERIFY(modules.bind(&listener));
    const auto moduleUrl = QStringLiteral("http://127.0.0.1:%1/").arg(listener.serverPort());
    const auto bootstrap = startServer(server) + QStringLiteral("/mathjax/math-test/mathjax-bootstrap.js");
    OfflineRequests requests;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&requests);
    QWebEnginePage page(&profile);
    const auto html = document(QByteArrayLiteral(R"(<p id="mixed">Before \(\frac{a}{b}\) after.</p><p id="display">\[\sqrt{x}\]</p><math xmlns="http://www.w3.org/1998/Math/MathML" id="mml"><mfrac><mi>a</mi><mi>b</mi></mfrac></math><p id="single">$x+1$</p><a id="ref" href="#mixed" data-role="anchor">Reference</a>)"),
                               QByteArrayLiteral("<script type=\"text/x-mathjax-config\">window.authorConfigExecuted=true</script>"));
    auto loadAndTypeset = [&]() {
        QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
        page.setHtml(QString::fromUtf8(html), QUrl(moduleUrl));
        if (!loaded.wait(5000)) return false;
        run(page, QStringLiteral(R"JS(import('%1epubcfi.js').then(CFI => {
            globalThis.CFI = CFI;
            const p = document.getElementById('mixed');
            const text = p.firstChild;
            const r = document.createRange(); r.setStart(text, text.length - 6); r.setEnd(text, text.length - 1);
            globalThis.savedProseCFI = CFI.fromRange(r);
            const m = document.getElementById('mml');
            const mr = document.createRange(); mr.selectNodeContents(m.firstChild.firstChild.firstChild);
            globalThis.savedMathMLCFI = CFI.fromRange(mr);
            const script = document.createElement('script'); script.src='%2'; script.dataset.ariannaMathjax='4';
            document.head.append(script);
        }).catch(e => globalThis.testError=String(e)))JS").arg(moduleUrl, bootstrap));
        return true;
    };
    QVERIFY(loadAndTypeset());
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("!!document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toBool(), 20000);
    QCOMPARE(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toString(), QStringLiteral("ready"));
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('mjx-container').length")).toInt(), 4);
    QVERIFY(!run(page, QStringLiteral("window.authorConfigExecuted === true")).toBool());
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('[data-arianna-math-source]').length")).toInt(), 4);
    const auto details = run(page, QStringLiteral("ariannaMathDetails(document.querySelector('#mixed mjx-container'))")).toMap();
    QCOMPARE(details.value(QStringLiteral("tex")).toString(), QStringLiteral("\\frac{a}{b}"));
    QVERIFY(details.value(QStringLiteral("mathml")).toString().contains(QStringLiteral("<mfrac")));
    QVERIFY(run(page, QStringLiteral(R"JS(Array.from(document.querySelectorAll('mjx-container')).every(n =>
        n.querySelector('svg path') && n.getBoundingClientRect().width > 0 && n.getBoundingClientRect().height > 0
        && n.getAttribute('data-semantic-speech') && n.getAttribute('data-semantic-locale') === 'de'))JS")).toBool());
    const auto cfi = run(page, QStringLiteral("savedProseCFI")).toString();
    QVERIFY(!cfi.isEmpty());
    QCOMPARE(run(page, QStringLiteral("CFI.toRange(document, CFI.parse(savedProseCFI)).toString()")).toString(), QStringLiteral("after"));
    QCOMPARE(run(page, QStringLiteral("CFI.fromRange(CFI.toRange(document, CFI.parse(savedProseCFI)))")).toString(), cfi);
    const auto formulaCFI = run(page, QStringLiteral(R"JS((() => {
        const r = document.createRange(); r.selectNodeContents(document.querySelector('mjx-container'));
        return CFI.fromRange(r);
    })())JS")).toString();
    QVERIFY(!formulaCFI.isEmpty());
    QVERIFY(run(page, QStringLiteral(R"JS((() => {
        const r = document.createRange(); r.setStart(document.querySelector('mjx-container'), 0); r.collapse(true);
        const restored = CFI.toRange(document, CFI.parse(CFI.fromRange(r)));
        return restored.collapsed && restored.startContainer.localName === 'mjx-container';
    })())JS")).toBool());
    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(reader.readAll());
    const auto segments = run(page, speechHelpers(source) + QStringLiteral(R"JS(
        (() => {
            const range = document.createRange(); range.selectNodeContents(document.getElementById('mixed'));
            const segments = collectSpeechContent(range, ['de'], undefined, r => CFI.fromRange(r));
            return segments.map(s => ({type:s.type ?? 'text',text:s.text,lang:s.lang,cfi:s.cfi}));
        })())JS")).toList();
    QCOMPARE(segments.size(), 3);
    QCOMPARE(segments[1].toMap().value(QStringLiteral("type")).toString(), QStringLiteral("math"));
    QCOMPARE(segments[1].toMap().value(QStringLiteral("lang")).toString(), QStringLiteral("de"));
    QCOMPARE(segments[1].toMap().value(QStringLiteral("text")).toString(), QStringLiteral("a geteilt durch b"));
    QVERIFY(run(page, QStringLiteral("CFI.toRange(document, CFI.parse(savedMathMLCFI)).getBoundingClientRect().width > 0")).toBool());
    QVERIFY(run(page, QStringLiteral("document.querySelector('#ref').getAttribute('href') === '#mixed' && !!document.querySelector('#mixed')")).toBool());
    // Exercise an actual v4 dynamic font fetch, beyond the default font tables.
    run(page, QStringLiteral(R"JS((async () => {
        try {
            const output=await MathJax.mathml2svgPromise('<math xmlns="http://www.w3.org/1998/Math/MathML"><mi>&#x416;</mi></math>', {display:false});
            document.body.append(output); globalThis.dynamicFontReady=!!output.querySelector('svg path');
        } catch(e) {globalThis.dynamicFontError=String(e)}
    })())JS"));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.dynamicFontReady || !!globalThis.dynamicFontError")).toBool(), 5000);
    QVERIFY2(run(page, QStringLiteral("globalThis.dynamicFontReady === true")).toBool(), qPrintable(run(page, QStringLiteral("globalThis.dynamicFontError")).toString()));
    QVERIFY(std::any_of(requests.local.cbegin(), requests.local.cend(), [](const QString &path) {
        return path.contains(QStringLiteral("/fonts/mathjax-newcm-font/svg/dynamic/cyrillic"));
    }));
    // Recreate the chapter realm, as chapter reload and a reader restart do.
    QVERIFY(loadAndTypeset());
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("!!document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toBool(), 20000);
    QCOMPARE(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toString(), QStringLiteral("ready"));
    QCOMPARE(run(page, QStringLiteral("CFI.toRange(document, CFI.parse(%1)).toString()").arg(QString::fromUtf8(QJsonDocument(QJsonArray{cfi}).toJson(QJsonDocument::Compact)).mid(1).chopped(1))).toString(), QStringLiteral("after"));
    const auto formula = QString::fromUtf8(QJsonDocument(QJsonArray{formulaCFI}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
    QVERIFY(run(page, QStringLiteral("CFI.toRange(document, CFI.parse(%1)).getBoundingClientRect().width > 0").arg(formula)).toBool());
    // Restored annotations use the real overlayer and the same visible range
    // as selections/reference targets, even in a freshly recreated chapter.
    run(page, QStringLiteral(R"JS((async () => {
        try {
            const { Overlayer } = await import('%1overlayer.js');
            const value = %2;
            const range = CFI.toRange(document, CFI.parse(value));
            const selection = document.getSelection();
            selection.removeAllRanges(); selection.addRange(range);
            const overlay = new Overlayer(); document.body.append(overlay.element);
            overlay.add(value, range, Overlayer.highlight); overlay.redraw();
            const rect = Array.from(range.getClientRects()).find(r => r.width > 0 && r.height > 0);
            globalThis.restoredAnnotationVerified = CFI.fromRange(selection.getRangeAt(0)) === value
                && !!overlay.element.querySelector('rect[width]')
                && overlay.hitTest({x:rect.left + rect.width / 2, y:rect.top + rect.height / 2})[0] === value;
        } catch(e) {globalThis.annotationError=String(e)}
    })())JS").arg(moduleUrl, formula));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.restoredAnnotationVerified !== undefined || !!globalThis.annotationError")).toBool(), 5000);
    QVERIFY2(run(page, QStringLiteral("globalThis.restoredAnnotationVerified === true")).toBool(), qPrintable(run(page, QStringLiteral("globalThis.annotationError ?? 'Restored math annotation failed'")).toString()));
    QVERIFY2(requests.remote.isEmpty(), qPrintable(requests.remote.join(QLatin1Char('\n'))));
}

void EpubMathTest::speechAbbreviationSentenceBoundary()
{
    QWebEnginePage page;
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    const QString firstSentence = QStringLiteral("1. Stufe: die in den vedischen Schriften empfohlen werden, wie z. B. haṭha-yoga, rāja-yoga.");
    const QString original = firstSentence + QStringLiteral(" 2. Stufe: Abschluss.");
    const QString quotedSpeech = QStringLiteral(
        "In jedem Augenblick werden wir von dem Verlangen geplagt, etwas zu besitzen: "
        "„Ich will ein Auto, ich will eine Frau, ich will Geld, ich will dies, ich will das usw.\" "
        "Auf der anderen Seite klagen wir ständig über etwas Verlorengegangenes: "
        "„Ich habe mein Geld verloren, ich habe meine Frau verloren, ich habe mein Auto verloren usw.\" "
        "Wenn man jedoch regelmäßig den Hare Kṛṣṇa-mantra chantet, wird man von diesen beiden Sorgen befreit.");
    page.setHtml(QStringLiteral("<html lang=\"de\"><body><p id=\"speech\">%1</p></body></html>").arg(original.toHtmlEscaped()));
    QVERIFY(loaded.wait(5000));

    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(reader.readAll());
    const auto result = run(page, speechHelpers(source) + QStringLiteral(R"JS(
        (() => {
            const range = document.createRange(); range.selectNodeContents(document.getElementById('speech'));
            return collectSpeechContent(range, ['de'], undefined, () => 'test-cfi')
                .map(segment => ({text: segment.text, sentenceId: segment.sentenceId}));
        })())JS")).toList();

    QCOMPARE(result.size(), 2);
    QCOMPARE(result.first().toMap().value(QStringLiteral("text")).toString(), firstSentence + QLatin1Char(' '));
    QCOMPARE(result.last().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("2. Stufe: Abschluss."));
    QVERIFY(result.first().toMap().value(QStringLiteral("sentenceId"))
            != result.last().toMap().value(QStringLiteral("sentenceId")));

    QSignalSpy quotedLoaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral("<html lang=\"de\"><body><p id=\"speech\">%1</p></body></html>").arg(quotedSpeech.toHtmlEscaped()));
    QVERIFY(quotedLoaded.wait(5000));
    const auto quotedResult = run(page, speechHelpers(source) + QStringLiteral(R"JS(
        (() => {
            const range = document.createRange(); range.selectNodeContents(document.getElementById('speech'));
            return collectSpeechContent(range, ['de'], undefined, () => 'test-cfi')
                .map(segment => ({text: segment.text, sentenceId: segment.sentenceId}));
        })())JS")).toList();
    QCOMPARE(quotedResult.size(), 3);
    QCOMPARE(quotedResult.at(0).toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("In jedem Augenblick werden wir von dem Verlangen geplagt, etwas zu besitzen: "
                            "„Ich will ein Auto, ich will eine Frau, ich will Geld, ich will dies, ich will das usw.\" "));
    QCOMPARE(quotedResult.at(1).toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Auf der anderen Seite klagen wir ständig über etwas Verlorengegangenes: "
                            "„Ich habe mein Geld verloren, ich habe meine Frau verloren, ich habe mein Auto verloren usw.\" "));
    QCOMPARE(quotedResult.at(2).toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Wenn man jedoch regelmäßig den Hare Kṛṣṇa-mantra chantet, wird man von diesen beiden Sorgen befreit."));
    QVERIFY(quotedResult.at(0).toMap().value(QStringLiteral("sentenceId")).toString()
            != quotedResult.at(1).toMap().value(QStringLiteral("sentenceId")).toString());
    QVERIFY(quotedResult.at(1).toMap().value(QStringLiteral("sentenceId")).toString()
            != quotedResult.at(2).toMap().value(QStringLiteral("sentenceId")).toString());

    QSignalSpy wordsLoaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral("<html lang=\"de\"><body><p id=\"speech\">In jedem <em>Augenblick</em> sprechen wir.</p></body></html>"));
    QVERIFY(wordsLoaded.wait(5000));
    const auto derivedWords = run(page, speechHelpers(source) + QStringLiteral(R"JS(
        (() => {
            const range = document.createRange(); range.selectNodeContents(document.getElementById('speech'));
            const text = range.toString();
            const marks = deriveSpeechWordMarks(document, range, text, 'de', word => word.toString());
            return {text, marks: marks.map(mark => ({
                text: mark.cfi, spoken: text.slice(mark.start, mark.start + mark.length),
            })), mismatch: deriveSpeechWordMarks(document, range, text.replace('Augenblick', 'Welt'), 'de', word => word.toString()).length};
        })())JS")).toMap();
    const auto words = derivedWords.value(QStringLiteral("marks")).toList();
    QCOMPARE(words.size(), 5);
    for (const auto &word : words) QCOMPARE(word.toMap().value(QStringLiteral("text")), word.toMap().value(QStringLiteral("spoken")));
    QCOMPARE(derivedWords.value(QStringLiteral("mismatch")).toInt(), 0);
}

void EpubMathTest::ownedMathJaxBaseline()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    QHttpServer modules;
    modules.router()->addConverter(QMetaType::fromType<QUrl>(), QStringLiteral("[A-Za-z0-9_./-]+"));
    const QString directory = QFINDTESTDATA("../qml/foliate-js/");
    modules.route(QStringLiteral("/<arg>"), [&](const QUrl &path) {
        QFile file(directory + path.toString());
        if (!file.open(QIODevice::ReadOnly)) return QHttpServerResponse(QHttpServerResponder::StatusCode::NotFound);
        return QHttpServerResponse(QByteArrayLiteral("text/javascript"), file.readAll());
    });
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QVERIFY(modules.bind(&listener));
    const QString moduleUrl = QStringLiteral("http://127.0.0.1:%1/").arg(listener.serverPort());
    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    const auto config = QStringLiteral(R"(<script>window.MathJax={loader:{paths:{mathjax:'%1'.replace(/\/$/,''),fonts:'%1fonts'}},output:{fontPath:'%1fonts/%%FONT%%-font'},options:{sre:{locale:'de'},worker:{path:'%1sre',maps:'%1sre/mathmaps'}},svg:{fontCache:'local'}};</script><script id="MathJax-script" src="%1tex-svg.js"></script>)").arg(base).toUtf8();
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setContent(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>Before \\(x^2+1\\) after.</p>"), config), base + QStringLiteral("mathjax-bootstrap.js")), QStringLiteral("application/xhtml+xml"), QUrl(moduleUrl));
    QVERIFY(loaded.wait(10000));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("!!document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toBool(), 20000);
    QCOMPARE(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toString(), QStringLiteral("ready"));
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('mjx-container').length")).toInt(), 1);
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('script[src$=\"tex-svg.js\"]').length")).toInt(), 1);
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('[data-arianna-math-wrapper]').length")).toInt(), 0);
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('script[src$=\"tex-mml-svg.js\"]').length")).toInt(), 0);
    QVERIFY(run(page, QStringLiteral("document.querySelector('mjx-container svg path') !== null")).toBool());
    QCOMPARE(run(page, QStringLiteral("ariannaMathDetails(document.querySelector('mjx-container')).tex")).toString(), QStringLiteral("x^2+1"));
    const QString signature = QStringLiteral(R"JS(JSON.stringify((() => {
        const svg = document.querySelector('mjx-container svg');
        return {viewBox:svg.getAttribute('viewBox'),width:svg.getAttribute('width'),height:svg.getAttribute('height'),
            paths:Array.from(svg.querySelectorAll('path'), p => p.getAttribute('d'))};
    })()))JS");
    const auto svg = run(page, signature).toString();
    const auto importCFI = [&]() {
        run(page, QStringLiteral("import('%1epubcfi.js').then(CFI => globalThis.CFI=CFI)").arg(moduleUrl));
    };
    importCFI();
    QTRY_VERIFY(run(page, QStringLiteral("!!globalThis.CFI")).toBool());
    const auto proseCFI = run(page, QStringLiteral(R"JS((() => {
        const r=document.createRange();const t=document.querySelector('p').lastChild;
        r.setStart(t,1);r.setEnd(t,6);return CFI.fromRange(r);
    })())JS")).toString();
    QCOMPARE(proseCFI, QStringLiteral("epubcfi(/4/2,/3:1,/3:6)"));
    const auto oldFormulaCFI = run(page, QStringLiteral(R"JS((() => {
        const r=document.createRange();r.setStart(document.querySelector('mjx-container'),0);r.collapse(true);
        return CFI.fromRange(r);
    })())JS")).toString();
    QCOMPARE(oldFormulaCFI, QStringLiteral("epubcfi(/4/2/2)"));
    // Saved CFIs from the existing rendered-DOM path retain their exact meaning.
    QSignalSpy reloaded(&page, &QWebEnginePage::loadFinished);
    page.setContent(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>Before \\(x^2+1\\) after.</p>"), config), base + QStringLiteral("mathjax-bootstrap.js")), QStringLiteral("application/xhtml+xml"), QUrl(moduleUrl));
    QVERIFY(reloaded.wait(10000));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus === 'ready'")).toBool(), 20000);
    importCFI();
    QTRY_VERIFY(run(page, QStringLiteral("!!globalThis.CFI")).toBool());
    QCOMPARE(run(page, QStringLiteral("CFI.toRange(document,CFI.parse('epubcfi(/4/2,/3:1,/3:6)')).toString()")).toString(), QStringLiteral("after"));
    QCOMPARE(run(page, QStringLiteral("CFI.toRange(document,CFI.parse('epubcfi(/4/2/2)')).startContainer.localName")).toString(), QStringLiteral("mjx-container"));
    // Applying reader preferences retains the original rendered tree behind
    // the visible output, so the existing saved coordinates need no migration.
    const QString preferences = QStringLiteral(R"({"scale":1.15,"align":"right","fontCache":"global"})");
    for (int reload = 0; reload < 2; ++reload) {
        reloaded.clear();
        page.setContent(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>Before \\(x^2+1\\) after.</p>"), config),
            base + QStringLiteral("mathjax-bootstrap.js?rendering=") + QString::fromUtf8(QUrl::toPercentEncoding(preferences))),
            QStringLiteral("application/xhtml+xml"), QUrl(moduleUrl));
        QVERIFY(reloaded.wait(10000));
        QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus === 'ready'")).toBool(), 20000);
        importCFI();
        QTRY_VERIFY(run(page, QStringLiteral("!!globalThis.CFI")).toBool());
        QCOMPARE(run(page, QStringLiteral("CFI.toRange(document,CFI.parse('epubcfi(/4/2,/3:1,/3:6)')).toString()")).toString(), QStringLiteral("after"));
        QVERIFY(run(page, QStringLiteral("CFI.toRange(document,CFI.parse('epubcfi(/4/2/2)')).startContainer.hasAttribute('data-arianna-math-output')")).toBool());
        QCOMPARE(run(page, QStringLiteral("CFI.fromRange(CFI.toRange(document,CFI.parse('epubcfi(/4/2/2)')))")).toString(), oldFormulaCFI);
        QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('script[src$=\"tex-svg.js\"]').length")).toInt(), 1);
        QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('script[src$=\"tex-mml-svg.js\"]').length")).toInt(), 0);
    }
    // Reader-supplied conversion uses the same SVG font outlines and geometry.
    reloaded.clear();
    page.setContent(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>Before \\(x^2+1\\) after.</p>")), base + QStringLiteral("mathjax-bootstrap.js")), QStringLiteral("application/xhtml+xml"), QUrl(moduleUrl));
    QVERIFY(reloaded.wait(10000));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus === 'ready'")).toBool(), 20000);
    QCOMPARE(run(page, signature).toString(), svg);
    QCOMPARE(run(page, QStringLiteral("document.querySelectorAll('[data-arianna-math-wrapper]').length")).toInt(), 1);
}

void EpubMathTest::paginationWaitsForMath_data()
{
    QTest::addColumn<bool>("math");
    QTest::addColumn<bool>("fixed");
    QTest::addColumn<bool>("owned");
    QTest::addColumn<bool>("preferences");
    QTest::newRow("paginated-tex") << true << false << false << false;
    QTest::newRow("fixed-math") << true << true << false << false;
    QTest::newRow("paginated-owned-math") << true << false << true << false;
    QTest::newRow("fixed-owned-math") << true << true << true << false;
    QTest::newRow("paginated-prose") << false << false << false << false;
    QTest::newRow("paginated-display-settings") << true << false << false << true;
    QTest::newRow("paginated-owned-display-settings") << true << false << true << true;
}

void EpubMathTest::paginationWaitsForMath()
{
    QFETCH(bool, math);
    QFETCH(bool, fixed);
    QFETCH(bool, owned);
    QFETCH(bool, preferences);
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    QHttpServer modules;
    modules.router()->addConverter(QMetaType::fromType<QUrl>(), QStringLiteral("[A-Za-z0-9_./-]+"));
    const QString directory = QFINDTESTDATA("../qml/foliate-js/");
    modules.route(QStringLiteral("/<arg>"), [&](const QUrl &path) {
        QFile file(path.toString().endsWith(QLatin1String("reader-interactions.js"))
            ? QFINDTESTDATA("../qml/reader-interactions.js") : directory + path.toString());
        if (!file.open(QIODevice::ReadOnly)) return QHttpServerResponse(QHttpServerResponder::StatusCode::NotFound);
        return QHttpServerResponse(QByteArrayLiteral("text/javascript"), file.readAll());
    });
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QVERIFY(modules.bind(&listener));
    const QString moduleUrl = QStringLiteral("http://127.0.0.1:%1/").arg(listener.serverPort());
    const QString assetBase = startServer(server) + QStringLiteral("/mathjax/math-test/");
    QByteArray head = QByteArrayLiteral("<meta name=\"viewport\" content=\"width=600,height=240\"/>");
    if (owned) head += QStringLiteral(R"(<script>window.MathJax={loader:{paths:{mathjax:'%1'.replace(/\/$/,''),fonts:'%1fonts'}},output:{fontPath:'%1fonts/%%FONT%%-font'},options:{sre:{locale:'de'},worker:{path:'%1sre',maps:'%1sre/mathmaps'},menuOptions:{settings:{zoom:'Click',alt:false}}},svg:{fontCache:'local'}};</script><script id="MathJax-script" src="%1tex-svg.js"></script>)").arg(assetBase).toUtf8();
    QByteArray section = document(QByteArrayLiteral("<p id=\"start\">Before.</p><p>\\[\\frac{a}{b}\\]</p><p id=\"end\">After.</p>"), head);
    if (preferences) section.replace("\\frac{a}{b}", "\\frac{a}{b}+1+2+3+4+5+6+7+8+9+10+11+12+13+14+15+16+17+18+19+20");
    QString bootstrap = assetBase + QStringLiteral("mathjax-bootstrap.js");
    if (preferences) bootstrap += QStringLiteral("?rendering=") + QString::fromUtf8(QUrl::toPercentEncoding(QStringLiteral(R"({"scale":1.15,"align":"right","overflow":"linebreak","inline":true,"fontCache":"local"})")));
    if (math) section = EpubMath::injectMathJax(section, bootstrap);
    else section.replace("\\[\\frac{a}{b}\\]", "Ordinary prose.");
    const QString encoded = QString::fromUtf8(QJsonDocument(QJsonArray{QString::fromUtf8(section)}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
    OfflineRequests requests;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&requests);
    QWebEnginePage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral("<html><body style='margin:0;width:600px;height:240px'></body></html>"), QUrl(moduleUrl));
    QVERIFY(loaded.wait(5000));
    run(page, QStringLiteral(R"JS((async () => {
        try {
            await import('%1%2.js');
            const content = %3;
            const url = URL.createObjectURL(new Blob([content], {type:'application/xhtml+xml'}));
            const renderer = document.createElement('%4');
            renderer.style.cssText='display:block;width:600px;height:240px';
            document.body.append(renderer); globalThis.renderer=renderer;
            globalThis.events=[];
            renderer.addEventListener('load', e => {
                const doc=e.detail.doc;
                events.push({status:doc.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus ?? 'prose',
                    formulas:Array.from(doc.querySelectorAll('mjx-container')).filter(node=>!node.closest('[data-arianna-math-source]')).length});
            });
            renderer.open({sections:[{load:() => url,linear:'yes'}],rendition:{},dir:'ltr'});
            await renderer.goTo({index:0});
            globalThis.paginated=true;
        } catch(e) {globalThis.testError=String(e)}
    })())JS").arg(moduleUrl, fixed ? QStringLiteral("fixed-layout") : QStringLiteral("paginator"), encoded,
                   fixed ? QStringLiteral("foliate-fxl") : QStringLiteral("foliate-paginator")));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.paginated || !!globalThis.testError")).toBool(), 25000);
    QVERIFY2(run(page, QStringLiteral("globalThis.testError ?? ''")).toString().isEmpty(), qPrintable(run(page, QStringLiteral("globalThis.testError")).toString()));
    QCOMPARE(run(page, QStringLiteral("events[0].status")).toString(), math ? QStringLiteral("ready") : QStringLiteral("prose"));
    QCOMPARE(run(page, QStringLiteral("events[0].formulas")).toInt(), math ? 1 : 0);
    if (math) {
        QVERIFY(run(page, QStringLiteral("renderer.getContents().some(c => (c.doc?.querySelector('[data-arianna-math-output]') ?? c.doc?.querySelector('mjx-container'))?.getBoundingClientRect().height > 0)")).toBool());
        if (preferences) {
            // goTo() must include width-dependent typesetting, not just startup.
            QVERIFY(run(page, QStringLiteral("renderer.getContents()[0].doc.querySelector('[data-arianna-math-output]').getBoundingClientRect().height > 30")).toBool());
            const auto geometry = run(page, QStringLiteral("JSON.stringify([renderer.getContents()[0].doc.body.scrollWidth,renderer.getContents()[0].doc.getElementById('end').getBoundingClientRect().x])")).toString();
            QTest::qWait(150);
            QCOMPARE(run(page, QStringLiteral("JSON.stringify([renderer.getContents()[0].doc.body.scrollWidth,renderer.getContents()[0].doc.getElementById('end').getBoundingClientRect().x])")).toString(), geometry);
            run(page, QStringLiteral("renderer.style.width='360px';renderer.render().then(()=>globalThis.resized=true)"));
            QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.resized === true")).toBool(), 5000);
        }
        QFile reader(QFINDTESTDATA("../qml/reader.js"));
        QVERIFY(reader.open(QIODevice::ReadOnly));
        const QString readerSource = QString::fromUtf8(reader.readAll());
        const auto selectorStart = readerSource.indexOf(QStringLiteral("const crossReferenceAnchorSelector ="));
        const auto selectorEnd = readerSource.indexOf(QStringLiteral("const compactAnnotationId ="));
        const auto clickStart = readerSource.indexOf(QStringLiteral("installReaderInteractions(doc, {"));
        const auto clickEnd = readerSource.indexOf(QStringLiteral("this.#cursorAutohider.cloneFor"), clickStart);
        const auto contextStart = readerSource.indexOf(QStringLiteral("doc.addEventListener('contextmenu'"));
        const auto contextEnd = readerSource.indexOf(QStringLiteral("let isSelecting ="), contextStart);
        QVERIFY(selectorStart >= 0 && selectorEnd > selectorStart && clickStart >= 0 && clickEnd > clickStart);
        QVERIFY(contextStart >= 0 && contextEnd > contextStart);
        run(page, QStringLiteral(R"JS((async () => {
            try {
                const CFI = await import('%1epubcfi.js');
                const {installReaderInteractions, readerInteractionCSS} = await import('%1reader-interactions.js');
                const doc = renderer.getContents().find(c => c.doc?.querySelector('mjx-container')).doc;
                const cursorStyle = doc.createElement('style'); cursorStyle.textContent = readerInteractionCSS; doc.head.append(cursorStyle);
                %2
                const actions = [];
                const dispatch = action => actions.push(action);
                class ClickHarness {
                    #contextImage = null;
                    #bookReferenceHandler = null;
                    #annotationKeyByValue = new Map();
                    #showAnnotation() { throw Error('Unexpected annotation action on plain formula'); }
                    view = new EventTarget();
                    constructor() { this.view.getCFI = (index, range) => CFI.fromRange(range); }
                    attach(doc, index = 0) { this.view.renderer = {getContents:() => [{doc}]}; %3 %4 }
                }
                new ClickHarness().attach(doc);
                const formula = doc.querySelector('[data-arianna-math-output]') ?? doc.querySelector('mjx-container');
                const range = doc.createRange(); range.setStart(formula, 0); range.collapse(true);
                const value = CFI.fromRange(range);
                const snapshot = () => JSON.stringify({math:formula.outerHTML,
                    width:doc.body.scrollWidth,height:doc.body.scrollHeight,
                    rect:Array.from(['x','y','width','height'], key => formula.getBoundingClientRect()[key])});
                const before = snapshot();
                let clicks = 0, contexts = 0;
                doc.addEventListener('click', () => ++clicks);
                doc.addEventListener('contextmenu', () => ++contexts);
                const target = formula.querySelector('svg path');
                const rect = target.getBoundingClientRect();
                const mouse = {bubbles:true,cancelable:true,view:doc.defaultView,
                    clientX:rect.left + rect.width / 2,clientY:rect.top + rect.height / 2};
                target.dispatchEvent(new doc.defaultView.PointerEvent('pointermove', mouse));
                const mathCursor = doc.documentElement.getAttribute('data-arianna-reader-cursor') === 'math'
                    && document.querySelector('[data-arianna-reader-cursor-badge="math"]')?.style.display === 'block';
                for (const type of ['mousedown','mouseup','click','dblclick','contextmenu'])
                    target.dispatchEvent(new doc.defaultView.MouseEvent(type, {...mouse,buttons:type === 'mousedown' ? 1 : 0}));
                await new Promise(resolve => setTimeout(resolve, 300));
                globalThis.mathClickResult = {unchanged:before === snapshot(), clicks, contexts,
                    mathCursor,
                    popup:!!doc.querySelector('dialog[open],.CtxtMenu_MenuFrame,mjx-container.mjx-explorer-active'),
                    cfi:CFI.fromRange(CFI.toRange(doc, CFI.parse(value))) === value,
                    speech:!!formula.getAttribute('data-semantic-speech'),
                    opens:actions.filter(a => a.type === 'show-math').length,
                    contextOpens:actions.filter(a => a.type === 'show-math-context-menu').length,
                    contextAnchor:actions.find(a => a.type === 'show-math-context-menu')?.payload.speechSegment.cfi === value,
                    imageOpens:actions.filter(a => a.type === 'show-image-context-menu').length,
                    source:actions[0]?.payload.mathml.includes('<mfrac'),
                    speechAnchor:actions[0]?.payload.speechSegment.cfi === value};
                const selection = doc.getSelection();
                const selected = doc.createRange(); selected.selectNodeContents(doc.getElementById('start'));
                selection.removeAllRanges(); selection.addRange(selected);
                target.dispatchEvent(new doc.defaultView.MouseEvent('click', mouse));
                globalThis.mathClickResult.selectionPreserved = actions.filter(a => a.type === 'show-math').length === 1 && selection.toString() === 'Before.';
                target.dispatchEvent(new doc.defaultView.PointerEvent('pointermove', mouse));
                globalThis.mathClickResult.selectionCursor = !doc.documentElement.hasAttribute('data-arianna-reader-cursor');
            } catch(e) {globalThis.mathClickError=String(e)}
        })())JS").arg(moduleUrl, readerSource.mid(selectorStart, selectorEnd - selectorStart), readerSource.mid(clickStart, clickEnd - clickStart),
                      readerSource.mid(contextStart, contextEnd - contextStart)));
        QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("!!globalThis.mathClickResult || !!globalThis.mathClickError")).toBool(), 5000);
        const auto result = run(page, QStringLiteral("globalThis.mathClickResult")).toMap();
        QVERIFY2(!result.isEmpty(), qPrintable(run(page, QStringLiteral("globalThis.mathClickError")).toString()));
        QVERIFY2(result.value(QStringLiteral("unchanged")).toBool(), "MathJax clicks changed formula DOM or pagination geometry");
        QVERIFY(!result.value(QStringLiteral("popup")).toBool());
        QCOMPARE(result.value(QStringLiteral("clicks")).toInt(), 1);
        QCOMPARE(result.value(QStringLiteral("contexts")).toInt(), 1);
        QVERIFY(result.value(QStringLiteral("cfi")).toBool());
        QVERIFY(result.value(QStringLiteral("speech")).toBool());
        QVERIFY(result.value(QStringLiteral("mathCursor")).toBool());
        QVERIFY(result.value(QStringLiteral("selectionCursor")).toBool());
        QCOMPARE(result.value(QStringLiteral("opens")).toInt(), 1);
        QCOMPARE(result.value(QStringLiteral("contextOpens")).toInt(), 1);
        QVERIFY(result.value(QStringLiteral("contextAnchor")).toBool());
        QCOMPARE(result.value(QStringLiteral("imageOpens")).toInt(), 0);
        QVERIFY(result.value(QStringLiteral("source")).toBool());
        QVERIFY(result.value(QStringLiteral("speechAnchor")).toBool());
        QVERIFY(result.value(QStringLiteral("selectionPreserved")).toBool());
    }
    QVERIFY2(requests.remote.isEmpty(), qPrintable(requests.remote.join(QLatin1Char('\n'))));
}

void EpubMathTest::scriptPolicies()
{
    // A restrictive EPUB CSP stays authoritative; the reader never removes it.
    const auto html = document(QByteArrayLiteral("<p>\\(x\\)</p>"), QByteArrayLiteral("<meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'none'\"/>"));
    const auto served = EpubMath::injectMathJax(html, QStringLiteral("http://127.0.0.1/mathjax/math-test/mathjax-bootstrap.js"));
    QVERIFY(served.contains("script-src 'none'"));
    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QString::fromUtf8(served));
    QVERIFY(loaded.wait(5000));
    QVERIFY(run(page, QStringLiteral("typeof window.ariannaMathReady === 'undefined' && !document.querySelector('mjx-container')")).toBool());
}

void EpubMathTest::disabledEpubScripts()
{
    QHttpServer modules;
    modules.router()->addConverter(QMetaType::fromType<QUrl>(), QStringLiteral("[A-Za-z0-9_./-]+"));
    const QString directory = QFINDTESTDATA("../qml/foliate-js/");
    modules.route(QStringLiteral("/<arg>"), [&](const QUrl &path) {
        QFile file(directory + path.toString());
        if (!file.open(QIODevice::ReadOnly)) return QHttpServerResponse(QHttpServerResponder::StatusCode::NotFound);
        return QHttpServerResponse(QByteArrayLiteral("text/javascript"), file.readAll());
    });
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QVERIFY(modules.bind(&listener));
    const QString moduleUrl = QStringLiteral("http://127.0.0.1:%1/").arg(listener.serverPort());
    const QJsonObject files{
        {QStringLiteral("META-INF/container.xml"), QStringLiteral("<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container'><rootfiles><rootfile full-path='package.opf' media-type='application/oebps-package+xml'/></rootfiles></container>")},
        {QStringLiteral("package.opf"), QStringLiteral("<package xmlns='http://www.idpf.org/2007/opf' version='3.0' unique-identifier='id'><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:title>Policy</dc:title><dc:identifier id='id'>policy</dc:identifier></metadata><manifest><item id='chapter' href='chapter.xhtml' media-type='application/xhtml+xml'/></manifest><spine><itemref idref='chapter'/></spine></package>")},
        {QStringLiteral("chapter.xhtml"), QString::fromUtf8(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>\\(x\\)</p><a id='event' onclick='window.authorExecuted=true' href='javascript:window.authorExecuted=true'>Click</a>"),
              QByteArrayLiteral("<script>window.authorExecuted=true</script><script type='text/x-mathjax-config'>window.authorExecuted=true</script>")),
              QStringLiteral("http://127.0.0.1/mathjax/math-test/mathjax-bootstrap.js")))},
    };
    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral("<html><body></body></html>"), QUrl(moduleUrl));
    QVERIFY(loaded.wait(5000));
    run(page, QStringLiteral(R"JS((async () => {
        try {
            const { EPUB }=await import('%1epub.js'); const files=%2;
            const book=await new EPUB({allowScript:false,loadText:async key => files[key],
                loadBlob:async key => new Blob([files[key]]),getSize:key => files[key]?.length ?? 0}).init();
            const url=await book.sections[0].load();
            const frame=document.createElement('iframe'); document.body.append(frame);
            await new Promise(resolve => { frame.onload=resolve; frame.src=url });
            const doc=frame.contentDocument; doc.getElementById('event').click();
            globalThis.policyVerified=doc.defaultView.authorExecuted !== true
                && !doc.defaultView.ariannaMathReady && !doc.defaultView.MathJax
                && !doc.querySelector('script[src],script[data-arianna-mathjax],[onclick],a[href]');
        } catch(e) {globalThis.testError=String(e)}
    })())JS").arg(moduleUrl, QString::fromUtf8(QJsonDocument(files).toJson(QJsonDocument::Compact))));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.policyVerified !== undefined || !!globalThis.testError")).toBool(), 5000);
    QVERIFY2(run(page, QStringLiteral("globalThis.policyVerified === true")).toBool(), qPrintable(run(page, QStringLiteral("globalThis.testError ?? 'EPUB scripts executed'")).toString()));
}

void EpubMathTest::previewMenuIsolated()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    OfflineRequests requests;
    QWebEngineProfile bookProfile, previewProfile;
    bookProfile.setUrlRequestInterceptor(&requests);
    previewProfile.setUrlRequestInterceptor(&requests);
    CheckedPage book(&bookProfile), preview(&previewProfile);
    QSignalSpy bookLoaded(&book, &QWebEnginePage::loadFinished);
    book.setContent(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>Before \\(\\newcommand{\\foo}{x^2}\\foo+1\\) after.</p>")), base + QStringLiteral("mathjax-bootstrap.js")),
                    QStringLiteral("application/xhtml+xml"), QUrl(base + QStringLiteral("chapter.xhtml")));
    QVERIFY(bookLoaded.wait(5000));
    QTRY_COMPARE_WITH_TIMEOUT(run(book, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toString(), QStringLiteral("ready"), 20000);
    const auto original = run(book, QStringLiteral("document.body.outerHTML")).toString();
    const auto details = run(book, QStringLiteral("ariannaMathDetails(document.querySelector('mjx-container'))")).toMap();
    QVERIFY(details.value(QStringLiteral("mathml")).toString().contains(QStringLiteral("<msup")));
    QSignalSpy loaded(&preview, &QWebEnginePage::loadFinished);
    preview.load(QUrl(base + QStringLiteral("mathjax-preview.html")));
    QVERIFY(loaded.wait(10000));
    const QString payload = QString::fromUtf8(QJsonDocument::fromVariant(details).toJson(QJsonDocument::Compact));
    run(preview, QStringLiteral("showFormula(%1)").arg(payload));
    QTRY_COMPARE_WITH_TIMEOUT(run(preview, QStringLiteral("formulaStatus")).toString(), QStringLiteral("ready"), 20000);
    QCOMPARE(run(preview, QStringLiteral("document.querySelector('mjx-container').getAttribute('jax')")).toString(), QStringLiteral("SVG"));
    QVERIFY(run(preview, QStringLiteral("document.querySelector('mjx-container svg path') !== null")).toBool());
    QVERIFY(run(preview, QStringLiteral("MathJax.startup.document.menu.toMML(Array.from(MathJax.startup.document.math)[0]).includes('application/x-tex')")).toBool());
    run(preview, QStringLiteral("openFormulaMenu().then(() => globalThis.nativeTestMenu = formulaNativeState().events.find(event => event.kind === 'menu'))"));
    QTRY_VERIFY_WITH_TIMEOUT(run(preview, QStringLiteral("!!globalThis.nativeTestMenu")).toBool(), 10000);
    QVERIFY(run(preview, QStringLiteral("!MathJax.startup.document.menu.menu.html.isConnected")).toBool());
    // Every visible menu type and branch comes from the real MathJax model.
    QVERIFY(run(preview, QStringLiteral(R"JS((() => {
        const branches = nativeTestMenu.items.filter(item => item.type === 'submenu').map(item => item.id);
        const flatten = items => items.flatMap(item => [item, ...flatten(item.children)]);
        const types = new Set(flatten(nativeTestMenu.items).map(item => item.type));
        return ['Show','Copy','Settings','Speech','Braille','Explorer','Options'].every(id => branches.includes(id))
            && ['submenu','command','checkbox','radio','slider','label','rule'].every(type => types.has(type));
    })())JS")).toBool());
    run(preview, QStringLiteral(R"JS(globalThis.nativeDoAction = async path => {
        let items = nativeTestMenu.items, item;
        for (const id of path) {item = items.find(item => item.id === id); items = item.children}
        await formulaNativeAction(item.token, null, nativeTestMenu.revision);
        globalThis.nativeTestEvents = formulaNativeState().events;
    })JS"));
    const auto action = [&](const QStringList &path) {
        const auto json = QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(path)).toJson(QJsonDocument::Compact));
        run(preview, QStringLiteral("globalThis.nativeActionDone=false; globalThis.nativeActionError=''; nativeDoAction(%1).then(() => globalThis.nativeActionDone=true).catch(e => globalThis.nativeActionError=String(e))").arg(json));
        QTRY_VERIFY_WITH_TIMEOUT(run(preview, QStringLiteral("globalThis.nativeActionDone || !!globalThis.nativeActionError")).toBool(), 15000);
        QVERIFY2(run(preview, QStringLiteral("globalThis.nativeActionDone === true")).toBool(), qPrintable(run(preview, QStringLiteral("globalThis.nativeActionError")).toString()));
    };
    action({QStringLiteral("Show"), QStringLiteral("MathMLcode")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].kind === 'dialog' && nativeTestEvents[0].text.includes('<math')")).toBool());
    action({QStringLiteral("Copy"), QStringLiteral("CopyAnnotation"), QStringLiteral("TeX")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].kind === 'copy' && nativeTestEvents[0].text.includes('foo')")).toBool());
    action({QStringLiteral("Settings"), QStringLiteral("Scale")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].kind === 'slider'")).toBool());
    run(preview, QStringLiteral("formulaNativeAction(nativeTestEvents[0].token, 150, nativeTestMenu.revision).then(() => globalThis.scaleChanged=true)"));
    QTRY_VERIFY(run(preview, QStringLiteral("globalThis.scaleChanged === true")).toBool());
    QCOMPARE(run(preview, QStringLiteral("MathJax.startup.document.outputJax.options.scale")).toDouble(), 1.5);
    action({QStringLiteral("Speech"), QStringLiteral("Clearspeak"), QStringLiteral("ClearspeakPreferences")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].kind === 'preferences' && nativeTestEvents[0].items.length > 10")).toBool());
    run(preview, QStringLiteral(R"JS((async () => {
        const entry = nativeTestEvents[0].items.find(item => item.label === 'Fraction');
        const before = MathJax.startup.document.menu.settings.speechRules;
        await formulaNativeAction(entry.token, 'invalid choice', nativeTestMenu.revision);
        globalThis.rejectedPreference = before === MathJax.startup.document.menu.settings.speechRules;
        await formulaNativeAction(entry.token, entry.choices.find(value => value !== entry.value), nativeTestMenu.revision);
        globalThis.preferenceChanged = before !== MathJax.startup.document.menu.settings.speechRules;
    })())JS"));
    QTRY_VERIFY(run(preview, QStringLiteral("globalThis.preferenceChanged === true")).toBool());
    QVERIFY(run(preview, QStringLiteral("globalThis.rejectedPreference === true")).toBool());
    action({QStringLiteral("Settings"), QStringLiteral("ZoomTrigger"), QStringLiteral("ZoomNow")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].kind === 'zoom' && nativeTestEvents[0].html.includes('<svg') && !nativeTestEvents[0].html.includes('<script')")).toBool());
    action({QStringLiteral("Help")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].kind === 'dialog' && nativeTestEvents[0].text.includes('Accessibility') && !nativeTestEvents[0].text.includes('<p>')")).toBool());
    action({QStringLiteral("About")});
    QVERIFY(run(preview, QStringLiteral("nativeTestEvents[0].text.includes('Modules Loaded')")).toBool());
    const bool inlineBreaks = run(preview, QStringLiteral("MathJax.startup.document.outputJax.options.linebreaks.inline")).toBool();
    action({QStringLiteral("Settings"), QStringLiteral("Overflow"), QStringLiteral("BreakInline")});
    QCOMPARE(run(preview, QStringLiteral("MathJax.startup.document.outputJax.options.linebreaks.inline")).toBool(), !inlineBreaks);
    run(preview, QStringLiteral(R"JS((async () => {
        const highlight = nativeTestMenu.items.find(item => item.id === 'Explorer').children.find(item => item.id === 'Highlight');
        const slider = highlight.children.find(item => item.variable === 'backgroundOpacity');
        await formulaNativeAction(slider.token, null, nativeTestMenu.revision);
        const event = formulaNativeState().events.find(event => event.kind === 'slider');
        await formulaNativeAction(event.token, 42, nativeTestMenu.revision);
        globalThis.opacityChanged = true;
    })())JS"));
    QTRY_VERIFY(run(preview, QStringLiteral("globalThis.opacityChanged === true")).toBool());
    QCOMPARE(run(preview, QStringLiteral("Number(MathJax.startup.document.options.a11y.backgroundOpacity)")).toInt(), 42);
    action({QStringLiteral("Options"), QStringLiteral("AutoCollapse")});
    QVERIFY(run(preview, QStringLiteral("MathJax.startup.document.menu.settings.autocollapse === false")).toBool());
    action({QStringLiteral("Settings"), QStringLiteral("Reset")});
    QCOMPARE(run(preview, QStringLiteral("Number(MathJax.startup.document.menu.settings.scale)")).toDouble(), 1.0);
    action({QStringLiteral("Settings"), QStringLiteral("Renderer"), QStringLiteral("CHTML")});
    QCOMPARE(run(preview, QStringLiteral("document.querySelector('mjx-container').getAttribute('jax')")).toString(), QStringLiteral("CHTML"));
    QVERIFY(run(preview, QStringLiteral("document.querySelector('mjx-container').getBoundingClientRect().height > 0")).toBool());
    QVERIFY(run(preview, QStringLiteral("!document.querySelector('dialog, .CtxtMenu_MenuFrame')")).toBool());
    // Right-click follows the same native route; previous menu tokens expire.
    run(preview, QStringLiteral("document.querySelector('mjx-container').dispatchEvent(new MouseEvent('contextmenu', {bubbles:true,cancelable:true,clientX:10,clientY:10}))"));
    QTRY_VERIFY_WITH_TIMEOUT(run(preview, QStringLiteral("globalThis.nativeContextMenu ||= formulaNativeState().events.find(event => event.kind === 'menu'); !!globalThis.nativeContextMenu")).toBool(), 10000);
    QVERIFY(run(preview, QStringLiteral("nativeContextMenu.revision > nativeTestMenu.revision && !MathJax.startup.document.menu.menu.html.isConnected")).toBool());
    run(preview, QStringLiteral("formulaNativeAction(nativeTestMenu.items.find(item => item.id === 'Settings').children.find(item => item.id === 'Reset').token, null, nativeTestMenu.revision).then(() => globalThis.staleRejected=true)"));
    QTRY_VERIFY(run(preview, QStringLiteral("globalThis.staleRejected === true")).toBool());
    QCOMPARE(run(preview, QStringLiteral("document.querySelector('mjx-container').getAttribute('jax')")).toString(), QStringLiteral("CHTML"));
    QCOMPARE(run(book, QStringLiteral("document.body.outerHTML")).toString(), original);
    QCOMPARE(run(book, QStringLiteral("MathJax.startup.document.menu.settings.renderer")).toString(), QStringLiteral("SVG"));
    QVERIFY(run(book, QStringLiteral("localStorage.getItem('MathJax-Menu-Settings') === null")).toBool());
    QVERIFY(std::any_of(requests.local.cbegin(), requests.local.cend(), [](const QString &path) { return path.endsWith(QStringLiteral(".woff2")); }));
    // Payloads are data: HTML, event handlers and links cannot escape MathML.
    run(preview, QStringLiteral(R"JS(showFormula({mathml:'<math xmlns="http://www.w3.org/1998/Math/MathML"><mtext onclick="window.authorExecuted=true">text<img xmlns="http://www.w3.org/1999/xhtml" src="https://example.invalid/evil" onerror="window.authorExecuted=true"/></mtext><annotation-xml><script xmlns="http://www.w3.org/1999/xhtml">window.authorExecuted=true</script></annotation-xml></math>',locale:'de'}))JS"));
    QTRY_COMPARE_WITH_TIMEOUT(run(preview, QStringLiteral("formulaStatus")).toString(), QStringLiteral("ready"), 10000);
    QVERIFY(run(preview, QStringLiteral("!window.authorExecuted && !document.querySelector('#formula [onclick],#formula img,#formula script,#formula [href]')")).toBool());
    QVERIFY2(requests.remote.isEmpty(), qPrintable(requests.remote.join(QLatin1Char('\n'))));
    QVERIFY2(book.diagnostics.isEmpty(), qPrintable(book.diagnostics.join(QLatin1Char('\n'))));
    QVERIFY2(preview.diagnostics.isEmpty(), qPrintable(preview.diagnostics.join(QLatin1Char('\n'))));
}

void EpubMathTest::nativeFormulaWindow()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    QQmlComponent component(&engine);
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    component.setData(QStringLiteral(R"(
        import org.kde.arianna
        MathJaxWindow {
            runtimeBaseUrl: "%1"
            preferredSpeechLocale: "de-DE"
            renderingOptions: ({scale:1, inlineScale:1, fontCache:"global"})
            property var scriptResult
            property bool scriptDone: false
        }
    )").arg(base).toUtf8(), QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.data());
    QVERIFY(window);
    const auto evaluate = [&](const QString &code) {
        QQmlExpression expression(qmlContext(window), window, code);
        const auto value = expression.evaluate();
        if (expression.hasError()) QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
        return value;
    };
    QVERIFY(!window->isVisible());
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxPreview")));
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView")));
    evaluate(QStringLiteral("open({mathml:'<math xmlns=\"http://www.w3.org/1998/Math/MathML\"><mi>x</mi></math>',tex:'x',locale:'de'})"));
    QTRY_VERIFY(window->isVisible());
    QCOMPARE(window->modality(), Qt::NonModal);
    QVERIFY(window->flags().testFlag(Qt::WindowTitleHint));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("status").toString(), QStringLiteral("ready"), 20000);
    auto *view = window->findChild<QObject *>(QStringLiteral("mathJaxPreview"));
    QVERIFY(view);
    engine.rootContext()->setContextProperty(QStringLiteral("formulaViewForTest"), view);
    const auto profile = view->property("profile").value<QObject *>();
    QVERIFY(profile && profile->property("offTheRecord").toBool());
    auto *menu = window->findChild<QObject *>(QStringLiteral("mathJaxMenuButton"));
    QVERIFY(menu && menu->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(menu, "clicked"));
    QObject *nativeMenu = nullptr;
    QTRY_VERIFY((nativeMenu = window->findChild<QObject *>(QStringLiteral("mathJaxNativeMenu"))));
    QTRY_VERIFY(nativeMenu->property("visible").toBool());
    // Exercise actual Qt menu keyboard navigation, including its submenu.
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Right);
    auto *showMenu = window->findChild<QObject *>(QStringLiteral("mathMenu_Show"));
    QVERIFY(showMenu);
    QTRY_VERIFY(showMenu->property("visible").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::keyClick(window, Qt::Key_Escape);
    evaluate(QStringLiteral("formulaViewForTest.runJavaScript('!MathJax.startup.document.menu.menu.html.isConnected', value => {scriptResult=value;scriptDone=true})"));
    QTRY_VERIFY(window->property("scriptDone").toBool());
    QVERIFY(window->property("scriptResult").toBool());
    const auto trigger = [&](const QString &name) {
        auto *item = window->findChild<QObject *>(name);
        QVERIFY2(item && item->property("enabled").toBool(), qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
    };
    trigger(QStringLiteral("mathMenu_Show_MathMLcode"));
    auto *info = window->findChild<QObject *>(QStringLiteral("mathJaxNativeInfo"));
    QVERIFY(info);
    QTRY_VERIFY(info->property("visible").toBool());
    auto *source = window->findChild<QObject *>(QStringLiteral("mathJaxNativeSource"));
    QVERIFY(source && source->property("text").toString().contains(QStringLiteral("<math")));
    auto *copy = window->findChild<QObject *>(QStringLiteral("mathJaxNativeCopy"));
    QVERIFY(copy && QMetaObject::invokeMethod(copy, "clicked"));
    QCOMPARE(QGuiApplication::clipboard()->text(), source->property("text").toString());
    QVERIFY(QMetaObject::invokeMethod(info, "close"));
    trigger(QStringLiteral("mathMenu_Settings_Scale"));
    auto *scaleDialog = window->findChild<QObject *>(QStringLiteral("mathJaxNativeScale"));
    QVERIFY(scaleDialog);
    QTRY_VERIFY(scaleDialog->property("visible").toBool());
    auto *percent = window->findChild<QObject *>(QStringLiteral("mathJaxNativePercent"));
    QVERIFY(percent && percent->setProperty("value", 1250));
    QVERIFY(QMetaObject::invokeMethod(scaleDialog, "accept"));
    trigger(QStringLiteral("mathMenu_Speech_Clearspeak_ClearspeakPreferences"));
    auto *preferences = window->findChild<QObject *>(QStringLiteral("mathJaxNativePreferences"));
    QVERIFY(preferences);
    QTRY_VERIFY(preferences->property("visible").toBool());
    auto *preferenceRows = window->findChild<QObject *>(QStringLiteral("mathJaxNativePreferenceRows"));
    QVERIFY(preferenceRows);
    QTRY_VERIFY(preferenceRows->property("count").toInt() > 10);
    QQuickItem *preferenceRow = nullptr;
    QVERIFY(QMetaObject::invokeMethod(preferenceRows, "itemAt", Q_RETURN_ARG(QQuickItem *, preferenceRow), Q_ARG(int, 0)));
    QVERIFY(preferenceRow);
    const auto preferenceChildren = preferenceRow->childItems();
    QVERIFY(std::any_of(preferenceChildren.cbegin(), preferenceChildren.cend(), [](QQuickItem *item) {
        return item->objectName().startsWith(QStringLiteral("mathPreference_")) && item->property("count").toInt() > 1;
    }));
    QVERIFY(QMetaObject::invokeMethod(preferences, "close"));
    trigger(QStringLiteral("mathMenu_Settings_ZoomTrigger_ZoomNow"));
    auto *zoom = window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoom"));
    QVERIFY(zoom);
    QTRY_VERIFY(zoom->property("visible").toBool());
    auto *zoomView = window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView"));
    QVERIFY(zoomView);
    QTRY_VERIFY(zoom->property("ready").toBool());
    engine.rootContext()->setContextProperty(QStringLiteral("formulaZoomForTest"), zoomView);
    window->setProperty("scriptDone", false);
    evaluate(QStringLiteral(R"JS(formulaZoomForTest.runJavaScript(`(() => {
        const uses = Array.from(document.querySelectorAll('mjx-container svg use'));
        return document.querySelector('mjx-container svg')?.getBoundingClientRect().height > 0
            && uses.length > 0 && uses.every(use => document.getElementById((use.getAttribute('href') || use.getAttribute('xlink:href')).slice(1)));
    })()`, 1, value => {scriptResult=value;scriptDone=true}))JS"));
    QTRY_VERIFY(window->property("scriptDone").toBool());
    QVERIFY(window->property("scriptResult").toBool());
    QVERIFY(QMetaObject::invokeMethod(zoom, "close"));
    QTRY_VERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView")));
    trigger(QStringLiteral("mathMenu_Settings_ZoomTrigger_ZoomNow"));
    QTRY_VERIFY(zoom->property("ready").toBool());
    QVERIFY(window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView")));
    QVERIFY(QMetaObject::invokeMethod(zoom, "close"));
    QTRY_VERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView")));
    evaluate(QStringLiteral("open({tex:'1+\\\\frac{1}{2^2}+\\\\cdots=\\\\frac{\\\\pi^2}{6}',locale:'en',speechSegment:{cfi:'epubcfi(/4/2/2)',sectionIndex:0}})"));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("status").toString(), QStringLiteral("ready"), 20000);
    QTRY_VERIFY(window->property("speechText").toString().contains(QStringLiteral("geteilt durch")));
    QCOMPARE(window->property("actualSpeechLocale").toString(), QStringLiteral("de"));
    auto *readButton = window->findChild<QObject *>(QStringLiteral("mathReadAloudButton"));
    QVERIFY(readButton && readButton->property("enabled").toBool());
    QSignalSpy readAloud(window, SIGNAL(readAloudRequested(QVariant)));
    QVERIFY(QMetaObject::invokeMethod(readButton, "clicked"));
    QCOMPARE(readAloud.size(), 1);
    const auto segment = readAloud.first().first().toMap();
    QCOMPARE(segment.value(QStringLiteral("text")).toString(), window->property("speechText").toString());
    QCOMPARE(segment.value(QStringLiteral("lang")).toString(), QStringLiteral("de"));
    QCOMPARE(segment.value(QStringLiteral("cfi")).toString(), QStringLiteral("epubcfi(/4/2/2)"));
    window->setProperty("speechLocale", QStringLiteral("en"));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("status").toString(), QStringLiteral("ready"), 20000);
    QTRY_COMPARE(window->property("actualSpeechLocale").toString(), QStringLiteral("en"));
    window->setProperty("conciseSpeech", false);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("status").toString(), QStringLiteral("ready"), 20000);
    QTRY_VERIFY(window->property("speechText").toString().contains(QStringLiteral("numerator")));
    window->close();
    QVERIFY(!window->isVisible());
    evaluate(QStringLiteral("open({mathml:'<math xmlns=\"http://www.w3.org/1998/Math/MathML\"><mn>2</mn></math>',locale:'en'})"));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("status").toString(), QStringLiteral("ready"), 10000);
    window->setProperty("readerActive", false);
    QTRY_VERIFY(!window->isVisible());
    QVERIFY(window->property("formula").isNull());
    QTRY_VERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxPreview")));
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView")));
    window->setProperty("readerActive", true);
    evaluate(QStringLiteral("open({tex:'x+1',locale:'en'})"));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("status").toString(), QStringLiteral("ready"), 10000);
    QVERIFY(window->findChild<QObject *>(QStringLiteral("mathJaxPreview")));
    window->setProperty("readerActive", false);
    QTRY_VERIFY(!window->findChild<QObject *>(QStringLiteral("mathJaxPreview")));
}

void EpubMathTest::viewerFormulaContextMenu()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server);
    const bool oldSupport = Config::self()->mathJaxSupport();
    Config::self()->setMathJaxSupport(true);
    const auto restore = qScopeGuard([&] { Config::self()->setMathJaxSupport(oldSupport); });
    QTemporaryDir dir;
    FakeSpeechDispatcher dispatcher(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(dispatcher.startServer());
    const auto oldAddress = qgetenv("SPEECHD_ADDRESS");
    const auto oldVoice = Config::speechVoice();
    const auto restoreSpeech = qScopeGuard([&] {
        if (oldAddress.isNull()) qunsetenv("SPEECHD_ADDRESS");
        else qputenv("SPEECHD_ADDRESS", oldAddress);
        Config::setSpeechVoice(oldVoice);
    });
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    Config::setSpeechVoice(QString());
    KZip zip(dir.filePath(QStringLiteral("context.epub")));
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
        R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
    QVERIFY(zip.writeFile(QStringLiteral("package.opf"),
        R"(<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Context</dc:title><dc:identifier id="id">context</dc:identifier><dc:language>de</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), EpubMath::injectMathJax(document(
        QByteArrayLiteral("<p id=\"before\">Before.</p><p>\\(1+\\frac{1}{2^2}+\\cdots=\\frac{\\pi^2}{6}\\)</p>"
                          "<svg id=\"picture\" xmlns=\"http://www.w3.org/2000/svg\" width=\"30\" height=\"30\"><rect width=\"30\" height=\"30\"/></svg>"),
        QByteArrayLiteral("<script type=\"text/x-mathjax-config\"></script>")), base + QStringLiteral("/mathjax/math-test/mathjax-bootstrap.js"))));
    QVERIFY(zip.close());
    QFile file(zip.fileName());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString url = QStringLiteral("data:application/epub+zip;base64,") + QString::fromLatin1(file.readAll().toBase64());
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), base);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("math-test"));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 1100; height: 850; visible: true
            property var bookListModel: QtObject { signal entryDataUpdated(var entry) }
            contextDrawer: Kirigami.ContextDrawer {
                property string currentTocId: ""
                property string currentHref: ""
                property var model: QtObject { function importFromJson(json) {} }
                function clearCurrentItem() {}
                signal goTo(var location)
            }
            pageStack.initialPage: Kirigami.Page {}
            Component.onCompleted: pageStack.layers.push(readerPage)
            Component {
                id: readerPage
                EpubViewerPage {
                    objectName: "reader"
                    entry: ({uniqueIdentifier: ""})
                    pageMode: "single"
                    property bool scriptDone: false
                    property var scriptResult
                }
            }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.data());
    QVERIFY(window && QTest::qWaitForWindowExposed(window));
    auto *page = window->findChild<QQuickItem *>(QStringLiteral("reader"));
    auto *model = window->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    QVERIFY(page && model);
    QVERIFY(!page->findChild<QObject *>(QStringLiteral("mathJaxPreview")));
    QVERIFY(!page->findChild<QObject *>(QStringLiteral("mathJaxNativeZoomView")));
    const auto evaluate = [&](const QString &code) {
        QQmlExpression expression(qmlContext(model), model, code);
        const auto value = expression.evaluate();
        if (expression.hasError()) QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
        return value;
    };
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(QStringLiteral("view.readerReady")).toBool(), 15000);
    QVERIFY(page->setProperty("url", QStringLiteral("context.epub")));
    const auto encodedUrl = QString::fromUtf8(QJsonDocument(QJsonArray{url}).toJson(QJsonDocument::Compact));
    evaluate(QStringLiteral("view.runJavaScript('openSync(' + JSON.stringify(%1[0]) + ', null, \"\", ' + JSON.stringify(root.currentReaderLayout()) + ')')").arg(encodedUrl));
    QTRY_VERIFY_WITH_TIMEOUT(evaluate(QStringLiteral("view.bookReady")).toBool(), 15000);
    const auto script = [&](const QString &code) {
        const auto encoded = QString::fromUtf8(QJsonDocument(QJsonArray{code}).toJson(QJsonDocument::Compact));
        evaluate(QStringLiteral("root.scriptDone=false; view.runJavaScript(%1[0], value => {root.scriptResult=value;root.scriptDone=true})").arg(encoded));
        QElapsedTimer timer;
        timer.start();
        while (!page->property("scriptDone").toBool() && timer.elapsed() < 5000) QTest::qWait(10);
        if (!page->property("scriptDone").toBool()) QTest::qFail("Reader script timed out", __FILE__, __LINE__);
        return page->property("scriptResult");
    };
    QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("reader.view.renderer.getContents().some(c => c.doc?.querySelector('mjx-container'))")).toBool(), 15000);
    script(QStringLiteral(R"JS(
        globalThis.doc = reader.view.renderer.getContents().find(c => c.doc?.querySelector('mjx-container')).doc;
        globalThis.formula = doc.querySelector('mjx-container');
        globalThis.contextOn = element => !element.dispatchEvent(new doc.defaultView.MouseEvent('contextmenu',
            {bubbles:true,cancelable:true,view:doc.defaultView,button:2}));
        globalThis.bookSnapshot = () => JSON.stringify({html:doc.body.innerHTML,width:doc.body.scrollWidth,
            height:doc.body.scrollHeight,rect:Array.from(['x','y','width','height'],k => formula.getBoundingClientRect()[k])});
        const selection = doc.getSelection();
        const range = doc.createRange();range.selectNodeContents(doc.getElementById('before'));
        selection.removeAllRanges();selection.addRange(range);
        globalThis.before = bookSnapshot();
        contextOn(formula.querySelector('svg path'));
    )JS"));
    auto *context = page->findChild<QObject *>(QStringLiteral("epubImageContextMenu"));
    auto *mathMenu = page->findChild<QObject *>(QStringLiteral("epubMathJaxContextActions"));
    auto *controls = page->findChild<QObject *>(QStringLiteral("epubMathJaxContextControls"));
    auto *preview = page->findChild<QQuickWindow *>(QStringLiteral("epubMathJaxWindow"));
    auto *inverse = page->findChild<QObject *>(QStringLiteral("epubImageNotInverseAction"));
    QVERIFY(context && mathMenu && controls && preview && inverse);
    QTRY_VERIFY(context->property("visible").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(mathMenu->property("count").toInt() > 5, 20000);
    QVERIFY(context->property("formulaContext").toBool());
    QVERIFY(!context->property("nativeImageActions").toBool());
    QVERIFY(!inverse->property("visible").toBool());
    QVERIFY(!preview->isVisible());
    QVERIFY(script(QStringLiteral("before === bookSnapshot() && doc.getSelection().toString() === 'Before.'")).toBool());
    const auto segment = preview->property("formula").toMap().value(QStringLiteral("speechSegment")).toMap();
    QVERIFY(segment.value(QStringLiteral("cfi")).toString().startsWith(QStringLiteral("epubcfi(")));
    QCOMPARE(segment.value(QStringLiteral("sectionIndex")).toInt(), 0);
    // Open the real Qt submenu hierarchy by keyboard, including a second level.
    engine.rootContext()->setContextProperty(QStringLiteral("contextForTest"), context);
    engine.rootContext()->setContextProperty(QStringLiteral("mathMenuForTest"), mathMenu);
    QVERIFY(evaluate(QStringLiteral("(() => {for(let i=0;i<5;i++) if(contextForTest.itemAt(i).visible) return false;return true})()")).toBool());
    QCOMPARE(evaluate(QStringLiteral("(() => {let count=0;for(let i=0;i<contextForTest.count;i++) if(contextForTest.itemAt(i).visible) count++;return count})()")).toInt(), 3);
    evaluate(QStringLiteral("for(let i=0;i<contextForTest.count;i++) if(contextForTest.menuAt(i) === mathMenuForTest) contextForTest.currentIndex=i"));
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_VERIFY(mathMenu->property("visible").toBool());
    QVERIFY(mathMenu->setProperty("currentIndex", 0));
    QTest::keyClick(window, Qt::Key_Right);
    auto *show = mathMenu->findChild<QObject *>(QStringLiteral("mathMenu_Show"));
    QVERIFY(show);
    QTRY_VERIFY(show->property("visible").toBool());
    auto *sourceAction = mathMenu->findChild<QObject *>(QStringLiteral("mathMenu_Show_MathMLcode"));
    QVERIFY(sourceAction && sourceAction->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(sourceAction, "triggered"));
    QVERIFY(QMetaObject::invokeMethod(context, "close"));
    auto *info = controls->findChild<QObject *>(QStringLiteral("mathJaxNativeInfo"));
    QTRY_VERIFY(info && info->property("visible").toBool());
    auto *source = controls->findChild<QObject *>(QStringLiteral("mathJaxNativeSource"));
    QVERIFY(source && source->property("text").toString().contains(QStringLiteral("<mfrac")));
    auto *copy = controls->findChild<QObject *>(QStringLiteral("mathJaxNativeCopy"));
    QVERIFY(copy && QMetaObject::invokeMethod(copy, "clicked"));
    QCOMPARE(QGuiApplication::clipboard()->text(), source->property("text").toString());
    QVERIFY(!preview->isVisible());
    QVERIFY(script(QStringLiteral("before === bookSnapshot()")).toBool());
    QVERIFY(QMetaObject::invokeMethod(info, "close"));
    QTRY_VERIFY(preview->property("contextMenuHost").isNull());
    // Escape during initialization must cancel the request without a late popup.
    script(QStringLiteral("contextOn(formula.querySelector('svg path'))"));
    QTRY_VERIFY(context->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(context, "close"));
    QTRY_VERIFY(preview->property("contextMenuHost").isNull());
    QTest::qWait(400);
    QVERIFY(!context->property("visible").toBool() && !info->property("visible").toBool());
    // Ordinary SVG images retain the existing image actions, with no MathJax submenu.
    script(QStringLiteral("contextOn(doc.getElementById('picture').firstElementChild)"));
    QTRY_VERIFY(context->property("visible").toBool());
    QVERIFY(!context->property("formulaContext").toBool());
    QVERIFY(inverse->property("visible").toBool());
    QVERIFY(evaluate(QStringLiteral("(() => {for(let i=0;i<5;i++) if(!contextForTest.itemAt(i).visible) return false;return true})()")).toBool());
    QVERIFY(evaluate(QStringLiteral("(() => {for(let i=0;i<contextForTest.count;i++) if(contextForTest.menuAt(i) === mathMenuForTest) return !contextForTest.itemAt(i).visible;return false})()")).toBool());
    QVERIFY(QMetaObject::invokeMethod(context, "close"));
    script(QStringLiteral("contextOn(formula.querySelector('svg path'))"));
    QTRY_VERIFY(context->property("visible").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(mathMenu->property("count").toInt() > 5, 15000);
    auto *open = page->findChild<QObject *>(QStringLiteral("epubOpenFormulaDialogAction"));
    QVERIFY(open && QMetaObject::invokeMethod(open, "clicked"));
    QTRY_VERIFY(preview->isVisible());
    QVERIFY(preview->property("contextMenuHost").isNull());
    QCOMPARE(preview->property("formula").toMap().value(QStringLiteral("speechSegment")).toMap(), segment);
    QVERIFY(script(QStringLiteral("before === bookSnapshot()")).toBool());
    QVERIFY(QMetaObject::invokeMethod(preview, "reset"));
    QVERIFY(!preview->isVisible());
    script(QStringLiteral("contextOn(formula.querySelector('svg path'))"));
    QTRY_VERIFY_WITH_TIMEOUT(mathMenu->property("count").toInt() > 5, 15000);
    auto *read = page->findChild<QObject *>(QStringLiteral("epubReadFormulaAction"));
    QTRY_VERIFY(read && read->property("enabled").toBool());
    QSignalSpy requested(preview, SIGNAL(readAloudRequested(QVariant)));
    QVERIFY(QMetaObject::invokeMethod(read, "clicked"));
    QVERIFY(QMetaObject::invokeMethod(context, "close"));
    QCOMPARE(requested.size(), 1);
    const auto reading = requested.first().first().toMap();
    QCOMPARE(reading.value(QStringLiteral("cfi")), segment.value(QStringLiteral("cfi")));
    QCOMPARE(reading.value(QStringLiteral("sectionIndex")), segment.value(QStringLiteral("sectionIndex")));
    QTRY_COMPARE_WITH_TIMEOUT(dispatcher.messages().size(), 1, 5000);
    QCOMPARE(dispatcher.messages().first().text, reading.value(QStringLiteral("text")).toString());
    QCOMPARE(dispatcher.messages().first().language, reading.value(QStringLiteral("lang")).toString());
    dispatcher.finish(dispatcher.messages().first().id);
    auto *speech = page->findChild<SpeechController *>(QStringLiteral("epubSpeechController"));
    QVERIFY(speech);
    QTRY_COMPARE(speech->state(), SpeechController::Idle);
    QVERIFY(!preview->isVisible());
    QVERIFY(QMetaObject::invokeMethod(preview, "reset"));
}

void EpubMathTest::strictFontPolicy()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    OfflineRequests requests;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&requests);
    CheckedPage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    const auto head = QByteArrayLiteral("<meta http-equiv='Content-Security-Policy' content=\"script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; font-src 'self'; worker-src 'self' blob:; connect-src 'self'\"/>")
        + QStringLiteral(R"(<script>window.MathJax={loader:{paths:{mathjax:'%1'.replace(/\/$/,''),fonts:'%1fonts'}},output:{fontPath:'%1fonts/%%FONT%%-font'},options:{worker:{path:'%1sre',maps:'%1sre/mathmaps'}},svg:{fontCache:'local'}};</script><script id="MathJax-script" src="%1tex-svg.js"></script>)").arg(base).toUtf8();
    page.setContent(EpubMath::injectMathJax(document(QByteArrayLiteral("<p>\\(1+1/2^2+1/3^2=\\pi^2/6\\)</p>"), head), base + QStringLiteral("mathjax-bootstrap.js")),
                    QStringLiteral("application/xhtml+xml"), QUrl(base + QStringLiteral("chapter.xhtml")));
    QVERIFY(loaded.wait(5000));
    QTRY_COMPARE_WITH_TIMEOUT(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toString(), QStringLiteral("ready"), 20000);
    QVERIFY(run(page, QStringLiteral("document.querySelector('mjx-container svg path') !== null")).toBool());
    run(page, QStringLiteral("document.fonts.load('18px MJX-ZERO').then(fonts => globalThis.zeroFontLoaded=fonts.length===1).catch(e => globalThis.fontError=String(e))"));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.zeroFontLoaded !== undefined || !!globalThis.fontError")).toBool(), 5000);
    QVERIFY2(page.diagnostics.isEmpty(), qPrintable(page.diagnostics.join(QLatin1Char('\n'))));
    QVERIFY(run(page, QStringLiteral("globalThis.zeroFontLoaded === true")).toBool());
    QVERIFY(requests.local.contains(QStringLiteral("/mathjax/math-test/fonts/mjx-zero.otf")));
    QVERIFY(run(page, QStringLiteral("document.querySelector('meta[http-equiv]').content.includes(\"font-src 'self';\")")).toBool());
    QVERIFY(requests.remote.isEmpty());
}

void EpubMathTest::previewSpeechLocale()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    QWebEngineProfile profile;
    CheckedPage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.load(QUrl(base + QStringLiteral("mathjax-preview.html")));
    QVERIFY(loaded.wait(10000));
    for (const QString &locale : {QStringLiteral("en"), QStringLiteral("de"), QStringLiteral("en")}) {
        run(page, QStringLiteral(R"JS(showFormula({tex:'1+\\frac{1}{2^2}+\\frac{1}{3^2}+\\frac{1}{4^2}+\\cdots=\\frac{\\pi^2}{6}',locale:'%1'}))JS").arg(locale));
        QTRY_COMPARE_WITH_TIMEOUT(run(page, QStringLiteral("formulaStatus")).toString(), QStringLiteral("ready"), 20000);
        const auto speech = run(page, QStringLiteral("document.querySelector('mjx-container').getAttribute('data-semantic-speech-none')")).toString();
        QVERIFY(!speech.isEmpty());
        QCOMPARE(run(page, QStringLiteral("document.querySelector('mjx-container').getAttribute('data-semantic-locale')")).toString(), locale);
        QVERIFY(speech.contains(locale == QLatin1String("de") ? QStringLiteral("Quadrat") : QStringLiteral("squared")));
        QVERIFY(speech.contains(locale == QLatin1String("de") ? QStringLiteral("geteilt durch") : QStringLiteral("over")));
        QVERIFY(speech.contains(locale == QLatin1String("de") ? QStringLiteral("und so weiter") : QStringLiteral("and so on")));
        QVERIFY(!speech.contains(QStringLiteral("Zähler")) && !speech.contains(QStringLiteral("numerator")));
        QCOMPARE(run(page, QStringLiteral("formulaSpeech().text")).toString(), speech);
    }
    QVERIFY2(page.diagnostics.isEmpty(), qPrintable(page.diagnostics.join(QLatin1Char('\n'))));
}

void EpubMathTest::displaySettings_data()
{
    QTest::addColumn<bool>("author");
    QTest::addColumn<QString>("cache");
    QTest::addColumn<QString>("overflow");
    QTest::addColumn<bool>("inlineBreaks");
    QTest::addColumn<bool>("mathml");
    QTest::newRow("reader-local-break") << false << QStringLiteral("local") << QStringLiteral("linebreak") << true << false;
    QTest::newRow("reader-global-break") << false << QStringLiteral("global") << QStringLiteral("linebreak") << false << false;
    QTest::newRow("reader-none-scale") << false << QStringLiteral("none") << QStringLiteral("scale") << false << false;
    QTest::newRow("reader-scroll") << false << QStringLiteral("local") << QStringLiteral("scroll") << false << false;
    QTest::newRow("reader-overflow") << false << QStringLiteral("local") << QStringLiteral("overflow") << false << false;
    QTest::newRow("author-global-break") << true << QStringLiteral("global") << QStringLiteral("linebreak") << true << false;
    QTest::newRow("author-none-scale") << true << QStringLiteral("none") << QStringLiteral("scale") << false << false;
    QTest::newRow("reader-mathml-local") << false << QStringLiteral("local") << QStringLiteral("linebreak") << true << true;
    QTest::newRow("reader-mathml-global") << false << QStringLiteral("global") << QStringLiteral("linebreak") << false << true;
}

void EpubMathTest::previewDisplaySettings()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    OfflineRequests requests;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&requests);
    CheckedPage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.load(QUrl(base + QStringLiteral("mathjax-preview.html")));
    QVERIFY(loaded.wait(5000));
    for (const QString &cache : {QStringLiteral("local"), QStringLiteral("global"), QStringLiteral("global"), QStringLiteral("none"), QStringLiteral("local")}) {
        run(page, QStringLiteral(R"JS(showFormula({tex:'x^2+1',locale:'de',display:true,
            rendering:{scale:1.15,inlineScale:.8,align:'right',overflow:'scroll',inline:false,fontCache:'%1'}}))JS").arg(cache));
        QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("['ready','failed'].includes(formulaStatus)")).toBool(), 15000);
        QCOMPARE(run(page, QStringLiteral("formulaError")).toString(), QString());
        QCOMPARE(run(page, QStringLiteral("formulaStatus")).toString(), QStringLiteral("ready"));
        QCOMPARE(run(page, QStringLiteral("MathJax.startup.document.outputJax.options.fontCache")).toString(), cache);
        QCOMPARE(run(page, QStringLiteral("document.querySelector('#formula mjx-container').getAttribute('justify')")).toString(), QStringLiteral("right"));
        QCOMPARE(run(page, QStringLiteral("document.querySelector('#formula mjx-container').getAttribute('overflow')")).toString(), QStringLiteral("scroll"));
        QVERIFY(run(page, QStringLiteral("document.querySelector('#formula mjx-container').style.fontSize==='115%'")).toBool());
        QVERIFY(run(page, QStringLiteral("Array.from(document.querySelectorAll('#formula use')).every(use=>document.getElementById((use.getAttribute('href') || use.getAttribute('xlink:href')).slice(1)))")).toBool());
    }
    double inlineHeight = 0;
    for (bool display : {false, true, false}) {
        run(page, QStringLiteral(R"JS(showFormula({tex:'x^2+1',locale:'de',display:%1,
            rendering:{scale:1.15,inlineScale:.8,inline:false}}))JS").arg(display ? QStringLiteral("true") : QStringLiteral("false")));
        QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("['ready','failed'].includes(formulaStatus)")).toBool(), 15000);
        QCOMPARE(run(page, QStringLiteral("formulaError")).toString(), QString());
        QCOMPARE(run(page, QStringLiteral("document.querySelector('#formula mjx-container').style.fontSize")).toString(),
            display ? QStringLiteral("115%") : QStringLiteral("80%"));
        const auto height = run(page, QStringLiteral("document.querySelector('#formula mjx-container > svg').getBoundingClientRect().height")).toDouble();
        QVERIFY(height > 0);
        if (display) QVERIFY(height > inlineHeight * 1.3);
        else if (inlineHeight) QCOMPARE(height, inlineHeight);
        else inlineHeight = height;
    }
    QCOMPARE(run(page, QStringLiteral("JSON.stringify(ariannaMathDisplay.normalize({scale:99,align:'invalid',overflow:'invalid',fontCache:'invalid',inline:0,loader:{load:['evil']}}))")).toString(),
        QStringLiteral(R"({"scale":2,"inlineScale":2,"align":"center","overflow":"linebreak","inline":true,"fontCache":"local"})"));
    QCOMPARE(run(page, QStringLiteral("ariannaMathDisplay.normalize(null).scale")).toInt(), 1);
    QCOMPARE(run(page, QStringLiteral("ariannaMathDisplay.normalize({scale:1.15}).inlineScale")).toDouble(), 1.15);
    QVERIFY2(page.diagnostics.isEmpty(), qPrintable(page.diagnostics.join(QLatin1Char('\n'))));
    QVERIFY(requests.remote.isEmpty());
}

void EpubMathTest::settingsPreview()
{
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), startServer(server));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("math-test"));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import org.kde.kirigami as Kirigami
        import org.kde.arianna
        Kirigami.ApplicationWindow {
            width: 800; height: 700; visible: true
            property var renderResult
            property bool renderDone: false
            MathJaxSettings { width: parent.width }
        }
    )", QUrl());
    QTRY_VERIFY(component.status() != QQmlComponent::Loading);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.data());
    QVERIFY(window && QTest::qWaitForWindowExposed(window));
    auto *button = window->findChild<QQuickItem *>(QStringLiteral("mathJaxSettingsPreview"));
    QVERIFY(button && button->isEnabled());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
    QQuickWindow *preview = nullptr;
    QTRY_VERIFY((preview = window->findChild<QQuickWindow *>(QStringLiteral("epubMathJaxWindow"))));
    QTRY_VERIFY(preview->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(preview->property("status").toString(), QStringLiteral("ready"), 20000);
    auto *view = preview->findChild<QObject *>(QStringLiteral("mathJaxPreview"));
    QVERIFY(view);
    engine.rootContext()->setContextProperty(QStringLiteral("formulaViewForTest"), view);
    engine.rootContext()->setContextProperty(QStringLiteral("settingsHostForTest"), window);
    QQmlExpression expression(qmlContext(preview), preview,
        QStringLiteral("formulaViewForTest.runJavaScript('document.querySelector(\"#formula mjx-container\").style.fontSize', value => {settingsHostForTest.renderResult=value;settingsHostForTest.renderDone=true})"));
    Config::setMathJaxScale(115);
    QTRY_COMPARE(preview->property("renderingOptions").toMap().value(QStringLiteral("scale")).toDouble(), 1.15);
    QTRY_COMPARE_WITH_TIMEOUT(preview->property("status").toString(), QStringLiteral("ready"), 10000);
    expression.evaluate();
    QVERIFY(!expression.hasError());
    QTRY_VERIFY(window->property("renderDone").toBool());
    QCOMPARE(window->property("renderResult").toString(), QStringLiteral("115%"));
    Config::setMathJaxInlineScale(80);
    auto *inlineButton = window->findChild<QQuickItem *>(QStringLiteral("mathJaxInlinePreview"));
    QVERIFY(inlineButton && inlineButton->isEnabled());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        inlineButton->mapToScene(QPointF(inlineButton->width() / 2, inlineButton->height() / 2)).toPoint());
    QTRY_COMPARE_WITH_TIMEOUT(preview->property("status").toString(), QStringLiteral("ready"), 10000);
    QCOMPARE(Config::mathJaxScale(), 115);
    window->setProperty("renderDone", false);
    expression.evaluate();
    QVERIFY(!expression.hasError());
    QTRY_VERIFY(window->property("renderDone").toBool());
    QCOMPARE(window->property("renderResult").toString(), QStringLiteral("80%"));
    if (qEnvironmentVariableIsSet("ARIANNA_TEST_SCREENSHOT"))
        QVERIFY(window->grabWindow().save(QStringLiteral("/tmp/arianna-math-settings.png")));
    const QPointer<QObject> previousView(view);
    Config::setMathJaxSupport(false);
    QTRY_VERIFY(!preview->isVisible());
    QVERIFY(preview->property("formula").isNull());
    QTRY_VERIFY(previousView.isNull());
    QVERIFY(!button->isEnabled());
    QVERIFY(!inlineButton->isEnabled());
    Config::setMathJaxSupport(true);
    Config::setMathJaxScale(100);
    Config::setMathJaxInlineScale(100);
}

void EpubMathTest::displaySettings()
{
    QFETCH(bool, author);
    QFETCH(QString, cache);
    QFETCH(QString, overflow);
    QFETCH(bool, inlineBreaks);
    QFETCH(bool, mathml);
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    QHttpServer modules;
    modules.router()->addConverter(QMetaType::fromType<QUrl>(), QStringLiteral("[A-Za-z0-9_./-]+"));
    modules.route(QStringLiteral("/<arg>"), [&](const QUrl &path) {
        QFile file(QFINDTESTDATA("../qml/foliate-js/") + path.toString());
        if (!file.open(QIODevice::ReadOnly)) return QHttpServerResponse(QHttpServerResponder::StatusCode::NotFound);
        return QHttpServerResponse(QByteArrayLiteral("text/javascript"), file.readAll());
    });
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QVERIFY(modules.bind(&listener));
    const QString moduleUrl = QStringLiteral("http://127.0.0.1:%1/").arg(listener.serverPort());
    const QJsonObject options{{QStringLiteral("scale"), 1.15}, {QStringLiteral("inlineScale"), 0.8}, {QStringLiteral("align"), QStringLiteral("right")},
        {QStringLiteral("overflow"), overflow}, {QStringLiteral("inline"), inlineBreaks}, {QStringLiteral("fontCache"), cache}};
    const auto json = QJsonDocument(options).toJson(QJsonDocument::Compact);
    const auto bootstrap = base + QStringLiteral("mathjax-bootstrap.js?rendering=") + QString::fromUtf8(QUrl::toPercentEncoding(QString::fromUtf8(json)));
    auto head = QByteArrayLiteral("<style>body{width:260px;margin:0;font-size:16px}</style>");
    if (author) head += QStringLiteral(R"(<script>window.MathJax={loader:{paths:{mathjax:'%1'.replace(/\/$/,''),fonts:'%1fonts'}},output:{fontPath:'%1fonts/%%FONT%%-font'},options:{sre:{locale:'en'},worker:{path:'%1sre',maps:'%1sre/mathmaps'}},svg:{fontCache:'local'}};</script><script id="MathJax-script" src="%1tex-svg.js"></script>)").arg(base).toUtf8();
    auto body = QByteArrayLiteral(R"(<p id="inline">Before \(a+b+c+d+e+f+g+h+i+j\) after.</p><div id="block">\[x=1+2+3+4+5+6+7+8+9+10+11+12+13+14+15+16+17+18+19+20\]</div><p id="after">After.</p>)");
    if (mathml) {
        QString inlineMath, displayMath;
        for (const auto letter : QStringLiteral("abcdefghij")) {
            if (!inlineMath.isEmpty()) inlineMath += QStringLiteral("<mo>+</mo>");
            inlineMath += QStringLiteral("<mi>%1</mi>").arg(letter);
        }
        for (int i = 1; i <= 20; ++i) {
            if (i > 1) displayMath += QStringLiteral("<mo>+</mo>");
            displayMath += QStringLiteral("<mn>%1</mn>").arg(i);
        }
        // Keep the inline sum flat so its operators offer inline break points.
        body = QStringLiteral(R"(<p id="inline">Before <math xmlns="http://www.w3.org/1998/Math/MathML">%1</math> after.</p><div id="block"><math xmlns="http://www.w3.org/1998/Math/MathML" display="block"><mrow><mi>x</mi><mo>=</mo>%2</mrow></math></div><p id="after">After.</p>)").arg(inlineMath, displayMath).toUtf8();
    }
    const auto xhtml = document(body, head);
    OfflineRequests requests;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&requests);
    CheckedPage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    QString savedCFI;
    for (int reload = 0; reload < 2; ++reload) {
        page.setContent(EpubMath::injectMathJax(xhtml, bootstrap), QStringLiteral("application/xhtml+xml"), QUrl(moduleUrl));
        QVERIFY(loaded.wait(5000));
        QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("['ready','failed'].includes(document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus)")).toBool(), 20000);
        QVERIFY2(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]').dataset.ariannaMathStatus === 'ready'")).toBool(),
            qPrintable(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]').dataset.ariannaMathError")).toString()));
        run(page, QStringLiteral(R"JS((async () => {
            try {
                const CFI = await import('%1epubcfi.js');
                await ariannaMathLayout(260);
                const formula = () => document.querySelector('#block [data-arianna-math-output]');
                const beforeHeight = formula().getBoundingClientRect().height;
                const originalOutput = formula();
                const selectionRange = document.createRange(); selectionRange.selectNodeContents(originalOutput);
                await ariannaMathLayout(150);
                const root = formula();
                const source = document.querySelector('#block [data-arianna-math-source]');
                const range = document.createRange(); range.setStart(document.getElementById('after').firstChild, 2); range.collapse(true);
                const cfi = CFI.fromRange(range);
                const formulaRange = document.createRange(); formulaRange.setStart(root, 0); formulaRange.collapse(true);
                const formulaCFI = CFI.fromRange(formulaRange);
                const restored = CFI.toRange(document, CFI.parse(formulaCFI));
                globalThis.settingsResult = {
                    cfi, roundtrip:CFI.fromRange(CFI.toRange(document, CFI.parse(cfi))),
                    sourceTag:source.localName, hidden:getComputedStyle(source).display==='none',
                    visible:restored.startContainer===root || root.contains(restored.startContainer),
                    taller:root.getBoundingClientRect().height>beforeHeight+5,
                    justify:root.getAttribute('justify'), overflow:root.getAttribute('overflow'),
                    width:root.getBoundingClientRect().width, height:root.getBoundingClientRect().height,
                    inlineSvgs:document.querySelectorAll('#inline [data-arianna-math-output] > svg').length,
                    inlineSize:document.querySelector('#inline [data-arianna-math-output]').style.fontSize,
                    validUses:Array.from(document.querySelectorAll('[data-arianna-math-output] use')).every(use=>
                        document.getElementById((use.getAttribute('href') || use.getAttribute('xlink:href')).slice(1))),
                    cache:MathJax.startup.document.outputJax.options.fontCache,
                    globalCache:!!document.getElementById('MJX-SVG-global-cache'),
                    paths:root.querySelectorAll('svg path').length,
                    defs:root.querySelectorAll('svg defs').length,
                    speech:root.getAttribute('data-semantic-speech')?.slice(0,60),
                    metrics:MathJax.startup.document.outputJax.math.metrics,
                    mathml:ariannaMathDetails(root).mathml.slice(0,500),
                    svg:Array.from(root.querySelectorAll(':scope > svg')).map(svg=>({
                        box:svg.getAttribute('viewBox'),style:svg.getAttribute('style'),
                        rect:[svg.getBoundingClientRect().width,svg.getBoundingClientRect().height],
                    })),
                    rootStyle:root.getAttribute('style'), css:[getComputedStyle(root).display,getComputedStyle(root).height],
                    beforeHeight,
                    retainedSelection:root===originalOutput && selectionRange.getBoundingClientRect().height>0,
                    sameRoot:root===originalOutput, children:Array.from(root.childNodes, node=>node.localName||node.nodeType),
                    range:[selectionRange.startContainer.localName,selectionRange.startOffset,
                        selectionRange.endContainer.localName,selectionRange.endOffset,selectionRange.getBoundingClientRect().height],
                };
            } catch(e) {globalThis.settingsError=String(e)}
        })())JS").arg(moduleUrl));
        QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("!!globalThis.settingsResult || !!globalThis.settingsError")).toBool(), 20000);
        QVERIFY2(run(page, QStringLiteral("!globalThis.settingsError")).toBool(), qPrintable(run(page, QStringLiteral("settingsError")).toString()));
        const auto result = run(page, QStringLiteral("settingsResult")).toMap();
        const QString details = QString::fromUtf8(QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact));
        QCOMPARE(result.value(QStringLiteral("cfi")).toString(), QStringLiteral("epubcfi(/4/6[after]/1:2)"));
        QCOMPARE(result.value(QStringLiteral("roundtrip")).toString(), result.value(QStringLiteral("cfi")).toString());
        if (reload) QCOMPARE(result.value(QStringLiteral("cfi")).toString(), savedCFI);
        savedCFI = result.value(QStringLiteral("cfi")).toString();
        QCOMPARE(result.value(QStringLiteral("sourceTag")).toString(), author ? QStringLiteral("mjx-container") : mathml ? QStringLiteral("math") : QStringLiteral("span"));
        QVERIFY2(result.value(QStringLiteral("hidden")).toBool() && result.value(QStringLiteral("visible")).toBool(), qPrintable(details));
        QVERIFY2(result.value(QStringLiteral("retainedSelection")).toBool(), qPrintable(details));
        QVERIFY2(result.value(QStringLiteral("validUses")).toBool(), qPrintable(details));
        QCOMPARE(result.value(QStringLiteral("cache")).toString(), cache);
        QCOMPARE(result.value(QStringLiteral("globalCache")).toBool(), cache == QLatin1String("global"));
        if (cache == QLatin1String("none")) QVERIFY(result.value(QStringLiteral("paths")).toInt() > 0);
        if (cache == QLatin1String("local")) QVERIFY(result.value(QStringLiteral("defs")).toInt() > 0);
        QCOMPARE(result.value(QStringLiteral("inlineSvgs")).toInt() > 1, inlineBreaks);
        QCOMPARE(result.value(QStringLiteral("inlineSize")).toString(), QStringLiteral("80%"));
        if (overflow != QLatin1String("scale")) QVERIFY2(result.value(QStringLiteral("rootStyle")).toString().contains(QStringLiteral("font-size: 115%")), qPrintable(details));
        QVERIFY2(!result.value(QStringLiteral("speech")).toString().isEmpty(), qPrintable(details));
        if (overflow == QLatin1String("linebreak")) QVERIFY2(result.value(QStringLiteral("taller")).toBool(), qPrintable(details));
    }
    QVERIFY(requests.remote.isEmpty());
    QVERIFY2(page.diagnostics.isEmpty(), qPrintable(page.diagnostics.join(QLatin1Char('\n'))));
}

void EpubMathTest::baselSeriesReading_data()
{
    QTest::addColumn<bool>("author");
    QTest::newRow("reader-mathjax") << false;
    QTest::newRow("epub-tex-svg") << true;
}

void EpubMathTest::baselSeriesReading()
{
    QFETCH(bool, author);
    BookServer server(QStringLiteral("math-test"));
    QVERIFY(server.isRunning());
    const QString base = startServer(server) + QStringLiteral("/mathjax/math-test/");
    QHttpServer modules;
    modules.router()->addConverter(QMetaType::fromType<QUrl>(), QStringLiteral("[A-Za-z0-9_./-]+"));
    modules.route(QStringLiteral("/<arg>"), [&](const QUrl &path) {
        QFile file(QFINDTESTDATA("../qml/foliate-js/") + path.toString());
        if (!file.open(QIODevice::ReadOnly)) return QHttpServerResponse(QHttpServerResponder::StatusCode::NotFound);
        return QHttpServerResponse(QByteArrayLiteral("text/javascript"), file.readAll());
    });
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    QVERIFY(modules.bind(&listener));
    const QString moduleUrl = QStringLiteral("http://127.0.0.1:%1/").arg(listener.serverPort());
    const auto authorConfig = author ? QStringLiteral(R"(<script>window.MathJax={loader:{paths:{mathjax:'%1'.replace(/\/$/,''),fonts:'%1fonts'}},output:{fontPath:'%1fonts/%%FONT%%-font'},options:{sre:{locale:'en'},worker:{path:'%1sre',maps:'%1sre/mathmaps'}},svg:{fontCache:'local'}};</script><script id="MathJax-script" src="%1tex-svg.js"></script>)").arg(base).toUtf8() : QByteArray{};
    OfflineRequests requests;
    QWebEngineProfile profile;
    profile.setUrlRequestInterceptor(&requests);
    CheckedPage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    const auto original = document(QByteArrayLiteral(R"(<p>Let’s look at the 12 theorems that the readers voted as the top half:</p>
<p class="list1">(1)&#160;\(e^{\pi i}=-1\).&#160;</p>
<p class="list1">(2)&#160;&#160;The Euler Polyhedron Formula: <i>V</i> − <i>E</i> + <i>F</i> = 2.</p>
<p class="list1">(3)&#160;&#160;The number of primes is infinite.</p>
<p class="list1">(4)&#160;&#160;There are five regular polyhedra.</p>
<p id="five" class="list1">(5)&#160;&#160;\(1+\frac{1}{2^2}+\frac{1}{3^2}+\frac{1}{4^2}+\cdots=\frac{\pi^2}{6}\).</p>
<p class="list1">(6)&#160;&#160;A continuous mapping of a closed unit disk into itself has a fixed point.</p>
<p class="list1">(7)&#160;&#160;There is no rational number whose square is 2. (The number \(\sqrt{2}\) is irrational.)</p>
<p class="list1">(8)&#160;&#160;The number <i>π</i> is transcendental.</p>)"), authorConfig).replace("lang=\"de\"", "lang=\"en\"");
    page.setContent(EpubMath::injectMathJax(original, base + QStringLiteral("mathjax-bootstrap.js")), QStringLiteral("application/xhtml+xml"), QUrl(moduleUrl));
    QVERIFY(loaded.wait(5000));
    QTRY_COMPARE_WITH_TIMEOUT(run(page, QStringLiteral("document.querySelector('[data-arianna-mathjax]')?.dataset.ariannaMathStatus")).toString(), QStringLiteral("ready"), 20000);
    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(reader.readAll());
    run(page, speechHelpers(source) + QStringLiteral(R"JS((async () => {
        try {
            const CFI = await import('%1epubcfi.js');
            const range = document.createRange(); range.selectNodeContents(document.getElementById('five'));
            const formula = document.querySelector('#five mjx-container');
            const locate = source => CFI.fromRange(source);
            const anchor = document.createRange(); anchor.setStart(formula, 0); anchor.collapse(true);
            const cfi = locate(anchor);
            const selectionCFI = locate(range);
            const snapshot = () => JSON.stringify({dom:document.body.outerHTML,
                rect:Array.from(['x','y','width','height'], key => formula.getBoundingClientRect()[key])});
            const before = snapshot();
            // Delay a real German SRE reply across a switch to an English voice.
            const generate = window.ariannaMathSpeech;
            let release, arrived;
            const held = new Promise(resolve => release = resolve);
            const ready = new Promise(resolve => arrived = resolve);
            window.ariannaMathSpeech = async (formula, options) => {
                const speech = await generate(formula, options);
                if (options.locale === 'de') { arrived(); await held; }
                return speech;
            };
            setMathSpeechOptions({locale:'de'});
            const pending = prepareMathSpeech(document, range);
            await ready;
            setMathSpeechOptions({locale:'en'});
            await prepareMathSpeech(document, range);
            release(); await pending;
            window.ariannaMathSpeech = generate;
            if (collectSpeechContent(range, ['en'], undefined, locate)[1]?.lang !== 'en')
                throw Error('A late worker reply restored the previous speech language');
            globalThis.baselResults = [];
            for (const locale of ['', 'de', 'en']) {
                setMathSpeechOptions({locale,concise:true});
                await prepareMathSpeech(document, range);
                const segments = collectSpeechContent(range, ['en'], undefined, locate);
                baselResults.push({locale:locale || 'en',segments});
                const after = snapshot();
                if (before !== after) {
                    let at = 0; while (before[at] === after[at]) ++at;
                    throw Error('Formula speech changed DOM/geometry at ' + at + ': '
                        + before.slice(at, at + 160) + ' -> ' + after.slice(at, at + 160));
                }
                const restored = locate(CFI.toRange(document, CFI.parse(cfi)));
                if (restored !== cfi || locate(range) !== selectionCFI)
                    throw Error('Saved formula/selection CFI changed: ' + cfi + ' -> ' + restored);
            }
            globalThis.baselFinished = true;
        } catch(e) {globalThis.baselError=String(e)}
    })())JS").arg(moduleUrl));
    QTRY_VERIFY_WITH_TIMEOUT(run(page, QStringLiteral("globalThis.baselFinished || !!globalThis.baselError")).toBool(), 20000);
    QVERIFY2(run(page, QStringLiteral("globalThis.baselFinished === true")).toBool(), qPrintable(run(page, QStringLiteral("globalThis.baselError")).toString()));
    QTemporaryDir dir;
    FakeSpeechDispatcher dispatcher(dir.filePath(QStringLiteral("speech.sock")));
    QVERIFY(dispatcher.startServer());
    qputenv("SPEECHD_ADDRESS", QByteArray("unix_socket:").append(QFile::encodeName(dir.filePath(QStringLiteral("speech.sock")))));
    Config::setSpeechVoice(QString());
    SpeechController speech;
    const auto results = run(page, QStringLiteral("baselResults")).toList();
    QCOMPARE(results.size(), 3);
    for (const auto &value : results) {
        const auto result = value.toMap();
        const QString locale = result.value(QStringLiteral("locale")).toString();
        const auto segments = result.value(QStringLiteral("segments")).toList();
        QCOMPARE(segments.size(), 3);
        QVERIFY(segments.first().toMap().value(QStringLiteral("text")).toString().startsWith(QStringLiteral("(5)")));
        QCOMPARE(segments.last().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("."));
        const auto math = segments.at(1).toMap();
        const QString text = math.value(QStringLiteral("text")).toString();
        QCOMPARE(math.value(QStringLiteral("lang")).toString(), locale);
        QCOMPARE(math.value(QStringLiteral("symbolCount")).toInt(), 23);
        QVERIFY(!math.value(QStringLiteral("tooComplex")).toBool());
        QVERIFY(!text.contains(QStringLiteral("numerator")) && !text.contains(QStringLiteral("Zähler")) && !text.contains(QStringLiteral("\\frac")));
        QVERIFY2(text.contains(locale == QLatin1String("de") ? QStringLiteral("geteilt durch") : QStringLiteral("over")), qPrintable(text));
        QVERIFY2(text.contains(locale == QLatin1String("de") ? QStringLiteral("und so weiter") : QStringLiteral("and so on")), qPrintable(text));
        QCOMPARE(math.value(QStringLiteral("type")).toString(), QStringLiteral("math"));
        QVERIFY(!math.value(QStringLiteral("cfi")).toString().isEmpty());
        const int previous = dispatcher.messages().size();
        // Exercise the exact extracted math segment through the native SSIP client.
        speech.speakSegments({math});
        QTRY_COMPARE(dispatcher.messages().size(), previous + 1);
        QCOMPARE(dispatcher.messages().last().text, text);
        QCOMPARE(dispatcher.messages().last().language, locale);
        dispatcher.finish(dispatcher.messages().last().id);
        QTRY_COMPARE(speech.state(), SpeechController::Idle);
    }
    QVERIFY(requests.remote.isEmpty());
    QVERIFY2(page.diagnostics.isEmpty(), qPrintable(page.diagnostics.join(QLatin1Char('\n'))));
}

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid()) return 1;
    for (const char *name : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
        qputenv(name, QFile::encodeName(environment.path()));
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("arianna");
    QTcpServer port;
    if (!port.listen(QHostAddress::LocalHost, 0)) return 1;
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(port.serverPort());
    port.close();
    EpubMathTest test;
    return QTest::qExec(&test, argc, argv);
}

#endif
#include "epubmathtest.moc"
