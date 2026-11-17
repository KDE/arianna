// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "readercontextbuilder.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QWebEnginePage>
#include <QtWebEngineQuick>

namespace
{
QVariant run(QWebEnginePage &page, const QString &script)
{
    QVariant value;
    bool done = false;
    page.runJavaScript(script, [&](const QVariant &result) {
        value = result;
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 5000)
        QTest::qWait(10);
    return value;
}
}

class ReaderContextTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void missingMetadataAndFiltering()
    {
        ReaderContextBuilder builder;
        const QString text = QStringLiteral("  चेतनश्चेतनानाम् — cetanaś cetanānām\n");
        const auto snapshot = builder.build({}, {{QStringLiteral("content"), text}}, {});
        QCOMPARE(snapshot.value(QStringLiteral("selection")).toMap().value(QStringLiteral("text")).toString(), text);
        QVERIFY(snapshot.value(QStringLiteral("bibliography")).toMap().isEmpty());
        QVERIFY(snapshot.value(QStringLiteral("location")).toMap().isEmpty());
        QVERIFY(!snapshot.value(QStringLiteral("snapshotId")).toString().isEmpty());
        const auto edited = builder.withOptions(snapshot, {QStringLiteral("selection.text")}, QStringLiteral("An edit"));
        QCOMPARE(edited.value(QStringLiteral("selection")).toMap().value(QStringLiteral("text")).toString(), text);
        QCOMPARE(edited.value(QStringLiteral("selection")).toMap().value(QStringLiteral("userEditedText")).toString(), QStringLiteral("An edit"));
        QCOMPARE(snapshot.value(QStringLiteral("selection")).toMap().size(), 1);
        QVERIFY(builder
                    .build({{QStringLiteral("bookId"), QStringLiteral("new")}},
                           {{QStringLiteral("bookId"), QStringLiteral("old")}, {QStringLiteral("content"), text}},
                           {})
                    .isEmpty());
        const auto rich = builder.build(
            {{QStringLiteral("author"), QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Author")}}}},
             {QStringLiteral("translator"), QStringLiteral("Translator")},
             {QStringLiteral("editor"), QStringLiteral("Editor")},
             {QStringLiteral("edition"), QStringLiteral("2nd edition")}},
            {{QStringLiteral("content"), text},
             {QStringLiteral("readerContext"), QVariantMap{{QStringLiteral("paragraph"), text}, {QStringLiteral("section"), QStringLiteral("Larger")}}}},
            {});
        const auto filtered = builder.withOptions(rich, {QStringLiteral("bibliography.translators"), QStringLiteral("surrounding.section")}, text);
        QVERIFY(!filtered.value(QStringLiteral("bibliography")).toMap().contains(QStringLiteral("translators")));
        QVERIFY(!filtered.value(QStringLiteral("surrounding")).toMap().contains(QStringLiteral("section")));
        QVERIFY(rich.value(QStringLiteral("surrounding")).toMap().contains(QStringLiteral("section")));
        QCOMPARE(QJsonDocument::fromJson(builder.toJson(filtered).toUtf8()).object(), QJsonObject::fromVariantMap(filtered));
    }

    void domCapture_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("rangeScript");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("verse");
        QTest::newRow("inside EPUB paragraph") << QStringLiteral(
            "<h2>Chapter</h2><p id='p' lang='en'>First sentence. A selected term matters. Last sentence.</p>")
                                               << QStringLiteral("r.setStart(p.firstChild, 18); r.setEnd(p.firstChild, 31)") << QStringLiteral("selected term")
                                               << QStringLiteral("en") << QString();
        QTest::newRow("inline language switch") << QStringLiteral("<p id='p' lang='de'>Begriff <span lang='sa-Latn'>cetanaś cetanānām</span> erklärt.</p>")
                                                << QStringLiteral("r.selectNodeContents(p)") << QStringLiteral("Begriff cetanaś cetanānām erklärt.")
                                                << QStringLiteral("mul") << QString();
        QTest::newRow("explicit verse Unicode") << QStringLiteral("<p id='p' data-verse='2.2.13' xml:lang='sa-Deva'>चेतनश्चेतनानाम्</p>")
                                                << QStringLiteral("r.selectNodeContents(p)") << QStringLiteral("चेतनश्चेतनानाम्") << QStringLiteral("sa-Deva")
                                                << QStringLiteral("2.2.13");
        QTest::newRow("ambiguous verses") << QStringLiteral(
            "<h2>2.2.13</h2><p id='p' lang='sa' data-verse='2.2.13'>cetanaś <span data-verse='2.2.14'>cetanānām</span></p>")
                                          << QStringLiteral("r.selectNodeContents(p)") << QStringLiteral("cetanaś cetanānām") << QStringLiteral("sa")
                                          << QString();
        QTest::newRow("numbers do not imply verse") << QStringLiteral("<h2>2.2.13</h2><p id='p' lang='sa' class='verse' id='verse-13'>cetanaś cetanānām</p>")
                                                    << QStringLiteral("r.selectNodeContents(p)") << QStringLiteral("cetanaś cetanānām") << QStringLiteral("sa")
                                                    << QString();
        QTest::newRow("explicit empty language") << QStringLiteral("<p id='p' lang=''>A source.</p>") << QStringLiteral("r.selectNodeContents(p)")
                                                 << QStringLiteral("A source.") << QString() << QString();
        QTest::newRow("contradictory verse attributes")
            << QStringLiteral("<p id='p' lang='sa' data-verse='13' data-verse-number='14'>cetanaś cetanānām</p>") << QStringLiteral("r.selectNodeContents(p)")
            << QStringLiteral("cetanaś cetanānām") << QStringLiteral("sa") << QString();
        QTest::newRow("element selection boundaries") << QStringLiteral(
            "<h2>Chapter</h2><p id='p' lang='en'>First sentence. A <span>selected term</span> matters. Last sentence.</p>")
                                                      << QStringLiteral("r.setStart(p, 1); r.setEnd(p, 2)") << QStringLiteral("selected term")
                                                      << QStringLiteral("en") << QString();
    }

