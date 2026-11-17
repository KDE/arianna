// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QtWebEngineQuick>

class EpubImageInversionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void svgCoverContextMenuKeepsOriginalColors_data();
    void svgCoverContextMenuKeepsOriginalColors();
};

void EpubImageInversionTest::svgCoverContextMenuKeepsOriginalColors_data()
{
    QTest::addColumn<QString>("stylesheetName");
    QTest::newRow("inverted") << QStringLiteral("invertStylesheet");
    QTest::newRow("inverted-with-background") << QStringLiteral("invertBackgroundStylesheet");
}

void EpubImageInversionTest::svgCoverContextMenuKeepsOriginalColors()
{
    QFETCH(QString, stylesheetName);
    QFile qml(QFINDTESTDATA("../qml/EpubViewerPage.qml"));
    QVERIFY(qml.open(QIODevice::ReadOnly));
    const QString qmlSource = QString::fromUtf8(qml.readAll());
    const qsizetype styleDeclaration = qmlSource.indexOf(QStringLiteral("const %1 =").arg(stylesheetName));
    QVERIFY(styleDeclaration >= 0);
    const qsizetype styleStart = qmlSource.indexOf(QLatin1Char('`'), styleDeclaration) + 1;
    const qsizetype styleEnd = qmlSource.indexOf(QLatin1Char('`'), styleStart);
    QVERIFY(styleEnd > styleStart);
    const QString stylesheet = qmlSource.mid(styleStart, styleEnd - styleStart);

    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(reader.readAll());
    const auto section = [&source](const QString &startMarker, const QString &endMarker) {
        const qsizetype start = source.indexOf(startMarker);
        const qsizetype end = source.indexOf(endMarker, start);
        return start >= 0 && end > start ? source.mid(start, end - start) : QString();
    };
    const QString imageHelpers = section(QStringLiteral("const contextImageElement ="), QStringLiteral("const setImageNotInverse ="));
    const QString rangeHelper = section(QStringLiteral("const rangeForNode ="), QStringLiteral("const getLang ="));
    const QString contextHandler = section(QStringLiteral("doc.addEventListener('contextmenu'"), QStringLiteral("let isSelecting ="));
    const QString applyAction = section(QStringLiteral("setContextImageNotInverse() {"), QStringLiteral("setAppearance({"));
    QVERIFY(!imageHelpers.isEmpty());
    QVERIFY(!rangeHelper.isEmpty());
    QVERIFY(!contextHandler.isEmpty());
    QVERIFY(!applyAction.isEmpty());

    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral(R"(
        <!DOCTYPE html><html><head><style>%1</style></head><body>
        <svg id="cover" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
             class="cover" width="1053" height="1499" viewBox="0 0 1053 1499" preserveAspectRatio="none">
            <g id="cover-group"><image id="photo" class="picture" width="1053" height="1499" xlink:href="data:image/png;base64,"/></g>
        </svg>
        <svg id="untouched" xmlns="http://www.w3.org/2000/svg"><image href="data:image/png;base64,"/></svg>
        <img id="plain" data-arianna-original-src="images/photo.png" src="data:image/png;base64,">
        </body></html>
    )").arg(stylesheet));
    QVERIFY(loaded.wait(10000));
    QVERIFY(loaded.constFirst().constFirst().toBool());

    const QString script = imageHelpers + rangeHelper + QStringLiteral(R"(
        (async () => {
            const actions = [];
            const dispatch = action => actions.push(action.type);
            const dispatchBookReloadNeeded = () => {};
            let resolveSave;
            let setImageNotInverse = () => new Promise(resolve => { resolveSave = resolve });
            const bookReferenceClickSelector = 'a[data-role="anchor"]';
            const cfiFilter = null;
            class ContextHarness {
                #contextImage = null;
                #bookReferenceHandler = null;
                bookId = 'svg-cover-book';
                view = {getCFI: () => 'epubcfi(/6/2!/2/2/2)'};
                get contextImage() { return this.#contextImage; }
                attach(doc, index) {
    )") + contextHandler + QStringLiteral("}\n") + applyAction + QStringLiteral(R"(
            }
            const harness = new ContextHarness();
            harness.attach(document, 0);
            const svg = document.getElementById('cover');
            const image = document.getElementById('photo');
            const plain = document.getElementById('plain');
            const untouched = document.getElementById('untouched');
            const checks = [];
            const check = (condition, label) => { if (!condition) checks.push(label); };
            check(getComputedStyle(svg).filter.includes('invert(1)'), 'SVG initially inverted');
            check(contextImageElement(image) === image, 'SVG image recognized');
            check(contextImageElement(document.getElementById('cover-group')) === svg, 'SVG wrapper recognized');
            check(contextImageSource(svg) === image.getAttributeNS('http://www.w3.org/1999/xlink', 'href'), 'xlink image source');
            check(contextImageSource(plain) === 'images/photo.png', 'original EPUB image source preferred over rewritten src');

            const menuEvent = new MouseEvent('contextmenu', {bubbles: true, cancelable: true});
            image.dispatchEvent(menuEvent);
            check(menuEvent.defaultPrevented, 'SVG native menu suppressed');
            check(actions.includes('show-image-context-menu'), 'SVG image menu requested');
            check(harness.contextImage.element === image && !!harness.contextImage.cfi, 'SVG image selection captured');
            const pendingSave = harness.setContextImageNotInverse();
            check(!svg.classList.contains('not_inverse') && !image.classList.contains('not_inverse'), 'no optimistic local marker');
            resolveSave({status: 'updated', changed: true, modifiedBookIds: ['svg-cover-book']});
            await pendingSave;
            check(svg.classList.contains('not_inverse'), 'SVG marker');
            check(image.classList.contains('not_inverse'), 'image marker');
            check(svg.classList.contains('cover') && image.classList.contains('picture'), 'existing classes preserved');
            check(getComputedStyle(svg).filter === 'none' && getComputedStyle(image).filter === 'none', 'cover filters disabled');
            check(getComputedStyle(untouched).filter.includes('invert(1)'), 'unrelated SVG unchanged');
            check(getComputedStyle(plain).filter.includes('invert(1)'), 'unrelated img unchanged');

            svg.classList.remove('not_inverse');
            image.classList.remove('not_inverse');
            svg.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}));
            setImageNotInverse = () => Promise.resolve({status: 'updated', changed: true, modifiedBookIds: ['svg-cover-book']});
            await harness.setContextImageNotInverse();
            check(svg.classList.contains('not_inverse') && image.classList.contains('not_inverse'), 'wrapper action marks image');

            const plainEvent = new MouseEvent('contextmenu', {bubbles: true, cancelable: true});
            plain.dispatchEvent(plainEvent);
            check(!plainEvent.defaultPrevented, 'native img menu retained');
            check(harness.contextImage.src === 'images/photo.png', 'original EPUB image source sent to server');
            setImageNotInverse = () => Promise.reject(new Error('save rejected'));
            await harness.setContextImageNotInverse();
            check(!plain.classList.contains('not_inverse'), 'failed save is not shown as a local success');
            check(actions.includes('image-not-inverse-error'), 'failed save reported');

            const svg2 = new DOMParser().parseFromString(
                '<s:svg xmlns:s="http://www.w3.org/2000/svg"><s:svg><s:image href="cover.jpeg"/></s:svg></s:svg>', 'image/svg+xml');
            const nestedImage = svg2.getElementsByTagNameNS('http://www.w3.org/2000/svg', 'image')[0];
            check(contextImageElement(nestedImage) === nestedImage, 'prefixed image recognized');
            check(contextImageSource(nestedImage) === 'cover.jpeg', 'SVG2 href');
            markImageNotInverse(nestedImage);
            check(nestedImage.parentElement.classList.contains('not_inverse') && svg2.documentElement.classList.contains('not_inverse'),
                  'all SVG filter ancestors marked');
            return JSON.stringify(checks);
        })().then(result => { globalThis.imageContextTestResult = result });
    )");
    bool finished = false;
    QVariant result;
    QElapsedTimer timer;
    timer.start();
    page.runJavaScript(script);
    while (!finished && timer.elapsed() < 10000) {
        page.runJavaScript(QStringLiteral("globalThis.imageContextTestResult"), [&finished, &result](const QVariant &value) {
            if (value.isValid()) {
                result = value;
                finished = true;
            }
        });
        if (!finished) QTest::qWait(10);
    }
    QVERIFY(finished);
    QVERIFY2(result.isValid(), "Image context test JavaScript failed");
    const QJsonDocument failures = QJsonDocument::fromJson(result.toString().toUtf8());
    QVERIFY(failures.isArray());
    QVERIFY2(failures.array().isEmpty(), qPrintable(result.toString()));
}

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid()) {
        return 1;
    }
    const QByteArray directory = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", directory);
    qputenv("XDG_CONFIG_HOME", directory);
    qputenv("XDG_CACHE_HOME", directory);
    qputenv("XDG_RUNTIME_DIR", directory);
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    EpubImageInversionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "epubimageinversiontest.moc"
