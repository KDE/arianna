// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later

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

class EpubReferenceAnchorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void spanSourcesSupportClicksAndContextMenus();
};

#ifndef Q_MOC_RUN
void EpubReferenceAnchorTest::spanSourcesSupportClicksAndContextMenus()
{
    QFile reader(QFINDTESTDATA("../qml/reader.js"));
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QString source = QString::fromUtf8(reader.readAll());
    const auto section = [&source](const QString &startMarker, const QString &endMarker) {
        const qsizetype start = source.indexOf(startMarker);
        const qsizetype end = source.indexOf(endMarker, start);
        return start >= 0 && end > start ? source.mid(start, end - start) : QString();
    };
    const QString selectors = section(QStringLiteral("const crossReferenceAnchorSelector ="), QStringLiteral("const compactAnnotationId ="));
    const QString click = section(QStringLiteral("installReaderInteractions(doc, {"), QStringLiteral("this.#cursorAutohider.cloneFor"));
    const QString context = section(QStringLiteral("doc.addEventListener('contextmenu'"), QStringLiteral("let isSelecting ="));
    QVERIFY(!selectors.isEmpty());
    QVERIFY(!click.isEmpty());
    QVERIFY(!context.isEmpty());
    QFile interactions(QFINDTESTDATA("../qml/reader-interactions.js"));
    QVERIFY(interactions.open(QIODevice::ReadOnly));
    QString interactionHelpers = QString::fromUtf8(interactions.readAll());
    interactionHelpers.replace(QStringLiteral("export const "), QStringLiteral("const "));

    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
    page.setHtml(QStringLiteral(R"(<html><body>
        <span id="uuid_01a0a4a8-a9f0-7d8d-8933-2bc7a047bb8d" data-role="anchor" data-anchor-type="crossref" title="B.S. Vers 5.33"><em>B.S. 5.33</em></span>&#x20;
        <a id="existing" data-role="anchor" data-anchor-type="crossref">Existing</a>
        <span class="bookref" data-ref="legacy">Legacy</span>
        <a id="internal" data-role="anchor" href="#target">Internal</a>
        <span id="ordinary">Ordinary</span>
        <span id="target" data-role="anchor" data-anchor-type="target">Target</span>
        <span id="annotation" data-role="anchor" data-anchor-type="annotation">Annotation</span>
        </body></html>)"));
    QVERIFY(loaded.wait(10000));
    QVERIFY(loaded.constFirst().constFirst().toBool());
    const QString script = interactionHelpers + QLatin1Char('\n') + selectors + QStringLiteral(R"(;
        (() => {
            const errors = [];
            const check = (condition, message) => { if (!condition) errors.push(message); };
            const clicks = [], menus = [];
            class Harness {
                #contextImage = null;
                #annotationKeyByValue = new Map();
                #showAnnotation() { throw Error('Unexpected annotation action on plain reference'); }
                #bookReferenceHandler = {showContextMenu: (book, detail) => { menus.push(detail); return Promise.resolve(); }};
                book = {};
                view = new EventTarget();
                attach(doc, index = 0) {
                    this.view.renderer = {getContents:() => [{doc}]};
    )") + click + context + QStringLiteral(R"(
                }
            }
            const harness = new Harness();
            harness.view.addEventListener('book-reference', event => clicks.push(event.detail));
            harness.attach(document);
            const span = document.querySelector('span[data-anchor-type="crossref"]');
            check(document.querySelectorAll(bookReferenceListSelector).length === 4, 'reference list includes both source tags and the internal link');
            for (const element of [span, document.getElementById('existing'), document.querySelector('.bookref')]) {
                const child = element.firstElementChild || element;
                for (const type of ['click', 'contextmenu']) {
                    const event = new MouseEvent(type, {bubbles: true, cancelable: true});
                    child.dispatchEvent(event);
                    check(event.defaultPrevented, type + ' handled for ' + element.outerHTML);
                }
            }
            check(clicks.length === 3 && menus.length === 3, 'all source types dispatch once');
            for (const id of ['internal', 'ordinary', 'target', 'annotation']) {
                const event = new MouseEvent('contextmenu', {bubbles:true,cancelable:true});
                document.getElementById(id).dispatchEvent(event);
                check(!event.defaultPrevented, id + ' retains native context menu');
            }
            const modified = new MouseEvent('contextmenu', {bubbles:true,cancelable:true,shiftKey:true});
            span.dispatchEvent(modified);
            check(!modified.defaultPrevented && menus.length === 3, 'modified right-click retains native menu');
            for (const detail of [clicks[0], menus[0]]) {
                check(detail?.ref === span.id, 'UUID preserved');
                check(detail?.title === 'B.S. Vers 5.33' && detail?.text === 'B.S. 5.33', 'source text and title preserved');
            }
            for (const id of ['internal', 'ordinary', 'target', 'annotation'])
                check(!document.getElementById(id).matches(bookReferenceClickSelector), id + ' is not a crossref source');
            check(getComputedStyle(span).display !== 'none' && span.textContent === 'B.S. 5.33', 'source text stays visible');
            const xml = new DOMParser().parseFromString('<x:span xmlns:x="http://www.w3.org/1999/xhtml" id="prefixed" data-role="anchor" data-anchor-type="crossref"/>', 'application/xhtml+xml');
            check(xml.documentElement.matches(crossReferenceAnchorSelector), 'prefixed XHTML span recognized');
            return JSON.stringify(errors);
        })();
    )");
    bool finished = false;
    QVariant result;
    const QString guardedScript = QStringLiteral("(() => { try { return eval(%1[0]); } catch (error) { return JSON.stringify([String(error)]); } })()")
                                      .arg(QString::fromUtf8(QJsonDocument(QJsonArray{script}).toJson(QJsonDocument::Compact)));
    page.runJavaScript(guardedScript, [&](const QVariant &value) {
        result = value;
        finished = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
    const auto failures = QJsonDocument::fromJson(result.toString().toUtf8());
    QVERIFY2(failures.isArray(), "Reader JavaScript test did not complete");
    QVERIFY2(failures.array().isEmpty(), qPrintable(result.toString()));
}

int main(int argc, char **argv)
{
    QTemporaryDir environment;
    if (!environment.isValid()) return 1;
    const QByteArray directory = QFile::encodeName(environment.path());
    qputenv("XDG_DATA_HOME", directory);
    qputenv("XDG_CONFIG_HOME", directory);
    qputenv("XDG_CACHE_HOME", directory);
    qputenv("XDG_RUNTIME_DIR", directory);
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    EpubReferenceAnchorTest test;
    return QTest::qExec(&test, argc, argv);
}

#endif

#include "epubreferenceanchortest.moc"