    void domCapture()
    {
        QFETCH(QString, html);
        QFETCH(QString, rangeScript);
        QFETCH(QString, text);
        QFETCH(QString, language);
        QFETCH(QString, verse);
        QFile source(QStringLiteral(ARIANNA_SOURCE_DIR "/src/qml/reader-context.js"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QString helper = QString::fromUtf8(source.readAll());
        helper.replace(QStringLiteral("export const collectReaderSelectionContext"), QStringLiteral("const collectReaderSelectionContext"));
        QWebEnginePage page;
        QSignalSpy loaded(&page, &QWebEnginePage::loadFinished);
        page.setHtml(QStringLiteral("<html lang='en'><body>") + html + QStringLiteral("</body></html>"));
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
        QVERIFY(loaded.first().first().toBool());
        const QString script = QStringLiteral(
                                   "(() => { %1; const original = document.documentElement.outerHTML; "
                                   "const p = document.getElementById('p'); const r = document.createRange(); %2; "
                                   "const selectedText = r.toString(); const offsets = [r.startOffset, r.endOffset]; "
                                   "const context = collectReaderSelectionContext(r, {bookLanguage: 'de'}); "
                                   "return JSON.stringify({context, selectedText, unchanged: original === document.documentElement.outerHTML "
                                   "&& offsets[0] === r.startOffset && offsets[1] === r.endOffset}); })()")
                                   .arg(helper, rangeScript);
        const auto captured = QJsonDocument::fromJson(run(page, script).toString().toUtf8()).object();
        QVERIFY(!captured.isEmpty());
        QVERIFY(captured.value(QStringLiteral("unchanged")).toBool());
        QCOMPARE(captured.value(QStringLiteral("selectedText")).toString(), text);
        const auto dom = captured.value(QStringLiteral("context")).toObject();
        QCOMPARE(dom.value(QStringLiteral("language")).toString(), language);
        QCOMPARE(dom.value(QStringLiteral("verse")).toString(), verse);
        const auto segments = dom.value(QStringLiteral("languageSegments")).toArray();
        QString reconstructed;
        for (const auto &segment : segments)
            reconstructed += segment.toObject().value(QStringLiteral("text")).toString();
        QCOMPARE(reconstructed, text);
        if (language == QLatin1String("mul")) {
            QCOMPARE(segments.size(), 3);
            QCOMPARE(segments.at(1).toObject().value(QStringLiteral("language")).toString(), QStringLiteral("sa-Latn"));
        }
        if (text == QLatin1String("selected term")) {
            QCOMPARE(dom.value(QStringLiteral("sentence")).toString(), QStringLiteral("A selected term matters. "));
            QCOMPARE(dom.value(QStringLiteral("heading")).toString(), QStringLiteral("Chapter"));
            QCOMPARE(dom.value(QStringLiteral("paragraph")).toString(), QStringLiteral("First sentence. A selected term matters. Last sentence."));
        }
        ReaderContextBuilder builder;
        const auto snapshot = builder.build({}, {{QStringLiteral("content"), text}, {QStringLiteral("readerContext"), dom.toVariantMap()}}, {});
        QCOMPARE(snapshot.value(QStringLiteral("selection")).toMap().value(QStringLiteral("text")).toString(), text);
        QVERIFY(!snapshot.value(QStringLiteral("bibliography")).toMap().contains(QStringLiteral("authors")));
        if (verse.isEmpty())
            QVERIFY(!snapshot.value(QStringLiteral("location")).toMap().contains(QStringLiteral("verse")));
    }
};

int main(int argc, char **argv)
{
    QtWebEngineQuick::initialize();
    QGuiApplication app(argc, argv);
    ReaderContextTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "readercontexttest.moc"
