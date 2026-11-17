// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bookserver.h"
#include "bookserverconfig.h"
#include "config.h"
#include "epubmath.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KZip>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>
#include <QtWebEngineQuick>

Q_IMPORT_QML_PLUGIN(org_kde_ariannaPlugin)

#ifndef Q_MOC_RUN
namespace
{
QString encoded(const QString &value)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)) + QStringLiteral("[0]");
}

QVariant evaluate(QObject *scope, const QString &code)
{
    QQmlExpression expression(qmlContext(scope), scope, code);
    const auto result = expression.evaluate();
    if (expression.hasError()) QTest::qFail(qPrintable(expression.error().toString()), __FILE__, __LINE__);
    return result;
}

QByteArray xhtml(const QByteArray &body)
{
    return QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"de\"><head><title>Notes</title></head><body>")
        + body + QByteArrayLiteral("</body></html>");
}
}
#endif

class EpubFootnoteTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void nativeWindowAndNavigation();
};

#ifndef Q_MOC_RUN
void EpubFootnoteTest::nativeWindowAndNavigation()
{
    BookServer server(QStringLiteral("footnote-test"));
    QVERIFY(server.isRunning());
    server.registerReaderSession(QStringLiteral("footnote-test"));
    Config::setSpeechVoice(QString());
    QTemporaryDir directory;
    const QString backgroundPath = directory.filePath(QStringLiteral("reader-background.png"));
    QImage background(64, 64, QImage::Format_RGB32);
    background.fill(QColor(QStringLiteral("#b87939")));
    QVERIFY(background.save(backgroundPath));
    Config::setReaderBackgroundPath(backgroundPath);
    Config::setReaderTheme(Config::EnumReaderTheme::Normal);
    const QString path = directory.filePath(QStringLiteral("footnotes.epub"));
    KZip zip(path);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    QVERIFY(zip.writeFile(QStringLiteral("mimetype"), "application/epub+zip"));
    QVERIFY(zip.writeFile(QStringLiteral("META-INF/container.xml"),
        R"(<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="package.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"));
    QVERIFY(zip.writeFile(QStringLiteral("package.opf"), R"(
        <package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
        <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Footnotes</dc:title><dc:identifier id="id">footnotes</dc:identifier><dc:language>de</dc:language></metadata>
        <manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
        <item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/>
        <item id="target" href="target.xhtml" media-type="application/xhtml+xml"/>
        <item id="picture" href="picture.svg" media-type="image/svg+xml"/></manifest>
        <spine><itemref idref="chapter"/><itemref idref="notes"/><itemref idref="target"/></spine></package>)"));
    QVERIFY(zip.writeFile(QStringLiteral("chapter.xhtml"), xhtml(R"(
        <p id="reading">Reading with <a id="footref" epub:type="noteref" href="notes.xhtml#foot">1</a>,
        <a id="endref" epub:type="noteref" href="notes.xhtml#end">2</a> and
        <a id="otherref" epub:type="noteref" href="notes.xhtml#other">3</a>.</p>)")));
    const auto bootstrap = BookServerConfig::baseUrl() + QStringLiteral("/mathjax/footnote-test/mathjax-bootstrap.js");
    QVERIFY(zip.writeFile(QStringLiteral("notes.xhtml"), EpubMath::injectMathJax(xhtml(R"(
        <aside id="foot" epub:type="footnote"><p>Footnote <em>kursiv</em> \(x^2+1\)</p>
        <img src="picture.svg" alt="Embedded picture"/>
        <a href="target.xhtml#destination">Visit target</a><div style="height:1500px">Long note</div></aside>
        <section id="end" epub:type="endnote"><p>Endnote <strong>bold</strong>.</p>
        <a href="target.xhtml#destination">Visit target</a></section>
        <aside id="other" epub:type="footnote"><p>The other footnote.</p></aside>)"), bootstrap)));
    QVERIFY(zip.writeFile(QStringLiteral("target.xhtml"), xhtml("<p id=\"destination\">The destination chapter.</p>")));
    QVERIFY(zip.writeFile(QStringLiteral("picture.svg"), R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="20"><rect width="30" height="20" fill="blue"/></svg>)"));
    QVERIFY(zip.close());
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = original.readAll();
    original.close();
    const QString bookUrl = QStringLiteral("data:application/epub+zip;base64,") + QString::fromLatin1(sourceBytes.toBase64());

    QQmlEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerBaseUrl"), BookServerConfig::baseUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), QStringLiteral("footnote-test"));
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
    QScopedPointer<QObject> application(component.create());
    QVERIFY2(application, qPrintable(component.errorString()));
    auto *mainWindow = qobject_cast<QQuickWindow *>(application.data());
    QVERIFY(mainWindow && QTest::qWaitForWindowExposed(mainWindow));
    auto *readerPage = application->findChild<QQuickItem *>(QStringLiteral("reader"));
    auto *scope = application->findChild<QObject *>(QStringLiteral("epubSearchModel"));
    auto *note = application->findChild<QQuickWindow *>(QStringLiteral("epubFootnoteWindow"));
    QVERIFY(readerPage && scope && note);
    const auto qml = [&](const QString &code) { return evaluate(scope, code); };
    const auto script = [&](const QString &code, bool preview = false) {
        qml(QStringLiteral("root.scriptDone=false; %1.runJavaScript(%2, %3, value => {root.scriptResult=value;root.scriptDone=true})")
                .arg(preview ? QStringLiteral("footnoteWindow.preview") : QStringLiteral("view"), encoded(code), preview ? QStringLiteral("1") : QStringLiteral("0")));
        QElapsedTimer timer;
        timer.start();
        while (!readerPage->property("scriptDone").toBool() && timer.elapsed() < 5000) QTest::qWait(10);
        if (!readerPage->property("scriptDone").toBool()) QTest::qFail("WebEngine script timed out", __FILE__, __LINE__);
        return readerPage->property("scriptResult");
    };
    QTRY_VERIFY_WITH_TIMEOUT(qml(QStringLiteral("view.readerReady")).toBool(), 15000);
    QVERIFY(readerPage->setProperty("url", QStringLiteral("footnotes.epub")));
    qml(QStringLiteral("view.runJavaScript('openSync(' + JSON.stringify(%1) + ', null, \"\", ' + JSON.stringify(root.currentReaderLayout()) + ')')").arg(encoded(bookUrl)));
    QTRY_VERIFY_WITH_TIMEOUT(qml(QStringLiteral("view.bookReady")).toBool(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("!!reader.view.renderer.getContents().find(c=>c.doc?.getElementById('footref'))")).toBool(), 15000);
    script(QStringLiteral(R"JS(
        import(new URL('footnote-preview.js', document.baseURI).href).then(({createFootnotePreview}) => {
            const source = new DOMParser().parseFromString(`<html lang="de"><head><style>em{color:blue}</style></head>
                <body><p onclick="bad()">Safe <em>formatted</em></p><script>bad()</script><form><input/></form>
                <iframe src="https://example.invalid"></iframe><a href="target.xhtml#ok">Link</a>
                <a href="javascript:bad()">Bad</a></body></html>`, 'text/html');
            const before = source.documentElement.outerHTML;
            const result = createFootnotePreview(source, {sections:[{resolveHref:h=>'OPS/'+h}],isExternal:()=>false}, 0, 'test');
            const copy = new DOMParser().parseFromString(result.html, 'text/html');
            globalThis.snapshotCheck = {unchanged:source.documentElement.outerHTML===before,
                passive:!copy.querySelector('script,iframe,form,input,[onclick]'),
                formatting:copy.querySelector('em').textContent==='formatted' && !!copy.querySelector('style'),
                target:result.links.length===1 && result.links[0].href==='OPS/target.xhtml#ok'};
        }); true
    )JS"));
    QTRY_VERIFY(script(QStringLiteral("!!globalThis.snapshotCheck")).toBool());
    QVERIFY(script(QStringLiteral("Object.values(snapshotCheck).every(Boolean)")).toBool());
    const auto click = [&](const QString &id) {
        return script(QStringLiteral("reader.view.renderer.getContents().find(c=>c.doc?.getElementById(%1)).doc.getElementById(%1).click();true").arg(encoded(id)));
    };

    const auto readingCfi = script(QStringLiteral("reader.view.lastLocation.cfi"));
    const int fullHeight = note->height();
    click(QStringLiteral("footref"));
    QTRY_VERIFY_WITH_TIMEOUT(note->isVisible(), 20000);
    QVERIFY(note->flags().testFlag(Qt::WindowTitleHint));
    QVERIFY(note->flags().testFlag(Qt::WindowCloseButtonHint));
    QCOMPARE(note->modality(), Qt::NonModal);
    QCOMPARE(note->transientParent(), mainWindow);
    QTRY_VERIFY(!qml(QStringLiteral("footnoteWindow.preview.loading")).toBool());
    QVERIFY(!note->property("loadFailed").toBool());
    QVERIFY(!qml(QStringLiteral("footnoteWindow.preview.settings.javascriptEnabled")).toBool());
    QVERIFY(qml(QStringLiteral("!footnoteWindow.preview.webChannel || footnoteWindow.preview.webChannel.registeredObjects.length === 0")).toBool());
    QCOMPARE(script(QStringLiteral("reader.view.lastLocation.cfi")), readingCfi);
    QCOMPARE(script(QStringLiteral("document.querySelector('em')?.textContent"), true).toString(), QStringLiteral("kursiv"));
    QTRY_VERIFY_WITH_TIMEOUT(script(QStringLiteral("document.querySelector('img')?.complete && document.querySelector('img').naturalWidth === 30"), true).toBool(), 5000);
    QVERIFY(script(QStringLiteral("!!document.querySelector('mjx-container svg')"), true).toBool());
    auto *go = note->findChild<QQuickItem *>(QStringLiteral("epubFootnoteGoButton"));
    QVERIFY(go && !go->isVisible());
    QTRY_COMPARE(note->height(), fullHeight);

    // The same reader CSS, overlay and image are used in the passive preview.
    const QString previewBackground = QStringLiteral("getComputedStyle(document.documentElement,'::before').backgroundImage");
    const auto readerBackground = [&] {
        return script(QStringLiteral("(() => {const el=document.createElement('div');el.style.backgroundImage=reader.style.readerBackgroundImage;return el.style.backgroundImage})()"));
    };
    QCOMPARE(script(previewBackground, true), readerBackground());
    QCOMPARE(script(QStringLiteral("getComputedStyle(document.documentElement,'::before').backgroundSize"), true).toString(), QStringLiteral("cover, cover"));
    QCOMPARE(script(QStringLiteral("getComputedStyle(document.documentElement,'::before').position"), true).toString(), QStringLiteral("fixed"));
    script(QStringLiteral("globalThis.backgroundProbe=new Image();backgroundProbe.src=%1+'/static/background-image';true").arg(encoded(BookServerConfig::baseUrl())), true);
    QTRY_VERIFY(script(QStringLiteral("backgroundProbe.complete && backgroundProbe.naturalWidth===64"), true).toBool());
    const auto readerColor = [&] {
        return script(QStringLiteral("(() => {const d=reader.view.renderer.getContents()[0].doc;return d.defaultView.getComputedStyle(d.querySelector('p')).color})()"));
    };
    const QString previewColor = QStringLiteral("getComputedStyle(document.querySelector('p')).color");
    QCOMPARE(script(previewColor, true), readerColor());
    QCOMPARE(script(QStringLiteral("getComputedStyle(document.documentElement).fontFamily"), true),
             script(QStringLiteral("(() => {const d=reader.view.renderer.getContents()[0].doc;return d.defaultView.getComputedStyle(d.documentElement).fontFamily})()")));
    const QString previewPath = qEnvironmentVariable("ARIANNA_FOOTNOTE_TEST_PREVIEW");
    if (!previewPath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(note->grabWindow().save(previewPath));
    }

    const QString themedRequest = note->property("requestId").toString();
    const auto themedGeometry = note->geometry();
    script(QStringLiteral("document.body.dataset.retained='yes';globalThis.retainedMath=document.querySelector('mjx-container svg');scrollTo(0,180);true"), true);
    QTRY_VERIFY(script(QStringLiteral("scrollY>=170"), true).toBool());
    const int scrollPosition = script(QStringLiteral("scrollY"), true).toInt();
    qml(QStringLiteral("root.setReaderThemeMode(1)"));
    QTRY_COMPARE(script(previewColor, true).toString(), QStringLiteral("rgb(255, 255, 255)"));
    QCOMPARE(script(previewBackground, true), readerBackground());
    QCOMPARE(script(QStringLiteral("getComputedStyle(document.documentElement).backgroundColor"), true).toString(), QStringLiteral("rgb(0, 0, 0)"));
    QCOMPARE(script(QStringLiteral("scrollY"), true).toInt(), scrollPosition);
    QVERIFY(script(QStringLiteral("document.body.dataset.retained==='yes' && retainedMath===document.querySelector('mjx-container svg')"), true).toBool());
    QVERIFY(script(QStringLiteral("getComputedStyle(document.querySelector('img')).filter.includes('invert(1)')"), true).toBool());
    QCOMPARE(script(QStringLiteral("getComputedStyle(document.querySelector('mjx-container svg')).filter"), true).toString(), QStringLiteral("none"));
    if (!previewPath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(note->grabWindow().save(previewPath + QStringLiteral(".inverted.png")));
    }
    qml(QStringLiteral("root.setReaderThemeMode(2)"));
    QTRY_COMPARE(script(previewColor, true), readerColor());
    QTRY_COMPARE(script(previewBackground, true), readerBackground());
    qml(QStringLiteral("root.setReaderThemeMode(0)"));
    QTRY_COMPARE(script(previewColor, true).toString(), QStringLiteral("rgb(0, 0, 0)"));
    QCOMPARE(script(previewBackground, true), readerBackground());
    QCOMPARE(script(QStringLiteral("getComputedStyle(document.querySelector('img')).filter"), true).toString(), QStringLiteral("none"));
    QCOMPARE(note->property("requestId").toString(), themedRequest);
    QCOMPARE(note->geometry(), themedGeometry);
    QCOMPARE(script(QStringLiteral("reader.view.lastLocation.cfi")), readingCfi);
    // Removing the image also removes its old fixed layer in an open preview.
    script(QStringLiteral("reader.setAppearance({style:{...reader.style,readerBackgroundImage:''}});true"));
    QTRY_COMPARE(script(previewBackground, true).toString(), QStringLiteral("none"));
    qml(QStringLiteral("backend.applyStyle()"));
    QTRY_COMPARE(script(previewBackground, true), readerBackground());

    // Move and resize the actual native QQuickWindow, independently of the reader.
    const auto mainGeometry = mainWindow->geometry();
    note->setPosition(note->position() + QPoint(70, 30));
    note->resize(620, 470);
    const auto noteGeometry = note->geometry();
    QCOMPARE(mainWindow->geometry(), mainGeometry);
    const QString oldRequest = note->property("requestId").toString();
    QVERIFY(!oldRequest.isEmpty());
    click(QStringLiteral("otherref"));
    QTRY_VERIFY_WITH_TIMEOUT(note->isVisible() && note->property("requestId").toString() != oldRequest, 15000);
    QTRY_VERIFY(note->height() < fullHeight / 2);
    QCOMPARE(note->position(), noteGeometry.topLeft());
    QCOMPARE(note->width(), noteGeometry.width());
    QVERIFY(script(QStringLiteral("document.body.getBoundingClientRect().bottom <= innerHeight + 1"), true).toBool());
    const int shortHeight = note->height();
    if (!previewPath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(note->grabWindow().save(previewPath + QStringLiteral(".short.png")));
    }
    qml(QStringLiteral("footnoteWindow.readerZoom = 2"));
    QTRY_VERIFY(note->height() > shortHeight && note->height() <= fullHeight);
    QVERIFY(script(QStringLiteral("document.body.getBoundingClientRect().bottom <= innerHeight + 1"), true).toBool());
    qml(QStringLiteral("footnoteWindow.readerZoom = 1"));
    QTRY_COMPARE(note->height(), shortHeight);
    script(QStringLiteral("reader.closeFootnote(%1)").arg(encoded(oldRequest)));
    qml(QStringLiteral("footnoteWindow.reset(%1)").arg(encoded(oldRequest)));
    QVERIFY(note->isVisible());
    QTRY_VERIFY(script(QStringLiteral("document.body.textContent.includes('The other footnote.')"), true).toBool());
    note->requestActivate();
    QTest::keyClick(note, Qt::Key_Escape);
    QTRY_VERIFY(!note->isVisible());
    QTRY_COMPARE(script(QStringLiteral("document.getElementById('footnote-source').childElementCount")).toInt(), 0);

    // Returning from a short note to a long one restores the existing height cap.
    click(QStringLiteral("footref"));
    QTRY_VERIFY_WITH_TIMEOUT(note->isVisible(), 15000);
    QTRY_COMPARE(note->height(), fullHeight);
    QVERIFY(script(QStringLiteral("document.scrollingElement.scrollHeight > innerHeight"), true).toBool());
    note->requestActivate();
    QTest::keyClick(note, Qt::Key_Escape);
    QTRY_VERIFY(!note->isVisible());

    click(QStringLiteral("endref"));
    QTRY_VERIFY_WITH_TIMEOUT(note->isVisible(), 15000);
    QTRY_VERIFY(note->height() < fullHeight / 2);
    QVERIFY(go->isVisible());
    QVERIFY(script(QStringLiteral("document.body.getBoundingClientRect().bottom <= innerHeight + 1"), true).toBool());
    QVERIFY(QMetaObject::invokeMethod(go, "clicked"));
    QTRY_VERIFY(!note->isVisible());
    QTRY_COMPARE(script(QStringLiteral("reader.view.lastLocation.section.current")).toInt(), 1);

    script(QStringLiteral("reader.view.goTo('chapter.xhtml#reading');true"));
    QTRY_COMPARE(script(QStringLiteral("reader.view.lastLocation.section.current")).toInt(), 0);
    click(QStringLiteral("endref"));
    QTRY_VERIFY_WITH_TIMEOUT(note->isVisible(), 15000);
    QTRY_VERIFY(!qml(QStringLiteral("footnoteWindow.preview.loading")).toBool());
    const auto href = script(QStringLiteral("document.querySelector('a').href"), true).toString();
    QVERIFY(href.startsWith(QStringLiteral("https://arianna.invalid/footnote/")));
    const auto linkRect = script(QStringLiteral("(() => {const r=document.querySelector('a').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()"), true).toMap();
    auto *previewItem = note->findChild<QQuickItem *>(QStringLiteral("epubFootnotePreview"));
    QVERIFY(previewItem);
    const qreal zoom = qml(QStringLiteral("footnoteWindow.readerZoom")).toDouble();
    const QPoint point = previewItem->mapToScene(QPointF(linkRect.value(QStringLiteral("x")).toDouble() * zoom,
                                                       linkRect.value(QStringLiteral("y")).toDouble() * zoom)).toPoint();
    QTest::mouseClick(note, Qt::LeftButton, Qt::NoModifier, point);
    QTRY_VERIFY(!note->isVisible());
    QTRY_COMPARE(script(QStringLiteral("reader.view.lastLocation.section.current")).toInt(), 2);

    script(QStringLiteral("reader.view.goTo('chapter.xhtml#reading');true"));
    QTRY_COMPARE(script(QStringLiteral("reader.view.lastLocation.section.current")).toInt(), 0);
    click(QStringLiteral("footref"));
    QTRY_VERIFY_WITH_TIMEOUT(note->isVisible(), 15000);
    qml(QStringLiteral("view.closeReader()"));
    QTRY_VERIFY(!note->isVisible());
    QTRY_VERIFY(note->property("requestId").toString().isEmpty());
    QTRY_COMPARE(script(QStringLiteral("document.getElementById('footnote-source').childElementCount")).toInt(), 0);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), sourceBytes);
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
    if (!port.listen(QHostAddress::LocalHost, 0)) {
        qCritical() << "Cannot reserve local BookServer test port:" << port.errorString();
        return 1;
    }
    Config::setBookServerAddress(QStringLiteral("127.0.0.1"));
    Config::setBookServerPort(port.serverPort());
    port.close();
    EpubFootnoteTest test;
    return QTest::qExec(&test, argc, argv);
}

#endif
#include "epubfootnotetest.moc"
